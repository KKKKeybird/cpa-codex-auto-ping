# Plugin Store submission

Repository: https://github.com/KKKKeybird/cpa-codex-auto-ping

Latest release: v0.1.0

Capability: periodically sends a minimal model request through CLIProxyAPI's host.model.execute callback to keep Codex rolling usage windows active without directly touching OAuth refresh tokens.

Proposed registry entry:

```json
{
  "id": "codex-auto-ping",
  "name": "Codex Auto Ping",
  "description": "Periodically sends a minimal Codex model request through CLIProxyAPI to keep rolling usage windows active with minimal token consumption.",
  "author": "KKKKeybird",
  "version": "0.1.0",
  "repository": "https://github.com/KKKKeybird/cpa-codex-auto-ping",
  "homepage": "https://github.com/KKKKeybird/cpa-codex-auto-ping",
  "license": "MIT",
  "tags": [
    "Codex",
    "Usage"
  ]
}
```

Suggested PR title:

```text
Add Codex Auto Ping plugin
```

Suggested PR body:

```text
Adds the Codex Auto Ping plugin to the official CLIProxyAPI plugin registry.

- Repository: https://github.com/KKKKeybird/cpa-codex-auto-ping
- Latest release: v0.1.0
- Capability: sends a tiny scheduled Codex request through CLIProxyAPI's host.model.execute callback.
- Release assets: platform-specific ZIP archives plus checksums.txt in the format required by the plugin store.

The plugin does not directly read or refresh OAuth refresh tokens.
```
