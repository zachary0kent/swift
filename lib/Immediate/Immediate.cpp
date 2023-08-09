//===--- Immediate.cpp - the swift immediate mode -------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This is the implementation of the swift interpreter, which takes a
// source file and JITs it.
//
//===----------------------------------------------------------------------===//

#include "swift/Immediate/Immediate.h"
#include "ImmediateImpl.h"
#include "swift/Immediate/SwiftMaterializationUnit.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/IRGenRequests.h"
#include "swift/AST/Module.h"
#include "swift/AST/SILGenRequests.h"
#include "swift/AST/TBDGenRequests.h"
#include "swift/Basic/LLVM.h"
#include "swift/Frontend/Frontend.h"
#include "swift/IRGen/IRGenPublic.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Config/config.h"
#include "llvm/ExecutionEngine/Orc/DebugUtils.h"
#include "llvm/ExecutionEngine/Orc/EPCIndirectionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/TargetExecutionUtils.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Path.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#define DEBUG_TYPE "swift-immediate"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace swift;
using namespace swift::immediate;

static void *loadRuntimeLib(StringRef runtimeLibPathWithName) {
#if defined(_WIN32)
  return LoadLibraryA(runtimeLibPathWithName.str().c_str());
#else
  return dlopen(runtimeLibPathWithName.str().c_str(), RTLD_LAZY | RTLD_GLOBAL);
#endif
}

static void *loadRuntimeLibAtPath(StringRef sharedLibName,
                                  StringRef runtimeLibPath) {
  // FIXME: Need error-checking.
  llvm::SmallString<128> Path = runtimeLibPath;
  llvm::sys::path::append(Path, sharedLibName);
  return loadRuntimeLib(Path);
}

static void *loadRuntimeLib(StringRef sharedLibName,
                            ArrayRef<std::string> runtimeLibPaths) {
  for (auto &runtimeLibPath : runtimeLibPaths) {
    if (void *handle = loadRuntimeLibAtPath(sharedLibName, runtimeLibPath))
      return handle;
  }
  return nullptr;
}

static void DumpLLVMIR(const llvm::Module &M) {
  std::string path = (M.getName() + ".ll").str();
  for (size_t count = 0; llvm::sys::fs::exists(path); )
    path = (M.getName() + llvm::utostr(count++) + ".ll").str();

  std::error_code error;
  llvm::raw_fd_ostream stream(path, error);
  if (error)
    return;
  M.print(stream, /*AssemblyAnnotationWriter=*/nullptr);
}

void *swift::immediate::loadSwiftRuntime(ArrayRef<std::string>
                                         runtimeLibPaths) {
#if defined(_WIN32)
  return loadRuntimeLib("swiftCore" LTDL_SHLIB_EXT, runtimeLibPaths);
#else
  return loadRuntimeLib("libswiftCore" LTDL_SHLIB_EXT, runtimeLibPaths);
#endif
}

static bool tryLoadLibrary(LinkLibrary linkLib,
                           SearchPathOptions searchPathOpts) {
  llvm::SmallString<128> path = linkLib.getName();

  // If we have an absolute or relative path, just try to load it now.
  if (llvm::sys::path::has_parent_path(path.str())) {
    return loadRuntimeLib(path);
  }

  bool success = false;
  switch (linkLib.getKind()) {
  case LibraryKind::Library: {
    llvm::SmallString<32> stem;
    if (llvm::sys::path::has_extension(path.str())) {
      stem = std::move(path);
    } else {
      // FIXME: Try the appropriate extension for the current platform?
      stem = "lib";
      stem += path;
      stem += LTDL_SHLIB_EXT;
    }

    // Try user-provided library search paths first.
    for (auto &libDir : searchPathOpts.LibrarySearchPaths) {
      path = libDir;
      llvm::sys::path::append(path, stem.str());
      success = loadRuntimeLib(path);
      if (success)
        break;
    }

    // Let loadRuntimeLib determine the best search paths.
    if (!success)
      success = loadRuntimeLib(stem);

    // If that fails, try our runtime library paths.
    if (!success)
      success = loadRuntimeLib(stem, searchPathOpts.RuntimeLibraryPaths);
    break;
  }
  case LibraryKind::Framework: {
    // If we have a framework, mangle the name to point to the framework
    // binary.
    llvm::SmallString<64> frameworkPart{std::move(path)};
    frameworkPart += ".framework";
    llvm::sys::path::append(frameworkPart, linkLib.getName());

    // Try user-provided framework search paths first; frameworks contain
    // binaries as well as modules.
    for (const auto &frameworkDir : searchPathOpts.getFrameworkSearchPaths()) {
      path = frameworkDir.Path;
      llvm::sys::path::append(path, frameworkPart.str());
      success = loadRuntimeLib(path);
      if (success)
        break;
    }

    // If that fails, let loadRuntimeLib search for system frameworks.
    if (!success)
      success = loadRuntimeLib(frameworkPart);
    break;
  }
  }

  return success;
}

bool swift::immediate::tryLoadLibraries(ArrayRef<LinkLibrary> LinkLibraries,
                                        SearchPathOptions SearchPathOpts,
                                        DiagnosticEngine &Diags) {
  SmallVector<bool, 4> LoadedLibraries;
  LoadedLibraries.append(LinkLibraries.size(), false);

  // Libraries are not sorted in the topological order of dependencies, and we
  // don't know the dependencies in advance.  Try to load all libraries until
  // we stop making progress.
  bool HadProgress;
  do {
    HadProgress = false;
    for (unsigned i = 0; i != LinkLibraries.size(); ++i) {
      if (!LoadedLibraries[i] &&
          tryLoadLibrary(LinkLibraries[i], SearchPathOpts)) {
        LoadedLibraries[i] = true;
        HadProgress = true;
      }
    }
  } while (HadProgress);

  return std::all_of(LoadedLibraries.begin(), LoadedLibraries.end(),
                     [](bool Value) { return Value; });
}

/// Workaround for rdar://94645534.
///
/// The framework layout of some frameworks have changed over time, causing
/// unresolved symbol errors in immediate mode when running on older OS versions
/// with a newer SDK. This workaround scans through the list of dependencies and
/// manually adds the right libraries as necessary.
///
/// FIXME: JITLink should emulate the Darwin linker's handling of ld$previous
/// mappings so this is handled automatically.
static void addMergedLibraries(SmallVectorImpl<LinkLibrary> &AllLinkLibraries,
                               const llvm::Triple &Target) {
  assert(Target.isMacOSX());

  struct MergedLibrary {
    StringRef OldLibrary;
    llvm::VersionTuple MovedIn;
  };

  using VersionTuple = llvm::VersionTuple;

  static const llvm::StringMap<MergedLibrary> MergedLibs = {
    // Merged in macOS 14.0
    {"AppKit", {"libswiftAppKit.dylib", VersionTuple{14}}},
    {"HealthKit", {"libswiftHealthKit.dylib", VersionTuple{14}}},
    {"Network", {"libswiftNetwork.dylib", VersionTuple{14}}},
    {"Photos", {"libswiftPhotos.dylib", VersionTuple{14}}},
    {"PhotosUI", {"libswiftPhotosUI.dylib", VersionTuple{14}}},
    {"SoundAnalysis", {"libswiftSoundAnalysis.dylib", VersionTuple{14}}},
    {"Virtualization", {"libswiftVirtualization.dylib", VersionTuple{14}}},
    // Merged in macOS 13.0
    {"Foundation", {"libswiftFoundation.dylib", VersionTuple{13}}},
  };

  SmallVector<StringRef> NewLibs;
  for (auto &Lib : AllLinkLibraries) {
    auto I = MergedLibs.find(Lib.getName());
    if (I != MergedLibs.end() && Target.getOSVersion() < I->second.MovedIn)
      NewLibs.push_back(I->second.OldLibrary);
  }

  for (StringRef NewLib : NewLibs)
    AllLinkLibraries.push_back(LinkLibrary(NewLib, LibraryKind::Library));
}

bool swift::immediate::autolinkImportedModules(ModuleDecl *M,
                                               const IRGenOptions &IRGenOpts) {
  // Perform autolinking.
  SmallVector<LinkLibrary, 4> AllLinkLibraries(IRGenOpts.LinkLibraries);
  auto addLinkLibrary = [&](LinkLibrary linkLib) {
    AllLinkLibraries.push_back(linkLib);
  };

  M->collectLinkLibraries(addLinkLibrary);

  auto &Target = M->getASTContext().LangOpts.Target;
  if (Target.isMacOSX())
    addMergedLibraries(AllLinkLibraries, Target);

  tryLoadLibraries(AllLinkLibraries, M->getASTContext().SearchPathOpts,
                   M->getASTContext().Diags);
  return false;
}

/// The suffix appended to function bodies when creating lazy reexports
static const std::string ManglingSuffix = "$impl";

/// Mangle a function for a lazy reexport
static std::string mangleFunctionBody(const StringRef Unmangled) {
  return Unmangled.str() + ManglingSuffix;
}

/// Creates an `LLJIT` instance with the given target options and an
/// attached generator that resolves symbols from the current process
static llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>>
createLLJIT(const IRGenOptions &IRGenOpts, ASTContext &Ctx) {
  llvm::TargetOptions TargetOpt;
  std::string CPU;
  std::string Triple;
  std::vector<std::string> Features;
  std::tie(TargetOpt, CPU, Features, Triple) =
      getIRTargetOptions(IRGenOpts, Ctx);
  auto JTMB = llvm::orc::JITTargetMachineBuilder(llvm::Triple(Triple))
                  .setRelocationModel(llvm::Reloc::PIC_)
                  .setOptions(std::move(TargetOpt))
                  .setCPU(std::move(CPU))
                  .addFeatures(Features)
                  .setCodeGenOptLevel(llvm::CodeGenOpt::Default);
  auto J = llvm::orc::LLJITBuilder()
               .setJITTargetMachineBuilder(std::move(JTMB))
               .create();
  if (!J)
    return J.takeError();
  auto G = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
      (*J)->getDataLayout().getGlobalPrefix());
  if (!G)
    return G.takeError();
  (*J)->getMainJITDylib().addGenerator(std::move(*G));
  return J;
}

/// Dump the contents of `Module` if requested
static void dumpJIT(llvm::orc::LLJIT &JIT, const llvm::Module &Module,
                    const IRGenOptions &IRGenOpts) {
  LLVM_DEBUG(llvm::dbgs() << "Module to be executed:\n"; Module.dump());
  switch (IRGenOpts.DumpJIT) {
  case JITDebugArtifact::None:
    break;
  case JITDebugArtifact::LLVMIR:
    DumpLLVMIR(Module);
    break;
  case JITDebugArtifact::Object:
    JIT.getObjTransformLayer().setTransform(llvm::orc::DumpObjects());
    break;
  }
}

/// IRGen the provided `SILModule` with the specified options.
/// Returns `std::nullopt` if a compiler error is encountered
static std::optional<GeneratedModule>
generateModule(const CompilerInstance &CI, std::unique_ptr<SILModule> SM) {
  // TODO: Use OptimizedIRRequest for this.
  const auto &Context = CI.getASTContext();
  auto *swiftModule = CI.getMainModule();
  const auto PSPs = CI.getPrimarySpecificPathsForAtMostOnePrimary();
  const auto &Invocation = CI.getInvocation();
  const auto &TBDOpts = Invocation.getTBDGenOptions();
  const auto &IRGenOpts = Invocation.getIRGenOptions();

  // Lower the SIL module to LLVM IR
  auto GenModule = performIRGeneration(
      swiftModule, IRGenOpts, TBDOpts, std::move(SM),
      swiftModule->getName().str(), PSPs, ArrayRef<std::string>());

  if (Context.hadError()) {
    return std::nullopt;
  }

  assert(GenModule && "Emitted no diagnostics but IR generation failed?");
  auto *Module = GenModule.getModule();

  // Run LLVM passes on the resulting module
  performLLVM(IRGenOpts, Context.Diags, /*diagMutex*/ nullptr,
              /*hash*/ nullptr, Module, GenModule.getTargetMachine(),
              CI.getPrimarySpecificPathsForAtMostOnePrimary().OutputFilename,
              CI.getOutputBackend(), Context.Stats);

  if (Context.hadError()) {
    return std::nullopt;
  }

  return GenModule;
}

/// Log a compilation error to standard error
static void logError(llvm::Error Err) {
  logAllUnhandledErrors(std::move(Err), llvm::errs(), "");
}

/// Lazily materializes an entire SIL module
class SILMaterializationUnit : public llvm::orc::MaterializationUnit {
public:
  SILMaterializationUnit(SwiftJIT &JIT, const CompilerInstance &CI,
                         const IRGenOptions &IRGenOpts,
                         std::unique_ptr<SILModule> SM)
      : MaterializationUnit(getInterface(JIT, CI)), JIT(JIT), CI(CI),
        IRGenOpts(IRGenOpts), SM(std::move(SM)) {}

  void materialize(
      std::unique_ptr<llvm::orc::MaterializationResponsibility> R) override {

    auto GenModule = generateModule(CI, std::move(SM));

    if (!GenModule) {
      R->failMaterialization();
      return;
    }

    auto *Module = GenModule->getModule();

    // Dump IR if requested
    dumpJIT(*Module);

    // Now we must register all other public symbols defined by
    // the module with the JIT
    llvm::orc::SymbolFlagsMap Symbols;
    // Register all global values, including global
    // variables and functions
    for (const auto &GV : Module->global_values()) {
      addGlobal(Symbols, GV);
    }
    // Register the symbols we have discovered with the JIT
    if (auto Err = R->defineMaterializing(Symbols)) {
      logError(std::move(Err));
    }
    auto TSM = std::move(*GenModule).intoThreadSafeContext();
    JIT.getIRCompileLayer().emit(std::move(R), std::move(TSM));
  }

  StringRef getName() const override { return "SILMaterializationUnit"; }

private:
  /// Dump the contents of `Module` if requested
  void dumpJIT(const llvm::Module &Module) {
    ::dumpJIT(JIT.getJIT(), Module, IRGenOpts);
  }

  /// All global value `Global` to `Symbols` if it is a public definition
  void addGlobal(llvm::orc::SymbolFlagsMap &Symbols,
                 const llvm::GlobalValue &Global) {
    // Ignore all symbols that will not appear in symbol table
    if (Global.hasLocalLinkage() || Global.isDeclaration() ||
        Global.hasAppendingLinkage())
      return;
    auto Name = Global.getName();
    // The entry point is already registered up front with the
    // interface, so ignore it as well
    if (Name == CI.getASTContext().getEntryPointFunctionName())
      return;
    auto MangledName = JIT.mangleAndIntern(Name);
    // Register this symbol with the proper flags
    Symbols[MangledName] = llvm::JITSymbolFlags::fromGlobalValue(Global);
  }

  void discard(const llvm::orc::JITDylib &JD,
               const llvm::orc::SymbolStringPtr &Sym) override {}

  /// Get the public interface of the main module, which for a script just
  /// comprises the entry point
  static MaterializationUnit::Interface
  getInterface(SwiftJIT &JIT, const CompilerInstance &CI) {
    const auto &EntryPoint = CI.getASTContext().getEntryPointFunctionName();
    auto MangledEntryPoint =
        JIT.mangleAndIntern(mangleFunctionBody(EntryPoint));
    auto Flags =
        llvm::JITSymbolFlags::Callable | llvm::JITSymbolFlags::Exported;
    llvm::orc::SymbolFlagsMap Symbols{{MangledEntryPoint, Flags}};
    return {std::move(Symbols), nullptr};
  }

  SwiftJIT &JIT;
  const CompilerInstance &CI;
  const IRGenOptions &IRGenOpts;
  std::unique_ptr<SILModule> SM;
};

/// Lookup the entry point in `J` and run it with the given command line
/// arguments `CmdLine`. Returns `-1` if failed to compile, or the status
/// returned by the entry point following execution.
static int runMain(llvm::orc::LLJIT &J, const ProcessCmdLine &CmdLine) {
  LLVM_DEBUG(llvm::dbgs() << "Running static constructors\n");
  if (auto Err = J.initialize(J.getMainJITDylib())) {
    logError(std::move(Err));
    return -1;
  }

  auto MainSym = J.lookup("main");
  if (!MainSym) {
    logError(MainSym.takeError());
    return -1;
  }

  using MainFnTy = int (*)(int, char *[]);
  MainFnTy JITMain = MainSym->toPtr<MainFnTy>();

  LLVM_DEBUG(llvm::dbgs() << "Running main\n");
  int Result = llvm::orc::runAsMain(JITMain, CmdLine);

  LLVM_DEBUG(llvm::dbgs() << "Running static destructors\n");
  if (auto Err = J.deinitialize(J.getMainJITDylib())) {
    logError(std::move(Err));
    return -1;
  }

  return Result;
}

int swift::RunImmediately(CompilerInstance &CI, const ProcessCmdLine &CmdLine,
                          const IRGenOptions &IRGenOpts,
                          const SILOptions &SILOpts,
                          std::unique_ptr<SILModule> &&SM) {
  
  auto &Context = CI.getASTContext();
  
  // Load libSwiftCore to setup process arguments.
  //
  // This must be done here, before any library loading has been done, to avoid
  // racing with the static initializers in user code.
  // Setup interpreted process arguments.
  using ArgOverride = void (* SWIFT_CC(swift))(const char **, int);
#if defined(_WIN32)
  auto stdlib = loadSwiftRuntime(Context.SearchPathOpts.RuntimeLibraryPaths);
  if (!stdlib) {
    CI.getDiags().diagnose(SourceLoc(),
                           diag::error_immediate_mode_missing_stdlib);
    return -1;
  }
  auto module = static_cast<HMODULE>(stdlib);
  auto emplaceProcessArgs = reinterpret_cast<ArgOverride>(
    GetProcAddress(module, "_swift_stdlib_overrideUnsafeArgvArgc"));
  if (emplaceProcessArgs == nullptr)
    return -1;
#else
  // In case the compiler is built with swift modules, it already has the stdlib
  // linked to. First try to lookup the symbol with the standard library
  // resolving.
  auto emplaceProcessArgs
     = (ArgOverride)dlsym(RTLD_DEFAULT, "_swift_stdlib_overrideUnsafeArgvArgc");

  if (dlerror()) {
    // If this does not work (= the Swift modules are not linked to the tool),
    // we have to explicitly load the stdlib.
    auto stdlib = loadSwiftRuntime(Context.SearchPathOpts.RuntimeLibraryPaths);
    if (!stdlib) {
      CI.getDiags().diagnose(SourceLoc(),
                             diag::error_immediate_mode_missing_stdlib);
      return -1;
    }
    dlerror();
    emplaceProcessArgs
            = (ArgOverride)dlsym(stdlib, "_swift_stdlib_overrideUnsafeArgvArgc");
    if (dlerror())
      return -1;
  }
#endif

  SmallVector<const char *, 32> argBuf;
  for (size_t i = 0; i < CmdLine.size(); ++i) {
    argBuf.push_back(CmdLine[i].c_str());
  }
  argBuf.push_back(nullptr);

  (*emplaceProcessArgs)(argBuf.data(), CmdLine.size());

  auto *swiftModule = CI.getMainModule();
  if (autolinkImportedModules(swiftModule, IRGenOpts))
    return -1;

  auto &Target = swiftModule->getASTContext().LangOpts.Target;
  if (Target.isMacOSX()) {
    auto JIT = SwiftJIT::Create(CI);
    if (auto Err = JIT.takeError()) {
      logError(std::move(Err));
      return -1;
    }

    auto MU = std::make_unique<SILMaterializationUnit>(**JIT, CI, IRGenOpts,
                                                       std::move(SM));
    if (auto Err = (*JIT)->addSwift((*JIT)->getMainJITDylib(), std::move(MU))) {
      logError(std::move(Err));
      return -1;
    }

    return runMain((*JIT)->getJIT(), CmdLine);
  }
  auto JIT = createLLJIT(IRGenOpts, swiftModule->getASTContext());
  if (auto Err = JIT.takeError()) {
    logError(std::move(Err));
    return -1;
  }
  auto GenModule = generateModule(CI, std::move(SM));
  if (!GenModule)
    return -1;
  auto *Module = GenModule->getModule();
  dumpJIT(**JIT, *Module, IRGenOpts);
  auto TSM = std::move(*GenModule).intoThreadSafeContext();
  if (auto Err = (*JIT)->addIRModule(std::move(TSM))) {
    logError(std::move(Err));
    return -1;
  }
  return runMain(**JIT, CmdLine);
}

int swift::RunImmediatelyFromAST(CompilerInstance &CI) {
  CI.performSema();
  auto &Context = CI.getASTContext();
  if (Context.hadError()) {
    return -1;
  }
  const auto &Invocation = CI.getInvocation();
  const auto &FrontendOpts = Invocation.getFrontendOptions();

  const ProcessCmdLine &CmdLine = ProcessCmdLine(
      FrontendOpts.ImmediateArgv.begin(), FrontendOpts.ImmediateArgv.end());

  // Load libSwiftCore to setup process arguments.
  //
  // This must be done here, before any library loading has been done, to avoid
  // racing with the static initializers in user code.
  // Setup interpreted process arguments.
  using ArgOverride = void (*SWIFT_CC(swift))(const char **, int);
#if defined(_WIN32)
  auto stdlib = loadSwiftRuntime(Context.SearchPathOpts.RuntimeLibraryPaths);
  if (!stdlib) {
    CI.getDiags().diagnose(SourceLoc(),
                           diag::error_immediate_mode_missing_stdlib);
    return -1;
  }
  auto module = static_cast<HMODULE>(stdlib);
  auto emplaceProcessArgs = reinterpret_cast<ArgOverride>(
      GetProcAddress(module, "_swift_stdlib_overrideUnsafeArgvArgc"));
  if (emplaceProcessArgs == nullptr)
    return -1;
#else
  // In case the compiler is built with swift modules, it already has the stdlib
  // linked to. First try to lookup the symbol with the standard library
  // resolving.
  auto emplaceProcessArgs =
      (ArgOverride)dlsym(RTLD_DEFAULT, "_swift_stdlib_overrideUnsafeArgvArgc");

  if (dlerror()) {
    // If this does not work (= the Swift modules are not linked to the tool),
    // we have to explicitly load the stdlib.
    auto stdlib = loadSwiftRuntime(Context.SearchPathOpts.RuntimeLibraryPaths);
    if (!stdlib) {
      CI.getDiags().diagnose(SourceLoc(),
                             diag::error_immediate_mode_missing_stdlib);
      return -1;
    }
    dlerror();
    emplaceProcessArgs =
        (ArgOverride)dlsym(stdlib, "_swift_stdlib_overrideUnsafeArgvArgc");
    if (dlerror())
      return -1;
  }
#endif

  SmallVector<const char *, 32> argBuf;
  for (size_t i = 0; i < CmdLine.size(); ++i) {
    argBuf.push_back(CmdLine[i].c_str());
  }
  argBuf.push_back(nullptr);

  (*emplaceProcessArgs)(argBuf.data(), CmdLine.size());

  auto *swiftModule = CI.getMainModule();
  const auto &IRGenOpts = Invocation.getIRGenOptions();
  if (autolinkImportedModules(swiftModule, IRGenOpts))
    return -1;

  auto &Target = swiftModule->getASTContext().LangOpts.Target;
  assert(Target.isMacOSX());
  auto JIT = SwiftJIT::Create(CI);
  if (auto Err = JIT.takeError()) {
    logError(std::move(Err));
    return -1;
  }

  auto MU = SwiftMaterializationUnit::Create(**JIT, CI);
  if (auto Err = (*JIT)->addSwift((*JIT)->getMainJITDylib(), std::move(MU))) {
    logError(std::move(Err));
    return -1;
  }

  return runMain((*JIT)->getJIT(), CmdLine);
}
