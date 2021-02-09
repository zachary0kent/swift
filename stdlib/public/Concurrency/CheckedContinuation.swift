import Swift

@_silgen_name("swift_continuation_logFailedCheck")
internal func logFailedCheck(_ message: UnsafeRawPointer)

/// Implementation class that holds the `UnsafeContinuation` instance for
/// a `CheckedContinuation`.
internal final class CheckedContinuationCanary {
  // The instance state is stored in tail-allocated raw memory, so that
  // we can atomically check the continuation state.

  private init() { fatalError("must use create") }

  private static func _create(continuation: UnsafeRawPointer, function: String)
      -> Self {
    let instance = Builtin.allocWithTailElems_1(self,
      1._builtinWordValue,
      (UnsafeRawPointer?, String).self)

    instance._continuationPtr.initialize(to: continuation)
    instance._functionPtr.initialize(to: function)
    return instance
  }

  private var _continuationPtr: UnsafeMutablePointer<UnsafeRawPointer?> {
    return UnsafeMutablePointer<UnsafeRawPointer?>(
      Builtin.projectTailElems(self, (UnsafeRawPointer?, String).self))
  }
  private var _functionPtr: UnsafeMutablePointer<String> {
    let tailPtr = UnsafeMutableRawPointer(
      Builtin.projectTailElems(self, (UnsafeRawPointer?, String).self))

    let functionPtr = tailPtr 
        + MemoryLayout<(UnsafeRawPointer?, String)>.offset(of: \(UnsafeRawPointer?, String).1)!

    return functionPtr.assumingMemoryBound(to: String.self)
  }

  internal static func create<T, E>(continuation: UnsafeContinuation<T, E>,
                                 function: String) -> Self {
    return _create(
        continuation: unsafeBitCast(continuation, to: UnsafeRawPointer.self),
        function: function)
  }

  internal var function: String {
    return _functionPtr.pointee
  }

  // Take the continuation away from the container, or return nil if it's
  // already been taken.
  internal func takeContinuation<T, E>() -> UnsafeContinuation<T, E>? {
    // Atomically exchange the current continuation value with a null pointer.
    let rawContinuationPtr = unsafeBitCast(_continuationPtr,
      to: Builtin.RawPointer.self)
    let rawOld = Builtin.atomicrmw_xchg_seqcst_Word(rawContinuationPtr,
      0._builtinWordValue)

    return unsafeBitCast(rawOld, to: UnsafeContinuation<T, E>?.self)
  }

  deinit {
    _functionPtr.deinitialize(count: 1)
    // Log if the continuation was never consumed before the instance was
    // destructed.
    if _continuationPtr.pointee != nil {
      logFailedCheck("SWIFT TASK CONTINUATION MISUSE: \(function) leaked its continuation!\n")
    }
  }
}

/// A wrapper class for `UnsafeContinuation` that logs misuses of the
/// continuation, logging a message if the continuation is resumed
/// multiple times, or if an object is destroyed without its continuation
/// ever being resumed.
///
/// Raw `UnsafeContinuation`, like other unsafe constructs, requires the
/// user to apply it correctly in order to maintain invariants. The key
/// invariant is that the continuation must be resumed exactly once,
/// and bad things happen if this invariant is not upheld--if a continuation
/// is abandoned without resuming the task, then the task will be stuck in
/// the suspended state forever, and conversely, if the same continuation is
/// resumed multiple times, it will put the task in an undefined state.
///
/// `UnsafeContinuation` avoids enforcing these invariants at runtime because
/// it aims to be a low-overhead mechanism for interfacing Swift tasks with
/// event loops, delegate methods, callbacks, and other non-`async` scheduling
/// mechanisms. However, during development, being able to verify that the
/// invariants are being upheld in testing is important.
///
/// `CheckedContinuation` is designed to be a drop-in API replacement for
/// `UnsafeContinuation` that can be used for testing purposes, at the cost of
/// an extra allocation and indirection for the wrapper object. Changing a call
/// of `withUnsafeContinuation` or `withUnsafeThrowingContinuation` into a call
/// of `withCheckedContinuation` or `withCheckedThrowingContinuation` should be
/// enough to obtain the extra checking without further source modification in
/// most circumstances.
public struct CheckedContinuation<T, E: Error>: _Continuation {
  private let canary: CheckedContinuationCanary
  
  /// Initialize a `CheckedContinuation` wrapper around an
  /// `UnsafeContinuation`.
  ///
  /// In most cases, you should use `withCheckedContinuation` or
  /// `withCheckedThrowingContinuation` instead. You only need to initialize
  /// your own `CheckedContinuation<T, E>` if you already have an
  /// `UnsafeContinuation` you want to impose checking on.
  ///
  /// - Parameters:
  ///   - continuation: a fresh `UnsafeContinuation` that has not yet
  ///     been resumed. The `UnsafeContinuation` must not be used outside of
  ///     this object once it's been given to the new object.
  ///   - function: a string identifying the declaration that is the notional
  ///     source for the continuation, used to identify the continuation in
  ///     runtime diagnostics related to misuse of this continuation.
  public init(continuation: UnsafeContinuation<T, E>, function: String = #function) {
    canary = CheckedContinuationCanary.create(
      continuation: continuation,
      function: function)
  }
  
  public func resume(returning x: __owned T) {
    if let c: UnsafeContinuation<T, E> = canary.takeContinuation() {
      c.resume(returning: x)
    } else {
      fatalError("SWIFT TASK CONTINUATION MISUSE: \(canary.function) tried to resume its continuation more than once, returning \(x)!\n")
    }
  }
  
  public func resume(throwing x: __owned E) {
    if let c: UnsafeContinuation<T, E> = canary.takeContinuation() {
      c.resume(throwing: x)
    } else {
      fatalError("SWIFT TASK CONTINUATION MISUSE: \(canary.function) tried to resume its continuation more than once, throwing \(x)!\n")
    }
  }
}

public func withCheckedContinuation<T>(
    function: String = #function,
    _ body: (CheckedContinuation<T, Never>) -> Void
) async -> T {
  return await withUnsafeContinuation {
    body(CheckedContinuation(continuation: $0, function: function))
  }
}

public func withCheckedThrowingContinuation<T>(
    function: String = #function,
    _ body: (CheckedContinuation<T, Error>) -> Void
) async throws -> T {
  return try await withUnsafeThrowingContinuation {
    body(CheckedContinuation(continuation: $0, function: function))
  }
}

