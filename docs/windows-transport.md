# Windows source cancellation

The transport opens WinHTTP with `WINHTTP_FLAG_ASYNC`. HTTP handshake operations
and WebSocket reads/writes complete through the status callback. The receiver
thread waits for each completion, so protocol parsing remains serial.

`stop()` sets a cancellation flag under the condition-variable mutex and wakes
the receiver. It does not call WinHTTP. The receiver closes its request/socket
after the initiating API call has returned, then waits for the final
`WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING` notification. This cancels pending
asynchronous I/O without closing a handle during a synchronous API call.
[Microsoft handle rules](https://learn.microsoft.com/en-us/windows/win32/winhttp/hinternet-handles-in-winhttp),
[asynchronous cancellation rules](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpclosehandle).

Each asynchronous handle owns its callback state, receive buffer and outgoing
message. These remain alive until `HANDLE_CLOSING`. Callbacks only update this
state and wake the receiver; they never call the source consumer or schedule
another WinHTTP operation. The source consumer runs on the receiver thread.
The callback state survives an exceptional close failure, while the source
reports an error and ends. It never frees memory still accessible to WinHTTP.

WebSocket receive results come from `WINHTTP_WEB_SOCKET_STATUS` in the read
completion. Synchronous output parameters are not used for an asynchronous
WebSocket. [Microsoft callback contract](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nc-winhttp-winhttp_status_callback).

## Cancellation check

Build `tests/windows_source_cancel.cpp` with the source and core libraries,
linking `winhttp`. Run on Windows:

```text
python tests/windows_cancel_smoke.py path/to/fah-source-cancel.exe
```

The Python script uses synthetic loopback servers. It does not contact a folding
client or run a renderer. It checks repeated cancellation at initial startup,
during a stalled HTTP upgrade, during an idle WebSocket receive and after frame
delivery. Each probe requires `stop()` to return within one second and rejects
consumer callbacks after `stop()` returns. The enclosing process timeout catches
a hung cancellation. These are test thresholds, not a measured OS guarantee.

Cross-compilation verifies Windows API declarations. Runtime cancellation still
requires the Windows check above; a successful compile does not establish it.
