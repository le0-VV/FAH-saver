# Direct LAN and Tailscale sources

A default FAH v8 client listens only on loopback. A remote screensaver cannot
reach that listener by entering the folding computer's LAN or Tailscale address.
The server must listen on the chosen interface and permit the viewing client.

FAHSaver 0.1.2 connects directly to the configured source. Its macOS session uses
an explicit local PAC policy that always returns `DIRECT`, rather than inheriting
system proxies. Other apps and system proxy settings are unchanged. The 0.1.1
enable-flags-only setting was insufficient for remote numeric addresses in the
tested macOS configuration. See [macOS networking](macos-source.md).

## FAH server configuration

Use the installed client's `--help` and actual data directory to check its
configuration path and supported options. FAH 8.5.6 uses `http-addresses`,
`allow` and `deny`. It does not use an `http-allow` option.

The following is a template. Replace each uppercase address placeholder with
the relevant computer's actual IPv4 address before using it:

```xml
<config>
  <http-addresses v="127.0.0.1:7396 PC_LAN_IP:7396 PC_TAILSCALE_IP:7396"/>
  <allow v="127.0.0.1 MAC_LAN_IP MAC_TAILSCALE_IP"/>
  <deny v="0/0"/>
</config>
```

Preserve any existing configuration and intended loopback/IPv6 access. Append
the required fields or addresses rather than replacing unrelated settings.
Keep private backups outside Git. Listener settings load during client startup,
so an existing client needs a normal graceful restart to use new binds. Check
the current WU/checkpoint before stopping, and verify WU resumption afterwards.

On Windows, permit TCP port 7396 only for the FAH executable, selected private
local interface addresses and intended client addresses. Keep the rule scoped
to the relevant Private profiles. Changing browser origins is not needed for
the native client. The API also accepts folding-control commands, so do not
expose it broadly or forward it from the public internet.

Verify the intended binds and a successful WebSocket upgrade, then receive a
protein frame from the viewing machine. A reachable TCP port alone does not
prove that access rules or the visualisation subscription work.

## Source addresses

Use either:

- `ws://PC_LAN_IP:7396/api/websocket` on the same LAN.
- `ws://PC_TAILSCALE_IP:7396/api/websocket` or the PC's fully qualified Tailscale
  DNS name while both machines are connected to the tailnet.

A short hostname can resolve to a LAN address when a local DNS search domain
takes precedence. Check its resolved address before treating it as a Tailscale
route. A Tailscale IP or fully qualified tailnet name removes that ambiguity.
Update listener/access/firewall addresses if DHCP assignments change.

After replacing an installed `.saver`, close its Options/Preview and reopen it
so the host can load the updated module.

## Failure meanings

- HTTP 401/403: the endpoint or an intermediary denied the connection.
- HTTP 502/503/504: the endpoint or proxy could not provide the connection.
- URL error -1004: no connection to the selected host/port.
- A timeout: check the listener, route and firewall before changing rendering.

The original -1011 report can accompany a failed WebSocket HTTP upgrade. The
updated client includes an HTTP status when one is available, without exposing
response bodies, headers or credentials.
