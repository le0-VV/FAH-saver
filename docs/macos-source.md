# macOS source connections

The FAH source session connects directly to its configured private LAN or VPN
endpoint. It explicitly disables HTTP, HTTPS, SOCKS, automatic proxy configuration
and proxy discovery for that session. It does not change system proxy settings,
v2rayN, another app's networking or TLS certificate validation.

This is a source-specific connection policy. A failed direct connection is
reported as a failure; the client does not try a proxy or another transport.

`NSURLSessionConfiguration.connectionProxyDictionary` controls proxies within
the session. Its default `nil` value uses system settings. The configuration
uses public CFNetwork proxy-enable constants available before the macOS 11
minimum target. [Apple session configuration documentation](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/connectionproxydictionary).

If the configured host is unreachable, check its hostname/address, port, FAH
listener and host access rules. Direct routing cannot make an unreachable
listener available. The local API normally uses port 7396 and `/api/websocket`.

## Error messages

When the WebSocket task has an HTTP response, diagnostics include its status
code alongside the URL Loading System error code. For example, a rejected
upgrade can report `HTTP 403; URL error -1011`, rather than an unexplained URL
error. The message distinguishes an upgrade failure from an established
connection failure and gives the relevant endpoint/access/proxy advice.
[Apple task response documentation](https://developer.apple.com/documentation/foundation/urlsessiontask/response).

Diagnostics do not include response headers, response bodies, raw client state,
URLs or credentials. If no HTTP response exists, no status is invented. A
refused TCP connection reports its URL error and listener advice.

## Checks

`mac_source_diagnostics` in CTest runs actual native requests against synthetic
loopback servers returning HTTP 403 and 503, then checks a refused connection.
It verifies status/code/context reporting, absence of private response data and
absence of capture output on failed connections.

To check direct LAN routing while the existing system HTTP/HTTPS/SOCKS proxies
are enabled, run:

```sh
python3 tests/mac_source_diagnostics.py build/fah_capture --lan-host YOUR_MAC_LAN_IP
```

The optional check binds a temporary synthetic WebSocket server to this Mac's
own non-loopback LAN address. It receives one invented protein frame through the
native source client and verifies that system proxy settings are unchanged.
It does not contact the folding PC. The process needs permission to access this
Mac's LAN address; the Codex sandbox can prevent that part of the check.

On 16 November 2026 the native checks passed, including the optional LAN case
outside the sandbox with system HTTP/HTTPS/SOCKS proxies enabled. This verifies
the client route, not the remote FAH listener or its access configuration.
