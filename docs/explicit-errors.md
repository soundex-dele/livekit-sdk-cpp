# Explicit SDK errors

Project-owned SDK operations use `Result<T>`, `ErrorCode`, or an existing explicit status. Public APIs no longer expose `SDKError`, `exception_ptr`, or exception-based completion. This is a source-breaking change; callers must check an operation before reading its value.

```cpp
auto result = operation();
if (!result) {
  return Result<void>::failure(result.failure_record());
}
auto value = *std::move(result);
```

Propagate `failure_record()` to retain the error ID, domain, code, backend details and context. Use `with_context()` when an enclosing operation adds useful track/node information. Do not replace a failed result with a default value or convert it to an exception.

- `AsyncResult<T>::get()` and `as_future().get()` return `Result<T>`. An abandoned completion reports cancellation; duplicate completion returns an explicit state error. Continuations must return their own failure as a `Result`.
- Room service RPC, data and stream futures contain `Result<T>`. Callback interfaces that can reject work return a result. Event dispatchers report scheduling rejection explicitly.
- Media and player operations report initialization, input/output and processing failures explicitly. Types retaining a constructor expose `status()` or their documented `check()` method; check it before use. Media consumers return `Result<void>`.
- Server SDK calls, token creation, encoding, Studio operations and remote-control setup return results. Examples show the updated call pattern.
- `support/rtc_result.hpp` was removed. The similarly named helper under `tests/` is an assertion for tests only, and must not be used by applications.

Migrated business targets are compiled with exceptions disabled. CLI11 argument parsing and Windows C++/WinRT capture are isolated exception-enabled object targets; their typed catches immediately convert external failures to the local result/exit status. GraphFlow remains a separate dependency with a compatibility adapter; SDK graphs use its explicit `GraphError` channel.

Run `python tests/no_exceptions_contract.py` to check all project-owned C++ sources, headers, examples and tests for exception constructs. The guard allows only the two named external adapter files and their expected catch types. It complements compiler checks and behavioral tests; it does not establish that external libraries never throw, or substitute for platform/device acceptance.
