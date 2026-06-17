# HCP2 Standard ESP32 Realtime Backend Plan

This is an implementation plan for adding an experimental standard ESP32 HCP2
backend that shares the current ESP32-C6 LP-core protocol/mailbox design as much
as possible.

Assumption for this design: reboot, panic, WDT reset, serial flashing, and
bootloader gaps are accepted. The goal is high reliability while the HP cores are
running, not LP-style continuity through HP reset.

Reliability boundary: `esp32_realtime` is not a reduced C6 LP backend. It is a
separate experimental backend that may be healthy only while the HP cores are
running. A classic ESP32 missed-poll gap during reboot, panic, WDT, serial
flashing, or bootloader is accepted; actively driving the RS-485 bus during those
gaps is not accepted unless the transceiver is physically isolated from the motor.

Closed implementation decisions:

- First implementation scope is Phase A/B/C only: backend abstraction, shared
  responder runtime, and config/validation. Do not implement the custom ESP32
  realtime ISR backend in this pass.
- First classic target is ESP32-WROOM without PSRAM.
- HCP2 responder backends are single-instance per device for now.
- ESP32-WROOM defaults are UART2, RX GPIO16, TX GPIO17, DE GPIO18, and /RE GPIO19.
- RS485 modes are `de_re` and `auto_direction`. `de_re` is the hardened default.
  `auto_direction` is supported in config from the start, but approved for
  real-motor use only after that exact board/transceiver passes HIL.
- UART RTS RS-485 mode is out of scope for now.
- `esp32_realtime` OTA policy is configurable through `allow_unsafe_ota`, default
  `false`; component/config validation should block OTA when unsafe OTA is not
  allowed.
- `esp32_realtime` restart policy is configurable as `restart_policy:
  no_auto_restart` by default, with `restart_policy: auto_restart` as a bench/HIL
  option.
- Motor-facing `esp32_realtime` setups must also reject configured whole-device
  auto-reboot paths, not only backend self-restart. Treat `esp32_realtime` as
  motor-facing by default; bench/HIL relaxations require explicit bench-only
  overrides.
- The classic ESP32 backend remains experimental only for now and is not a public
  release image target.
- Do not add new mailbox ABI fields for ESP32 realtime diagnostics in the initial
  implementation. Use existing fields, protocol/trace events, or private HIL-only
  stats until a shared need is proven.
- Timer path for the eventual ISR backend starts with GPTimer behind a replaceable
  abstraction.
- Debug UI and protocol logging should use the same schema/format as the C6 version,
  with memory budget gates rather than different defaults. Destructive HP actions
  such as restart, CPU reset, and panic are capability-gated for `esp32_realtime`.

Phase A/B/C deliverable boundary: these phases do not implement an active ESP32
realtime responder. Phase C may add a compile-only inert `esp32_realtime` backend
stub so ESPHome config/codegen/build validation can run, but that stub must not
install UART, timer, or ISR handlers, must not touch RS-485 pins, must not transmit,
and must report the backend as unavailable until Phase D/E. No non-ISR responding
task is part of Phase C.

## Target Outcome

The ESPHome-facing HCP2 component should support two mailbox-backed responder
backends:

- `esp32c6_lp`: current production path. The responder runs on the ESP32-C6 LP
  core, uses LP-UART, survives HP-core restart cases already proven by HIL, and
  exchanges state/commands through LP SRAM.
- `esp32_realtime`: experimental standard ESP32 path. The responder runs on a
  high-priority HP-core ISR/timer/UART path, uses a normal ESP32 UART, and
  exchanges state/commands through the same logical mailbox ABI in internal DRAM.
  It does not survive HP reset/reboot/bootloader gaps and must remain
  experimental until bench HIL proves zero-miss operation under load. First target
  is ESP32-WROOM without PSRAM only. During Phase C this backend is only an inert
  compile/configuration stub; the active responder path starts later.

The entity layer, cover logic, debug UI, protocol log format, simulator tests, and
Home Assistant behavior should not need to know which backend is active except for
capability reporting, safety semantics, and user-visible diagnostics.

## Non-Goals

- Do not promise bus continuity through `esp_restart`, OTA reboot, panic reset, WDT
  reset, serial flashing, bootloader, or power loss on standard ESP32.
- Do not replace the ESP32-C6 LP backend or weaken its safety gates.
- Do not modify HCP1 code.
- Do not copy GPL HCP2 implementation code.
- Do not route the realtime bus path through normal ESPHome UART, logging, web UI,
  heap allocation, sockets, JSON builders, or slow component code.

## Current Code Seams

The repo already has the important pieces:

- `components/hcp2bridge/core/hcp2_engine.*`: portable parser/responder state
  machine. This is the correct shared protocol core.
- `components/hcp2bridge/core/hcp2_mailbox.*`: fixed mailbox layout, seqlock state,
  command epoch/ack, stop-trigger, counters, protocol events, trace ring.
- `components/hcp2bridge/core/hcp2_supervisor.*`: HP-side command/session/state
  accessors over the mailbox.
- `firmware/hcp2-lp/main/lp_core/hcp2_lp_core.c`: C6 LP backend implementation.
- `components/hcp2bridge/hcp2bridge.cpp`: ESPHome integration. It currently has a
  production LP path and an HP fallback path.

Important negative seam: `HCP2Bridge::lp_supervisor_task_loop_()` is not
backend-neutral. It reads mailbox state, probes LP health, and may call
`load_and_start_lp_()`. Do not reuse it for `esp32_realtime`. First split the code
into:

1. backend-neutral mailbox reader/state/protocol/trace drain logic;
2. C6-only LP start/skip/reload policy;
3. backend-specific health and restart policy.

Important missing shared seam: command consumption, command acking, stop-trigger
evaluation, mailbox counter publishing, protocol-event mailbox publication, and
trace publication currently live in the LP firmware file. Before implementing the
ESP32 realtime backend, move those semantics into a shared C module, for example
`components/hcp2bridge/core/hcp2_responder_runtime.{c,h}`, with small backend
ports for time, TX scheduling, DE control, RX input, diagnostics, and backend
health hooks.

The current HP fallback is useful for bring-up, but it is not the final standard
ESP32 design. It uses the IDF UART driver event queue, FreeRTOS task timing,
`uart_write_bytes()`, and `uart_wait_tx_done()`. It may only be used to prove
entity/mailbox compatibility, never as the production realtime backend.

## Mailbox Compatibility Strategy

Keep the mailbox layout as the cross-backend responder ABI.

Physical storage may differ:

- C6 LP backend: fixed LP SRAM address, currently `HCP2_LP_MAILBOX_ADDR`.
- ESP32 realtime backend: statically allocated internal DRAM, aligned and retained
  only for the current boot.

Logical semantics must match:

- `magic`, `abi_version`, `struct_size`, `firmware_version`;
- `heartbeat`;
- state seqlock fields;
- command `epoch`, `sequence`, `deadline`, `ack`, `ack_result`;
- stop-trigger fields;
- poll/response/error/latency counters;
- protocol event ring;
- trace ring.

Do not add ESP32-only mailbox fields at first. If the UI needs backend capability
metadata, expose it from `HCP2Bridge` rather than changing the ABI. If an ABI change
becomes necessary later, make it backend-neutral and bump
`HCP2_LP_MAILBOX_ABI_VERSION`.

Mailbox ABI changes must preserve the current header/health prefix offsets used by
C6 skip-reload and reload-defer decisions. New backend-neutral fields must be
append-only, preferably consuming `reserved1`, or must come with explicit
old-HP/new-LP and new-HP/old-LP compatibility tests. Do not insert fields before
existing magic/ABI/version/heartbeat/state/command/health counters.

The mailbox is the responder ABI, not conceptually the LP ABI. During the first
implementation the C type may remain `hcp2_lp_mailbox_t`, but all new code should
use aliases:

```c
typedef hcp2_lp_mailbox_t hcp2_responder_mailbox_t;
typedef hcp2_lp_command_id_t hcp2_responder_command_id_t;
typedef hcp2_lp_command_result_t hcp2_responder_command_result_t;
```

Field semantics:

- `firmware_version`: responder-runtime version. The C6 LP backend sets it to the
  LP blob version. The ESP32 realtime backend sets it to the realtime responder
  runtime version.
- `lp_reset_count`: responder backend start/reinit count for `esp32_realtime`.
  Expose it to new UI/configs as "Responder resets" later, but keep the raw field
  for ABI compatibility.
- `lp_time_us`: backend-local monotonic microseconds. On C6 it is LP cycle time; on
  ESP32 it is `esp_timer_get_time()` or a hardware timer count.
- `reload_decision`: C6 LP-only policy. For `esp32_realtime`, heartbeat and ABI
  checks are diagnostics/health inputs only. They must not call LP reload functions.

Add backend-neutral mailbox initialization helpers:

```c
void hcp2_responder_mailbox_init(volatile hcp2_lp_mailbox_t *mailbox,
                                 uint32_t firmware_version);
void hcp2_responder_mailbox_repair_header(volatile hcp2_lp_mailbox_t *mailbox,
                                          uint32_t firmware_version);
```

Keep LP-named wrappers for the C6 path. The ESP32 realtime backend must not call an
init/repair helper that hardcodes `HCP2_LP_FIRMWARE_VERSION`.

## Backend Interface

Add a small backend layer under `components/hcp2bridge/`:

- `hcp2_backend.h`
- `hcp2_backend_lp.cpp`
- `hcp2_backend_esp32_realtime.cpp`

The backend interface must stay outside the hot ISR path. A simple C++ interface or
plain function table is acceptable because it is used at setup/supervisor level only.

Suggested shape:

```cpp
enum class HCP2BackendKind {
  ESP32C6_LP,
  ESP32_REALTIME,
  HP_FALLBACK,
};

enum class HCP2BackendHealthAction {
  OK,
  STALE,
  ABI_MISMATCH,
  VERSION_MISMATCH,
  RESTART_BACKEND_ALLOWED_GAP,
  FAILED,
};

struct HCP2BackendCapabilities {
  const char *name;
  bool survives_hp_restart;
  bool supports_reload;
  bool supports_stop_trigger;
  bool uses_mailbox;
};

struct HCP2BackendHandle {
  volatile hcp2_lp_mailbox_t *mailbox;
  HCP2BackendCapabilities capabilities;
  uint32_t (*now_us)(void *user);
  void *user;
};
```

`HCP2Bridge` should:

- choose a backend from config;
- call backend setup/start;
- initialize `hcp2_hp_supervisor_t` against the backend mailbox when present;
- use the same command, stop-trigger, state, protocol-event, and trace-drain paths
  for all mailbox backends;
- keep backend health actions backend-specific;
- only allow `esp32c6_lp` to load, stop, or restart the LP blob.

The backend handle exposes the responder timebase. `HCP2Bridge` must use
`hcp2_hp_supervisor_send_command_at()` and
`hcp2_hp_supervisor_arm_stop_trigger_at()` with backend time for every mailbox
backend. Do not derive new command deadlines from a periodically published mailbox
time field.

`safe_for_ota_restart` must be capability-aware:

- `esp32c6_lp`: may be true when LP continuity is healthy and the existing C6 rules
  pass.
- `esp32_realtime`: always false/unsupported, because OTA reboot necessarily stops
  the responder. A separate diagnostic may report "healthy while running"; it must
  not be named or interpreted as OTA/restart safe.
- `hp_fallback`: false/unsupported.

`restart_policy` controls HCP2 backend self-restart after backend health failure.
For `esp32_realtime`, motor-facing configs must also pass a separate
whole-device no-auto-reboot validation profile. That profile rejects configured OTA
completion, restart buttons, API/Wi-Fi reboot timeouts, safe mode, and similar
automatic reboot sources unless an explicit bench/HIL override is set.
Unavoidable crash/WDT/power loss/manual reset/serial flashing cases remain accepted
missed-poll gaps, not continuity-safe behavior.

## ESPHome Config Changes

Current schema is C6-only and always enables LP-core IDF settings. Add explicit
backend selection:

```yaml
hcp2bridge:
  backend: esp32c6_lp       # default on ESP32-C6
  # backend: esp32_realtime # explicit on classic ESP32
```

Planned ESP32 realtime options:

```yaml
hcp2bridge:
  backend: esp32_realtime
  rs485_mode: de_re             # default; explicit DE + /RE
  # rs485_mode: auto_direction  # HIL-gated for motor use
  allow_unsafe_ota: false       # default
  restart_policy: no_auto_restart
  # restart_policy: auto_restart
```

Defaulting:

- Existing C6 configs with no `backend:` keep behavior-identical `esp32c6_lp`.
- Classic ESP32 configs must explicitly select `backend: esp32_realtime` until HIL
  proves enough to consider any default.
- `backend:` is authoritative. `hp_fallback: true` with no `backend:` maps to
  `backend: hp_fallback` with a deprecation warning. Supplying both is invalid
  unless they agree.

Validation rules:

- `esp32c6_lp`:
  - ESP-IDF only;
  - variant must be ESP32C6;
  - require RX GPIO4, TX GPIO5, DE GPIO0, /RE GPIO1;
  - only backend allowed to enable ULP/LP-core sdkconfig options.
- `esp32_realtime`:
  - ESP-IDF only;
  - allow ESP32-WROOM without PSRAM only for the first implementation. ESPHome
    validation cannot prove the physical module package for every generic board
    ID, so Phase C should allowlist known no-PSRAM classic ESP32 boards or require
    an explicit WROOM/no-PSRAM board profile acknowledgement; reject `psram:`;
    consider WROVER/PSRAM, ESP32-S3, or other variants later only after tests;
  - reject unicore builds;
  - default to UART2, RX GPIO16, TX GPIO17, DE GPIO18, and /RE GPIO19;
  - support `rs485_mode: de_re` with explicit `de_pin` and `re_pin`;
  - support `rs485_mode: auto_direction` with RX/TX only, but mark it not
    motor-approved until that exact board/transceiver passes HIL;
  - do not enable ULP sdkconfig options;
  - reject UART0 motor TX unless explicitly `bench_allow_uart0: true`;
  - reject pin conflicts with logger/flashing/strapping-sensitive pins unless
    explicitly overridden for bench-only use;
  - do not add UART ISR/GPTimer/cache-safety sdkconfig until Phase E, when a
    compiled hot path exists to audit;
  - default `allow_unsafe_ota` to false. Use ESPHome final validation, not OTA
    runtime listeners, to fail if any `ota:` platform is configured while unsafe
    OTA is disabled. This must include native ESPHome OTA, web-server OTA,
    HTTP-request OTA, and future OTA platforms that appear in the final config;
  - default `restart_policy` to `no_auto_restart`; `auto_restart` is bench/HIL only
    and controls backend self-restart;
  - treat configs as motor-facing by default and use final validation to reject
    configured whole-device auto-reboot sources: restart buttons/switches, API
    reboot timeouts, Wi-Fi reboot timeouts, safe-mode reboot behavior, OTA reboot
    paths, and similar configured reboot triggers. Allow relaxations only through
    explicit bench/HIL overrides;
  - reject `rs485_mode: rts` / UART RTS RS-485 mode. Only `de_re` and
    `auto_direction` are in scope for now.
- `lp_uart_clock_source`:
  - valid only for `esp32c6_lp`;
  - rejected for `esp32_realtime` unless left unset/default and ignored.
- `hp_fallback`:
  - keep hidden or mark as bring-up only.

`MULTI_CONF` must be constrained before `esp32_realtime` lands. HCP2 responder
backends are single-instance per device for now; change the component away from
`MULTI_CONF = True` or add final validation that rejects more than one
`hcp2bridge` instance for every HCP2 backend, including C6 LP and HP fallback.

Add a dedicated config later, for example:

- `configs/supramatic-4-esp32-realtime.yaml`

New ESP32 realtime configs should expose new Home Assistant labels as "Responder"
rather than "LP". Existing C6 tester entity names can remain stable for
compatibility.

## ESP32 Realtime Backend Design

Use a dedicated low-level IDF implementation, not the current UART event queue.

### Hardware

- UART at 57600 baud, 8E1.
- First target is ESP32-WROOM without PSRAM.
- Default pins are UART2 RX GPIO16, TX GPIO17, DE GPIO18, and /RE GPIO19.
- Initial motor-facing backend must use UART1 or UART2, not UART0. UART0 is reserved
  for boot/flashing/logging unless a bench-only override is enabled and an LA reset
  matrix proves no unsafe TX/DE behavior.
- `rs485_mode: de_re` uses explicit DE and /RE and is the hardened default.
- `rs485_mode: auto_direction` uses RX/TX only. It is supported for HIL from the
  start, but real-motor use is allowed only after the exact board/transceiver
  combination passes the reset/boot/flash LA matrix.
- External DE pulldown remains mandatory for `de_re`.
- `auto_direction` has no firmware-controlled DE guarantee. If the transceiver has
  an enable/shutdown pin, model it as a separate safe-default enable control rather
  than as DE; otherwise require physical isolation or a proven safe hardware default
  until the exact hardware passes HIL.
- TX idle pull-up is still recommended.
- Serial flashing while connected to a motor requires physical transceiver isolation
  unless the exact board/pin/transceiver combination has passed reset/flashing LA
  tests.
- Use an isolated RS485 transceiver for real motor use.

### Core And Interrupt Ownership

Preferred classic ESP32 layout:

- Core 0: Wi-Fi, ESPHome, API, OTA, debug UI.
- Core 1: HCP2 realtime backend task, UART ISR, timer ISR.

The backend setup task must be pinned to the intended core before allocating
UART/timer interrupts. Allocate and free those interrupts from the same core.
Reject accidental unicore FreeRTOS builds for this backend.

The bus path must not call ESPHome or debug UI code.

### Realtime Ownership Model

The realtime backend must have one explicit ownership model:

1. UART RX ISR drains the hardware FIFO and feeds a single-owner IRAM parser context,
   or drains into a fixed internal-DRAM ring consumed by one highest-priority pinned
   realtime task.
2. When a complete valid request schedules a response, the response is copied into an
   immutable TX slot.
3. Timer ISR consumes only the immutable TX slot.
4. UART TX-done ISR releases `/RE` then DE and marks the slot done.
5. Any new response while a slot is pending or transmitting is dropped/counted, never
   allowed to overwrite the active TX slot.

No ISR may call ESPHome, logging, sockets, heap allocation, C++ virtual methods, or
protocol-log JSON builders.

### RX Path

- Install a custom UART ISR with `ESP_INTR_FLAG_IRAM`.
- Read UART FIFO directly into a fixed internal-DRAM ring or feed bytes directly to
  the single-owner parser context.
- Propagate parity/framing/overflow flags as `HCP2_RX_PARITY_ERROR` and
  `HCP2_RX_FRAMING_ERROR`.
- Keep RX ISR short. If direct parsing proves too long, drain FIFO in ISR into a
  fixed ring and wake the realtime task at highest priority. HIL decides which is
  reliable enough.

### Response Timing

The strongest design is:

1. RX path completes a valid request and schedules a response for `response_delay_us`.
2. Program a hardware timer alarm for the due time.
3. Timer ISR asserts DE and /RE.
4. Timer ISR writes the immutable prepared response into the TX FIFO.
5. UART TX-done/idle ISR deasserts /RE then DE after the full frame plus tail margin.

Classic ESP32 UART FIFO is expected to hold the current HCP2 responses comfortably
(9, 15, and 21 bytes; current max frame length 32), so the first implementation can
prefer complete-frame FIFO write plus TX-done interrupt. Keep refill/underrun
counters for proof and future portability.

Add a compile-time or startup assertion for the selected target:
`UART_HW_FIFO_LEN >= HCP2_MAX_FRAME_LEN` for the complete-frame TX path. If that
assertion fails for a future ESP32-family backend, force the refill/underrun state
machine path.

TX timing should be two-stage unless HIL proves a one-stage path: assert DE and `/RE`
at `response_due_us - de_lead_us`, write FIFO at `response_due_us`, release after TX
idle plus tail margin. `de_lead_us` starts conservative and is calibrated by LA.

Use GPTimer first, configured for cache-safe ISR operation, behind a replaceable
timer abstraction. Do not use FreeRTOS tick timing or the UART event queue as the
timing authority. If HIL shows GPTimer jitter, cache-safety complexity, or linker-map
contamination, switch the timer backend to direct timer-group registers.

The realtime backend must prevent clock/sleep modes that perturb UART/timer timing
while attached:

- no light sleep while backend active;
- acquire required power-management locks for APB/timer stability if PM is enabled;
- test with modem power save enabled and disabled, but do not rely on power save
  being benign.

### DE And Bus Safety

Copy the C6 safety shape:

- external DE pulldown;
- firmware-owned DE and `/RE`;
- stale RX flush rules before local TX if needed;
- measured DE lead time before first TX bit;
- full-frame tail hold;
- max-DE-hold counter;
- DE deadman release if TX idle never arrives;
- no TX outside DE;
- `/RE` high only during DE when separate `/RE` is used.

Bootloader/reboot gaps being accepted means missed replies are acceptable. It does
not mean the board may actively drive the RS-485 bus during boot, flashing, or reset.

### IRAM/DRAM Discipline

`ESP_INTR_FLAG_IRAM` is necessary but not sufficient. Every function and every datum
reachable from the hot ISR/timer path must live in internal memory. That includes
indirect calls and read-only constants.

Everything used by the bus path must be IRAM/DRAM safe:

- UART ISR, timer ISR, TX-done ISR;
- direct UART/GPIO/timer register helpers;
- `hcp2_engine_rx_byte()` or a dedicated IRAM-safe wrapper;
- response claim/start/done helpers;
- CRC and frame builders;
- active engine state;
- immutable response slots;
- mailbox command/stop-trigger helpers used in the realtime path;
- constants used by frame building and button encodings;
- callback/user context.

Avoid:

- `ESP_LOG*`;
- `malloc`/`new`;
- `std::string`;
- C++ virtual calls;
- ESPHome component calls;
- socket/debug code;
- FreeRTOS queues in ISR;
- mutexes/spinlocks in the hot path unless proven bounded and IRAM safe.

Implementation may require marking selected core functions with a portable macro:

```c
#if defined(HCP2_ESP32_REALTIME_HOT_PATH)
#define HCP2_HOT_TEXT IRAM_ATTR
#define HCP2_HOT_DATA DRAM_ATTR
#else
#define HCP2_HOT_TEXT
#define HCP2_HOT_DATA
#endif
```

Do not key hot placement only on `ESP_PLATFORM`; the C6 LP blob and host tests
compile the same C core and must not inherit HP-realtime section attributes
accidentally. Use hot placement only where required; host tests must still compile.

CI must inspect the linker map for the ESP32 realtime config and fail if any
hot-path symbol or hot-path constant is placed in flash or PSRAM. Audit at least:

- UART ISR, timer ISR, TX-done ISR;
- direct UART/GPIO/timer register helpers;
- engine RX/path functions used by ISR;
- CRC implementation and tables, if any;
- frame builders and button encoding constants;
- mailbox command/stop-trigger helpers used by realtime backend;
- active engine state, mailbox, response slots, rings, and callback context.
- ISR-reachable `memcpy`, `memmove`, `memset`, division helpers, switch/jump tables,
  literal pools, and compiler-emitted helper calls unless each resolves to ROM/IRAM
  safe code and DRAM data.

### Mailbox Publisher

The realtime backend owns a static mailbox:

```cpp
static DRAM_ATTR hcp2_lp_mailbox_t hcp2_esp32_mailbox;
```

This sketch depends on the closed single-instance decision. Do not add per-instance
mailbox allocation in the first implementation; reject multiple HCP2 bridge
instances instead.

At backend start:

- zero/init mailbox;
- repair header;
- begin a fresh supervisor session;
- increment `lp_reset_count`;
- initialize engine and counters;
- start UART/timer ISR path.

During operation:

- increment `heartbeat` from the realtime task or timer-owned loop;
- publish state after broadcasts;
- publish counters at bounded cadence;
- publish protocol events through the same mailbox ring format as the LP path;
- handle mailbox commands at poll boundaries or from the realtime task;
- arm/disarm/fire stop-trigger using the shared responder runtime.

Mailbox protocol-event and trace rings are single-writer unless replaced with an
explicit IRAM-safe claim API. RX ISR, timer ISR, and TX-done ISR must not all publish
directly into the mailbox ring. Use one realtime owner, or a small hot-path event ring
drained by one mailbox publisher.

Do not let the debug UI drain rings faster than the backend can tolerate. The UI
should continue to read from normal ESPHome context only.

Debug UI actions must be backend-capability aware. Protocol-log format, health JSON,
and non-destructive diagnostics should match C6, but restart, CPU-reset, panic, and
similar HP-destructive actions must be hidden, disabled, or interlocked for
`esp32_realtime` unless an explicit bench/HIL policy enables them. On standard ESP32
those actions intentionally create missed-poll gaps.

Before adding ESP32 realtime-specific diagnostics, create a mapping table for each
desired counter (`timer_lateness`, `slot_drops`, `tx_underrun`, `isr_max_time`, and
future equivalents). In the initial implementation, each item must map to exactly
one of:

- an existing backend-neutral mailbox field;
- a protocol/trace event only;
- private HIL-only backend stats.

Do not add new mailbox ABI fields for these diagnostics at first, and do not
overload misleading LP fields for unrelated ESP32-only diagnostics. Revisit
append-only ABI fields only after a shared UI or C6-compatible need is proven.

## Required Refactors Before ESP32 Backend

1. Add mailbox aliases and responder-backend terminology.
2. Add backend enum, capabilities, and backend-specific health actions.
3. Split C6 LP setup/start/skip/reload into `hcp2_backend_lp.cpp`.
   After this phase, a classic ESP32 `esp32_realtime` compile must not include or
   reference `hcp2_lp_blob.h`, `ulp_lp_core.h`, `lp_core_uart.h`,
   `HCP2_LP_MAILBOX_ADDR`, or any `ulp_lp_core_*` symbol from common translation
   units. These belong only to `hcp2_backend_lp.cpp`.
4. Split mailbox read/state/protocol/trace drain from LP reload policy.
5. Add shared `hcp2_responder_runtime.{c,h}` for command lifecycle, stop-trigger,
   protocol-event publishing, trace publishing, and common counters.
6. Generalize `lp_ready_` to `backend_ready_`.
7. Make `update_state_from_mailbox_()` backend-neutral.
8. Make `queue_button_()` and `arm_stop_trigger()` use the mailbox for all mailbox
   backends.
9. Make `safe_for_ota_restart` backend-capability aware.
10. Enforce the closed single-instance policy for all HCP2 responder backends.
11. Keep HP fallback unchanged until the C6 LP backend still passes tests after the
    refactor.

## Engine Changes Required For The ISR Backend

The current engine is single-owner and callback-driven. The LP loop is safe because
one context owns `pending_tx`. A UART RX ISR plus timer ISR plus TX-done ISR can
corrupt or overwrite pending responses unless the engine exposes a stricter TX-slot
API.

Required additions before the custom ISR path:

- `hcp2_engine_rx_byte()` must be callable from an IRAM-safe context, or a new
  `hcp2_engine_rx_byte_iram()` wrapper must exist.
- Add a two-phase response API:
  - `hcp2_engine_pending_tx_ready()`;
  - `hcp2_engine_pending_tx_due_us()`;
  - `hcp2_engine_claim_due_tx(now_us, out_buf, out_len, out_meta)`;
  - `hcp2_engine_mark_tx_started(now_us)`;
  - `hcp2_engine_mark_tx_done(now_us)`.
- The claim API must make the response immutable for the TX ISR and prevent overwrite
  by duplicate, corrupt, or fault-injected RX frames.
- Host tests must prove no pending response corruption when RX, timer, and TX-done
  events interleave.

Do not change protocol semantics. Existing host tests and LP ISS tests are the
regression guard.

## Test Plan

### C6 LP Non-Regression Gates

Run these after every backend-neutral refactor:

- existing HCP2 host core tests;
- LP blob build;
- LP emulator tests;
- dual-ISS mailbox/interleaving tests;
- ESPHome C6 tester compile;
- Wokwi manual gate or HIL smoke where available;
- no HCP1 file changes unless the user explicitly asks.

Before moving LP runtime semantics into shared code, capture canonical C6 LP
host/ISS traces for scan, steady-state, command, stop-trigger, bad CRC, and HP
reboot. After the refactor, those traces must match except for explicitly reviewed
diagnostic/version changes.

### Host Tests

- Existing `tests/hcp2/test_hcp2_core.c` must keep passing.
- Add mailbox alias tests if new names are introduced.
- Add backend-neutral supervisor/runtime tests:
  - fresh session;
  - stale epoch ignored;
  - command expiry;
  - stop-trigger arm/disarm/fire;
  - protocol event ring wrap;
  - trace ring wrap.
- Add an ESP32 realtime backend model test with a simulated UART FIFO and timer:
  - split RX frames;
  - parity/framing errors;
  - FIFO high-water behavior;
  - complete-frame TX FIFO write;
  - optional TX FIFO refill/underrun path;
  - DE deasserts only after TX done plus tail margin;
  - no response to bad CRC;
  - duplicate poll while TX slot pending;
  - corrupt frame while TX slot pending;
  - repeated commands through mailbox;
  - debug/UI mailbox drain pressure.
- Add differential non-fork tests that run the same simulator scenarios against C6
  LP emulation and the ESP32 realtime model. Compare response bytes, command ack
  results, stop-trigger fire/disarm semantics, state snapshots, and protocol-event
  streams.

### ESPHome Compile And Validation Tests

- Existing C6 tester config compiles unchanged.
- New ESP32 realtime config compiles through the Phase C inert backend stub without
  ULP sdkconfig options, without UART ISR/GPTimer/cache-safety sdkconfig, and
  without touching RS-485 pins.
- Unsupported combinations fail validation:
  - `esp32c6_lp` on classic ESP32;
  - `esp32_realtime` on Arduino framework;
  - `esp32_realtime` on non-classic ESP32 variants;
  - `esp32_realtime` on unicore build;
  - `esp32_realtime` with `psram:`;
  - multiple `hcp2bridge` instances;
  - conflicting `backend:` and `hp_fallback:`;
  - `rs485_mode: de_re` with missing DE or /RE pin;
  - `rs485_mode: auto_direction` treated as bench/HIL-gated, not motor-approved;
  - `esp32_realtime` with any `ota:` platform while `allow_unsafe_ota: false`;
  - motor-facing `esp32_realtime` with configured restart buttons/switches,
    nonzero API/Wi-Fi reboot timeouts, safe-mode reboot behavior, or other
    configured whole-device auto-reboot sources unless an explicit bench/HIL
    override is set;
  - `rs485_mode: rts` or other UART RTS RS-485 mode;
  - UART0 motor TX without bench override;
  - known pin conflicts without bench override.
- Linker-map audit passes for realtime hot-path IRAM/DRAM placement once Phase E
  adds a compiled hot path. Phase C must not claim this gate.
- Size-budget gates record and threshold `.iram0.text`, `.dram0.data`,
  `.dram0.bss`, free heap after boot, and protocol-log/debug memory. Debug UI and
  protocol logging should behave like the C6 version, but the WROOM/no-PSRAM memory
  budget must prove that configuration is safe.

### HIL Tests

Use the existing HCP2 simulator as master over isolated real RS485. The logic
analyzer is the timing/electrical authority.

Minimum gates before any real-motor TX:

- compile gates for both C6 LP and ESP32 realtime configs;
- C6 LP non-regression suite unchanged;
- ESP32 realtime bench HIL with simulator master;
- LA decode of 57600 8E1, CRC-valid responses, zero missing status counters;
- no TX outside DE;
- `/RE` high only during DE if separate `/RE` is used;
- DE lead/tail within measured limits and DE deadman verified;
- 1 hour zero-miss idle;
- explicit flash-write stress: OTA data transfer, NVS commit loop if used, and raw
  partition erase/write test while bus is active; reboot at the end is expected to
  miss polls;
- Wi-Fi RX saturation, Wi-Fi TX saturation, reconnect storm, API spam, debug UI
  streaming, high CPU load on both cores;
- flash erase/write, NVS commit, OTA transfer, API spam, and CPU-load tests must
  sweep phase alignment relative to status-poll RX completion and the 4.2 ms
  response due time. The pass condition is not only zero aggregate misses; LA decode
  must show no late or truncated response in the vulnerable window;
- negative reset matrix: `esp_restart`, panic, task WDT, serial flashing/download
  reset all documented as missed-poll gaps, but must not actively drive RS-485 unless
  transceiver is isolated;
- pin/boot matrix for the exact target board.

Minimum gate before tester-ready:

- 24 hour zero-miss mixed-load HIL with LA side captures and simulator verdicts.

### Real Motor Preflight

Real motor use starts listen-only with TX physically disconnected. Only after passive
captures match simulator expectations should TX be connected, and only with an
external power-cycle escape prepared. The backend remains experimental until HIL data,
not architecture intent, proves it.

## Migration Phases

### Phase A: Backend-Neutral Mailbox Cleanup

- Add mailbox aliases.
- Add backend capabilities and backend-specific health actions.
- Split mailbox state/protocol/trace drain from C6 LP reload policy.
- Refactor C6 LP path behind backend interface.
- No behavior change.
- Run C6 LP non-regression gates.

### Phase B: Shared Responder Runtime

- Move command lifecycle, stop-trigger evaluation, protocol-event mailbox publication,
  trace publication, and common counters out of LP firmware into shared C.
- Port C6 LP firmware to the shared runtime.
- Run C6 LP non-regression gates before any ESP32 realtime code.

### Phase C: Config Support

- Add `backend:` option.
- Default to `esp32c6_lp` on C6.
- Require explicit `backend: esp32_realtime` on classic ESP32.
- Add a compile-only inert `esp32_realtime` backend stub for ESPHome codegen/build
  validation. It must not install UART/timer/ISR handlers, must not touch RS-485
  pins, must not transmit, and must report unavailable until the active backend
  phases land.
- Add `rs485_mode: de_re | auto_direction`, default `de_re`.
- Add `allow_unsafe_ota`, default `false`.
- Add `restart_policy: no_auto_restart | auto_restart`, default `no_auto_restart`.
- Add ESP32-WROOM/no-PSRAM config and validation with UART2/GPIO16/17/18/19
  defaults, rejecting `psram:` and unsupported ESP32-family variants.
- Use ESPHome final validation for cross-component checks: reject all OTA platforms
  when `allow_unsafe_ota: false`, reject multiple `hcp2bridge` instances, and reject
  conflicting `backend:`/`hp_fallback:` combinations.
- Add the motor-facing whole-device no-auto-reboot validation profile for
  `esp32_realtime`, with only explicit bench/HIL overrides allowed.
- Reject UART RTS RS-485 mode; only `de_re` and `auto_direction` are valid.
- Keep current configs behavior-identical.
- Add validation tests.

### Phase D: ESP32 Realtime Skeleton

- Add static/aligned DRAM mailbox storage according to the closed single-instance
  rule.
- Add setup path that initializes mailbox and supervisor.
- Add a non-ISR prototype task that publishes through the mailbox.
- This phase proves entity/debug/mailbox compatibility, not final timing.
- Do not use skeleton HIL results to justify motor TX.

### Phase E: Custom UART/Timer Realtime Path

- Replace event-queue timing with custom UART ISR plus hardware timer.
- Add immutable TX-slot API and interleaving tests.
- Move hot path into IRAM/DRAM.
- Add direct FIFO write, TX-done DE release, tail margin, and DE deadman.
- Add counters for ISR max time, TX underrun, RX FIFO high-water, timer lateness,
  slot drops, and DE deadman.
- Add linker-map audit.
- Default health policy while motor-attached is "deassert DE, mark backend unhealthy,
  do not auto-restart." Backend restart with an accepted poll gap is bench-only unless
  explicitly enabled.

### Phase F: HIL Hardening

- Run simulator and LA gates.
- Tune ISR/task split based on measured latency.
- Add docs for accepted failure modes.
- Only after clean HIL, consider real motor listen-only.

## Deferred Decisions

These do not block Phase A/B/C and are intentionally deferred until after HIL data.

- Whether the experimental classic ESP32 backend should ever become a public release
  image target.
- Whether any ESP32 realtime diagnostics deserve append-only ABI fields after the
  initial private/protocol-event mapping is proven useful.

## Practicality Verdict

Solid:

- Sharing the parser, mailbox, command semantics, entity layer, protocol log format,
  simulator, and HIL master is the right architecture.
- Keeping C6 LP as the production reliability path is correct.
- Making classic ESP32 explicit and experimental is correct.

Risky but plausible:

- A classic ESP32 can plausibly answer HCP2 polls while running if the bus path is a
  strict IRAM/DRAM UART/timer ISR design, with manual DE and aggressive HIL.
- The hard risks are flash-cache behavior, Wi-Fi coexistence, ISR data placement,
  power management, reset/boot pin behavior, and RX/timer/TX concurrency.

Not practical:

- The current HP fallback task is not a production standard ESP32 backend.
- A classic ESP32 backend cannot claim C6 LP continuity class.
- Keeping command lifecycle and stop-trigger behavior duplicated per backend would
  create a fork even if the frame parser is shared.

## Recommended First Implementation Step

Start with Phase A only. It is low risk and useful even if the ESP32 backend is never
finished: the code becomes clearer because "mailbox responder" becomes the abstraction
and "C6 LP core" becomes one backend implementation. After Phase A and Phase B, the
ESP32 backend can be prototyped without disturbing the tested C6 LP path.
