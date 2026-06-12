# Tools

This directory contains the Python helpers used to build, test, capture, and calibrate the SupraMatic E2 ESPHome firmware. Run them through `uv` from the repository root so the pinned dependencies and console scripts are used.

## Script Index

| Command | Source | Purpose |
| --- | --- | --- |
| `uv run garage-phone-sync` | [run_phone_video_sync_capture.py](run_phone_video_sync_capture.py) | Fullscreen phone-video sync display, HCP command runner, and ESP persistent-log coordinator |
| `uv run garage-decode-phone-sync-video` | [decode_phone_sync_video.py](decode_phone_sync_video.py) | Decode the fullscreen QR timecode from a phone video |
| `uv run garage-fetch-hcp2-reference` | [fetch_hcp2_reference.py](fetch_hcp2_reference.py) | Fetch and normalize the pinned public HCP2 reference corpus into ignored `captures/hcp2/` |
| `uv run garage-analyze-aruco-video` | [analyze_garage_aruco_video.py](analyze_garage_aruco_video.py) | Extract door motion curves from ArUco marker videos |
| `uv run garage-hcp-timing-calibration` | [run_hcp_timing_calibration.py](run_hcp_timing_calibration.py) | Run a normal HCP open/close timing capture through ESPHome |
| `uv run garage-analyze-hcp-timing` | [analyze_hcp_timing.py](analyze_hcp_timing.py) | Align persistent HCP logs with previously extracted motion curves |
| `uv run garage-init-secrets` | [init_secrets.py](init_secrets.py) | Generate local ESPHome API, OTA, and proxy secrets |
| `uv run garage-supramatic-sim` | [supramatic_sim](supramatic_sim/) | Virtual SupraMatic 4 HCP2 master for closed-loop host and HIL tests |
| `uv run garage-hcp2-hil-la` | [hcp2_hil_la.py](hcp2_hil_la.py) | Capture and analyze HCP2 HIL logic-analyzer traces for DE/TX reset-safety checks |
| `uv run garage-hcp2-hil-load` | [hcp2_hil_load.py](hcp2_hil_load.py) | Run HCP2 HIL simulator scenarios while host/Wi-Fi/API load commands are active |
| `uv run garage-test-wizard` | [garage_test_wizard.py](garage_test_wizard.py) | Guided manual Home Assistant position tests using measured clear-opening height |
| `uv run garage-generate-aruco-markers` | [generate_aruco_marker_pdfs.py](generate_aruco_marker_pdfs.py) | Generate printable ArUco marker PDFs |
| `uv run garage-hcp-proxy-client` | [hcp_proxy_client.py](hcp_proxy_client.py) | Laptop-side RS-485 proxy experiments |

## Visual Calibration Workflow

This workflow is optional. It is not needed to build, flash, wire, or use the opener as a normal Home Assistant garage-door cover. Skip it if you only want open, close, stop, light, endpoint state, diagnostics, and HomeKit Bridge.

The visual workflow is for one narrow problem: making percentage position control more accurate. Because the HCP1 status broadcasts do not expose a trustworthy continuous position, the firmware estimates position from timing. The marker/video process is a way to measure that timing instead of guessing.

It is also fair to call this overengineered for most installs. It combines phone video, printed ArUco tracking markers, a fullscreen QR timecode, and ESP protocol logs so the motion model can be audited later. The latest successful visual run is `AUTO_04` from 2026-05-27.

The capture measures:

- The visible opening and closing motion curves.
- The delay between command send, visible movement, and the HCP endpoint report.
- The difference between an intermediate stop position and the final settled door position.

### 1. Record A Synchronized Video

The phone records the physical door, printed ArUco markers, and the MacBook fullscreen QR timecode in one shot. The ESP captures the HCP persistent protocol log at the same time.

```bash
uv run garage-phone-sync \
  --sequence position_targets_no_calibration \
  --yes
```

Use `--dry-run` first to check the fullscreen layout without moving the opener.

<p align="center">
  <img src="../docs/research/analysis/phone-sync-auto04-20260527/auto04_event_contact_sheet.jpg" width="560" alt="AUTO_04 event contact sheet showing ArUco door markers and MacBook QR timecode">
</p>

The contact sheet above shows the important calibration ingredients:

- ArUco markers attached to the moving door segment and fixed references.
- A large QR timecode on the MacBook screen for video-to-run alignment.
- Command milestones such as `target_25_from_closed`, `target_75_from_open`, and endpoint resets.

### 2. Decode Time And Marker Position

The QR decoder maps video time to ESP run time. For `AUTO_04`, the accepted QR samples fitted:

```text
run_elapsed_s = 0.99997635 * video_time_s + 17.862
```

The ArUco pass then extracts the physical door position. The timeline below compares video-derived position with the firmware estimate that was active during the run.

<p align="center">
  <img src="../docs/research/analysis/phone-sync-auto04-20260527/auto04_position_timeline.png" width="860" alt="AUTO_04 timeline comparing video ArUco position with firmware cover estimate">
</p>

### 3. Align Full-Travel Timing

Full open and full close use the endpoint motion model. The timing-aligned full-travel capture measured visible motion separately from the HCP command-to-end-state timing:

| Direction | Visible motion | Command to HCP endpoint | Combined offset |
| --- | ---: | ---: | ---: |
| Opening | `10.215 s` | `12.825 s` | `2.610 s` |
| Closing | `18.565 s` | `22.274 s` | `3.709 s` |

<p align="center">
  <img src="../docs/research/analysis/garage-door-motion-20260527/hcp-video-timing-alignment.png" width="760" alt="Opening and closing HCP timing alignment against video-derived motion curves">
</p>

### 4. Fit Interrupted-Stop Behavior

Percentage targets do not use the same profile as full open or full close. For intermediate positions, the opener is stopped abruptly and then settles slightly. The firmware therefore predicts the final settled position from the stop position.

Recovered from the timing-aligned `AUTO_02` and `AUTO_04` runs:

| Direction | Stop-response model | Fit quality |
| --- | --- | ---: |
| Opening from closed | `settled = 0.975103 * stop + 0.0208324` | `0.30 pp` RMSE |
| Closing from open | `settled = 0.992568 * stop - 0.00163212` | `0.10 pp` RMSE |

<p align="center">
  <img src="../docs/research/analysis/motion-model-fit-20260527/recovered_interrupted_model_fit.png" width="860" alt="Recovered interrupted motion model from AUTO_02 and AUTO_04 stop observations">
</p>

The top row shows why the first naive model was wrong: requested target and final settled position are not the same for abrupt stops. The bottom row is the model the firmware uses: stop position versus settled position.

### 5. Apply The Calibration

The calibrated values live in [configs/supramatic-e2.yaml](../configs/supramatic-e2.yaml) and are documented in [docs/time-based-position.md](../docs/time-based-position.md). The key behavior is:

- Full open/close uses the measured endpoint curves and waits for HCP endpoint confirmation.
- Intermediate targets use the interrupted-stop model.
- Exact `0%` closed is only published after the HCP closed bit is decoded.
- The persistent protocol log is PSRAM-only for these captures, so repeated calibration runs do not wear ESP flash.

## Reference Artifacts

| Artifact | What to inspect |
| --- | --- |
| [phone-sync-auto04-20260527](../docs/research/analysis/phone-sync-auto04-20260527/README.md) | Latest successful synchronized phone-video run |
| [motion-model-fit-20260527](../docs/research/analysis/motion-model-fit-20260527/README.md) | Stop-response model and fit metrics |
| [garage-door-motion-20260527](../docs/research/analysis/garage-door-motion-20260527/README.md) | Full-travel opening/closing curves and HCP timing alignment |
