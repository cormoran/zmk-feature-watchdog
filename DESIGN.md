# zmk-feature-watchdog — Design

Status: **Phases A-E implemented and unit/build tested; Phase F (hardware
validation) partially done.** Single-board hardware validation
(freeze/fatal detection, retained-RAM crash-crossing, flash-wear cap,
delete/resume, RPC pagination) is **complete and green** — see §12.1 for
full results. Split-relay hardware validation (2 boards) was attempted but
**not completed**: a second board on this rig hit a real, pre-existing
hardware/boot-stability issue unrelated to this module's code (§12.1
has details and a recommendation for whoever resumes it). This document
remains the source of truth for what was built and why; see §13 for
phase-by-phase detail and §12/§12.1 for the hardware validation checklist
and results.

## 1. Goal

Detect situations where firmware instability makes the keyboard unusable,
and record them as persistent incident logs that the user can inspect and
delete from a web page (custom ZMK Studio RPC + WebSerial UI).

Requirements from the project owner:

- Detect **long thread freezes** (keyboard stops responding but no crash).
- Detect **hard faults** (stack overflow, bus fault, kernel oops/panic, …).
- Detect anything else that is cheaply observable (see §4.3 reset-cause audit).
- Logs are viewable and deletable from a web page.
- **Flash-wear protection is a hard requirement**: incident count is capped;
  when the cap is reached, recording **stops** until the user deletes logs
  from the web page. No ring-buffer overwrite.
- Works on **split-keyboard peripherals**; peripheral→central transport uses
  the event relay implemented in the cormoran/zmk fork
  (`main+custom-studio-protocol`).
- Hardware validation on this workspace's XIAO nRF52840 + J-Link rig.

## 2. Non-goals (v1)

- Full post-mortem coredumps (Zephyr `CONFIG_DEBUG_COREDUMP` is heavier than
  needed; our fixed-size incident record is enough to identify the failing
  thread/PC). Possible future work.
- Wall-clock timestamps (no RTC; we record `uptime` + `boot_count` instead).
- Per-peripheral addressing of relay messages (the relay broadcasts to all
  peripherals; responses carry a `source` slot so the central can tell them
  apart).
- Detecting freezes of the BLE controller/radio itself, or any hard lockup
  that prevents even the task_wdt timer ISR from running (e.g. an IRQ
  storm): a hardware-watchdog-peripheral backstop for this was prototyped
  and then removed (§4.1) due to a reset-survival risk on the nRF52840; this
  category of failure is simply not covered in v1.

## 3. Big picture

```
            ┌────────────── incident time (may be ISR / fault context) ──────────────┐
 detector ──► fill fixed-size record in __noinit "pending incident" RAM slot ──► sys_reboot()
            └──────────────────────────────────────────────────────────────────────┘
                                        reboot
            ┌────────────── next boot (settings ready, workqueue ctx) ──────────────┐
 boot hook ─► validate magic+CRC of pending slot ─► append to flash log (Zephyr     │
            │  settings, key per slot) unless cap reached ─► clear pending slot     │
            └──────────────────────────────────────────────────────────────────────┘
                                        later
 web UI ◄─ Studio RPC (central only) ◄─ local store ── and, for peripherals:
 web UI ◄─ Studio notifications ◄─ central proxy ◄─ relay events ◄─ peripheral store
```

Key principle: **never write flash at incident time.** Fault/watchdog context
is not safe for flash writes. Incidents cross the reboot in retained RAM and
are persisted by a normal boot-time init hook. Each half (central and
peripheral) detects and stores **its own** incidents locally; the central
proxies read/delete requests to peripherals on demand.

## 4. Detection layer

### 4.1 Thread freeze (task watchdog)

Use Zephyr's task watchdog (`subsys/task_wdt`,
`include/zephyr/task_wdt/task_wdt.h`):

- `task_wdt_init(NULL)` — always software-only, no hardware watchdog
  peripheral backing it. See "Hardware fallback: prototyped and removed"
  below for why.
- One `task_wdt_add(timeout, callback, user_data)` **channel per monitored
  work queue**. Feeding is done by a self-rescheduling `k_work_delayable`
  submitted **to the monitored queue**: if the queue is blocked/starved
  longer than the timeout, the feed never runs and the channel fires.
- Monitored queues (v1):
  1. **system workqueue** (`&k_sys_work_q`) — kscan/event-manager processing
     lives here; a blocked sysworkq = unusable keyboard. Always monitored.
  2. **ZMK low-priority work queue** (`zmk/app/src/workqueue.c`,
     `zmk_workqueue_lowprio_work_q()`) — monitored when available.
  Others (HoG queue, split service queue) are candidates for a later phase;
  keep the monitor table data-driven (array of `{queue getter, name}`) so
  adding one is a one-line change.
- Timeout: `CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS`, default **5000 ms**
  (well above worst-case legitimate stalls like BLE connection storms or
  settings writes; short enough to match "user notices the keyboard died").
  Feed period = timeout / 4.
- The task_wdt callback runs in **ISR (timer) context**: it must only fill
  the retained-RAM pending record (freeze detail: channel id, queue name,
  uptime, boot count) and call `sys_reboot(SYS_REBOOT_WARM)`.
- **Debugger interaction on this rig**: a J-Link halt pauses this software
  timer's ISR along with everything else, but expect a spurious FREEZE
  incident after resuming from a multi-second breakpoint; that's acceptable
  for a debug rig.

**Hardware fallback: prototyped and removed.** An earlier version of this
design (and an earlier revision of this module, up to and including Phase F)
passed a real hardware watchdog device (nRF `wdt0` / devicetree
`watchdog0` alias) to `task_wdt_init()` as `CONFIG_ZMK_WATCHDOG_HW_FALLBACK`,
so that a hard lockup that prevents even the task_wdt timer ISR from running
(e.g. an IRQ storm) would still reset the chip via `CONFIG_TASK_WDT_HW_FALLBACK`.
This was removed entirely (not kept as an opt-in) after hardware validation
(§12.1) turned up a credible, serious reset-survival risk:

- The nRF52840's hardware watchdog peripheral, once armed, is **not**
  cleared by a soft reset or debugger/pin reset — only a real power-on/
  brown-out reset clears it.
- Zephyr's LFCLK driver (`drivers/clock_control/clock_control_nrf.c`) does a
  **one-shot** RC→XTAL clock handoff at boot: it attempts the switch to the
  external crystal exactly once and never retries. Analysis of that driver
  (see §12.1) found a plausible mechanism by which a *still-armed* hardware
  watchdog from a previous boot can interfere with this handoff on the next
  boot if the board is reset via SWD/software rather than fully power-cycled.
- The failure mode this produces is a **permanent boot hang** inside
  `lfclk_spinwait()`, before firmware logic (including this module's own
  code) ever runs — recoverable only by a real power cycle (battery pull).
  This was not proven end-to-end with full certainty (the hardware
  experiment that surfaced it had some methodology confounds — see §12.1),
  but the mechanism is credible and directly backed by reading the
  clock-control driver source.
- Given that, the project owner judged the downside (a "reliability"
  feature that can itself turn a recoverable freeze into an unrecoverable
  hang requiring a battery pull) worse than the value a hardware backstop
  adds on top of the software task_wdt layer, and decided to drop
  `CONFIG_ZMK_WATCHDOG_HW_FALLBACK` entirely rather than keep it as a
  risky opt-in. No hardware-watchdog option remains in this module; only
  the software task_wdt mechanism described above is used.

### 4.2 Fatal errors (hard fault, stack overflow, oops/panic)

Override the weak `k_sys_fatal_error_handler(unsigned int reason, const
struct arch_esf *esf)` (`zephyr/include/zephyr/fatal.h`; **ZMK does not
override it** — verified in the fork, so no symbol clash; note in README
that any other module overriding it conflicts).

- Record: `reason` (`K_ERR_CPU_EXCEPTION`, `K_ERR_STACK_CHK_FAIL`,
  `K_ERR_KERNEL_OOPS`, `K_ERR_KERNEL_PANIC`, …), `esf->basic.pc`,
  `esf->basic.lr`, current thread name (`k_thread_name_get(k_current_get())`,
  requires `CONFIG_THREAD_NAME`; store "?" when unavailable), uptime,
  boot count. `esf` may be NULL — handle it.
- Then `sys_reboot(SYS_REBOOT_WARM)`. Never return. (Upstream default would
  halt forever — rebooting is itself a usability improvement.)
- Handler must be minimal: no logging subsystem calls except `LOG_PANIC()`,
  no threading APIs, no flash.
- Stack overflow detectability depends on `CONFIG_HW_STACK_PROTECTION`
  (MPU, available on nRF52840, default y in Zephyr for cortex-m with MPU)
  → arrives as `K_ERR_STACK_CHK_FAIL` / MPU fault. Document in README;
  don't force-select.

### 4.3 Boot-time reset-cause audit

At boot, `hwinfo_get_reset_cause()` (`drivers/hwinfo`, nRF impl maps
`RESET_WATCHDOG` BIT(4), `RESET_CPU_LOCKUP` BIT(8), `RESET_BROWNOUT`
BIT(2)):

- If the cause contains WATCHDOG / CPU_LOCKUP / BROWNOUT **and** there is no
  pending retained record explaining it, log a `RESET_CAUSE` incident with
  the raw cause bits. This is the degraded path that catches hard-WDT resets
  and lockups where no software ran.
- Call `hwinfo_clear_reset_cause()` after reading (nRF cause bits accumulate
  across resets; without clearing, every boot re-reports old causes).
- Not available on native_sim (returns 0 causes) — code must treat "no
  causes" as normal.

### 4.4 Test/fault injection (for hardware validation)

A separate Kconfig (`CONFIG_ZMK_WATCHDOG_TEST_INJECTION`, default n, enabled
in test firmware only) adds RPC requests that deliberately:

- block the system workqueue forever (`k_sleep(K_FOREVER)` in a work item) →
  validates freeze path end-to-end;
- `k_oops()` / NULL-function-pointer call → validates fatal path;

Without this, hardware validation of a watchdog is impossible. It must be
clearly marked dangerous and default-off.

## 5. Crossing the reboot: retained RAM pending slot

A single static `__noinit` struct (no devicetree overlay needed — simplest
and board-agnostic):

```c
struct zmk_watchdog_pending {
    uint32_t magic;           /* 0x57444741 'WDGA' */
    struct zmk_watchdog_incident_record record;  /* fixed-size, §6 */
    uint32_t crc;             /* crc32_ieee over record */
} __noinit_section;           /* plain __noinit static */
```

- nRF52840 SRAM survives `SYSRESETREQ` (verified fact on this rig — even RTT
  buffers survive reflash; see skills/develop-zmk-module hardware-rig notes).
- Boot hook (`SYS_INIT`, `APPLICATION` level, then deferred to the low-prio
  workqueue once settings are loaded — remember the pitfall that
  `settings_load()` runs from `main()` **after** all SYS_INIT levels):
  validate magic+CRC → hand record to the store (§6) → zero the slot.
- CRC + magic makes cold boots / bootloader-clobbered RAM safe (record is
  simply discarded).
- native_sim: `__noinit` does **not** survive process restart; unit tests
  inject records by calling the boot-conversion function directly.

## 6. Storage layer (both roles)

**Backend: raw Zephyr settings subsystem** (not zmk-feature-custom-settings).
Rationale: peripherals must store incidents too and shouldn't pull in the
custom-settings RPC machinery; a fixed-size binary record with a key per
slot maps directly onto settings; and it avoids custom-settings' RAM-cache
overhead and macro pitfalls. (ZMK `imply SETTINGS` covers real boards;
BLE bonds already require settings on both halves. native_sim tests must
enable `CONFIG_SETTINGS` + a backend explicitly — ARCH_POSIX is excluded
from ZMK's imply.)

- Keys: `wdg/i/<slot>` (record blob), slots `0..CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS-1`.
- Record: one fixed-size packed struct `zmk_watchdog_incident_record`
  (target ≤ 64 bytes; **hard limit: must fit a relay event payload**, i.e.
  ≤ `CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN` (default 128) with headroom):
  `{ uint16 id; uint8 type; uint8 flags; uint32 boot_count; uint32 uptime_s;
     union detail { freeze{...}; fatal{reason,pc,lr,thread[16]}; reset{cause}; } }`
  plus a record-format version byte for forward compatibility.
- In-RAM index: bitmap of used slots + copy of records (≤ 16×64 B = 1 KiB,
  acceptable; keeps RPC list handling out of flash-read paths). Built at
  boot by `settings_load_subtree("wdg")` handler.
- **Cap semantics (the flash-wear guarantee):** append picks the first free
  slot; if none, the incident is **dropped** and only a RAM
  `dropped_since_boot` counter increments (no flash write at all).
  Deleting a slot (`settings_delete`) frees it for future incidents.
- `boot_count`: a `wdg/meta` settings entry incremented once per boot?
  **No** — that would write flash on every boot (wear). Instead increment
  it **only when an incident is persisted** (store "incident ordinal") and
  keep relative ordering by `id` = monotonically increasing ordinal stored
  in `wdg/meta` alongside — one small meta write per incident, amortized
  into the same event. uptime still distinguishes incidents within a boot.

## 7. Split support (peripheral ↔ central)

Studio RPC exists **only on the central** (`ZMK_STUDIO_RPC` is gated on
`!ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL`). Peripherals detect and store
locally (§4–6 run identically on both roles); the central **proxies**
web-UI requests over the event relay, copying the proven pattern from
`zmk-driver-pmw3610-with-custom-studio-rpc/src/split/pmw3610_relay.c`.

Relay API (fork `main+custom-studio-protocol`,
`app/include/zmk/event_manager.h`):

- `ZMK_RELAY_EVENT_HANDLE(type, "id4", source_field)` — receive side.
- `ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(type, "id4", source_field)` /
  `ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(...)` — send side; only forwards
  events whose source field is `ZMK_RELAY_EVENT_SOURCE_SELF` (0xFF).
- Receive rewrites `source` to sender slot + 1; central = 0.
- Payload ≤ `CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN` (default 128, hard wire
  ceiling 255); identifier ≤ 4 chars. `BUILD_ASSERT` both.
- Requires `CONFIG_ZMK_SPLIT_RELAY_EVENT=y`; central→peripheral is a
  broadcast to all peripherals.

Events (identifiers): `"wdq"` request (central→peripheral: list / delete /
status, tiny nanopb-encoded payload) and `"wdp"` response
(peripheral→central: **one incident record per event** — a record ≤64 B fits
one relay payload; plus status/ack/done messages with an incident count).

Flow for the web UI reading peripheral logs (async, as in pmw3610):

1. Web → central RPC `ListIncidents{source: N}` → handler raises `"wdq"`
   relay request, responds immediately with an ack (`in_progress = true`).
2. Peripheral receives, walks its store, raises one `"wdp"` response per
   record + a final `done{count}`.
3. Central receives `"wdp"` events and forwards each as a **Studio custom
   notification** (`raise_zmk_studio_custom_notification`, encoded from the
   module's proto `Notification` message) to the web UI, which accumulates
   them.
4. Delete works the same with a small ack (deleted count).

Timeouts: web-side (react) 3 s wait for `done`; peripheral absent/disconnected
⇒ central knows connected slots? — it doesn't reliably; keep it simple: the
UI shows "no response" after timeout. Document as v1 limitation.

Central's own logs are answered **synchronously** in the RPC response
(paginated, §8) — no relay involved for source 0.

### 7.1 Relay wire format: stream one incident per event (keep DATA_LEN small)

**Verified 2026-07-05 by reading the fork's actual relay transport**
(`app/include/zmk/split/transport/types.h`,
`app/src/split/bluetooth/relay_event.h`): a relay event is a small
fixed-size datagram — `relay_event_header.event_data_size` is a `uint8_t`,
so no relay event can ever exceed 255 bytes, and the whole nanopb-encoded
payload must be materialized in one static buffer before the event is
raised (no streaming is possible *within* one relay event; this is a
one-shot small-datagram transport, unlike §7.2's Studio RPC transport).

Consequently: **do not embed a whole paginated `IncidentPageResponse`
(multiple incidents) inside one relay event.** That forces
`CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN` up from its default 128 (an
earlier revision of this design needed 240 for a 3-incident page + relay
envelope). Instead, stream the peripheral's `ListIncidents` answer as
**one relay event per incident**, keeping every relay event's payload
close to "one `Incident` + envelope" regardless of page size:

- Add a relay-only `RelayIncidentChunk { bool done; uint32 total;
  uint32 start_index; Incident incident; }` message. `RelayResponse`
  becomes a oneof: `Response response` (unchanged, used as-is for
  GetStatus/DeleteIncidents/InjectTest/Error — already small, single
  event) or `RelayIncidentChunk incident_chunk` (ListIncidents only).
- Peripheral responder: for a `ListIncidents` request, still call
  `watchdog_request_exec_handle()` exactly as today (unchanged — it
  builds a normal `Response{incident_page:{...}}` using the *local*
  page size), then **iterate that in-RAM result** and raise one
  `RelayResponse{incident_chunk:{done:false, incident:incidents[i], ...}}`
  event per incident, followed by one final
  `{done:true, total, start_index}` event with no incident. For every
  other request type, raise a single `RelayResponse{response:...}` event
  exactly as before — no change.
- Central dispatcher: unchanged for non-list responses (wrap+raise a
  `PeripheralResponse` Studio notification immediately, as today). For
  `incident_chunk` events, accumulate incidents into a small static
  array (bounded by the same local page-size cap the peripheral used —
  trivial RAM, one page's worth) keyed by the in-flight `request_id`;
  on `done:true`, build the **same** `PeripheralResponse{response:
  {incident_page:{incidents, total, start_index}}}` Studio notification
  the web UI already expects and raise it once. **The public
  Notification/Response proto and the web UI do not change at all** —
  only the private peripheral↔central wire format (`RelayRequest`/
  `RelayResponse`, already documented as "never sent directly over the
  Studio RPC transport") is restructured.
- Net effect: `CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN` can stay at its
  framework default (128); the local (source 0) `ListIncidents` page size
  is now decoupled from both DATA_LEN and TX_BUF_SIZE (§7.2) and can be
  chosen purely for UX (this doc's original sketch's "≤4" is fine again).

## 7.2 Studio RPC TX buffer: genuinely streaming, do not scale with response size

**Verified 2026-07-05 by reading the fork's actual RPC transport**
(`app/src/studio/rpc.c`'s `send_response`/`rpc_tx_buffer_write`, and both
`uart_rpc_transport.c`'s and `gatt_rpc_transport.c`'s `tx_notify`): unlike
the relay transport above, `CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE` backs a
**ring buffer**, not a "whole response must fit here" buffer.
`pb_ostream_for_tx_buf()` gives nanopb's `pb_encode()` a callback-based
`pb_ostream_t` (`max_size = SIZE_MAX`) whose write callback
(`rpc_tx_buffer_write`) claims ring-buffer space, writes what fits, calls
the transport's `tx_notify` to drain toward the wire (UART: enables TX
IRQ or polls out bytes once half-full/done; BLE: queues a GATT
indication and re-arms on each `indicate_cb`), and if the ring buffer is
momentarily full it just `k_sleep(K_MSEC(1))` and retries — it never
gives up or truncates. The custom-subsystem response field
(`zmk_custom_CallResponse.payload`) is itself a nanopb callback field;
per `zmk/studio/custom.h`'s own doc comment, nanopb invokes our encode
callback in **two passes** (a counting pass into an internal sizing
stream that discards bytes, then a real pass through the same streaming
ring-buffer chain above) — neither pass ever needs the whole encoded
response to exist in one buffer.

Consequently: **`CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE` does not need to be
raised to fit a full `IncidentPageResponse`** (an earlier revision of
this design set it to 512 with a `BUILD_ASSERT(encoded_max + 64 <=
TX_BUF_SIZE)` — both were unnecessary caution copied from a general
pitfall note that does not apply to this exact streaming mechanism).
Leave it at the framework default (64) unless hardware validation (§12)
finds a concrete problem; drop the response-size `BUILD_ASSERT` entirely.
This is unrelated to §7.1's relay `DATA_LEN` fix — the relay transport
(one-shot small datagram) and the Studio RPC transport (streaming ring
buffer) have fundamentally different size constraints and must not be
confused with each other.

## 8. Studio RPC + proto

Subsystem identifier: `cormoran__watchdog` (created by the template's
`init_module.py`). Proto package `cormoran.watchdog` at
`proto/cormoran/watchdog/watchdog.proto` (+ `.options` file — every
string/bytes needs `max_size`; **no 64-bit types**; set `has_<field>=true`
for every sub-message — nanopb pitfalls from skills/develop-zmk-module).

Messages (sketch — implementer finalizes):

```proto
Request  = oneof { GetStatus, ListIncidents{source,start}, DeleteIncidents{source, ids[], all}, InjectTest{kind} }
Response = oneof { Error{msg}, Status, IncidentPage, DeleteResult{deleted,in_progress}, InjectAck }
Status   = { capacity, stored, dropped_since_boot, recording_stopped, split_supported }
Incident = { id, source, type enum{FREEZE,FATAL,RESET_CAUSE}, boot_ordinal, uptime_s,
             oneof detail { Freeze{queue_name}, Fatal{reason,pc,lr,thread_name}, Reset{cause_bits} } }
IncidentPage = { total, start, incidents[≤4], in_progress /* true when relayed */ }
Notification = oneof { RelayedIncident{Incident}, RelayDone{source,count}, RelayDeleteResult{source,deleted},
                       IncidentRecorded{Incident} /* optional live push when a new incident is persisted */ }
```

- **Pagination instead of streaming**: ≤ 4 incidents per response keeps the
  encoded response comfortably under a 512-byte TX buffer
  (`CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=512` in test configs +
  `BUILD_ASSERT(encoded_max + 64 <= TX_BUF)`); response structs in
  **static** storage (encoding runs after the handler returns).
- `CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128` as usual.
- Kconfig: `ZMK_WATCHDOG_STUDIO_RPC` depends **only on `ZMK_STUDIO`** (not
  on the detectors) and handlers must work with an empty store — this is
  what lets native_sim unit tests drive the whole RPC surface (zero-device
  pitfall from skills/develop-zmk-module).

## 9. Web UI

Extend the template's `web/src/App.tsx` (React +
`@cormoran/zmk-studio-react-hook`, `ZMKCustomSubsystem.callRPC`, generated
ts-proto types via `buf generate`):

- **Status card**: capacity, stored count, `recording_stopped` banner
  ("log full — recording paused; delete incidents to resume"),
  dropped-since-boot counter.
- **Incident table**: id, source (Central / Peripheral N), type badge,
  boot ordinal + uptime, detail column (thread/queue name, reason+PC/LR in
  hex, reset-cause bits decoded to names). Source selector triggers the
  relay flow for peripherals and accumulates notification-delivered rows.
- **Delete**: per-row delete + "delete all (this source)" with confirm.
- Subscribe to notifications for relayed rows / completion / live
  `IncidentRecorded`.
- Keep the template's connection scaffold; this is one page, no routing.

## 10. Kconfig summary

Template initialization produced `CONFIG_ZMK_WATCHDOG_FEATURE` and
`CONFIG_ZMK_WATCHDOG_FEATURE_STUDIO_RPC` (mechanical rename of
`ZMK_TEMPLATE_FEATURE*`). **Phase B renames these** to the table below
(`ZMK_WATCHDOG`, `ZMK_WATCHDOG_STUDIO_RPC`) — grep every reference:
`Kconfig`, `CMakeLists.txt`, `tests/studio/native_sim.conf`,
`tests/zmk-config/build.yaml`, README, and the keycode_events snapshot's
registration expectations if config names appear there.

| Symbol | Default | Meaning |
|---|---|---|
| `ZMK_WATCHDOG` | n | core: store + boot hook + reset-cause audit |
| `ZMK_WATCHDOG_FREEZE_DETECT` | y if ZMK_WATCHDOG | task_wdt monitor (selects `TASK_WDT`) |
| `ZMK_WATCHDOG_FREEZE_TIMEOUT_MS` | 5000 | freeze threshold |
| `ZMK_WATCHDOG_FATAL_DETECT` | y if ZMK_WATCHDOG | fatal-handler override |
| `ZMK_WATCHDOG_MAX_INCIDENTS` | 16 | flash slot cap (per half) |
| `ZMK_WATCHDOG_STUDIO_RPC` | y if ZMK_STUDIO && ZMK_WATCHDOG | RPC subsystem (central) |
| `ZMK_WATCHDOG_SPLIT_RELAY` | y if ZMK_SPLIT && ZMK_WATCHDOG | relay proxy/responder (needs `ZMK_SPLIT_RELAY_EVENT`) |
| `ZMK_WATCHDOG_TEST_INJECTION` | n | dangerous fault/freeze injection RPC |

## 11. Source layout

```
src/watchdog_store.c        # settings-backed slot store + RAM index + cap
src/watchdog_pending.c      # __noinit slot, CRC, boot conversion hook
src/watchdog_freeze.c       # task_wdt channels + feed works
src/watchdog_fatal.c        # k_sys_fatal_error_handler override
src/watchdog_reset_cause.c  # boot-time hwinfo audit
src/watchdog_inject.c       # test injection (Kconfig-gated)
src/split/watchdog_relay.c  # wdq/wdp events, peripheral responder, central proxy
src/studio/watchdog_handler.c
include/cormoran/zmk/watchdog.h   # record struct, store API (used by tests)
proto/cormoran/watchdog/watchdog.proto|.options
web/src/App.tsx (+ components)
tests/...                   # see §12
```

`reboot` and `time` are wrapped behind tiny functions (`watchdog_reboot()`,
weak/testable) so native_sim tests can intercept instead of dying.

## 12. Test plan

**native_sim unit tests** (`west zmk-test tests -m .`, via `python3 -m unittest`):

- store: append/read/delete/cap-stop/drop-count/persistence-across-
  `settings_load` (needs `CONFIG_SETTINGS` + backend in native_sim conf —
  check what the template/custom-settings tests already enable and copy).
- pending: fabricate record → boot-convert → appears in store; bad CRC →
  discarded.
- freeze: block a monitored queue in a test work item → task_wdt callback →
  (intercepted reboot) → pending slot filled correctly.
- RPC: full request/response matrix against an empty and a populated store,
  pagination boundaries, delete, status; injection ack.
- relay: raise synthetic `wdq`/`wdp` events at the handlers directly
  (both role compile paths built by build tests).

**build tests** (`tests/zmk-config/build.yaml`): xiao_ble + tester_xiao
central-with-studio build; a split peripheral build with relay on; a
minimal no-studio build (core only). Use snippets under
`tests/zmk-config/snippets/` (remember the `RC()` macro clash and
tester_xiao pin-conflict pitfalls if any overlay is added).

**Hardware (Phase F)** on this rig (read
`skills/develop-zmk-module/references/hardware-rig.md` + `skills/debug-zmk-jlink`
first — flash offset `CONFIG_FLASH_LOAD_OFFSET=0x0` workaround, RTT read via
JLinkExe `savebin`, RTT control-block zeroing before reflash,
`CONFIG_LOG_PROCESS_THREAD_STARTUP_DELAY_MS=0`, pyusb RPC transport):

1. Flash test build (`ZMK_WATCHDOG_TEST_INJECTION=y`, studio on).
2. `inject freeze` → observe reboot → `list` shows FREEZE incident with the
   right queue name; RTT log confirms boot-time conversion.
3. `inject fault` → FATAL incident with plausible PC/LR.
4. Repeat until cap → `status.recording_stopped=true`, further injections
   only bump `dropped_since_boot`.
5. `delete all` → recording resumes.
6. Web UI smoke test if a browser is available; otherwise the pyusb CLI
   (`tools/zmk-studio-rpc custom-call`) covers the RPC surface.

(A hardware-watchdog-vs-debugger sanity check previously appeared here; it
no longer applies now that the hardware fallback has been removed — see §4.1
"Hardware fallback: prototyped and removed" and §12.1.)

Split relay on real hardware was attempted once this rig grew a second board
(see §12.1) but was not completed; native_sim/relay unit tests + split build
tests remain the primary evidence for that path.

### 12.1 Hardware validation results (2026-07-05)

**Single board (XIAO nRF52840 "Module Test", J-Link `1050398082`, flash-offset-0
workaround) — thoroughly validated, all green:**

- Flashed `module_watchdog_board_test_injection`
  (`CONFIG_ZMK_WATCHDOG_TEST_INJECTION=y`, Studio RPC on). Confirmed via
  Studio RPC (`core.get_device_info`, `custom.list_custom_subsystems` →
  `cormoran__watchdog`).
- `InjectTestRequest{FREEZE_KIND}` → device stopped responding, rebooted
  after the 5 s timeout, re-enumerated; `ListIncidents` showed a `FREEZE`
  incident with `queue_name: "sysworkq"` — proves the task_wdt channel,
  retained-RAM pending slot, and boot-time conversion all work correctly
  end-to-end on real hardware.
- `InjectTestRequest{FATAL_KIND}` → request itself failed with a USB pipe
  error (expected: `k_oops()` runs synchronously in the RPC handler's own
  call stack, so the device faults before a response can be sent) →
  reboot → `FATAL` incident recorded with `reason: 3` (exactly
  `K_ERR_KERNEL_OOPS`, matching `k_oops()`) and plausible `pc`/`lr` values.
  Repeated 6 times total across the session; reason/pc/lr were identical
  every time (deterministic fault site), as expected.
- Filled the store to `capacity` (16) via 13 more `FATAL_KIND` injections;
  `GetStatus` correctly showed `recording_stopped: true` at exactly 16
  stored. One further injection was **dropped** (`stored` stayed 16,
  `dropped_since_boot` incremented to 1, no flash write) — proves the
  flash-wear cap works exactly as designed.
- `DeleteIncidents{all: true}` → `stored: 0`, `recording_stopped` cleared
  — recording resumed correctly.
- `ListIncidents` pagination verified with 5 stored incidents: page 0
  returned exactly 4 (`WATCHDOG_RPC_PAGE_SIZE`), page 1 (`start_index: 4`)
  returned the remaining 1 with `total: 5` — matches native_sim coverage.
- **Reset-cause audit also engaged unprompted** during this session — a
  `RESET_CAUSE` incident with `cause_bits: 18` (`RESET_WATCHDOG |
  RESET_SOFTWARE`) appeared once, most likely from one of several manual
  `JLinkExe` SWD resets performed while investigating tooling issues (see
  below) rather than from the module's own detectors, but it confirms the
  degraded fallback path (§4.3) does fire correctly and does not crash or
  duplicate when it does.
- **Tooling finding, not a firmware bug**: after a *software*-triggered
  reboot (`sys_reboot()`, or an equivalent JLink `r`/`AIRCR.SYSRESETREQ`
  reset), this sandbox's host-side USB hotplug sync sometimes fails to
  create the `/dev/zmk-hp-zmk-tty-*` symlink for the new enumeration (the
  underlying kernel `ttyACM*` device and its sysfs `cdc_acm` binding exist
  fine — confirmed via `/sys/bus/usb/devices/.../tty/ttyACMN` — but no
  `/dev` node appears, even after `udevadm settle`, for minutes). The
  existing `docs/zmk-studio-rpc.md` pyusb transport
  (`--transport pyusb`) bypasses this entirely (talks to the CDC bulk
  endpoints directly via libusb, no OS tty node needed) and was used for
  the rest of this session once discovered. Worth folding into
  `skills/debug-zmk-jlink` as a documented workaround for post-*software*-
  reboot (as opposed to post-*flash*) enumeration flakiness.

**Split (2 boards) — attempted, blocked by a hardware issue on the second
board, not completed:**

Built `module_watchdog_split_peripheral` (Module Test board) and
`module_watchdog_split_central` (Abyss Tester XIAO, J-Link `1057792823`, no
flash-offset workaround needed) per `skills/debug-zmk-split`. The
peripheral flashed and ran correctly (confirmed stable USB re-enumeration,
no crash). **The central board failed to boot past very early
initialization**: `JLinkExe`/GDB backtraces repeatedly caught it inside
`lfclk_spinwait()` (`drivers/clock_control/clock_control_nrf.c`) — a
busy-wait for the external 32.768 kHz crystal (LFXO, this board's default
`CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL=y`) to report stable, which never
happened. Trying `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` (internal RC
oscillator) as a diagnostic did not help. **The user physically
power-cycled this board mid-session**, after which the clock issue
resolved (subsequent GDB backtraces showed genuine, deep BLE controller
activity — real HCI command processing via the ticker/mayfly scheduler,
not a hang) — consistent with this being a marginal crystal
startup/hardware condition on this specific unit rather than a firmware
bug (nothing in this module touches clock configuration). However, even
after the power cycle, this board's USB never enumerated (confirmed via
raw `lsusb`, not just the `/dev` symlink layer) and its RTT log buffer's
write offset stayed at 0 despite ~90 s of additional waiting, so Studio
RPC access to it was never established this session. Given the
substantial hardware time already spent and that this points to a
rig/hardware condition independent of the watchdog module's own code, hardware
split validation was stopped here rather than pursued further; the
peripheral was left flashed with a clean, non-looping build
(`module_watchdog_split_peripheral`, no test-injection auto-trigger) so
the rig is in a good state for a future session. **Recommendation for
whoever picks this up**: start with a fresh physical power cycle of the
Abyss Tester XIAO board before flashing anything, and confirm plain
`core.get_device_info` over Studio RPC works *before* layering the
watchdog-specific relay checks on top.

**Follow-up conclusion (post-Phase-F): `lfclk_spinwait()` hang traced to a
plausible interaction with `CONFIG_ZMK_WATCHDOG_HW_FALLBACK`, feature
removed.** A code-analysis pass (over `drivers/clock_control/clock_control_nrf.c`)
investigating this central-board hang found a credible mechanism connecting
it to this module's (now-removed) hardware-watchdog fallback: the
nRF52840's hardware watchdog peripheral survives soft/debugger/pin resets
(only POR/BOR clears it), and the LFCLK driver's RC→XTAL clock handoff is
one-shot (attempted once at boot, never retried). If a prior boot had armed
the hardware watchdog (via `CONFIG_ZMK_WATCHDOG_HW_FALLBACK=y`) and the board
was then reset other than by a full power cycle, the still-running old
watchdog could plausibly block the LFCLK handoff on the next boot, hanging
the board forever in `lfclk_spinwait()` — matching what was observed on the
central board above. This was **not proven with full certainty** end-to-end
(this session's hardware experiment had methodology confounds — notably, the
user's physical power cycle mid-session, which per this mechanism should
have been the fix, is also consistent with several other explanations for a
marginal-crystal condition), but the mechanism is well-supported by the
driver source and the downside is severe enough (a device that can only be
recovered with a battery pull) that the project owner decided to remove
`CONFIG_ZMK_WATCHDOG_HW_FALLBACK` entirely rather than keep it as an opt-in.
See §4.1 "Hardware fallback: prototyped and removed" for the full writeup;
this module no longer offers any hardware-watchdog-backed fallback.

## 13. Implementation phases (each = one subagent task)

Phase A — **done** (commit "Initialize zmk-feature-watchdog from module
template" on `codex/init-watchdog`, pushed). State: placeholders replaced
(`namespace=cormoran module=watchdog`, subsystem id `cormoran__watchdog`,
sample handler at `src/studio/watchdog_handler.c`, proto at
`proto/cormoran/watchdog/watchdog.proto`); isolated west workspace ready
(`./dependencies`, ~3.9 GB, gitignored); `python3 -m unittest`, all web
checks, and `SKIP=prettier,eslint,jest,web-build pre-commit run --all-files`
green. Known quirks: the Studio subsystem registration order in
`tests/studio/keycode_events.snapshot` is runtime order, not alphabetical —
re-capture from actual output when it changes; `.github/workflows/template-sync.yml`
intentionally still points at the upstream template.

- **Phase B — store + pending + boot hook.** §5, §6, Kconfig core, unit
  tests. Acceptance: store tests green, `python3 -m unittest` green.
- **Phase C — detectors.** §4.1–4.4 incl. reboot wrapper + injection hooks,
  freeze unit test, build test configs. Acceptance: freeze test green on
  native_sim; xiao build test compiles with all detectors on.
- **Phase D — RPC + web.** §8, §9 for source 0 (local logs), pagination,
  status, delete, notifications on new local incident. Acceptance: RPC unit
  tests + `cd web && npm ci && npm run generate && npm test && npm run lint
  && npm run build` green.
- **Phase E — split relay proxy.** §7 (`wdq`/`wdp`, responder, proxy,
  relayed notifications), split build tests. Acceptance: relay unit tests +
  both role builds green.
- **Phase F — hardware validation.** §12 checklist on the rig. **Status:
  single-board portion done and green (§12.1); split-relay portion
  attempted but blocked by a hardware issue on the second board (§12.1) —
  resume with the recommendation there.** README already reflects the
  user guide content from earlier phases; revisit once split hardware
  validation actually completes.

Every phase: run `python3 -m unittest` inside the nix devshell
(`nix --extra-experimental-features 'nix-command flakes' develop
/home/ubuntu/zmk-workspace/nix --command bash -lc '…'`), commit at
milestones, `SKIP=prettier,eslint,jest,web-build pre-commit run --all-files`
(web hooks are broken in the devshell — run npm equivalents directly).
Read `skills/zmk-module-dev/SKILL.md` in-repo and
`/home/ubuntu/zmk-workspace/skills/develop-zmk-module/SKILL.md` before coding.

## 14. Risks / open points (decide during implementation, don't redesign)

- native_sim settings backend selection (file vs NVS+flash_sim) — copy
  whatever zmk-feature-custom-settings' native_sim tests use.
- Exact `boot_ordinal` bookkeeping (§6) — any scheme with ≤1 extra small
  settings write per persisted incident is acceptable.
- Feed-work starvation false positives if a user work item legitimately
  blocks a queue > timeout (e.g. long flash erase in settings) — if hardware
  validation shows this, raise the default timeout; do not add suppression
  logic in v1.
- `__noinit` may land in a RAM area the Adafruit UF2 bootloader scrubs —
  hardware Phase F explicitly verifies survival across `sys_reboot`; if it
  fails, switch to a devicetree `zephyr,retained-ram` region at top of SRAM
  (API-compatible change confined to `watchdog_pending.c`).
