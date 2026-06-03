# Research Data

This directory contains calibration and reverse-engineering artifacts from the tested SupraMatic E2 installation.

Normal users do not need these files to build or flash the firmware. They are kept so the motion model, protocol assumptions, and plots can be audited.

Private details such as exact serial number and local filesystem paths are intentionally redacted.

## Visual Calibration Gallery

The latest successful synchronized video run is [phone-sync-auto04-20260527](analysis/phone-sync-auto04-20260527/README.md). It combines:

- A phone video of the door and ArUco markers.
- A large MacBook QR timecode visible in the same frame.
- ESP persistent HCP logs captured during the command sequence.

<p align="center">
  <img src="analysis/phone-sync-auto04-20260527/auto04_event_contact_sheet.jpg" width="540" alt="AUTO_04 calibration contact sheet">
</p>

The extracted position timeline compares the video-derived ArUco position against the firmware's live cover estimate:

<p align="center">
  <img src="analysis/phone-sync-auto04-20260527/auto04_position_timeline.png" width="820" alt="AUTO_04 position timeline">
</p>

The full-travel timing alignment keeps the endpoint open/close model separate from the interrupted-stop model used for percentage targets:

<p align="center">
  <img src="analysis/garage-door-motion-20260527/hcp-video-timing-alignment.png" width="760" alt="HCP and video timing alignment">
</p>

The recovered interrupted-stop model explains the calibrated percentage-control behavior:

<p align="center">
  <img src="analysis/motion-model-fit-20260527/recovered_interrupted_model_fit.png" width="820" alt="Recovered interrupted-stop motion model">
</p>

See [../time-based-position.md](../time-based-position.md) for the firmware settings derived from these artifacts, and [../../tools/README.md](../../tools/README.md) for the tool workflow.
