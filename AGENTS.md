# AGENTS.md

Guidance for AI coding agents working in this repository.

## Overview

CalmDownComputer (renamed from CalmDownGPU in v1.3.0; old names may linger in
the deployed machine: legacy exe/task/registry are migrated, see Deployment)
is a tiny Windows system-tray app (single static-CRT exe) that keeps the
machine cool. GPU: regulates NVIDIA temperature by **capping core clocks**
(`nvidia-smi -lgc`). CPU: caps the AC "Maximum processor state" at 95% when
idle (disables Ryzen Core Performance Boost) and restores 100% on sustained
load. The **entire application is `main.cpp`** (~1100 lines) — there is no
other source. C++17, pure Win32, no third-party dependencies. (main.cpp is
~1230 lines.)

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

Output: `build/bin/Release/CalmDownComputer.exe`.

**Gotcha:** MSVC commands need a **Visual Studio Developer prompt** (or
`INCLUDE`/`LIB` env vars) — from a plain shell configure/link fails with
`LNK1104: cannot open file 'kernel32.lib'`. Environmental, not a project bug.
The build dir may be left in Ninja state by other sessions — if
`cmake --build` complains about missing `build.ninja`, `rm -rf build` and
reconfigure with the VS generator above.

- CMake 3.20+, C++17, `WIN32` exe, target `CalmDownComputer`.
- MSVC: `/W4 /utf-8 /permissive- /EHsc`, `/MT` static CRT by default
  (`CALMDOWN_STATIC_CRT`), `/MANIFEST:NO` (manifest embedded via resource.rc).
- `.exe` embeds VERSIONINFO + `requireAdministrator` elevation manifest.
- **No tests, no CI.** Verification is manual: deploy, user double-clicks,
  watch the log.

## Deployment (production machine)

- Deployed binary: `E:\Apps\CalmDownComputer.exe` (v1.3.0+; the pre-rename
  `E:\Apps\CalmDownGPU.exe` still exists until the user exits the old
  instance and deletes it). Deploy:
  `cmd.exe /c copy /Y <staged exe> "E:\Apps\CalmDownComputer.exe"`, verify
  with `certutil -hashfile ... SHA256` on both sides.
- **NEVER launch the exe from a sandbox shell.** It is
  `requireAdministrator`; shell-launched instances become unkillable
  "UAC-limb zombies" (no log, no tray, ~11MB, stuck). The user double-clicks
  in Explorer and approves UAC, or the `CalmDownComputer` scheduled task runs
  it. The sandbox shell also cannot create/delete scheduled tasks (access
  denied, non-elevated) — task migration is done by the app itself.
- The running instance locks the exe — user must exit it (tray → Exit)
  before a deploy copy can succeed.
- Log: `E:\Apps\CalmDownComputer.log` (next to the exe, resolved via
  `GetModuleFileNameW`; `%TEMP%` fallback if the dir is read-only). The first
  line of each session prints the resolved path.
- Rename migration (v1.3.0): on first startup the new exe deletes the legacy
  `CalmDownGPU` logon task; registry values are carried over from
  `HKCU\Software\CalmDownGPU` when the new key is empty. The user re-enables
  "Start at logon" once to create the `CalmDownComputer` task.

## Architecture

Two threads. The **UI thread** only does control math, logging, menu — it
never blocks on the outside world. A **worker thread** owns every blocking
call: nvidia-smi/powercfg queries/commands and tray painting (`Shell_NotifyIcon` can
block on a wedged explorer). `runHidden()` is the only subprocess runner —
`CreateProcess` + `CREATE_NO_WINDOW`, non-blocking pipe drain, 15s watchdog
that `TerminateProcess`es hung children.

Control flow: `wWinMain` → hidden tool window + tray → `SetTimer` 10s →
`onTimerTick()` (UI): snapshot `g_shared` → apply confirmed `capDone` →
`requestTrayRefresh` → `regulateTick()`. The worker loop (~1/s): `queryTemp`
→ apply pending cap (`-lgc`/`-rgc`) → `cpuTick()` (CPU governor) → publish
temp → tray refresh. While the CPU governor is engaged, the tray blink
alternates GPU temp ↔ CPU state icons on fixed wall-clock phases
(`TRAY_BLINK_MS` 10s), posting only on phase changes — a state change shows
its colour immediately. CPU phases paint the frozen CPU load %.
UI→worker commands: `enqueueCap()`; worker→UI results: `capDone`
(-1 restored, >0 applied MHz, <0 failed). `capMhz` in the Controller is
updated only from confirmed results.

**CPU boost governor (`cpuTick`)**, ~1/s on the worker thread. Two sensors:
total CPU load via `GetSystemTimes`, and the FOREGROUND process's CPU time
via `GetForegroundWindow` + `GetProcessTimes` (as % of one core — the game
detector: desktop background alone idles at 13-20% total on this machine,
which is useless for discrimination; games pin a core). State machine with
double hysteresis — busy = (fg ≥`FG_ON_PCT` 25% OR total ≥`CPU_USAGE_ON_PCT`
25%) sustained `CPU_ON_MS` (10s) → apply 100%; quiet = fg ≤`FG_OFF_PCT` 10%
(foreground ONLY — total load is busy-side exclusive, it spikes with desktop
background) for `CPU_OFF_MS` (45s) → apply `CPU_CAP_PCT` (95%); between:
hold. Unknown fg (-1: no window, OpenProcess failed) never counts as quiet.
Blip tolerance: streak timers cancel each other only when a transition
actually FIRES — one noisy sample must never reset the 45s wait (that bug
kept the boost alive indefinitely after gaming). Actuation is `powercfg
/setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMAX N` +
`/setactive SCHEME_CURRENT` (two `runHidden` calls — runHidden cannot run
`&&`), then verified by parsing the LAST TWO `0x…` index lines of `powercfg
/query` (AC, then DC; locale-independent). Only undoes caps it applied
itself (`applied == CPU_CAP_PCT`); mode OFF/Exit restores 100%. DC (battery)
is never touched. Menu: `IDM_CPU_AUTO`/`IDM_CPU_OFF`, persisted as
`HKCU\Software\CalmDownComputer\CpuAuto`; worker reconciles against
`g_shared.cpuMode` each tick.

**Regulation (`regulateTick`)**, 10s ticks, history of 6 samples, weighted
average (newer weighs more), effective = midpoint(avg, latest) when climbing
else avg, deadband ±1.5°C, steps PROPORTIONAL to the error:
- over target+deadband: dive err×10MHz per step, clamped 15–150MHz (the
  emergency dive is just the clamp max — no separate rule)
- under target−deadband: after 4 consecutive cool ticks, climb err×5MHz
  clamped 15–30MHz; fully recovered → `-rgc`
- inside deadband: hold; coolTicks resets (climb needs sustained cold)
- after ANY cap change: settle 1 tick (2 for ≥90MHz steps) so the next
  decision uses post-effect temperatures
- Voltage-cliff learning: a climb step that raises temp ≥5°C (CLIFF_RISE_C)
  marks the pre-climb cap as `cliffMhz`; later climbs approach but never
  cross it (tooltip shows "cliff X MHz", reset on preset change). A target
  that falls inside a V/F gap is unreachable — parking at the cliff is the
  optimum (max clocks, coolest stable temp).

Key plumbing:
- `runHidden()` — only way external commands run (`nvidia-smi`, `powercfg`,
  `schtasks`).
- Target temp persisted in `HKCU\Software\CalmDownComputer\TargetTemp`
  (REG_DWORD); CPU governor in `...\CpuAuto`. Old `CalmDownGPU` key values
  are migrated on first run.
- Logon autostart: scheduled task `CalmDownComputer` created/deleted via
  `schtasks` (`isStartupTaskEnabled`/`createStartupTask`/`deleteStartupTask`).
- Single instance via named mutex `CalmDownComputer_Mutex`.
- `IDM_EXIT` synchronously runs `-rgc`, restores the startup power limit,
  and uncaps the CPU boost if the governor capped it.

## Conventions

- Tuning constants are `#define`s at the top of `main.cpp` (~line 100):
  `HISTORY_LEN`, `DEADBAND_C`, `CLOCK_KP`, `CLOCK_STEP_MIN/MAX`,
  `CLOCK_KP_UP`, `CLOCK_STEP_UP_MAX`, `CLOCK_COOL_TICKS`, `CLOCK_MIN`,
  `CLOCK_LOW`, and the CPU governor block `CPU_CAP_PCT`,
  `CPU_USAGE_ON_PCT/OFF_PCT`, `CPU_ON_MS/OFF_MS`.
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
- **Voltage cliff on this card:** ~1906MHz runs ~65°C while 1934MHz runs
  ~80°C under the same load — a V/F voltage step. The controller learns the
  edge and parks below it.
- **Manifest duplication hazard:** manifest embedded via `resource.rc`
  (`1 24 "app.manifest"`), linker auto-manifest suppressed with
  `/MANIFEST:NO`. Don't add another manifest source.
- The one-shot 2s startup timer (`ID_TIMER + 1`) is handled **inside the
  message loop**, not `WndProc`.
- **`runHidden` cannot run `&&`** — it is CreateProcess, not a shell. Multi-
  step commands (powercfg set + setactive) must be separate `runHidden`
  calls.
- **powercfg `/query` output leads with "Possible Settings" bounds**
  (`0x00000000`/`0x64`/`1`) BEFORE the index lines — parse the LAST two `0x`
  tokens (AC, then DC; DC prints even on desktops), never the first.
  Subprocess churn from powercfg/nvidia-smi also feeds GetSystemTimes load
  sampling (~+5%) — keep idle thresholds above the loop's own noise floor.
- **CPU governor signal choice matters**: total load idles at 13-20% on this
  machine (desktop background + agent harness), so idle detection uses the
  FOREGROUND process (games pin ≥25% of a core, office ≤10%) with total load
  as a secondary busy signal. Idle thresholds sit ABOVE the ambient noise
  floor so the cap can re-engage. If transitions look wrong in the log, the
  `cpu: X% total / Y% fg …` lines show both signals.
- `createTempIcon`'s alpha-fix loop has an if/else with **identical
  branches** — dead else, safe cleanup candidate.
- The context menu is rebuilt on every open, which synchronously runs
  `schtasks /query` on the UI thread (bounded by runHidden's watchdog;
  acceptable).
- `nvidia-smi` must be on PATH; missing GPU reads are non-fatal (temp −1 →
  grey `--` icon, regulation pauses). If `clocks.max.sm` is unavailable,
  clock control is disabled entirely (the app has no other lever).
- **No ACPI thermal zones on this board** (`MSAcpi_ThermalZoneTemperature`
  returns an empty list): real CPU temp is unreachable without a kernel
  driver (Ryzen SMN needs ring0). The CPU blink phases show CPU load % —
  don't reintroduce WMI temp queries here.
- Everything interesting is logged to the exe-dir log via `log()`
  (mutex-protected, append per call).
