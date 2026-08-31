# 🌡️ CalmDownComputer

A tiny Windows system-tray app that keeps your machine cool. It regulates the
NVIDIA GPU by capping core clocks, and tames the CPU by switching Ryzen's
Core Performance Boost off when the machine is idle. No bloated overlays, no
background services — just a single ~240 KB static `.exe` in the tray.

![platform](https://img.shields.io/badge/platform-Windows%2010%2F11-lightgrey)
![license](https://img.shields.io/badge/license-MIT-green)

## What it does

- **Live GPU temperature in the tray icon** — a colour-coded number that shifts
  from green → yellow → orange → red as the GPU heats up. While the CPU
  governor is engaged, the icon slowly alternates: **blue** = 95% capped
  (office), **purple** = 100% boost — each CPU phase carrying the CPU load %.
  (Real CPU temp needs a kernel driver; consumer boards expose no ACPI sensor
  to user mode, so load is shown instead.) Icons are pre-rendered and posted
  only on phase changes — effectively zero cost.
- **GPU clock regulation** — click the tray to pick a target (Off, 60–90 °C,
  ∞). The app steers the GPU core-clock cap (`nvidia-smi -lgc`) proportionally:
  fast dive when hot, reluctant climb when cool, ±1.5 °C deadband, and it
  learns the card's voltage cliff (the clock above which temperature explodes)
  and parks just below it.
- **CPU boost governor** — Ryzen chips (e.g. 5600X) disable Core Performance
  Boost whenever the Windows AC "Maximum processor state" is below 100%:
  the CPU pins at 3.7 GHz base instead of boosting to 4.65 GHz, which is worth
  ~10–15 °C under office loads. The governor:
  - detects games by the **foreground process** (≥25% of one core sustained —
    office apps rarely exceed 10%) plus total load ≥25% as a secondary signal
  - caps the AC max processor state at **95%** after ~45 s of quiet
  - restores **100%** after only ~10 s of sustained heavy load — games get
    their boost back within seconds of launching, office stays cool
  - hysteresis on both level and time, so it never flaps
  - verify every write by reading the value back from `powercfg`
  - DC (battery) is never touched; a manual 95% you set yourself is left
    alone unless the governor applied one
- Power limits are never regulated: read once at startup, restored on exit.

## Why these levers?

- **GPU: clocks, not power.** The card enforces its power limit *through* its
  clock/voltage curve anyway — and on some driver versions the power-limit
  domain misreports (N/A ranges, readback ignoring applied limits). Clock caps
  apply instantly, can be verified, and keep working down to a few hundred MHz.
- **CPU: processor state, not PBO.** The 95% trick is the one lever that's
  settable and revertible from user space without a BIOS visit — and the
  temperature win under light load is large because boost voltage disappears.

## How it works

```
nvidia-smi --query-gpu=temperature.gpu   →  read GPU temp
nvidia-smi -lgc 210,<cap>                →  cap GPU core clocks
nvidia-smi -rgc                          →  restore default clocks

GetSystemTimes()                          →  total CPU load (1 s samples)
powercfg /setacvalueindex … PROCTHROTTLEMAX 95
powercfg /setactive SCHEME_CURRENT        →  CPU boost cap on/off
powercfg /query … PROCTHROTTLEMAX         →  verify the write
```

GPU regulation runs on 10-second ticks (weighted average, proportional steps,
settle windows). The CPU governor samples once a second on the worker thread.
All external commands and tray painting run on a worker thread, so a hung
driver call can never freeze the app (a 15-second watchdog kills stalled
subprocesses).

## Requirements

- **NVIDIA GPU** with `nvidia-smi` on the PATH (installed with NVIDIA drivers)
- **Windows 10 or 11**
- The app runs **as administrator** (required for clock locking). A UAC prompt
  appears on launch.

## Download

Grab the latest `CalmDownComputer.exe` from the
[Releases](../../releases) page. No installation — just run it.

## Building

```bash
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable will be at `build/bin/Release/CalmDownComputer.exe`.

Requires C++17, CMake 3.20+, and MSVC (or MinGW-w64; untested). Builds with a
statically linked CRT by default — no redistributable needed.

## Logging

The app writes a log **next to the executable** (`CalmDownComputer.log`) with
timestamps for every temperature reading, clock change, CPU boost transition,
and settle event. Useful for tuning or debugging.

## Notes

- Targets GPU index 0. Edit the `-i 0` arguments in `main.cpp` for a
  different GPU.
- CPU governor thresholds are `#define`s at the top of `main.cpp`
  (`CPU_CAP_PCT`, `CPU_USAGE_ON_PCT`, `CPU_USAGE_OFF_PCT`, `CPU_ON_MS`,
  `CPU_OFF_MS`).
- On startup the app resets any leftover GPU clock locks and removes the
  legacy `CalmDownGPU` logon task (pre-rename installations).
- Settings persist in `HKCU\Software\CalmDownComputer` (the old
  `CalmDownGPU` key is migrated automatically).
- Single instance only — a mutex prevents multiple copies from running.
- Exit restores everything: GPU clocks, power limit, and the CPU boost cap.

## License

[MIT](LICENSE)
