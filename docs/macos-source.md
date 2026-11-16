# macOS source connections

FAHSaver 0.1.2 connects directly to its configured private LAN or VPN endpoint.
Its source session uses a local PAC script with one unconditional result:

```javascript
function FindProxyForURL(url, host) { return 'DIRECT'; }
```

The session's `connectionProxyDictionary` sets HTTP, HTTPS and SOCKS enable flags
to false, PAC enable to true, `kCFNetworkProxiesProxyAutoConfigJavaScript` to this
constant script, and automatic proxy discovery to false. It fetches no PAC URL
and offers no proxy alternative. It does not change system proxy settings,
v2rayN, another app's networking or TLS certificate validation.

This is a source-specific connection policy. A failed direct connection is
reported as a failure; the client does not try a proxy or another transport.

`NSURLSessionConfiguration.connectionProxyDictionary` controls proxies within
the session. Its default `nil` value uses system settings. The configuration
uses public CFNetwork constants available before the macOS 11 minimum target.
The inline PAC JavaScript key is available from macOS 10.7.
[Apple session configuration documentation](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/connectionproxydictionary),
[Apple inline PAC key documentation](https://developer.apple.com/documentation/cfnetwork/kcfnetworkproxiesproxyautoconfigjavascript).

The earlier enable-flags-only configuration and an additional wildcard exception
did not stop native WebSocket requests to remote numeric addresses from taking
the proxy path in the tested macOS setup. The explicit PAC `DIRECT` policy did.

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

The optional check binds a synthetic WebSocket server to this Mac's own LAN
address. This is a local integration check; system local-address exceptions can
mask proxy inheritance, so it cannot establish remote direct routing. The
process needs LAN permission; the Codex sandbox can prevent that check.

For a routing regression, use an explicitly authorised remote FAH source with a
numeric address while system HTTP/HTTPS/SOCKS proxies remain enabled:

```sh
python3 tests/mac_source_diagnostics.py build/fah_capture \
  --remote-source ws://PC_LAN_IP:7396/api/websocket \
  --remote-source ws://PC_TAILSCALE_IP:7396/api/websocket
```

This sends only visualisation requests, receives two snapshots per endpoint and
checks that system proxy settings remain unchanged. Temporary topology/frame
files are deleted. Remote checks are optional and never run by ordinary CTest.

On 16 November 2026, the final PAC policy received 4,163 atoms, 7,730 bonds and two
snapshots through each actual remote numeric LAN and Tailscale endpoint. The
same numeric-address check failed with HTTP 400 using the enable-flags-only
control binary. Local HTTP 403/503 and connection-refused checks also passed.
The local-address test alone had previously given insufficient routing evidence.
