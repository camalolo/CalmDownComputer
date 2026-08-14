# 🌡️ CalmDownGPU

A tiny Windows system-tray app that keeps your NVIDIA GPU temperature in check by
dynamically adjusting the power limit. No bloated overlays, no background services —
just a single 48 KB `.exe` that sits in your tray and does one job well.

![temperature targets](https://img.shields.io/badge/temp%20targets-60%E2%80%9390%E2%84%83%20%E2%88%9E-blue)
![platform](https://img.shields.io/badge/platform-Windows%2010%2F11-lightgrey)
![license](https://img.shields.io/badge/license-MIT-green)

## What it does

- **Live temperature in the tray icon** — a colour-coded number that shifts from
  green → yellow → orange → red as your GPU heats up. No clicking needed.
- **Click to set a target** — pick from presets (Off, 60–90 °C in 5° steps, or ∞ for
  max power). The app remembers your choice across restarts.
- **Smart power control** — adjusts the GPU power limit every 10 seconds using
  `nvidia-smi` to converge on your target temperature:
  - **Proportional steps** based on how far off target (5–30 W per adjustment)
  - **Emergency brake** that cuts power immediately on overshoot (≥2 °C over target)
  - **Cooldown** after each change to let thermals settle (prevents oscillation)
  - **Deadband** of ±1.5 °C — once stable, power stays put, no constant fiddling
- **Auto-detects** your GPU's power range and current settings on startup.

## How it works

```
nvidia-smi --query-gpu=temperature.gpu   →  read current temp
nvidia-smi -pl <watts>                    →  set power limit
```

The regulation loop runs every 10 seconds:

1. Read GPU temperature and add to a 60-second rolling window.
2. **Emergency brake**: if temp exceeds target by ≥2 °C, cut power immediately
   (`overshoot × 3 W`, min 5 W, max 30 W).
3. **Normal control**: if no brake fired, compute a weighted average (recent
   samples weigh more), and if it's outside the ±1.5 °C deadband, adjust power
   proportionally to the error.
4. After any change, a 20-second cooldown lets the GPU thermally settle before
   the next evaluation.

The result: it finds a stable power level and stays there, only reacting when the
thermal situation actually changes.

## Requirements

- **NVIDIA GPU** with `nvidia-smi` on the PATH (installed with NVIDIA drivers)
- **Windows 10 or 11**
- The app runs **as administrator** (required by `nvidia-smi -pl`). A UAC prompt
  appears on launch.

## Download

Grab the latest `CalmDownGPU.exe` from the
[Releases](../../releases) page. No installation — just run it.

## Building

```bash
cmake -B build -S .
cmake --build build --config Release
```

The executable will be at `build/Release/CalmDownGPU.exe`.

Requires a C++17 compiler and CMake 3.15+. Tested with MSVC 19.50.

## Logging

The app writes a log to `%TEMP%\CalmDownGPU.log` with timestamps for every
temperature reading, power adjustment, and brake event. Useful for tuning or
debugging.

## Notes

- Targets GPU index 0. Edit the `-i 0` arguments in `main.cpp` to target a
  different GPU.
- Power limit changes only cap the *maximum* draw — they can't force the GPU to
  use more power. When idle, temperature will be below target; regulation is
  effective under load.
- Single instance only — a mutex prevents multiple copies from running.

## License

[MIT](LICENSE)
