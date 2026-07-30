# Xymon HTTP gateway

`xymon-http-gateway.py` accepts HTTP POST requests from clients such as the
official Windows PowerShell client and forwards the raw request body to a
local `xymonproxy` listener.

TLS and client authentication belong in the reverse proxy. The Python service
listens on `127.0.0.1` by default and must not be exposed directly.

## Default policy

The gateway accepts these Xymon commands:

```text
client data status usermsg
```

The `+LIFETIME` and `/group:GROUP` suffixes on `status` are supported. Other
commands receive HTTP 403. Add a command only when the clients require it:

```text
--allow-command=download
```

Do not enable `drop`, `rename`, `disable`, `enable`, or other administrative
commands on a client ingestion endpoint. Do not enable `combo` or `extcombo`:
Xymon recursively dispatches their embedded messages, which would bypass the
gateway's command allowlist.

## Client compatibility

The official Windows PowerShell client can POST directly to this gateway.

The Axians MrBig client on its `develop` branch is not directly compatible yet.
It emits allowed `client` and `status[+TTL]` messages, but sends them over raw
TCP and does not implement HTTP or TLS.

A future MrBig HTTP(S) transport can use both gateway endpoints. It must POST
the existing report unchanged to `/xymon-ingest` and GET its configuration from
`/xymon-minicfg`. Both requests must validate the TLS certificate and use the
configured authentication. Do not widen the gateway command allowlist for
MrBig.

The minicfg endpoint reproduces the legacy minicfg selection rules. It maps the
client IPv4 address to the first matching host in `hosts.cfg`, starts the result
with the `[mrbig]` section and `machine` setting, then reads the matching
`Clients/IP` file. If that file is absent, it uses `Includes/0.0.0.0`. Nested
`!include` directives are resolved below `Includes` to a maximum depth of five.
The response is limited to 100,000 bytes by default.

## Proxy-server deployment

1. Install the gateway:

   ```sh
   install -o root -g root -m 0755 xymon-http-gateway.py \
       /srv/xymon/server/bin/xymon-http-gateway.py
   ```

2. Ensure `xymonproxy` listens locally on TCP 1984 and forwards to the central
   Xymon server.

3. Install and enable the example systemd service:

   ```sh
   install -o root -g root -m 0644 xymon-http-gateway.service \
       /etc/systemd/system/xymon-http-gateway.service
   systemctl daemon-reload
   systemctl enable --now xymon-http-gateway.service
   ```

4. Adjust the `--minicfg-hosts-file` and `--minicfg-dir` paths in the systemd
   unit for the local installation. The gateway user needs read access to both.

5. Add `xymon-http-gateway.nginx` to the proxy's HTTPS server block. Create the
   Basic authentication file using the distribution's `htpasswd` utility, then
   validate and reload Nginx. Keep the gateway bound to loopback: the Nginx
   configuration overwrites `X-Real-IP`, which selects the client configuration.

6. Configure the Windows client:

   ```xml
   <serverUrl>https://proxy.example.com/xymon-ingest</serverUrl>
   <serverHttpUsername>xymonclient</serverHttpUsername>
   <serverHttpPassword>initial-password</serverHttpPassword>
   ```

## Test

Test the gateway through Nginx:

```sh
curl --fail-with-body --user xymonclient \
    --data-binary $'client test.example.com.powershell powershell XymonPS\n' \
    https://proxy.example.com/xymon-ingest
```

The response may contain the matching client configuration from the central
Xymon server. Check the gateway journal and `xymonproxy` report after testing.

Test minicfg selection through Nginx from a client address present in
`hosts.cfg`:

```sh
curl --fail-with-body --user xymonclient \
   https://proxy.example.com/xymon-minicfg
```
