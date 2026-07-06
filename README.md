# zmk-feature-watchdog

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)
[![Test](https://github.com/cormoran/zmk-feature-watchdog/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-feature-watchdog/actions/workflows/zmk-module.yml) [![Devcontainer](https://github.com/cormoran/zmk-feature-watchdog/actions/workflows/devcontainer.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-feature-watchdog/actions/workflows/devcontainer.yml)

Watchdog: detects and logs firmware instability incidents (thread freezes,
hard faults) and exposes the incident log via custom Studio RPC / web UI.

This module uses the **unofficial** custom ZMK Studio RPC protocol.

## Summary

This module includes:

- **Firmware**: Custom Studio RPC handler (`src/studio/watchdog_handler.c`)
- **Protocol**: Protobuf definition (`proto/cormoran/watchdog/watchdog.proto`)
- **Web UI**: React + TypeScript app (`web/`) using [@cormoran/zmk-studio-react-hook](https://github.com/cormoran/react-zmk-studio)
- **Tests**: Firmware unit tests (`tests/studio/`, `tests/watchdog/`) and build tests (`tests/zmk-config/`)

On a split keyboard, the central also proxies read/delete requests to a
connected peripheral's own incident log over ZMK's split relay event
mechanism (`CONFIG_ZMK_WATCHDOG_SPLIT_RELAY`, see DESIGN.md SS7 and
`src/split/watchdog_relay.c`) -- select "Peripheral N" as the source in the
web UI's status card.

## More Info

For more info on modules, you can read through through the [Zephyr modules page](https://docs.zephyrproject.org/3.5.0/develop/modules.html) and [ZMK's page on using modules](https://zmk.dev/docs/features/modules). [Zephyr's west manifest page](https://docs.zephyrproject.org/3.5.0/develop/west/manifest.html#west-manifests) may also be of use.

## Module User Guide

1. Add dependency to your `config/west.yml`. Note: this module requires a patched ZMK with custom Studio RPC support.

   ```yml
   manifest:
       remotes:
           ...
           - name: cormoran
           url-base: https://github.com/cormoran
       projects:
           ...
           - name: zmk-feature-watchdog
           remote: cormoran
           revision: main+custom-studio-protocol # or latest commit hash
           import: true
           ...
           # Required: patched ZMK with custom Studio RPC support
           - name: zmk
           remote: cormoran
           revision: main+custom-studio-protocol
           import:
               file: app/west.yml
   ```

2. Enable flags in your `config/<shield>.conf`

   ```conf
   CONFIG_ZMK_WATCHDOG=y

   # Optional: flash slot cap (default 16). Once this many incidents are
   # stored, recording stops until incidents are deleted (no ring-buffer
   # overwrite -- see "Flash-wear protection" below).
   CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS=16

   # Detectors -- default y once CONFIG_ZMK_WATCHDOG=y, only shown here for
   # reference. Disable individually if you don't want a given detector.
   CONFIG_ZMK_WATCHDOG_FREEZE_DETECT=y
   CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS=5000
   CONFIG_ZMK_WATCHDOG_FATAL_DETECT=y

   # Enable custom Studio RPC + web UI. Studio RPC only exists on the
   # central (or a non-split board); a split peripheral does not need
   # CONFIG_ZMK_STUDIO at all -- see "Split keyboards" below.
   CONFIG_ZMK_STUDIO=y
   CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y
   CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128
   ```

   **Important conflict note**: `CONFIG_ZMK_WATCHDOG_FATAL_DETECT` overrides
   Zephyr's weak `k_sys_fatal_error_handler()` symbol. If any other module or
   application code in your build also defines that symbol, one of the two
   definitions silently wins at link time (no compile error) -- only one
   fatal handler can be active.

   **Why there's no hardware-watchdog-peripheral option**: freeze detection
   above is a *software* timer only (Zephyr's task watchdog) -- it does not
   arm the chip's real hardware watchdog peripheral, and this module does not
   offer that as an option. This was tried and deliberately backed out: on
   the nRF52840, once the hardware watchdog peripheral has been armed, it
   keeps running across a soft/debugger reset (only a full power cycle clears
   it), and while it's running it can prevent the chip's low-frequency clock
   from ever finishing startup on the next boot -- hanging the keyboard
   before firmware even starts, recoverable only by removing the battery.
   The software-only timer used here does not have this failure mode, so it
   remains the only freeze-detection mechanism this module provides.

   **Split keyboards**: enable `CONFIG_ZMK_WATCHDOG=y` on *both* halves so
   each detects and stores its own incidents. `CONFIG_ZMK_WATCHDOG_SPLIT_RELAY`
   defaults to `y` whenever `CONFIG_ZMK_SPLIT=y`, so no extra flag is usually
   needed -- it pulls in nanopb on its own (via a hidden
   `CONFIG_ZMK_WATCHDOG_PROTOBUF`) so a peripheral build compiles the relay
   responder without needing `CONFIG_ZMK_STUDIO` at all. Only the central
   needs `CONFIG_ZMK_STUDIO`/`CONFIG_ZMK_WATCHDOG_STUDIO_RPC`. See
   "Split keyboard limitations" below.

3. Open the [web UI](https://cormoran.github.io/zmk-feature-watchdog/) (or
   run it locally, see `web/README.md`) and connect over serial via ZMK
   Studio's WebSerial transport. The page shows:
   - a **source selector**: "Central" (this device's own log) or
     "Peripheral N" (a connected split peripheral's log, relayed over
     `CONFIG_ZMK_WATCHDOG_SPLIT_RELAY` -- see "Split keyboard limitations"
     below);
   - a **status card**: capacity, stored count, dropped-since-boot, and a
     "recording paused" banner once the store is full (delete incidents to
     resume);
   - an **incident table**: id, source ("Central" or "Peripheral N"), type
     badge, boot ordinal + uptime, and a detail column decoded per incident
     type (freeze queue name, fatal reason/PC/LR, reset-cause bits);
   - per-row **delete** and **delete all** (with confirmation);
   - new local (Central) incidents appear live via a push notification,
     without needing to refresh.

   Implementation reference:
   - `include/cormoran/zmk/watchdog.h` — incident record type + store/pending/detector API
   - `src/watchdog_store.c` — settings-backed incident store
   - `src/watchdog_pending.c` — retained-RAM pending slot + boot conversion
   - `src/watchdog_freeze.c` — task_wdt-based freeze detector (sysworkq + ZMK low-priority queue)
   - `src/watchdog_fatal.c` — `k_sys_fatal_error_handler()` override (hard faults, oops/panic)
   - `src/watchdog_reset_cause.c` — boot-time `hwinfo` reset-cause audit
   - `src/watchdog_inject.c` — dangerous test/fault injection helpers (`CONFIG_ZMK_WATCHDOG_TEST_INJECTION`, default n)
   - `proto/cormoran/watchdog/watchdog.proto` — message types (`Request`/`Response`/`Notification`)
   - `src/studio/watchdog_request_exec.c` — request execution against the local store (shared by the RPC handler and the split relay responder)
   - `src/studio/watchdog_handler.c` — firmware RPC handler (GetStatus/ListIncidents/DeleteIncidents), routes `source != 0` to the relay
   - `src/split/watchdog_relay.c` — split relay bridge (peripheral responder + central proxy/notifications)
   - `web/src/App.tsx`, `web/src/IncidentsSection.tsx` — web UI

### Split keyboard limitations

- **Broadcast, not addressed**: the underlying split relay event transport
  sends a central→peripheral request to *every* connected peripheral, not
  to one addressed peripheral. With more than one peripheral, every
  peripheral answers every relayed request (each correctly tagged with its
  own source on the way back) -- fine for the common single-peripheral
  split, but does not compose well with a UI that expects an answer from
  only one specific peripheral.
- **No connectivity discovery**: there is no RPC to ask "which peripherals
  are currently connected." A relayed request to a disconnected/missing
  peripheral simply never gets a response; the web UI times out after ~3s
  and shows an error rather than hanging forever.
- **Relaying is asynchronous**: unlike a local (Central) request, a
  peripheral request always returns a `DeferredResponse` immediately and the
  real answer arrives later as a `PeripheralResponse` Studio notification.
  The web UI handles this automatically; if you build your own client
  against this RPC surface, watch for `deferred` in the `Response` oneof.

### Flash-wear protection

Incident storage is capped at `CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS` (default
16) slots. Once every slot is used, new incidents are **dropped** (not
overwritten) and only counted; recording resumes automatically as soon as
existing incidents are deleted.

### Web UI

See [web/README.md](./web/README.md) for web UI development instructions.

### Publishing Web UI

**GitHub Pages**: Merge a pull request into `main+custom-studio-protocol` to deploy to `https://<account>.github.io/<repo>/`.

**Cloudflare Workers (PR previews)**: Configure `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID` secrets.

## Module Development Guide

### Setup for running test

#### Option0: Dev container (recommended)

Open this repository in VS Code with the [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers). The container automatically initializes the west workspace using the isolated layout.

#### Option1: west workspace directory layout

Set west topdir as parent of repository root and download dependencies under `../`.
This layout is useful to reduce disk usage by sharing dependencies with other zephyr modules.
The build result is located in `../build`.

```bash
mkdir west-workspace
cd west-workspace # this directory becomes west workspace root (topdir)
git clone <this repository>
# rm -r .west # if exists to reset workspace
west init -l . --mf west/west-test-workspace.yml
west update --narrow
west zephyr-export
```

#### Option2: isolated directory layout

Set west topdir as repository root and download dependencies under `./dependencies`.
This layout is useful if you don't want to share dependencies to other zephyr modules.
Dev container and github actions uses this layout.
The build result is located in `./build`.

```bash
git clone <this repository>
cd <cloned directory>
west init -l west --mf west-test-isolated.yml
west update --narrow
west zephyr-export
```

### Pre-commit

Every commit need to pass pre-commit verification. The verification contains formatting code and running tests.

```
pip install pre-commit
pre-commit install

# Run pre-commit manually
pre-commit run --all-files
# Run for git staged files
pre-commit run
```

### Running Test

```bash
# Run unit test + build test and verify the results
python3 -m unittest
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
# Run web tests
cd web && npm test
```

### Sync changes from template

Run `Actions > Sync Changes in Template > Run workflow` to get the latest template changes as a pull request.

If the template contains changes in `.github/workflows/*`, register a GitHub personal access token as `GH_TOKEN` repository secret (`repo` + `workflow` scopes).

### Coding agent on actions

Actions for github copilot and claude are available.

- Mention `@copilot`
- Setup `ANTHROPIC_API_KEY` secret and mention `@claude`
  - Or fix [claude.yml](./github/workflows/claude.yml) to use `CLAUDE_CODE_OAUTH_TOKEN`
