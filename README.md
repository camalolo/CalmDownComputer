# 🌡️ CalmDownGPU

A tiny Windows system-tray app that keeps your NVIDIA GPU temperature in check
by capping its core clocks. No bloated overlays, no background services — just
a single ~240 KB static `.exe` that sits in your tray and does one job well.

![temperature targets](https://img.shields.io/badge/temp%20targets-60%E2%80%9390%E2%84%83%20%E2%88%9E-blue)
![platform](https://img.shields.io/badge/platform-Windows%2010%2F11-lightgrey)
![license](https://img.shields.io/badge/license-MIT-green)

## What it does

- **Live temperature in the tray icon** — a colour-coded number that shifts from
  green → yellow → orange → red as your GPU heats up. No clicking needed.
- **Click to set a target** — pick from presets (Off, 60–90 °C in 5° steps, or ∞ for
  full clocks). The app remembers your choice across restarts.
- **Clock-only control** — regulates by locking GPU core clocks with
  `nvidia-smi -lgc`, on 10-second ticks:
  - **Fast dive** when hot: −30 MHz per tick (first response −150 MHz)
  - **Emergency dive** when ≥6 °C over target: −150 MHz per tick
  - **Reluctant climb** when cool: +15 MHz per minute, only after 60 s of
    sustained below-target temperature
  - **Settle window**: 20 s of hold after each change, so every decision uses
    post-effect temperatures, not stale heat
  - **Deadband** of ±1.5 °C — once stable, nothing moves
- Power limit is never touched: it is read once at startup and restored on exit.

## Why clocks, not power?

The card enforces its power limit *through* its clock/voltage curve anyway —
and on some driver versions the power-limit domain misreports: ranges read
N/A under load, and the `power.limit` readback ignores applied limits.
Clock caps apply instantly, can be verified, and keep working all the way
down to a few hundred MHz — there is no "100 W floor" to get stuck at.

## How it works

```
nvidia-smi --query-gpu=temperature.gpu   →  read current temp
nvidia-smi -lgc 210,<cap>                →  cap core clocks
nvidia-smi -rgc                          →  restore default clocks
```

The regulation loop runs every 10 seconds:

1. Read the GPU temperature into a 60-second rolling window (recent samples
   weigh more).
2. Over target? Dive: −30 MHz (first engagement −150 MHz); ≥6 °C over →
   emergency −150 MHz per tick.
3. After any change, hold for 20 s — clock caps act in milliseconds but the
   heat takes ~20 s to show, so acting sooner only reacts to stale data.
4. Under target for 60 s straight? Climb +15 MHz. Back at the ceiling? Unlock
   clocks (`-rgc`).

The result: a fast, calm descent to your target and a reluctant climb back —
it finds the clock that holds your temperature and quietly stays there.

All `nvidia-smi` calls and tray painting run on a worker thread, so a hung
driver call can never freeze the app (a 15-second watchdog kills stalled
subprocesses).

## Requirements

- **NVIDIA GPU** with `nvidia-smi` on the PATH (installed with NVIDIA drivers)
- **Windows 10 or 11**
- The app runs **as administrator** (required for clock locking). A UAC prompt
  appears on launch.

## Download

Grab the latest `CalmDownGPU.exe` from the
[Releases](../../releases) page. No installation — just run it.

## Building

```bash
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable will be at `build/bin/Release/CalmDownGPU.exe`.

Requires C++17, CMake 3.20+, and MSVC (or MinGW-w64; untested). Builds with a
statically linked CRT by default — no redistributable needed.

## Logging

The app writes a log **next to the executable** (`CalmDownGPU.log`) with
timestamps for every temperature reading, clock change, and settle event.
Useful for tuning or debugging.

## Notes

- Targets GPU index 0. Edit the `-i 0` arguments in `main.cpp` for a
  different GPU.
- A clock cap only limits the *maximum* boost — at idle the GPU runs cool and
  the app keeps its hands off. Regulation matters under load.
- On startup the app resets any leftover clock locks (including manual ones),
  so it always starts from a known state.
- Single instance only — a mutex prevents multiple copies from running.

## License

[MIT](LICENSE)
