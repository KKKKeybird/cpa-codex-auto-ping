# CLIProxyAPI Codex Auto Ping

A small native plugin for [CLIProxyAPI](https://github.com/router-for-me/CLIProxyAPI) that periodically sends a tiny model request through CLIProxyAPI's own `host.model.execute` callback.

The plugin is implemented in C++ and exposes CLIProxyAPI's language-neutral C ABI. Linux release libraries are built against musl, so they can be loaded by Alpine-based CLIProxyAPI images without embedding a second Go runtime.

It is intended to keep Codex's rolling usage window active with minimal token consumption. The plugin does **not** read, rotate, or refresh OAuth tokens itself.

## Default behavior

- first ping: 10 seconds after plugin starts
- interval: every 5 hours
- model: `gpt-5.6`
- prompt: `1`
- max output tokens: `1`
- requests per cycle: `1`

## CLIProxyAPI configuration

```yaml
plugins:
  enabled: true
  dir: "plugins"
  configs:
    codex-auto-ping:
      enabled: true
      priority: 1
      interval: "5h"
      startup_delay: "10s"
      run_on_start: true
      model: "gpt-5.6"
      prompt: "1"
      max_output_tokens: 1
      pings_per_cycle: 1
      ping_spacing: "3s"
```

If your Codex model alias is different, set `model` to the exact model name exposed by your CLIProxyAPI instance.

For multiple Codex accounts, `pings_per_cycle` can be set to the account count when CLIProxyAPI uses round-robin routing. This is currently best-effort; v0.1 cannot pin `host.model.execute` to an individual auth ID.

## Build

Requires CMake and a C++17 compiler:

```bash
make build
```

Linux output:

```text
dist/codex-auto-ping.so
```

Install it in a CLIProxyAPI plugin discovery path, for example:

```bash
mkdir -p /path/to/CLIProxyAPI/plugins/linux/amd64
cp dist/codex-auto-ping.so /path/to/CLIProxyAPI/plugins/linux/amd64/
```

Then restart CLIProxyAPI and enable the plugin configuration above.

## Status endpoint

The plugin registers:

```text
/v0/management/plugins/codex-auto-ping/status
```

The endpoint is protected by CLIProxyAPI's Management API authentication and returns JSON containing the last attempt, last success, last error, counters, and next scheduled run. It does not register any unauthenticated routes under `/v0/resource/plugins/`.

## Current limitation

v0.1 deliberately uses CLIProxyAPI's normal model scheduler. It therefore cannot guarantee exactly one ping per individual Codex credential.

A future version can use CLIProxyAPI's `host.auth.*` callbacks together with credential-specific execution if/when auth pinning is available to `host.model.execute`.

## License

MIT
