# AGENTS.md

Guidance for AI coding agents working in this repository.

## Overview

CalmDownComputer (renamed from CalmDownGPU in v1.3.0; old names may linger in
the deployed machine: legacy exe/task/registry are migrated, see Deployment)
is a tiny Windows system-tray app (single static-CRT exe) that keeps the
machine cool. GPU: regulates NVIDIA temperature by **capping core clocks**
(NVML `nvmlDeviceSetGpuLockedClocks`, the API behind `nvidia-smi -lgc`).
CPU: caps the AC "Maximum processor state" at 95% when
idle (disables Ryzen Core Performance Boost) and restores 100% on sustained
load. Display (opt-in, v1.4.0+): this box has VIRTUAL displays (streaming
driver; the GDI "primary" is one of them), so the governor targets a
USER-SELECTED display (auto-default: the largest current display). While
enabled it OWNS that display's resolution from settings: sustained heavy GPU
load drops it to the game resolution (e.g. 4K → 1440p), sustained quiet
restores the configured desktop resolution — so the resolution at startup is
irrelevant. Borderless games render at desktop resolution; a 4K desktop
drags them to 2160p. The **entire application is `main.cpp`** — there is no other source.
C++17, pure Win32, no third-party dependencies.

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

- Deployed binary: `D:\Apps\CalmDownComputer.exe` since v1.6.5 (was
  `E:\Apps\` before; user moved the Apps folder). Deploy:
  `cmd.exe /c copy /Y <staged exe> "D:\Apps\CalmDownComputer.exe"`, verify
  with `certutil -hashfile ... SHA256` on both sides.
- **NEVER launch the exe from a sandbox shell.** It is
  `requireAdministrator`; shell-launched instances become unkillable
  "UAC-limb zombies" (no log, no tray, ~11MB, stuck). The user double-clicks
  in Explorer and approves UAC, or the `CalmDownComputer` scheduled task runs
  it. The sandbox shell also cannot create/delete scheduled tasks (access
  denied, non-elevated) — task migration is done by the app itself.
- The running instance locks the exe — user must exit it (tray → Exit)
  before a deploy copy can succeed.
- Log: `D:\Apps\CalmDownComputer.log` (next to the exe, resolved via
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

Tray icon lifecycle is self-healing (v1.3.2+): `trayRefresh()` falls back
from `NIM_MODIFY` to `NIM_ADD`, the worker re-verifies presence every
`TRAY_ENSURE_MS` (60s), and `WndProc` re-adds on the `TaskbarCreated`
broadcast (registered in `wWinMain`). Never revert to a single unchecked
`NIM_ADD` at startup — the autostart task can fire before Explorer's taskbar
exists, and a lost add meant an invisible-but-running app (v1.3.0 bug).
Log lines carry `YYYY-MM-DD HH:MM:SS` timestamps (the log spans days; the
old time-only format made session boundaries ambiguous).

Control flow: `wWinMain` → hidden tool window + tray → `SetTimer` 10s →
`onTimerTick()` (UI): snapshot `g_shared` → apply confirmed `capDone` →
`requestTrayRefresh` → `regulateTick()`. The worker loop (EXACTLY 1 Hz —
`WORKER_TICK_MS`, paced by a `WaitForSingleObject` on the quit event, not
`Sleep(200)`, which with µs-fast NVML ticks would have spun at ~5/s):
`queryTempUtil` (temp + utilisation; NVML in-process first at ~0.1µs, ONE
nvidia-smi call as fallback — never split them back into two subprocesses,
the churn feeds load sampling) → apply pending cap (`nvmlDeviceSetGpuLocked
Clocks` / `nvmlDeviceResetGpuLockedClocks`, i.e. `-lgc`/`-rgc` without the
spawn; subprocess fallback) → `cpuTick()` (CPU governor) → `resTick()`
(resolution governor) → publish temp/util → tray refresh.
`queryCpuUsagePct` / `queryForegroundPct` are sampled ONCE per iteration in
the worker loop and passed into `cpuTick`/`resTick` — both keep delta state,
a second call per iteration would corrupt it. While the CPU governor is
engaged, the tray blink alternates GPU temp ↔ CPU state icons on fixed
wall-clock phases (`TRAY_BLINK_MS` 10s), posting only on phase changes — a
state change shows its colour immediately. CPU phases paint the frozen CPU
load %. UI→worker commands: `enqueueCap()`; worker→UI results: `capDone`
(-1 restored, >0 applied MHz, <0 failed). `capMhz` in the Controller is
updated only from confirmed results.

**NVML layer (v1.7.0, the nimble fix):** the hot loop reads temp/util and
applies clock caps through NVML — the driver-shipped `nvml.dll` (System32)
that nvidia-smi itself wraps — loaded dynamically in `main.cpp` (minimal
stable-C-ABI declarations, 7 function pointers; no SDK header). Measured on
this machine: an nvidia-smi spawn costs ~6ms of CPU charged to THIS process
per call (CreateProcess + image load + NVML re-init in the child) and the
old loop ran 2-4 spawns/s → the constant ~2%-of-a-core burn (2,373 CPU-s).
In-process NVML: ~0.1µs per temp+util pair. `nvmlEnsure()` gates every call;
load failures back off 60s (driver service may be starting at logon), a
mid-call failure (`nvmlFail`) drops to the subprocess path and retries NVML
after 5s (driver reload/hibernate self-heal). Failure logs are throttled to
one line per 10min. Startup discovery (name, maxClock, power limit) and
powercfg/schtasks stay on `runHidden` deliberately (3 spawns/session, exact
`clocks.max.sm` semantics). Log lines carry an "(nvml)" tag when the direct
path fired.

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

**Resolution governor (`resTick`)**, ~1/s on the worker thread, opt-in
(default OFF — a sudden display switch is surprising). This box has VIRTUAL
displays (streaming driver; the GDI "primary" is one), so it targets a
USER-SELECTED display: `ResDisp` (REG_SZ = the monitor's CCD friendly name,
e.g. `Acer ET322QK` — the panel's EDID product name, picked from a live
`enumDisplays()` menu snapshot; empty/absent = auto = largest current display
— the real 4K monitor, never the virtual ones). Pin format history: GDI names
(v1.4) → EDID device identities (v1.7.2) → **CCD friendly names (v1.7.5,
current)**. Identity comes from `QueryDisplayConfig`/`DisplayConfigGetDeviceInfo`
(dxgkrnl's own topology — the same source NVIDIA Control Panel reads), because
the GDI monitor sub-device proved untrustworthy on this machine (see Gotchas).
Matching (v1.7.5+): CCD friendly name first, then EDID device ID, then the
`MONITOR\<pnp>` prefix, then legacy GDI names — old pins still resolve except
`MONITOR\...`-format ones, which are retired to auto at startup (they read the
poisoned layer). Menu labels are user-friendly ("Acer ET322QK · 3840x2160 ·
primary"). OWNERSHIP model — while enabled,
BOTH ends come from settings, making the startup resolution irrelevant:
- **Source-gated engage**: `utilization.gpu` ≥`ResOnPct` (menu-configurable
  "Engage at GPU load" — 25/50/66/75/90%, default 66%, persisted
  `ResOnPct`) sustained `RES_ON_MS` 10s is only the CANDIDATE condition. The
  load must then be
  attributed to an application holding a borderless fullscreen window on
  the target display (v1.5.0, user-driven):
  `queryGfxUtilByPid` reads the per-PID graphics utilisation from the
  PDH "GPU Engine" counters (same data as Task Manager's GPU column;
  engtype_3D **plus engtype_Graphics_N** — DX12 multi-node titles like RE
  Engine report Graphics_1 and are invisible to a 3D-only filter, v1.5.2
  fix; CUDA inference shows under Compute, not graphics), sorts consumers
  desc and picks the FIRST holding a borderless fullscreen window on the
  target display — NOT strictly the top consumer (observed live: background
  chrome 89% out-ranked the fullscreen game 78%; the window decides, not
  the ranking). Candidacy floor: `min(GFX_SRC_MIN_PCT 15%, onPct/2)` per
   PID. `pidHasBorderlessFullscreenOn` (v1.6.5) requires the window to be
   **the foreground root** (the one the user is looking at — a fullscreen
   game that keeps rendering BEHIND the browser must not trigger a drop
   while the user works; observed live), top-level, visible, undecorated
   (no WS_CAPTION/WS_THICKFRAME), covering ≥99% of the target monitor
   (99%, not 95%: a maximised window with visible taskbar covers ~97-98%
   at 2160p). The engage log names the qualifying window class + rect.
   No graphics consumer (AI inference!) or no qualifying window
   (background compute/render, windowed tools, or the fullscreen app not
   being the foreground) → NO drop, logged once per
   load period (`srcLogged`, top-3 loaded pids included in the refusal
   line). Engage guard: target already at/below the game
   res → no-op (`naLogged`) — TRUE-exclusive fullscreen changes the mode
   itself and lands here, staying self-managed.
- **Release = source process exit, not quiet** (v1.5.0): at engage a
  `SYNCHRONIZE` process HANDLE is stored; the ONLY restore trigger is
  `WaitForSingleObject(handle, 0)` firing (pid-reuse-proof, immune to game
  pauses / alt-tab / menus / fps dips — that fixed the 2K↔4K flip-flop).
  Do NOT reintroduce quiet-based restore while dropped.
- Quiet-normalisation: while NOT dropped, all-quiet sustained `RES_OFF_MS`
  30s (GPU util ≤`RES_OFF_PCT` 40% AND fg ≤ `FG_OFF_PCT` AND total CPU
  <`CPU_USAGE_ON_PCT` — the CPU veto prevents flip-flop with an
  alt-tabbed rendering game) enforces the DESKTOP resolution
  (`g_defResPresets` → `ResDefaultW/H`, learned on first run from the target
  display's registry-default mode) — so a manual/startup odd resolution
  survives at most one quiet window.
Actuation has THREE tiers in `applyDisplayMode` (v1.6.0): first a **CCD
source-mode switch** (`ccdSetSourceMode` → `SetDisplayConfig` with
`Apply|UseSuppliedDisplayConfig|NoOptimization`, retry with
`+AllowChanges` — the exact technique of Monitor Profile Switcher, whose
source was studied): changes the desktop (SOURCE) size while leaving the
panel's TARGET timing untouched → GPU scaling on the native signal →
**no monitor re-sync, no black flash** (CDS always re-times and blanks
~2s — measured). Deliberately NO `SDC_SAVE_TO_DATABASE` — stays temporary,
logoff/reboot reverts. Tier 2: CDS with an exact native mode (see
findNativeMode); Tier 3: CDS relaxed PELS-only request. The app logs which
tier fired (`CCD switch … OK` vs `native mode … (CDS fallback)`).
`restoreToDefault` falls back to the display's registry default (NULL
devmode revert) when the configured desktop res is unset/rejected.
DISP_CHANGE_BADMODE marks the offending preset bad until that setting
changes (`badFallback`/`badDefault`); a PINNED display that vanished pauses
the governor (logged once, `missingLogged`) — it never silently switches to
another display. Toggling off / Exit (`restoreResolutionNow`, UI thread) /
worker back-door shutdown all restore the configured desktop resolution and
close the source handle (`endSourceTracking`).
**Wake-default alignment + drift guard (v1.6.1)**: display sleep/wake
re-arrives the monitor and Windows re-applies its DATABASE default mode —
a stale one (an old manual 1440p setting) made every wake come back at 2K
with resized windows. `ensureDatabaseDefault` aligns that saved default to
the configured desktop resolution via `CDS_UPDATEREGISTRY|CDS_NORESET`
(save-without-apply; throttled 5 min, only when not engaged, only exact
native modes). While ENGAGED, a drift guard re-checks the target display's
current mode every `RES_ON_MS` and re-applies the game resolution (CCD,
instant) if a wake reset it mid-game. Menu: `IDM_RES_AUTO` + three
submenus (game res, desktop res, target display from the live
`g_menuDisplays` snapshot; `IDM_RES_DISP_BASE`+i dispatch indexes that same
snapshot). Worker reconciles against `g_shared.resAuto/resTargetW/resTargetH/
resDefaultW/resDefaultH/resDisp` each tick.

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
  (REG_DWORD); CPU governor in `...\CpuAuto`; resolution governor in
  `...\ResAuto` + `...\ResAutoW/H` + `...\ResDefaultW/H` + `...\ResDisp`
  (REG_SZ). Old `CalmDownGPU` key values are migrated on first run.
- Logon autostart: scheduled task `CalmDownComputer` created/deleted via
  `schtasks` (`isStartupTaskEnabled`/`createStartupTask`/`deleteStartupTask`).
- Single instance via named mutex `CalmDownComputer_Mutex`.
- `IDM_EXIT` synchronously runs `-rgc`, restores the startup power limit,
  uncaps the CPU boost if the governor capped it, and restores the dropped
  desktop resolution.

## Conventions

- Tuning constants are `#define`s at the top of `main.cpp` (~line 100):
  `HISTORY_LEN`, `DEADBAND_C`, `CLOCK_KP`, `CLOCK_STEP_MIN/MAX`,
  `CLOCK_KP_UP`, `CLOCK_STEP_UP_MAX`, `CLOCK_COOL_TICKS`, `CLOCK_MIN`,
  `CLOCK_LOW`, `WORKER_TICK_MS`, the CPU governor block `CPU_CAP_PCT`,
  `CPU_USAGE_ON_PCT/OFF_PCT`, `CPU_ON_MS/OFF_MS`, and the resolution
  governor block `RES_ON_PCT/OFF_PCT`, `RES_ON_MS/OFF_MS`.
- Menu presets are table-driven: `g_presets` (temp targets), `g_resPresets`
  (game resolutions), `g_defResPresets` (desktop resolutions),
  `g_pctPresets` (engage thresholds) + `IDM_*` drive both menu building and
  `WM_COMMAND` dispatch. Extend the table, not if-branches. The
  target-display submenu is built from the live `enumDisplays()` snapshot
  (`g_menuDisplays`) instead of a static table.
- Unicode split: UI is wide (`wWinMain`, `*W` APIs, `std::wstring`);
  `runHidden` is ANSI; convert at the boundary (`buildTipText`).
- `UNICODE`/`_UNICODE`/`WIN32_LEAN_AND_MEAN`/`NOMINMAX` defined both in
  CMake and guarded in main.cpp — keep both in sync.

## Gotchas

- **GPU index 0 hardcoded** (`-i 0` in every nvidia-smi command; NVML device
  handle for index 0).
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
- **The GDI monitor sub-device lies on this machine (v1.7.5 lesson)**: a
  stale monitor devnode (SNY7105, "SONY TV") survived driver restarts,
  PnP uninstalls, and reboots, and `EnumDisplayDevicesW`'s monitor entry
  kept serving its cached EDID/model name for the desktop's display —
  while the CCD topology (`QueryDisplayConfig`, what NVIDIA Control Panel
  reads) always reported the live truth (Acer ET322QK on DisplayPort).
  Two full days were spent chasing EDIDs before a `ccdump`-style probe
  settled it: the layers disagree, CCD wins. NEVER reintroduce GDI
  monitor data as an identity/name source; `enumDisplays()` takes names
  from `DisplayConfigGetDeviceInfo` (fallback: the old naming). Also:
  `QueryDisplayConfig`/GDI are inaccessible from sandbox-spawned shells
  (window-station isolation) — build a small exe and have the user run
  it when a ground-truth dump is needed.
- **powercfg `/query` output leads with "Possible Settings" bounds**
  (`0x00000000`/`0x64`/`1`) BEFORE the index lines — parse the LAST two `0x`
  tokens (AC, then DC; DC prints even on desktops), never the first.
  Subprocess churn (powercfg/nvidia-smi) also feeds GetSystemTimes load
  sampling (~+5%) — keep idle thresholds above the noise floor; the NVML
  path produces no churn at all, so this mostly matters in fallback mode.
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
- `nvidia-smi` must be on PATH for the fallback path + startup discovery;
  the NVML hot path needs `nvml.dll` (System32 on driver installs, with a
  legacy NVSMI path fallback). Both missing: reads fail non-fatally
  (temp −1 → grey `--` icon, regulation pauses). If `clocks.max.sm` is
  unavailable, clock control is disabled entirely (the app has no other
  lever).
- **No ACPI thermal zones on this board** (`MSAcpi_ThermalZoneTemperature`
  returns an empty list): real CPU temp is unreachable without a kernel
  driver (Ryzen SMN needs ring0). The CPU blink phases show CPU load % —
  don't reintroduce WMI temp queries here.
- Everything interesting is logged to the exe-dir log via `log()`
  (mutex-protected, append per call).
