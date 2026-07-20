# Akrion

[English](./README.md) | [简体中文](./README.zh-CN.md)

[![License](https://img.shields.io/badge/license-Apache--2.0-1976d2)](./LICENSE)
![Focus](https://img.shields.io/badge/focus-real--device%20algorithm%20evaluation-2e7d32)
![Scope](https://img.shields.io/badge/scope-structured%20recording%20%2B%20replay%20%2B%20comparison-f57c00)

Akrion is a real-device-first experiment recording and evaluation framework for control, filtering, and estimation algorithms.

It is built for developers and learners who need one practical thing: run an algorithm against real inputs and real disturbances, record what happened in a structured way, and compare actual outputs against expected behavior without turning the workflow into a heavy simulation platform.

## Table Of Contents

- [Akrion](#akrion)
  - [Table Of Contents](#table-of-contents)
  - [What Akrion Is](#what-akrion-is)
  - [Why It Exists](#why-it-exists)
  - [What Users Get From It](#what-users-get-from-it)
  - [Typical Situations](#typical-situations)
  - [Core Workflow](#core-workflow)
  - [Design Philosophy](#design-philosophy)
  - [Quick Start](#quick-start)
  - [What Akrion Is Not](#what-akrion-is-not)
  - [Current Status](#current-status)
  - [License](#license)

## What Akrion Is

Akrion focuses on a narrow but common engineering problem:

- you wrote an algorithm
- it works in theory
- you ran it on real hardware
- now you need structured evidence about whether it actually behaved well

Akrion is meant to provide that evidence path.

The project is centered on:

- structured serial recording from real devices
- reproducible experiment runs
- pluggable inputs, noise sources, and algorithms
- time-aligned comparison between expected and actual outputs
- waveform and metrics output for later review

## Why It Exists

Many developers can implement PID, Kalman, median filtering, ADRC, or LQR, but still cannot answer the practical question that matters:

"Under the same conditions, did this algorithm actually perform well on the target device?"

That gap usually comes from missing infrastructure rather than missing equations:

- no consistent recording format
- no clean separation between control period and serial reporting period
- no reusable experiment metadata
- no easy way to compare repeated runs
- no stable way to replay real data against later algorithm versions

Akrion exists to make that workflow explicit and repeatable.

## What Users Get From It

Akrion is intended to give users a few practical outcomes:

- record real input and output streams from serial devices in a structured format
- preserve the exact algorithm, parameter set, and run configuration used in an experiment
- support user-defined inputs, disturbances, and algorithm implementations
- replay recorded runs and compare later implementations against the same conditions
- generate waveforms and basic evaluation metrics without forcing a heavyweight modeling environment

## Typical Situations

Akrion fits workflows like:

- comparing two PID parameter sets on the same motor speed loop
- checking whether a Kalman filter still behaves well after firmware timing changes
- recording a real disturbance profile from hardware and replaying it later
- teaching beginners how control period, report period, and timestamp alignment affect conclusions
- preserving experiment history so repeated runs stay searchable and inspectable

## Core Workflow

A typical Akrion workflow is expected to look like this:

1. define or select an input profile, disturbance profile, and algorithm
2. run the algorithm on a device or in a replayable evaluation path
3. record structured serial frames and run metadata
4. save artifacts for later comparison
5. inspect waveforms and summary metrics
6. rerun with a different algorithm or parameter set under comparable conditions

## Design Philosophy

Akrion is guided by a few deliberate choices:

- real-device-first instead of simulation-first
- structured experiment recording instead of ad hoc screenshots and notes
- simple user-facing workflow with room for extension
- reusable data and metadata instead of one-off debug sessions
- narrow scope over platform sprawl

The project is intentionally not trying to replace MATLAB, Simulink, generic plotting tools, or full HIL infrastructure.

## Quick Start

```bash
git clone https://github.com/Zw-awa/Akrion.git
cd Akrion
```

Build both applications, verify the environment, and record a short deterministic demo:

```powershell
.\tools\qt.ps1 build
.\tools\qt.ps1 cli doctor --json
.\tools\qt.ps1 cli init akrion.config.json
.\tools\qt.ps1 cli demo --config akrion.config.json --duration 10
.\tools\qt.ps1 run
```

`akrion` is the console recorder and run-management tool. `akrion-gui` is the live waveform and replay application.

## Current Status

The current implementation provides a shared typed C++ core, a complete recording CLI, and a Qt/QML desktop waveform application.

The CLI includes:

- `doctor`, `ports`, `init`, `record`, and `demo`
- `validate`, `runs list`, `runs show`, and `replay`
- `export`, `pack`, and `unpack`
- machine-readable `--json` output and distinct exit codes

The GUI provides bounded live buffering, pixel-level min/max downsampling, device/host time axes, pause/follow modes, zoom, pan, cursor values, channel grouping, algorithm-disabled bands, event markers, and single-run replay.

### Build and Run

Install Qt 6.5 or newer with `Core`, `Gui`, `Qml`, `Quick`, `QuickControls2`, and `SerialPort`.

```powershell
.\tools\qt.ps1 build
.\tools\qt.ps1 test
.\tools\qt.ps1 smoke
.\tools\qt.ps1 run
.\tools\qt.ps1 cli runs list --json
```

The unified entry point resolves Qt from command-line arguments, environment variables, `.env`, then automatic discovery, and pins CMake, MinGW, and `MinGW Makefiles` from that installation. For local configuration, copy `.env.example` to `.env` and set `QT_ROOT` and `QT_VERSION`; `.env` is ignored by Git. Codex environments automatically use the agent-safe CMake branch, while normal terminals retain standard `qt_add_qml_module` builds.

### Serial Protocol

Send one JSON object per line. The required structure is described in [`schemas/serial-frame.schema.json`](./schemas/serial-frame.schema.json).

```json
{"schema":"akrion.frame/1","device_time_us":20000,"algo_tick":20,"emit_tick":1,"seq":1,"algo_id":1,"algo_enabled":true,"values":{"target":1.0,"actual":0.82,"control":0.31}}
```

`algo_tick` is the algorithm execution counter; `emit_tick` is the serial reporting counter. Keep them separate.

Each capture is streamed into a crash-recoverable run directory:

```text
<run-id>/
  manifest.json
  config.json
  serial.raw
  frames.ndjson
  events.ndjson
  summary.json
```

`serial.raw` preserves the exact received bytes. Completed runs can be exported as CSV/NDJSON or packed into a standard `.akrion` ZIP container.

Akrion reports factual transport, timing, channel, MAE, and RMSE results only when the channel mapping is explicit. It does not recommend algorithms or infer whether an algorithm is good.

## License

Akrion is licensed under the Apache License 2.0. See [LICENSE](./LICENSE).

