# AGENTS.md

Guidance for AI coding agents working in this repository.

## Overview

CalmDownGPU is a tiny Windows system-tray app (single static-CRT exe) that
regulates NVIDIA GPU temperature by **capping core clocks** (`nvidia-smi
-lgc`). The **entire application is `main.cpp`** (~983 lines) — there is no
other source. C++17, pure Win32, no third-party dependencies.

**Why clock-only (v1.2 architecture, user-driven decision):** the card
enforces its power limit *through* the clock/voltage curve anyway, and the
power-limit domain on driver 595.97 misreports — `power.min_limit/max_limit`
read N/A under load and `power.limit` readback ignores applied limits (always
returns the ~270W default), so a power-regulating app cannot observe its own
actuator. Clock caps apply instantly, are confirmable, and give continuous
authority down to 300MHz (no 100W `-pl` floor). Power is read once at startup
and restored on exit, but never regulated.

## Build & Test

```bash
cmake -B build -S . -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Output: `build/bin/Release/CalmDownGPU.exe`.

**Gotcha:** MSVC commands need a **Visual Studio Developer prompt** (or
`INCLUDE`/`LIB` env vars) — from a plain shell configure/link fails with
`LNK1104: cannot open file 'kernel32.lib'`. Environmental, not a project bug.
The build dir may be left in Ninja state by other sessions — if
`cmake --build` complains about missing `build.ninja`, `rm -rf build` and
reconfigure with the VS generator above.

- CMake 3.20+, C++17, `WIN32` exe, target `CalmDownGPU`.
- MSVC: `/W4 /utf-8 /permissive- /EHsc`, `/MT` static CRT by default
  (`CALMDOWN_STATIC_CRT`), `/MANIFEST:NO` (manifest embedded via resource.rc).
- `.exe` embeds VERSIONINFO + `requireAdministrator` elevation manifest.
- **No tests, no CI.** Verification is manual: deploy, user double-clicks,
  watch the log.

## Deployment (production machine)

- Deployed binary: `E:\Apps\CalmDownGPU.exe`. Deploy:
  `cmd.exe /c copy /Y <staged exe> "E:\Apps\CalmDownGPU.exe"`, verify with
  `certutil -hashfile ... SHA256` on both sides.
- **NEVER launch the exe from a sandbox shell.** It is
  `requireAdministrator`; shell-launched instances become unkillable
  "UAC-limb zombies" (no log, no tray, ~11MB, stuck). The user double-clicks
  in Explorer and approves UAC, or the `CalmDownGPU` scheduled task runs it.
- The running instance locks the exe — user must exit it (tray → Exit)
  before a deploy copy can succeed.
- Log: `E:\Apps\CalmDownGPU.log` (next to the exe, resolved via
  `GetModuleFileNameW`; `%TEMP%` fallback if the dir is read-only). The first
  line of each session prints the resolved path.

## Architecture

Two threads. The **UI thread** only does control math, logging, menu — it
never blocks on the outside world. A **worker thread** owns every blocking
call: nvidia-smi queries/commands and tray painting (`Shell_NotifyIcon` can
block on a wedged explorer). `runHidden()` is the only subprocess runner —
`CreateProcess` + `CREATE_NO_WINDOW`, non-blocking pipe drain, 15s watchdog
that `TerminateProcess`es hung children.

Control flow: `wWinMain` → hidden tool window + tray → `SetTimer` 10s →
`onTimerTick()` (UI): snapshot `g_shared` → apply confirmed `capDone` →
`requestTrayRefresh` → `regulateTick()`. The worker loop (~1/s): `queryTemp`
→ apply pending cap (`-lgc`/`-rgc`) → publish temp → tray refresh.
UI→worker commands: `enqueueCap()`; worker→UI results: `capDone`
(-1 restored, >0 applied MHz, <0 failed). `capMhz` in the Controller is
updated only from confirmed results.

**Regulation (`regulateTick`)**, 10s ticks, history of 6 samples, weighted
average (newer weighs more), effective = midpoint(avg, latest) when climbing
else avg, deadband ±1.5°C:
- ≥6°C over target: emergency dive −150MHz/tick (no gating)
- above target+deadband: −30MHz/tick (first engagement from uncapped: −150MHz)
- below target−deadband: after 6 consecutive cool ticks, +15MHz; fully
  recovered → `-rgc`
- inside deadband: hold; coolTicks resets (climb needs sustained cold)

Key plumbing:
- `runHidden()` — only way external commands run (`nvidia-smi`, `schtasks`).
- Target temp persisted in `HKCU\Software\CalmDownGPU\TargetTemp` (REG_DWORD).
- Logon autostart: scheduled task `CalmDownGPU` created/deleted via
  `schtasks` (`isStartupTaskEnabled`/`createStartupTask`/`deleteStartupTask`).
- Single instance via named mutex `CalmDownGPU_Mutex`.
- `IDM_EXIT` synchronously runs `-rgc` + restores the startup power limit.

## Conventions

- Tuning constants are `#define`s at the top of `main.cpp` (~line 100):
  `HISTORY_LEN`, `DEADBAND_C`, `CLOCK_STEP_DOWN(_FIRST)`, `CLOCK_STEP_UP`,
  `CLOCK_COOL_TICKS`, `CLOCK_EMERGENCY_STEP`, `CLOCK_MIN`, `CLOCK_LOW`.
- Menu presets are table-driven: `g_presets` + `IDM_*` drive both menu
  building and `WM_COMMAND` dispatch. Extend the table, not if-branches.
- Unicode split: UI is wide (`wWinMain`, `*W` APIs, `std::wstring`);
  `runHidden` is ANSI; convert at the boundary (`buildTipText`).
- `UNICODE`/`_UNICODE`/`WIN32_LEAN_AND_MEAN`/`NOMINMAX` defined both in
  CMake and guarded in main.cpp — keep both in sync.

## Gotchas

- **GPU index 0 hardcoded** (`-i 0`) in every nvidia-smi command.
- **The power domain lies on this system** (driver 595.97, RTX 3070):
  power range reads N/A under load; `power.limit` readback ignores applied
  limits (returns the default). Never reintroduce power-limit regulation —
  the app cannot verify its own `-pl` writes. `power.draw` is N/A too.
  `clocks.sm` under load DOES reflect binding caps — use it to verify.
- Cooler is the hardware bottleneck: even ~100W reaches ~85°C under full
  load (fan 100%). Only clock capping gets below that; don't diagnose the
  card's ceiling as a controller bug.
- **Manifest duplication hazard:** manifest embedded via `resource.rc`
  (`1 24 "app.manifest"`), linker auto-manifest suppressed with
  `/MANIFEST:NO`. Don't add another manifest source.
- The one-shot 2s startup timer (`ID_TIMER + 1`) is handled **inside the
  message loop**, not `WndProc`.
- `createTempIcon`'s alpha-fix loop has an if/else with **identical
  branches** — dead else, safe cleanup candidate.
- The context menu is rebuilt on every open, which synchronously runs
  `schtasks /query` on the UI thread (bounded by runHidden's watchdog;
  acceptable).
- `nvidia-smi` must be on PATH; missing GPU reads are non-fatal (temp −1 →
  grey `--` icon, regulation pauses). If `clocks.max.sm` is unavailable,
  clock control is disabled entirely (the app has no other lever).
- Everything interesting is logged to the exe-dir log via `log()`
  (mutex-protected, append per call).
