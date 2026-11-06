// CalmDownComputer - Windows tray app that keeps the whole machine cool.
// GPU: regulates temperature by clock capping (nvidia-smi -lgc).
// CPU: caps the AC "Maximum processor state" at 95% while idle — on Ryzen,
//      anything below 100% disables Core Performance Boost (5600X: pinned at
//      3.7GHz base instead of 4.65GHz boost, much cooler under office load) —
//      and restores 100% as soon as a sustained load (a game) shows up.
// Display: on sustained heavy GPU load, temporarily drops the SELECTED
//      display to a game resolution (e.g. 4K → 1440p); on sustained quiet
//      restores the configured DESKTOP resolution. Borderless games render
//      at desktop resolution, so a 4K desktop drags every borderless game
//      to 2160p. Both ends are settings-driven, so the resolution the
//      machine happens to be in at startup is irrelevant. Virtual displays
//      (this box has them; the GDI "primary" is one) are never touched.
// Build: see CMakeLists.txt
//
// Key design decisions:
// - Clock-only control: the card enforces power THROUGH its clock/voltage
//   curve anyway, and the power-limit domain on driver 595.97 misreports
//   (N/A ranges, readback that ignores applied limits). Commanding clocks
//   directly is the same lever without the broken middleman, and the control
//   loop closes on temperature — the only sensor that never lies.
// - Two threads: the UI thread only runs control math, logging and the menu;
//   ALL blocking external work (nvidia-smi calls, tray painting) lives on a
//   worker thread, so a stalled call can never freeze the app.
// - Hidden commands: CreateProcess with CREATE_NO_WINDOW; runHidden is
//   bounded (non-blocking drain + 15s watchdog kill).
// - In-process GPU access: sensing (temp/util) and the clock-cap actuator
//   go through NVML — the driver-shipped nvml.dll that nvidia-smi itself
//   wraps — loaded dynamically, at ~0.1µs per reading. Spawning nvidia-smi
//   per tick was measured at ~6ms of CPU charged to this process per call
//   (CreateProcess + image load + full NVML re-init in the child) at 2-4
//   ticks/s: the entire historical background burn. The subprocess path
//   remains as the automatic fallback (and does the 3 one-time startup
//   discoveries), so a missing nvml.dll degrades to the old behavior.
// - Control: 10s ticks, weighted average, ±1.5°C deadband.
//   • Above target: proportional dive — 10MHz per °C over, 15–150MHz steps
//   • Below target: climb 15–30MHz after ~40s of sustained cool
//   • 10–20s settle window after each change (decisions use post-effect temps)
//   The power limit is left alone (read once at startup, restored on exit).
// - CPU governor: sensing is GetSystemTimes + GetProcessTimes of the
//   foreground process (cheap syscalls); powercfg runs on the worker thread
//   like every other external command, and every write is verified by
//   reading the index back.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Win10 baseline: GPU Engine counters need it
#endif

#include <windows.h>
#include <shellapi.h>
#include <pdh.h>       // per-process GPU utilisation (GPU Engine counters)

// Not declared by every SDK's pdh.h (the documented Win32 value).
#ifndef PDH_MORE_DATA
#define PDH_MORE_DATA ((PDH_STATUS)0x800007D2L)
#endif

#include <string>
#include <deque>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstdarg>
#include <mutex>
#include <map>

// ───────────────────────── Logging ─────────────────────────

static char g_logPath[MAX_PATH] = {};
static std::mutex g_logMutex;   // log() is called from the UI and worker threads

static void log(const char* fmt, ...) {
    if (!g_logPath[0]) return;
    std::lock_guard<std::mutex> lk(g_logMutex);
    FILE* f = nullptr;
    fopen_s(&f, g_logPath, "a");
    if (!f) return;
    time_t now = time(nullptr);
    struct tm tmv{};
    localtime_s(&tmv, &now);
    // Date included: the log accumulates sessions across days.
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static void logOpen() {
    // Log next to the executable — a fixed, predictable location that does
    // NOT depend on the launching shell's %TEMP% (which varies by how/where
    // the app is started). Falls back to %TEMP% if the exe dir is read-only.
    wchar_t modW[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, modW, MAX_PATH);
    std::wstring dirW(modW);
    auto slash = dirW.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dirW.erase(slash + 1);
    dirW += L"CalmDownComputer.log";
    WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), -1, g_logPath, MAX_PATH, nullptr, nullptr);

    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") != 0 || !f) {
        GetTempPathA(MAX_PATH, g_logPath);
        strcat_s(g_logPath, "CalmDownComputer.log");
    } else {
        fclose(f);
    }
    // Write a session separator.
    f = nullptr;
    if (fopen_s(&f, g_logPath, "a") == 0 && f) {
        fprintf(f, "\n══════════════════════════════════════════\n");
        fclose(f);
    }
    log("CalmDownComputer starting → log: %s", g_logPath);
}

// ───────────────────────── Constants ─────────────────────────

#define WM_TRAYICON     (WM_USER + 1)
#define ID_TIMER         1
#define TIMER_MS         10000       // 10 seconds
#define TRAY_BLINK_MS    10000       // CPU-state blink phase (~1 flip per 10s)
#define TRAY_ENSURE_MS   60000       // periodic tray-icon presence check
#define WORKER_TICK_MS   1000        // worker loop period — exact pacing (1 Hz)
#define HISTORY_LEN      6           // rolling window: 6 × 10s = 60s
#define DEADBAND_C       1.5         // ±1.5°C: don't adjust within this band

// Clock-only control constants. The card enforces power THROUGH clocks
// anyway; clock caps apply instantly, verify against clocks.sm under load,
// and give continuous authority all the way down (no 100W floor).
// Steps are PROPORTIONAL to the error: fine adjustments near the target
// (15MHz ≈ the ±1.5°C deadband in thermal effect), hard dives when far.
// There is no separate "emergency" — it is just the clamp maximum.
#define CLOCK_KP          10    // MHz of dive per °C over target
#define CLOCK_STEP_MIN    15    // smallest adjustment (matches the deadband)
#define CLOCK_STEP_MAX    150   // single-step dive limit (= former emergency)
#define CLOCK_KP_UP        5    // MHz of climb per °C under target (half as eager)
#define CLOCK_STEP_UP_MAX 30    // single-step climb limit
#define CLOCK_COOL_TICKS   4    // consecutive cool ticks before a climb (~40s)
#define CLIFF_RISE_C       5    // temp rise from one climb step = voltage cliff
#define CLOCK_MIN        300    // never cap below this (usability floor)
#define CLOCK_LOW        210    // low end of the -lgc range (idle clock, MHz)

// CPU boost governor. On Ryzen, AC "Maximum processor state" < 100% disables
// Core Performance Boost: the CPU sits at base clock — much cooler under
// office loads, but games lose the boost. So: cap while quiet, lift the cap
// when real work shows up. Two signals, with asymmetric roles:
//   • foreground process time (GetProcessTimes) — a game pins ≥25% of one
//     core; office apps rarely sustain >10%. Drives BOTH directions: heavy
//     foreground boosts, quiet foreground re-caps.
//   • total load (GetSystemTimes) — BUSY-side only: catches background
//     compiles and alt-tabbed games. Too noisy (13-25% ambient on a desktop)
//     to gate the cap, so it never vetoes one.
// Blip tolerance: streak timers only cancel each other when one ACTUALLY
// fires (boost after 10s sustained busy, cap after 45s sustained quiet) —
// single noisy samples never reset the 45s wait.
#define CPU_CAP_PCT       95    // AC max processor state while capped
#define CPU_USAGE_ON_PCT  25    // total CPU load above this counts as busy
#define FG_ON_PCT         25    // foreground proc above this % of one core = busy
#define FG_OFF_PCT        10    // foreground proc below this % of one core = quiet
#define CPU_ON_MS         10000 // busy must persist this long before boost
#define CPU_OFF_MS        45000 // quiet must persist this long before the cap

// Resolution governor. It OWNS the selected display's resolution while
// enabled: a sustained heavy GPU load that can be SOURCED to an application
// holding a borderless fullscreen window on that display (a game) → drop to
// the game (fallback) resolution; the desktop resolution comes back only
// when that application EXITS. Both ends are settings-driven, so the
// resolution the machine was in at startup is irrelevant.
//
// Why sourcing matters (user-driven decision):
//   • AI inference saturates the GPU for hours with no game running — heavy
//     but unsourced (compute engine, no fullscreen window) → never drops.
//   • Games that auto-pause when they lose the foreground (or just sit in
//     slow menus) dip their utilisation — if quiet dipped the resolution
//     back, you'd flip-flop 2K↔4K mid-session. Holding on the process
//     handle kills that: NOTHING restores until the source process exits.
//
// Sensing: total load from nvidia-smi utilisation (piggybacks the ONE
// per-iteration call), attribution from the GPU Engine performance counters
// (per-PID engtype_3D — the same data Task Manager shows), window check via
// EnumWindows (visible, undecorated root window covering ≥95% of the target
// display). A TRUE-exclusive fullscreen game changes the display mode
// itself, so the engage guard (current ≤ game res → "nothing to drop")
// leaves those alone by construction.
// Quiet-normalisation of the desktop resolution (feature on, nothing
// dropped, three-signal all-quiet — see RES_OFF_MS below) is unchanged: it
// makes a manual/startup odd resolution converge to the configured one.
#define RES_ON_PCT        66    // DEFAULT engage threshold — configurable via
                                // menu ("Engage at GPU load"), persisted ResOnPct
#define RES_OFF_PCT       40    // GPU util ≤ this = light load
#define RES_ON_MS       10000   // heavy load persists this long before the drop
#define RES_OFF_MS      10000   // GPU-quiet persists this long before enforcing
                                // the desktop resolution (NOT used while
                                // dropped — the source process owns release).
                                // Only GPU noise resets this window; foreground
                                // reading spikes don't (v1.8.1). At expiry the
                                // fire still requires a non-gaming foreground
                                // (RES_OFF_FG_PCT) so windowed games and
                                // deliberate low-res users don't get fought.
#define RES_OFF_FG_PCT    30    // foreground % allowed at the moment the
                                // normalization fires (reading ≈ 0-30, a
                                // windowed game ≈ 50-100)
#define GFX_SRC_MIN_PCT   15    // min per-PID graphics util to source a drop
                                // (floored at onPct/2 so low thresholds still work)

#define IDI_APP          100

// Menu command IDs
enum : UINT {
    IDM_OFF     = 2000,
    IDM_T60     = 2001,
    IDM_T65     = 2002,
    IDM_T70     = 2003,
    IDM_T75     = 2004,
    IDM_T80     = 2005,
    IDM_T85     = 2006,
    IDM_T90     = 2007,
    IDM_INF     = 2008,   // ∞ — max power
    IDM_STARTUP   = 2500,   // Start at logon toggle
    IDM_CPU_AUTO  = 2510,   // CPU governor: auto (cap when idle)
    IDM_CPU_OFF   = 2511,   // CPU governor: off (never touch it)
    IDM_RES_AUTO  = 2520,   // resolution governor: drop on heavy GPU load
    IDM_RES_1440P = 2521,   // game (fallback) resolution 2560x1440
    IDM_RES_1080P = 2522,   // game (fallback) resolution 1920x1080
    IDM_RES_DEF_4K   = 2530,   // desktop (rest) resolution 3840x2160
    IDM_RES_DEF_1440P= 2531,   // desktop (rest) resolution 2560x1440
    IDM_RES_DEF_1080P= 2532,   // desktop (rest) resolution 1920x1080
    IDM_RES_DISP_AUTO = 2540,  // target display: auto (largest current)
    IDM_RES_PCT_25 = 2550,     // engage threshold 25%
    IDM_RES_PCT_50 = 2551,     // engage threshold 50%
    IDM_RES_PCT_66 = 2552,     // engage threshold 66% (default)
    IDM_RES_PCT_75 = 2553,     // engage threshold 75%
    IDM_RES_PCT_90 = 2554,     // engage threshold 90%
    IDM_RES_DISP_BASE = 2600,  // + i: live display i from the menu snapshot
    IDM_EXIT      = 3000,
};

// CPU governor modes (persisted as HKCU…\CpuAuto = 1/0)
enum : int { CPU_MODE_AUTO = 1, CPU_MODE_OFF = 2 };

// Forward declarations (defined with the worker bridge below).
static void enqueueCap(int capMhz);          // -1 = restore default clocks
static void requestTrayRefresh(const std::wstring& tip);

// ───────────────────────── Helpers ─────────────────────────

struct ExecResult {
    std::string output;
    DWORD exitCode = static_cast<DWORD>(-1);
};

// Run a command line silently, capture stdout+stderr.  No console window flashes.
static ExecResult runHidden(const char* cmd) {
    ExecResult res;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
        return res;

    // Read end must NOT be inherited by the child.
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};

    // CreateProcess wants a writable buffer.
    std::string cmdline = cmd;
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr,
        buf.data(),
        nullptr, nullptr,
        TRUE,               // inherit handles
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    // Close our copy of the write end so ReadFile can hit EOF.
    CloseHandle(hWrite);

    if (!ok) {
        CloseHandle(hRead);
        return res;
    }

    // Non-blocking drain + watchdog. A plain ReadFile loop deadlocks forever
    // if the child (or a grandchild still holding the pipe write-end) hangs —
    // e.g. nvidia-smi stalling under GPU load — which freezes the UI thread.
    const DWORD t0 = GetTickCount();
    char chunk[512];
    DWORD avail = 0, n = 0;
    bool childDone = false;
    for (;;) {
        // Drain whatever is available right now (PeekNamedPipe never blocks).
        while (PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            if (!ReadFile(hRead, chunk, std::min(avail, (DWORD)sizeof(chunk)), &n, nullptr) || n == 0)
                break;
            res.output.append(chunk, n);
        }
        DWORD dw = WaitForSingleObject(pi.hProcess, 100);
        if (dw == WAIT_OBJECT_0) { childDone = true; break; }      // child exited
        if (dw != WAIT_TIMEOUT) break;                              // wait failed
        if (GetTickCount() - t0 >= 15000) {                         // watchdog
            TerminateProcess(pi.hProcess, 1);
            log("runHidden: watchdog killed hung process (%.30s)", cmd);
            break;
        }
    }
    // Final drain after the child exited (EOF arrived): collect leftovers.
    while (PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        if (!ReadFile(hRead, chunk, std::min(avail, (DWORD)sizeof(chunk)), &n, nullptr) || n == 0)
            break;
        res.output.append(chunk, n);
    }
    (void)childDone;
    GetExitCodeProcess(pi.hProcess, &res.exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return res;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ───────────────────── NVML — in-process GPU access ─────────────────────
// nvidia-smi is a command-line wrapper around NVML, the driver-shipped
// library (C:\Windows\System32\nvml.dll on every modern install). A spawn
// cost ~6ms of CPU charged to THIS process (CreateProcess + image load +
// a full NVML re-init inside the child each call), and the old worker loop
// ran 2-4 spawns per second — a constant ~2%-of-a-core burn, plus Defender
// scanning every spawn and ~+5% of subprocess churn polluting the CPU
// governor's own load sampling. The same readings and the same -lgc/-rgc
// actuator cost ~0.1 MICROseconds in-process (measured on this machine).
// The library is loaded dynamically: missing or failing → every wrapper
// below falls back to the original nvidia-smi subprocess path (pre-1.7
// behavior). Minimal stable-C-ABI surface only; the enum values mirror the
// documented nvml.h (NVML_SUCCESS = 0, NVML_TEMPERATURE_GPU = 0). The
// locked-clocks pair exists since driver 415 — nvidia-smi's -lgc/-rgc are
// its CLI spelling.

struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };
typedef struct nvmlDevice_st* nvmlDevice_t;

static nvmlDevice_t     g_nvmlDev      = nullptr;  // set by nvmlSetup()
static volatile LONG    g_nvmlReady    = 0;        // 1 = in-process access active
static ULONGLONG        g_nvmlReinitAt = 0;        // earliest next (re)init attempt
static ULONGLONG        g_nvmlErrLogMs = 0;        // failure-log throttle

static int (*p_nvmlInit)(void);
static int (*p_nvmlShutdown)(void);
static int (*p_nvmlGetHandle)(unsigned, nvmlDevice_t*);
static int (*p_nvmlGetTemp)(nvmlDevice_t, int /*NVML_TEMPERATURE_GPU*/, unsigned*);
static int (*p_nvmlGetUtil)(nvmlDevice_t, nvmlUtilization_t*);
static int (*p_nvmlLockClocks)(nvmlDevice_t, unsigned /*min*/, unsigned /*max*/);
static int (*p_nvmlUnlockClocks)(nvmlDevice_t);

// One line per failure kind of moment, then quiet for 10 minutes — a dead
// NVML must not spam the log at tick rate.
static void nvmlErr(const char* what, int rc) {
    const ULONGLONG now = GetTickCount64();
    if (g_nvmlErrLogMs && now - g_nvmlErrLogMs < 600000) return;
    g_nvmlErrLogMs = now;
    log("nvml: %s failed (rc=%d) — falling back to nvidia-smi", what, rc);
}

// A call that FAILED while active: stop using NVML, self-heal quickly
// (covers driver reload / resume from hibernate).
static void nvmlFail(const char* what, int rc) {
    nvmlErr(what, rc);
    InterlockedExchange(&g_nvmlReady, 0);
    g_nvmlDev = nullptr;
    g_nvmlReinitAt = GetTickCount64() + 5000;
}

// Load + init + device handle. Safe to call again after a failure —
// LoadLibrary/GetProcAddress are OS-cached, the whole call is microseconds.
static bool nvmlSetup() {
    InterlockedExchange(&g_nvmlReady, 0);
    g_nvmlDev = nullptr;
    HMODULE h = LoadLibraryW(L"nvml.dll");            // System32 on driver installs
    if (!h) h = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!h) { nvmlErr("nvml.dll not found", -1); return false; }
    if (!p_nvmlInit) {
        struct Sym { const char* name; void** pfn; };
        const Sym syms[] = {
            { "nvmlInit_v2",                    (void**)&p_nvmlInit },
            { "nvmlShutdown",                   (void**)&p_nvmlShutdown },
            { "nvmlDeviceGetHandleByIndex_v2",  (void**)&p_nvmlGetHandle },
            { "nvmlDeviceGetTemperature",       (void**)&p_nvmlGetTemp },
            { "nvmlDeviceGetUtilizationRates",  (void**)&p_nvmlGetUtil },
            { "nvmlDeviceSetGpuLockedClocks",   (void**)&p_nvmlLockClocks },
            { "nvmlDeviceResetGpuLockedClocks", (void**)&p_nvmlUnlockClocks },
        };
        for (const Sym& s : syms) {
            *s.pfn = (void*)GetProcAddress(h, s.name);
            if (!*s.pfn) { nvmlErr(s.name, -2); return false; }
        }
    }
    int rc = p_nvmlInit();
    if (rc != 0) { nvmlErr("nvmlInit", rc); return false; }
    rc = p_nvmlGetHandle(0, &g_nvmlDev);
    if (rc != 0 || !g_nvmlDev) {
        nvmlErr("nvmlDeviceGetHandleByIndex(0)", rc);
        g_nvmlDev = nullptr;
        return false;
    }
    InterlockedExchange(&g_nvmlReady, 1);
    return true;
}

// Gate for every NVML call: already active, or a (re)setup is due. Load
// failures back off 60s (the driver service may still be starting at
// logon); a mid-call failure retries after 5s (nvmlFail).
static bool nvmlEnsure() {
    if (g_nvmlReady) return true;
    const ULONGLONG now = GetTickCount64();
    if (now < g_nvmlReinitAt) return false;
    g_nvmlReinitAt = now + 60000;
    return nvmlSetup();
}

static bool nvmlQueryTempUtil(int* tempOut, int* utilOut) {
    unsigned t = 0;
    nvmlUtilization_t u{};
    int rc = p_nvmlGetTemp(g_nvmlDev, 0, &t);
    if (rc != 0) { nvmlFail("nvmlDeviceGetTemperature", rc); return false; }
    rc = p_nvmlGetUtil(g_nvmlDev, &u);
    if (rc != 0) { nvmlFail("nvmlDeviceGetUtilizationRates", rc); return false; }
    *tempOut = static_cast<int>(t);
    if (utilOut) *utilOut = static_cast<int>(u.gpu);
    return true;
}

static bool nvmlSetClockCap(int capMhz) {
    int rc = p_nvmlLockClocks(g_nvmlDev, CLOCK_LOW, static_cast<unsigned>(capMhz));
    if (rc != 0) { nvmlFail("nvmlDeviceSetGpuLockedClocks", rc); return false; }
    return true;
}

static bool nvmlResetClocks() {
    int rc = p_nvmlUnlockClocks(g_nvmlDev);
    if (rc != 0) { nvmlFail("nvmlDeviceResetGpuLockedClocks", rc); return false; }
    return true;
}

// Pair the init at exit (the worker is gone by then). Process teardown
// would clean up anyway — this is hygiene.
static void nvmlTeardown() {
    if (g_nvmlReady) p_nvmlShutdown();
    InterlockedExchange(&g_nvmlReady, 0);
}


// ───────────────────────── nvidia-smi wrappers ─────────────────────────

// Returns °C (or -1 on failure) and, via utilOut, GPU utilisation % (-1 when
// unknown). One subprocess for both readings: extra nvidia-smi invocations
// would add real CPU load and poison the governor's own load sampling.
// Utilisation is the heavy-3D detector for the resolution governor — under
// load, clocks.sm and utilisation both stay honest even with caps applied.
static int queryTempUtil(int* utilOut) {
    if (utilOut) *utilOut = -1;
    // In-process NVML first (~0.1µs). On any NVML problem, fall through to
    // the subprocess path — identical readings, pre-1.7 behavior.
    if (nvmlEnsure()) {
        int temp = -1;
        if (nvmlQueryTempUtil(&temp, utilOut)) return temp;
    }
    auto r = runHidden("nvidia-smi --query-gpu=temperature.gpu,utilization.gpu"
                       " --format=csv,noheader,nounits -i 0");
    auto s = trim(r.output);
    if (s.empty()) { log("queryTemp: empty response (exit=%lu)", r.exitCode); return -1; }
    int temp = -1;
    try {
        size_t comma = s.find(',');
        if (comma == std::string::npos) {
            temp = std::stoi(s);                      // old driver: temp only
        } else {
            temp = std::stoi(trim(s.substr(0, comma)));
            if (utilOut) *utilOut = std::stoi(trim(s.substr(comma + 1)));
        }
    } catch (...) { log("queryTemp: parse failed: %s", s.c_str()); }
    return temp;
}

// Returns watts, or -1 on failure. Only used at startup to learn the current
// limit (restored on exit) — never for regulation.
static int queryPowerLimit() {
    auto r = runHidden("nvidia-smi --query-gpu=power.limit --format=csv,noheader,nounits -i 0");
    auto s = trim(r.output);
    if (s.empty()) return -1;
    try { return static_cast<int>(std::stof(s)); }
    catch (...) { return -1; }
}

// Set power limit. Returns true on success. Only used for the startup-
// read/exit-restore parity — regulation never touches power.
static bool setPowerLimit(int watts) {
    std::string cmd = "nvidia-smi -pl " + std::to_string(watts) + " -i 0";
    auto r = runHidden(cmd.c_str());
    bool ok = (r.exitCode == 0);
    if (ok)
        log("setPowerLimit: %dW OK", watts);
    else
        log("setPowerLimit: %dW FAILED (exit=%lu) %s", watts, r.exitCode, trim(r.output).c_str());
    return ok;
}

static std::string queryGpuName() {
    auto r = runHidden("nvidia-smi --query-gpu=name --format=csv,noheader -i 0");
    return trim(r.output);
}

// Returns max SM clock in MHz, or 0 if unavailable.
static int queryMaxClock() {
    auto r = runHidden("nvidia-smi --query-gpu=clocks.max.sm --format=csv,noheader,nounits -i 0");
    auto s = trim(r.output);
    if (s.empty()) return 0;
    try { int c = std::stoi(s); return c > 0 ? c : 0; }
    catch (...) { return 0; }
}

// Cap core clocks to [CLOCK_LOW, capMhz]. Returns true on success.
static bool setClockCap(int capMhz) {
    // NVML first — the direct driver call is MORE trustworthy than a CLI
    // exit code (it IS what -lgc does underneath).
    if (nvmlEnsure() && nvmlSetClockCap(capMhz)) {
        log("setClockCap: %dMHz OK (nvml)", capMhz);
        return true;
    }
    std::string cmd = "nvidia-smi -lgc " + std::to_string(CLOCK_LOW) + "," +
                      std::to_string(capMhz) + " -i 0";
    auto r = runHidden(cmd.c_str());
    bool ok = (r.exitCode == 0);
    if (ok) log("setClockCap: %dMHz OK", capMhz);
    else    log("setClockCap: %dMHz FAILED (exit=%lu) %s", capMhz, r.exitCode,
                trim(r.output).c_str());
    return ok;
}

// Restore default clocks. Returns true on success.
static bool resetClocks() {
    if (nvmlEnsure() && nvmlResetClocks()) {
        log("resetClocks: OK (nvml)");
        return true;
    }
    auto r = runHidden("nvidia-smi -rgc -i 0");
    bool ok = (r.exitCode == 0);
    if (ok) log("resetClocks: OK");
    else    log("resetClocks: FAILED (exit=%lu)", r.exitCode);
    return ok;
}

// ───────────────────────── Dynamic tray icon ─────────────────────────

// Tray icon background by GPU temperature severity.
static COLORREF tempColor(int temp) {
    if (temp < 0)   return RGB(70, 70, 70);     // grey — error
    if (temp < 60)  return RGB(30, 140, 70);    // green
    if (temp < 70)  return RGB(80, 170, 50);    // light-green
    if (temp < 75)  return RGB(180, 170, 30);   // yellow
    if (temp < 80)  return RGB(210, 130, 30);   // orange
    if (temp < 85)  return RGB(210, 80, 30);    // dark-orange
    return RGB(200, 40, 40);                    // red
}

static std::string tempText(int temp) {
    if (temp < 0)   return "--";
    if (temp > 99)  return "99";
    return std::to_string(temp);
}

// Draw a tray icon: solid background + centred text (omitted when empty).
// All the GDI work happens here — callers that flash the icon must CACHE the
// result and alternate handles, never re-run this per blink.
static HICON createStatusIcon(COLORREF bg, const std::string& text) {
    const int sz = GetSystemMetrics(SM_CXSMICON);  // 16 at 100% DPI
    if (sz <= 0) return nullptr;

    HDC hScreen = GetDC(nullptr);
    HDC hdcMem  = CreateCompatibleDC(hScreen);

    // ── Colour bitmap ──
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = sz;
    bmi.bmiHeader.biHeight      = -sz;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBmp);

    // Fill background (rounded rect).
    HBRUSH hBrush = CreateSolidBrush(bg);
    RECT rc{ 0, 0, sz, sz };
    FillRect(hdcMem, &rc, hBrush);
    DeleteObject(hBrush);

    // Thin border.
    HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
    SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
    Rectangle(hdcMem, 0, 0, sz, sz);
    SelectObject(hdcMem, hOldPen);
    DeleteObject(hPen);

    // Optional centred text (CPU-state icons are plain colour blocks).
    if (!text.empty()) {
        SetBkMode(hdcMem, TRANSPARENT);
        SetTextColor(hdcMem, RGB(255, 255, 255));

        int fontH = sz * 10 / 16;  // proportional to icon size
        if (fontH < 8) fontH = 8;

        HFONT hFont = CreateFontA(
            fontH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            "Tahoma");
        HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);

        DrawTextA(hdcMem, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdcMem, hOldFont);
        DeleteObject(hFont);
    }

    // Fix alpha channel — GDI leaves it at 0; set to 255 for opaque pixels.
    DWORD* px = static_cast<DWORD*>(bits);
    for (int i = 0; i < sz * sz; i++) {
        // Only make non-black pixels fully opaque.
        // (Black border pixels can stay semi-opaque for a subtle edge.)
        DWORD c = px[i];
        if ((c & 0x00FFFFFF) != 0)
            px[i] = c | 0xFF000000;
        else
            px[i] = c | 0xFF000000;
    }

    // ── Mask bitmap (all zeros = fully opaque) ──
    HDC hdcMask  = CreateCompatibleDC(hScreen);
    HBITMAP hMask = CreateBitmap(sz, sz, 1, 1, nullptr);
    HBITMAP hOldMask = (HBITMAP)SelectObject(hdcMask, hMask);
    PatBlt(hdcMask, 0, 0, sz, sz, BLACKNESS);

    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmMask  = hMask;
    ii.hbmColor = hBmp;

    HICON hIcon = CreateIconIndirect(&ii);

    // Cleanup.
    SelectObject(hdcMem, hOld);
    SelectObject(hdcMask, hOldMask);
    DeleteObject(hBmp);
    DeleteObject(hMask);
    DeleteDC(hdcMem);
    DeleteDC(hdcMask);
    ReleaseDC(nullptr, hScreen);

    return hIcon;
}

static HICON createTempIcon(int temp) {
    return createStatusIcon(tempColor(temp), tempText(temp));
}

// ───────────────────────── Scheduled task helpers ─────────────────────────

static const char* TASK_NAME = "CalmDownComputer";

// Returns true if the autostart task exists.
static bool isStartupTaskEnabled() {
    std::string cmd = std::string("schtasks /query /tn ") + TASK_NAME + " /fo csv /nh";
    auto r = runHidden(cmd.c_str());
    return r.exitCode == 0;
}

// Create a logon task that runs this exe elevated.
static bool createStartupTask() {
    wchar_t wpath[MAX_PATH];
    GetModuleFileNameW(nullptr, wpath, MAX_PATH);
    char path[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, MAX_PATH, nullptr, nullptr);

    std::string cmd = std::string("schtasks /create /tn ") + TASK_NAME +
        " /tr \"" + path + "\" /sc onlogon /rl highest /f";
    auto r = runHidden(cmd.c_str());
    log("createStartupTask: exit=%lu", r.exitCode);
    return r.exitCode == 0;
}

// Delete the logon task.
static bool deleteStartupTask() {
    std::string cmd = std::string("schtasks /delete /tn ") + TASK_NAME + " /f";
    auto r = runHidden(cmd.c_str());
    log("deleteStartupTask: exit=%lu", r.exitCode);
    return r.exitCode == 0;
}

// ───────────────────────── Registry persistence ─────────────────────────

static const wchar_t* REG_KEY = L"Software\\CalmDownComputer";

static void saveDword(const wchar_t* name, int val) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD v = static_cast<DWORD>(val);
        RegSetValueExW(hKey, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(hKey);
    }
}

static int regReadDword(const wchar_t* subkey, const wchar_t* name, int def) {
    HKEY hKey;
    int val = def;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD v = 0, size = sizeof(v);
        if (RegQueryValueExW(hKey, name, nullptr, nullptr,
                             reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS)
            val = static_cast<int>(v);
        RegCloseKey(hKey);
    }
    return val;
}

// String persistence — the pinned display identity is a device name.
static void saveString(const wchar_t* name, const wchar_t* val) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, name, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(val),
                       static_cast<DWORD>((wcslen(val) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

static std::wstring readString(const wchar_t* name) {
    std::wstring val;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = 0, type = 0;
        if (RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
            type == REG_SZ && size >= sizeof(wchar_t)) {
            std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
            if (RegQueryValueExW(hKey, name, nullptr, nullptr,
                                 reinterpret_cast<BYTE*>(buf.data()), &size) == ERROR_SUCCESS)
                val.assign(buf.data());
        }
        RegCloseKey(hKey);
    }
    return val;
}

static void saveTargetTemp(int temp) { saveDword(L"TargetTemp", temp); }

static int loadTargetTemp() {
    int temp = regReadDword(REG_KEY, L"TargetTemp", -1);
    if (temp < 0) {
        // First run after the CalmDownGPU → CalmDownComputer rename: carry
        // the old setting over. (0 = Off is the default; nothing to carry.)
        temp = regReadDword(L"Software\\CalmDownGPU", L"TargetTemp", 0);
        if (temp != 0) saveTargetTemp(temp);
    }
    return temp;
}

// CPU governor preference; default ON — office-cool is the point of the app.
static bool loadCpuAuto() {
    int v = regReadDword(REG_KEY, L"CpuAuto", -1);
    if (v < 0) v = regReadDword(L"Software\\CalmDownGPU", L"CpuAuto", 1);
    return v != 0;
}

static void saveCpuAuto(bool on) { saveDword(L"CpuAuto", on ? 1 : 0); }

// Resolution governor preference. Default OFF — a sudden display switch is
// surprising, so this one is opt-in. Fallback defaults to 2560x1440 ("2K":
// the sweet spot for borderless games on a 4K desktop).
static bool loadResAuto() { return regReadDword(REG_KEY, L"ResAuto", 0) != 0; }
static void saveResAuto(bool on) { saveDword(L"ResAuto", on ? 1 : 0); }
static int  loadResW() { return regReadDword(REG_KEY, L"ResAutoW", 2560); }
static int  loadResH() { return regReadDword(REG_KEY, L"ResAutoH", 1440); }
static void saveResFallback(int w, int h) {
    saveDword(L"ResAutoW", w);
    saveDword(L"ResAutoH", h);
}

// Desktop ("rest") resolution — the governor restores TO this while the
// feature is on, so it is a real setting, not "whatever the registry said".
// Learned on first run from the target display's Windows-default mode.
static void saveResDefault(int w, int h) {
    saveDword(L"ResDefaultW", w);
    saveDword(L"ResDefaultH", h);
}

// Pinned target display (GDI device name, e.g. "\\.\DISPLAY2"). Empty =
// auto: the largest current display, which on this machine is the real
// monitor — never the virtual streaming displays.
static void saveResDisp(const wchar_t* devName) { saveString(L"ResDisp", devName); }
static std::wstring loadResDisp() { return readString(L"ResDisp"); }

// Engage threshold (GPU utilisation % that arms the source hunt), persisted
// so a tuned value survives reboots.
static void saveResOnPct(int pct) { saveDword(L"ResOnPct", pct); }
static int loadResOnPct() {
    int v = regReadDword(REG_KEY, L"ResOnPct", RES_ON_PCT);
    if (v < 5 || v > 100) v = RES_ON_PCT;   // registry garbage → default
    return v;
}

// ───────────────────── Displays & mode switching ─────────────────────
// This machine has REAL monitors AND virtual ones (a streaming driver) — so
// the governor targets a USER-SELECTED display, pinned by its EDID identity
// (the monitor's PnP device ID, e.g. MONITOR\ACR0631\{...}\0008). GDI names
// (\\.\DISPLAYn) are NOT stable: they follow path order and re-number across
// reboots and topology churn (TVs and virtual displays dropping off the bus
// reflow everything). EDID identities survive all of that.

// Friendly vendor names for the 3-letter PnP-ID prefixes (ACR0631 → Acer).
struct Ven { const wchar_t* code; const wchar_t* name; };
static const Ven g_vendors[] = {
    { L"ACR", L"Acer" },   { L"SNY", L"Sony" },   { L"DEL", L"Dell" },
    { L"SAM", L"Samsung" },{ L"BNQ", L"BenQ" },   { L"AOC", L"AOC" },
    { L"PHL", L"Philips" },{ L"HPN", L"HP" },     { L"GSM", L"LG" },
    { L"LGD", L"LG" },     { L"MSI", L"MSI" },    { L"MTT", L"Virtual Display" },
};

static std::wstring friendlyMonitorName(const wchar_t* edid,
                                        const wchar_t* fallback) {
    if (wcsncmp(edid, L"MONITOR\\", 8) == 0) {
        const wchar_t* code = edid + 8;
        const wchar_t* slash = wcschr(code, L'\\');   // model code = up to next '\'
        size_t n = slash ? (size_t)(slash - code) : wcslen(code);
        if (n > 16) n = 16;
        wchar_t model[24];
        wcsncpy_s(model, code, n);
        for (const auto& v : g_vendors)
            if (_wcsnicmp(code, v.code, 3) == 0)
                return std::wstring(v.name) + L" " + model;
        return model;   // unknown vendor — the bare model code
    }
    return fallback;   // EDID-less (or unknown) — the adapter/monitor string
}

struct LiveDisplay {
    wchar_t       devName[32];   // "\\.\DISPLAY2" — the ChangeDisplaySettingsExW handle
    wchar_t       edid[128];     // MONITOR\<pnp id>\{...}\<inst> — diagnostics only now
    std::wstring  label;         // menu text, user-friendly
    std::wstring  shortName;     // friendly model name — ALSO the stable pin
    DWORD         w = 0, h = 0;  // current mode
    bool          primary = false;
};

// Did the last enumDisplays() get names from CCD (1) or fall back to the
// GDI monitor sub-device (0)? Diagnostics: the worker thread has observed
// CCD-less enums while the UI thread sees the live truth.
static volatile LONG g_enumCcdOk = -1;

// All GDI displays currently attached to the desktop. Pure Win32 queries
// (no subprocess) — cheap enough for every menu rebuild and every governor
// fire. UI thread uses it for the menu, worker thread for actuation.
//
// Names come from the CCD layer (QueryDisplayConfig — dxgkrnl's own topology,
// the same source NVIDIA Control Panel reads), NOT from the GDI monitor
// sub-device: on this machine a stale monitor node survives reboots and serves
// the wrong model name through EnumDisplayDevices, while CCD always reports
// the live truth. The CCD friendly name doubles as the pin identity: it is
// the panel's EDID product name, stable across renumbering and topology churn.
static std::vector<LiveDisplay> enumDisplays() {
    std::vector<LiveDisplay> out;

    // devName → friendly monitor name, straight from the committed paths.
    std::map<std::wstring, std::wstring> friendly;
    UINT32 nPaths = 0, nModes = 0;
    bool ccdOk = false;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) ==
        ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(),
                               &nModes, modes.data(),
                               nullptr) == ERROR_SUCCESS) {
            ccdOk = true;
            for (UINT32 i = 0; i < nPaths; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof(src);
                src.header.adapterId = paths[i].sourceInfo.adapterId;
                src.header.id        = paths[i].sourceInfo.id;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
                tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgt.header.size = sizeof(tgt);
                tgt.header.adapterId = paths[i].targetInfo.adapterId;
                tgt.header.id        = paths[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS ||
                    DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS)
                    continue;
                if (tgt.monitorFriendlyDeviceName[0] &&
                    wcscmp(tgt.monitorFriendlyDeviceName,
                           L"Generic PnP Monitor") != 0)
                    friendly[src.viewGdiDeviceName] =
                        tgt.monitorFriendlyDeviceName;
            }
        }
    }

    DISPLAY_DEVICEW dd{};
    for (DWORD i = 0;; i++) {
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) continue;

        LiveDisplay d;
        wcsncpy_s(d.devName, dd.DeviceName, _TRUNCATE);
        d.w = dm.dmPelsWidth;
        d.h = dm.dmPelsHeight;
        d.primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;

        // Monitor sub-device: kept for diagnostics (d.edid) and as the naming
        // fallback when CCD came up empty (e.g. topology mid-transition).
        DISPLAY_DEVICEW mon{};
        mon.cb = sizeof(mon);
        const wchar_t* who = dd.DeviceString;
        if (EnumDisplayDevicesW(dd.DeviceName, 0, &mon, 0) && mon.DeviceString[0] &&
            wcscmp(mon.DeviceString, L"Generic PnP Monitor") != 0)
            who = mon.DeviceString;
        wcsncpy_s(d.edid, mon.DeviceID[0] ? mon.DeviceID : dd.DeviceName, _TRUNCATE);

        auto it = friendly.find(d.devName);
        d.shortName = (it != friendly.end()) ? it->second
                                             : friendlyMonitorName(d.edid, who);
        wchar_t res[48];
        swprintf_s(res, L" · %ux%u", (unsigned)d.w, (unsigned)d.h);
        d.label = d.shortName + res;
        if (d.primary) d.label += L" · primary";
        out.push_back(std::move(d));
    }
    InterlockedExchange(&g_enumCcdOk, ccdOk ? 1 : 0);
    return out;
}

// Does this display match the pinned identity? The CCD friendly name is the
// pin (the panel's EDID product name — stable across renumbering). Older pin
// formats still resolve: the EDID device ID, its MONITOR\<pnp> prefix, and
// the legacy GDI name.
static bool displayMatchesPinned(const LiveDisplay& d, const wchar_t* pinned) {
    if (!pinned || !pinned[0]) return false;
    if (_wcsicmp(d.shortName.c_str(), pinned) == 0) return true;
    if (wcscmp(d.edid, pinned) == 0) return true;
    if (wcsncmp(pinned, L"MONITOR\\", 8) == 0) {
        wchar_t code[64] = L"MONITOR\\";
        const wchar_t* rest = pinned + 8;
        const wchar_t* slash = wcschr(rest, L'\\');
        size_t n = slash ? (size_t)(slash - rest) : wcslen(rest);
        if (n >= 60) n = 60;
        wcsncpy_s(code + 8, 56, rest, n);
        if (_wcsnicmp(d.edid, code, wcslen(code)) == 0) return true;
    }
    return _wcsicmp(d.devName, pinned) == 0;   // legacy GDI-name pin
}

// Which display does the governor act on? A pinned selection wins; with no
// selection, the largest current mode. Returns nullptr when the PINNED
// display is not present (caller pauses + logs — never silently switches to
// a different display) or when nothing is attached.
static const LiveDisplay* resolveDisplay(const wchar_t* pinned,
                                         std::vector<LiveDisplay>& scratch) {
    scratch = enumDisplays();
    if (pinned && pinned[0]) {
        // Priority: CCD friendly name (the current pin format), then exact
        // EDID identity, the MONITOR\<pnp> prefix, then legacy GDI names.
        for (const auto& d : scratch)
            if (_wcsicmp(d.shortName.c_str(), pinned) == 0) return &d;
        const LiveDisplay* prefixHit = nullptr;
        const LiveDisplay* legacyHit = nullptr;
        wchar_t code[64] = L"MONITOR\\";
        if (wcsncmp(pinned, L"MONITOR\\", 8) == 0) {
            const wchar_t* rest = pinned + 8;
            const wchar_t* slash = wcschr(rest, L'\\');
            size_t n = slash ? (size_t)(slash - rest) : wcslen(rest);
            if (n >= 56) n = 56;
            wcsncpy_s(code + 8, 56, rest, n);
        }
        for (const auto& d : scratch) {
            if (wcscmp(d.edid, pinned) == 0) return &d;         // exact EDID
            if (!prefixHit && wcsncmp(pinned, L"MONITOR\\", 8) == 0 &&
                _wcsnicmp(d.edid, code, wcslen(code)) == 0)
                prefixHit = &d;
            if (!legacyHit && _wcsicmp(d.devName, pinned) == 0) legacyHit = &d;
        }
        if (prefixHit) return prefixHit;
        if (legacyHit) return legacyHit;
        return nullptr;
    }
    const LiveDisplay* best = nullptr;
    for (const auto& d : scratch)
        if (!best || d.w * d.h > best->w * best->h) best = &d;
    return best;
}

// Read the current mode of one display.
static bool getCurrentMode(const wchar_t* dev, DEVMODEW* dm) {
    ZeroMemory(dm, sizeof(*dm));
    dm->dmSize = sizeof(*dm);
    return EnumDisplaySettingsExW(dev, ENUM_CURRENT_SETTINGS, dm, 0) != 0;
}

// ─── CCD source-mode switch: the blink-free technique ───
// ChangeDisplaySettingsExW re-times the PANEL, so the monitor loses sync and
// blanks for the re-lock (the 2s black screen — measured, both CDS variants).
// SetDisplayConfig — the CCD API that Windows' own display UI uses, and the
// exact technique Monitor Profile Switcher is built on (studied: their
// CCDWrapper.cs, GetSourceName/UseSuppliedDisplayConfig/AllowChanges) — can
// change the desktop (SOURCE) size while leaving the panel's native TARGET
// timing alone: GPU scaling on the unchanged signal, no sync loss, no blink.
// No SDC_SAVE_TO_DATABASE → the change stays temporary: logoff/reboot
// reverts, same safety model as the CDS calls.
static bool ccdSetSourceMode(const wchar_t* gdiDev, int w, int h) {
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes)
        != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(),
                           &nModes, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;

    // Find the active path whose GDI view name matches the pinned display.
    int pathIdx = -1;
    for (UINT32 i = 0; i < nPaths; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = paths[i].sourceInfo.adapterId;
        sn.header.id        = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;
        if (_wcsicmp(sn.viewGdiDeviceName, gdiDev) == 0) { pathIdx = (int)i; break; }
    }
    if (pathIdx < 0) return false;

    // Mutate the SOURCE (desktop) mode only — panel timing untouched.
    const LUID    srcAdapter = paths[pathIdx].sourceInfo.adapterId;
    const UINT32  srcId      = paths[pathIdx].sourceInfo.id;
    DISPLAYCONFIG_MODE_INFO* src = nullptr;
    for (UINT32 i = 0; i < nModes; i++) {
        if (modes[i].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        if (modes[i].adapterId.LowPart  == srcAdapter.LowPart &&
            modes[i].adapterId.HighPart == srcAdapter.HighPart &&
            modes[i].id == srcId) { src = &modes[i]; break; }
    }
    if (!src) return false;
    if (src->sourceMode.width  == static_cast<UINT32>(w) &&
        src->sourceMode.height == static_cast<UINT32>(h))
        return true;   // already there — nothing to switch

    src->sourceMode.width  = static_cast<UINT32>(w);
    src->sourceMode.height = static_cast<UINT32>(h);

    // Apply. First WITHOUT SDC_ALLOW_CHANGES (the driver must not sneak a
    // timing change past us), then with it as the fallback — the MPS pattern.
    // NOTE: no SDC_SAVE_TO_DATABASE anywhere — deliberately temporary.
    LONG st = SetDisplayConfig(nPaths, paths.data(), nModes, modes.data(),
                               SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                               SDC_NO_OPTIMIZATION);
    if (st != ERROR_SUCCESS) {
        st = SetDisplayConfig(nPaths, paths.data(), nModes, modes.data(),
                              SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                              SDC_NO_OPTIMIZATION | SDC_ALLOW_CHANGES);
    }
    if (st != ERROR_SUCCESS) {
        log("res[%ls]: CCD switch to %dx%d failed (status 0x%lX)",
            gdiDev, w, h, (unsigned long)st);
        return false;
    }
    log("res[%ls]: CCD switch to %dx%d OK (panel timing unchanged — no blink)",
        gdiDev, w, h);
    return true;
}

// Pick a NATIVE supported mode matching w×h, refresh as close to the
// current one as the panel allows (exact first, then highest ≤ current).
// Commanding an exact native mode is a single clean timing switch; asking
// the driver to synthesise one (PELS-only) can add extra blanking.
static bool findNativeMode(const wchar_t* dev, int w, int h, DEVMODEW* out) {
    DEVMODEW cur{};
    if (!getCurrentMode(dev, &cur)) return false;
    const DWORD wantHz = cur.dmDisplayFrequency;
    auto score = [wantHz](DWORD hz) -> int {
        if (hz == wantHz) return 0;                       // exact: no cadence change
        if (hz > 1 && hz < wantHz) return 1 + static_cast<int>(wantHz - hz);   // highest below
        return 100000 + static_cast<int>(hz);             // above current: last resort
    };
    bool have = false;
    DEVMODEW best{};
    for (DWORD i = 0;; i++) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(dev, i, &dm, 0)) break;
        if (dm.dmPelsWidth != (DWORD)w || dm.dmPelsHeight != (DWORD)h) continue;
        if (dm.dmDisplayFlags & DM_INTERLACED) continue;
        if (dm.dmBitsPerPel && cur.dmBitsPerPel && dm.dmBitsPerPel != cur.dmBitsPerPel)
            continue;
        if (dm.dmDisplayFrequency <= 1) continue;         // driver-reported unknown
        if (!have || score(dm.dmDisplayFrequency) < score(best.dmDisplayFrequency)) {
            best = dm;
            have = true;
        }
    }
    if (have) *out = best;
    return have;
}

// Switch ONE display to w×h as a TEMPORARY mode change (no registry write —
// logoff/reboot reverts even if we die mid-drop). Tiers:
//   1. CCD source-mode switch (SetDisplayConfig): panel timing untouched —
//      instant, no blink. The normal path.
//   2. CDS with an exact native mode (findNativeMode) — one timing change.
//   3. CDS relaxed PELS-only request — driver synthesises.
// Returns a DISP_CHANGE_* code (CCD success maps to DISP_CHANGE_SUCCESSFUL).
static LONG applyDisplayMode(const wchar_t* dev, int w, int h) {
    DEVMODEW cur{};
    if (!getCurrentMode(dev, &cur)) return DISP_CHANGE_FAILED;
    if (cur.dmPelsWidth == (DWORD)w && cur.dmPelsHeight == (DWORD)h)
        return DISP_CHANGE_SUCCESSFUL;   // already there — nothing to do
    if (ccdSetSourceMode(dev, w, h))
        return DISP_CHANGE_SUCCESSFUL;
    DEVMODEW native{};
    if (findNativeMode(dev, w, h, &native) && native.dmDisplayFrequency > 1) {
        native.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        log("res[%ls]: native mode %dx%d @%uHz selected (CDS fallback)", dev, w, h,
            (unsigned)native.dmDisplayFrequency);
        LONG rc = ChangeDisplaySettingsExW(dev, &native, nullptr, 0, nullptr);
        if (rc == DISP_CHANGE_SUCCESSFUL) return rc;
        // rejected — fall through to the relaxed request
    }
    DEVMODEW relaxed = cur;
    relaxed.dmPelsWidth  = static_cast<DWORD>(w);
    relaxed.dmPelsHeight = static_cast<DWORD>(h);
    relaxed.dmFields    |= DM_PELSWIDTH | DM_PELSHEIGHT;
    return ChangeDisplaySettingsExW(dev, &relaxed, nullptr, 0, nullptr);
}

// Revert one display to its default (registry) mode. NULL devmode is the
// documented "revert to default".
static bool restoreDisplayMode(const wchar_t* dev) {
    return ChangeDisplaySettingsExW(dev, nullptr, nullptr, 0, nullptr)
           == DISP_CHANGE_SUCCESSFUL;
}

// ───────────── GPU load sourcing (Task-Manager-style) ─────────────
// The resolution governor must tell "a game fills the screen" apart from
// "background compute is chewing the GPU" (AI inference does the latter for
// hours). Two ingredients:
//   1. Per-PID graphics-engine utilisation from the "GPU Engine" performance
//      counters — the exact data behind Task Manager's GPU column. Games
//      show under 3D/Graphics_N; CUDA inference shows under Compute and is
//      invisible here, which is precisely the discrimination we want.
//   2. A window check: does that PID own a visible, UNDECORATED (borderless)
//      root window covering ≥95% of the target display? A fullscreen
//      borderless game does; an AI console, a training notebook or a
//      windowed tool does not.

struct PidUtil {
    DWORD pid = 0;
    int   pct = 0;   // summed graphics-engine utilisation, clamped to 100
};

// Sums GRAPHICS-engine utilisation per PID: "3D" (classic) plus
// "Graphics_N" (DX12/Vulkan multi-node — RE Engine, Unreal and friends
// report Graphics_1, invisible to a naive 3D-only filter). Compute
// (CUDA/DirectML-independent) deliberately never matches, so AI inference
// stays unsourced. Worker thread only (blocks ~0.5s for the counter delta).
// Returns false when the counters are unavailable — the caller then refuses
// to engage (no sourcing → no drop, by design).
static bool queryGfxUtilByPid(std::vector<PidUtil>& out) {
    out.clear();
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return false;

    // Expand "\GPU Engine(*)\Utilization Percentage" into concrete
    // instances; each path embeds pid_N…engtype_X. Two-call pattern:
    // NULL buffer → PDH_MORE_DATA + required size; allocate; retry.
    DWORD need = 0;
    if (PdhExpandCounterPathW(L"\\GPU Engine(*)\\Utilization Percentage",
                              nullptr, &need) != PDH_MORE_DATA || !need) {
        PdhCloseQuery(q);
        return false;
    }
    std::vector<wchar_t> buf(need, L'\0');
    if (PdhExpandCounterPathW(L"\\GPU Engine(*)\\Utilization Percentage",
                              buf.data(), &need) != ERROR_SUCCESS) {
        PdhCloseQuery(q);
        return false;
    }
    LPWSTR list = buf.data();

    std::vector<std::pair<PDH_HCOUNTER, DWORD>> added;
    for (LPWSTR p = list; *p; p += wcslen(p) + 1) {
        // Graphics engines only — see the function comment for why both.
        if (!wcsstr(p, L"engtype_3D") && !wcsstr(p, L"engtype_Graphics"))
            continue;
        const wchar_t* pidTag = wcsstr(p, L"pid_");
        if (!pidTag) continue;
        DWORD pid = wcstoul(pidTag + 4, nullptr, 10);
        if (!pid) continue;
        PDH_HCOUNTER c = nullptr;
        if (PdhAddCounterW(q, p, 0, &c) != ERROR_SUCCESS) continue;
        added.emplace_back(c, pid);
    }
    if (added.empty()) { PdhCloseQuery(q); return false; }

    // These counters report the busy fraction of the last interval; PDH
    // formats them as a plain percentage after two collections.
    PdhCollectQueryData(q);
    Sleep(500);
    PdhCollectQueryData(q);

    bool any = false;
    for (const auto& e : added) {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(e.first,
                                        PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                        nullptr, &v) != ERROR_SUCCESS)
            continue;
        if (v.doubleValue <= 0.0) continue;
        any = true;
        auto it = std::find_if(out.begin(), out.end(),
            [&](const PidUtil& pu) { return pu.pid == e.second; });
        const int add = static_cast<int>(v.doubleValue + 0.5);
        if (it == out.end()) out.push_back({ e.second, add });
        else                 it->pct += add;
    }
    PdhCloseQuery(q);
    for (auto& pu : out) pu.pct = std::min(100, pu.pct);
    return any;
}

// HMONITOR of a GDI device name ("\\.\DISPLAY1"), or nullptr.
struct MonCtx { const wchar_t* devName; HMONITOR hmon; };
static BOOL CALLBACK findMonitorCb(HMONITOR hmon, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<MonCtx*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi) && wcscmp(mi.szDevice, ctx->devName) == 0) {
        ctx->hmon = hmon;
        return FALSE;   // found — stop enumeration
    }
    return TRUE;
}
static HMONITOR monitorForDevice(const wchar_t* devName) {
    MonCtx ctx{ devName, nullptr };
    EnumDisplayMonitors(nullptr, nullptr, findMonitorCb,
                        reinterpret_cast<LPARAM>(&ctx));
    return ctx.hmon;
}

struct WinCtx {
    DWORD pid;
    HMONITOR hmon;
    HWND fgRootOwner;   // the window the user is actually looking at
    HWND fgRoot;
    HWND found;
    wchar_t cls[64];    // qualifying window's class (engage-log diagnostics)
    RECT rect;
};

// Accepts ONLY the window the user is looking at (foreground root), owned
// by the source PID, top-level, visible, non-minimised, UNDECORATED
// (borderless), covering ≥99% of the target monitor. 99% — not 95% —
// because a maximised window with a visible taskbar covers ~97-98% at
// 2160p and must NOT qualify: borderless fullscreen covers the taskbar
// region, maximised does not. Decorated windows (WS_CAPTION/WS_THICKFRAME)
// never qualify. The foreground requirement is what stops a fullscreen game
// that keeps rendering BEHIND the browser from triggering a drop while the
// user is working (v1.6.5, observed live).
static BOOL CALLBACK findBorderlessCb(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<WinCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;               // top-level only
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style & (WS_CAPTION | WS_THICKFRAME)) return TRUE;             // decorated
    if (hwnd != ctx->fgRootOwner && hwnd != ctx->fgRoot)
        return TRUE;   // not the foreground window — it is running behind something
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != ctx->hmon) return TRUE;

    RECT wr{}, mr{};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetWindowRect(hwnd, &wr) || !GetMonitorInfoW(ctx->hmon, &mi)) return TRUE;
    const RECT& r = mi.rcMonitor;
    const LONG ix = std::max(wr.left, r.left), iy = std::max(wr.top, r.top);
    const LONG ax = std::min(wr.right, r.right), ay = std::min(wr.bottom, r.bottom);
    const LONG iw = ax - ix, ih = ay - iy;
    if (iw <= 0 || ih <= 0) return TRUE;
    const LONGLONG cover = static_cast<LONGLONG>(iw) * ih;
    const LONGLONG monA = static_cast<LONGLONG>(r.right - r.left) * (r.bottom - r.top);
    if (cover * 100 < monA * 99) return TRUE;   // <99% coverage — maximised, not fullscreen
    ctx->found = hwnd;
    GetClassNameW(hwnd, ctx->cls, static_cast<int>(sizeof(ctx->cls) / sizeof(ctx->cls[0])));
    ctx->rect = wr;
    return FALSE;   // found — stop enumeration
}

static bool pidHasBorderlessFullscreenOn(DWORD pid, HMONITOR hmon,
                                         char* clsOut, size_t clsCap,
                                         RECT* rectOut) {
    HWND fg = GetForegroundWindow();
    WinCtx ctx{ pid, hmon,
                fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr,
                fg ? GetAncestor(fg, GA_ROOT) : nullptr,
                nullptr, L"", RECT{} };
    EnumWindows(findBorderlessCb, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found && clsOut && clsCap) {
        strncpy_s(clsOut, clsCap, "?", _TRUNCATE);
        WideCharToMultiByte(CP_UTF8, 0, ctx.cls, -1, clsOut,
                            static_cast<int>(clsCap), nullptr, nullptr);
        if (rectOut) *rectOut = ctx.rect;
    }
    return ctx.found != nullptr;
}

// Short ANSI basename of a process image for log lines.
static void logProcessName(HANDLE h, char* out, size_t cap) {
    wchar_t exe[260] = L"";
    DWORD len = 260;
    strcpy_s(out, cap, "?");
    if (QueryFullProcessImageNameW(h, 0, exe, &len)) {
        const wchar_t* base = wcsrchr(exe, L'\\');
        base = base ? base + 1 : exe;
        WideCharToMultiByte(CP_UTF8, 0, base, -1, out,
                            static_cast<int>(cap), nullptr, nullptr);
    }
}

// ───────────────────────── Controller ─────────────────────────
//
// Regulation loop (every TIMER_MS):
//   1. Read GPU temperature.
//   2. Push into rolling history (kept across cap changes — old data ages
//      out naturally).
//   3. Steer the clock cap: dive while hot (every tick), climb reluctantly
//      after sustained cool, hold inside the deadband. See regulateTick().

struct Controller {
    int  targetTemp = 0;      // 0 = off, -1 = uncapped (∞), >0 = target °C

    std::deque<int> history;
    int  maxClock  = 0;       // GPU max SM clock (MHz); 0 = unknown → control disabled
    int  capMhz    = 0;       // last CONFIRMED cap; 0 = uncapped (updated from capDone)
    int  coolTicks = 0;       // consecutive ticks comfortably below target
    int  settle    = 0;       // ticks to hold after a cap change (thermal lag ~15-20s)

    // Voltage-cliff learning: on this card, ~1906MHz runs ~65°C while 1934MHz
    // runs ~80°C — a V/F curve voltage step. Above cliffMhz the temp explodes.
    int  cliffMhz      = 0;   // learned cap ceiling (0 = unknown)
    int  climbFromCap  = 0;   // cap before the pending climb (cliff detection)
    int  climbFromTemp = 0;   // temp before the pending climb
    int  climbChecks   = 0;   // evals since the pending climb

    void reset() { history.clear(); coolTicks = 0; settle = 0;
                   cliffMhz = 0; climbFromCap = 0; climbFromTemp = 0; climbChecks = 0; }

    bool isActive() const { return targetTemp > 0; }

    void addSample(int temp) {
        if (temp < 0) return;
        history.push_back(temp);
        while ((int)history.size() > HISTORY_LEN)
            history.pop_front();
    }

    // Weighted average: recent samples weigh more (linear ramp).
    // This reacts faster to trends than a simple average.
    double weightedAvg() const {
        if (history.empty()) return 0;
        double sum = 0, wsum = 0;
        int i = 0;
        for (int v : history) {
            double w = 1.0 + i;          // oldest=1, newest=n
            sum  += v * w;
            wsum += w;
            i++;
        }
        return sum / wsum;
    }

    int latest() const {
        return history.empty() ? 0 : history.back();
    }

    // Sensing value used for control: midpoint between weighted average and the
    // latest reading when climbing (reacts fast), plain average when falling.
    double effective() const {
        double avg = weightedAvg();
        int    cur = latest();
        return (cur > avg) ? (avg + cur) / 2.0 : avg;
    }

    // Command a new cap (0 = restore defaults via -rgc). The worker applies
    // it; the UI tick updates capMhz from the confirmed result (capDone), so
    // capMhz always reflects the last KNOWN-good state.
    void setCap(int mhz) {
        int now = (capMhz > 0) ? capMhz : maxClock;
        int moved = std::abs(now - mhz);
        settle = (moved >= 90) ? 2 : 1;   // big dives need the long window
        if (mhz > 0) { enqueueCap(mhz); log("clock: %d → %dMHz (step %d)", now, mhz, moved); }
        else         { enqueueCap(-1);  log("clock: %d → uncapped (-rgc)", now); }
    }
};

// ───────────────────────── Application state ─────────────────────────

static NOTIFYICONDATAW g_nid{};

// NIM_MODIFY the live tray icon; if the icon is not actually in the tray
// (initial NIM_ADD lost because the taskbar didn't exist yet at logon, or
// Explorer restarted and the TaskbarCreated broadcast was missed), fall back
// to NIM_ADD. Keeps the icon self-healing instead of invisible forever.
static bool trayRefresh() {
    if (Shell_NotifyIconW(NIM_MODIFY, &g_nid)) return true;
    return Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

// "TaskbarCreated" broadcast, registered in wWinMain: Explorer sends it to
// every top-level window when the taskbar is (re)created — all tray icons
// are then gone and must be re-added.
static UINT g_msgTaskbarCreated = 0;

static HICON           g_hIcon    = nullptr;
static HWND            g_hwnd     = nullptr;
static HMENU           g_hMenu    = nullptr;
static Controller      g_ctrl{};
static std::string     g_gpuName;
static int             g_lastTemp = -1;

// ───────────────────────── Worker-thread bridge ─────────────────────────
// All nvidia-smi and tray/shell calls run on a worker thread: a stalled
// external call (hung nvidia-smi, wedged explorer tray) can never freeze
// the UI thread. runHidden is bounded, and the UI just reads the latest
// published readings.
struct SharedState {
    volatile int  temp         = -1;    // worker → UI: last temperature
    volatile int  defaultPower = 0;     // worker → UI: power limit found at startup (exit restore)
    volatile int  pendingCap   = 0;     // UI → worker: -1 reset, >0 cap, 0 none
    volatile int  capDone      = 0;     // worker → UI: -1 restored, >0 applied, <0 failed
    volatile int  maxClock     = 0;
    volatile int  cpuMode      = CPU_MODE_AUTO;  // UI → worker: governor mode
    volatile int  cpuUsage     = -1;             // worker → UI: total CPU load %
    volatile int  cpuState     = 0;              // worker → UI: applied AC max
    volatile int  gpuUtil      = -1;             // worker → UI: GPU utilisation %
    volatile int  resAuto      = 0;              // UI → worker: resolution governor on
    volatile int  resTargetW   = 2560;           // UI → worker: game (fallback) resolution
    volatile int  resTargetH   = 1440;
    volatile int  resDefaultW  = 0;              // UI → worker: desktop (rest) resolution
    volatile int  resDefaultH  = 0;              //   (learned at startup; 0 = unknown)
    volatile int  resOnPct     = RES_ON_PCT;     // UI → worker: engage threshold %
    volatile int  resEngaged   = 0;              // worker → UI: display currently dropped
    wchar_t       resDisp[128]{};                // UI → worker: pinned display EDID
                                                 // identity (or legacy GDI name); "" = auto
    volatile bool initDone     = false;
    volatile bool trayDirty    = false;
    wchar_t       trayTip[128]{};
    std::string   gpuName;
};
static SharedState      g_shared;
static CRITICAL_SECTION g_cs;
static HANDLE           g_worker    = nullptr;
static HANDLE           g_quitEvent = nullptr;

static void enqueueCap(int capMhz) {   // -1 = restore default clocks
    EnterCriticalSection(&g_cs);
    g_shared.pendingCap = capMhz;
    LeaveCriticalSection(&g_cs);
}

static void requestTrayRefresh(const std::wstring& tip) {
    EnterCriticalSection(&g_cs);
    wcsncpy_s(g_shared.trayTip, tip.c_str(), _TRUNCATE);
    g_shared.trayDirty = true;
    LeaveCriticalSection(&g_cs);
}

// ───────────────────── CPU boost governor ─────────────────────
// Office-cool by default: cap the AC max processor state at 95% (Ryzen:
// Core Performance Boost off → base clock → cool), lift the cap when the
// machine does sustained work. powercfg is a two-step write (setacvalueindex
// alone never takes effect) and runHidden cannot run "&&", so both steps run
// as separate children — then the index is read back to verify.

static int queryCpuUsagePct() {
    static ULONGLONG pIdle = 0, pTotal = 0;   // worker thread only
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return -1;
    ULONGLONG idle  = (ULONGLONG(fi.dwHighDateTime) << 32) | fi.dwLowDateTime;
    ULONGLONG total = ((ULONGLONG(fk.dwHighDateTime) << 32) | fk.dwLowDateTime)
                    + ((ULONGLONG(fu.dwHighDateTime) << 32) | fu.dwLowDateTime);
    if (!pTotal) { pIdle = idle; pTotal = total; return -1; }   // need a delta
    ULONGLONG dI = idle - pIdle, dT = total - pTotal;
    pIdle = idle; pTotal = total;
    return dT ? (int)((100 * (dT - dI)) / dT) : -1;
}

// CPU time of the FOREGROUND process, as % of one core (can exceed 100 for
// multi-thread saturation — clamped). -1 when unknown. This is the game
// detector: office apps barely register here, games pin a core.
static int queryForegroundPct() {
    static ULONGLONG pProc = 0, pWallMs = 0;
    static DWORD     pPid  = 0;                // worker thread only
    ULONGLONG wallMs = GetTickCount64();
    DWORD pid = 0;
    if (HWND hwnd = GetForegroundWindow())
        GetWindowThreadProcessId(hwnd, &pid);
    ULONGLONG proc = 0;
    if (pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            FILETIME fc, fe, fk, fu;
            if (GetProcessTimes(h, &fc, &fe, &fk, &fu))
                proc = ((ULONGLONG(fk.dwHighDateTime) << 32) | fk.dwLowDateTime)
                     + ((ULONGLONG(fu.dwHighDateTime) << 32) | fu.dwLowDateTime);
            CloseHandle(h);
        }
    }
    int pct = -1;
    if (pid && pid == pPid && pProc && pWallMs && wallMs > pWallMs) {
        ULONGLONG dP = proc - pProc, dMs = wallMs - pWallMs;
        pct = (int)std::min<ULONGLONG>(100, (100 * dP) / (dMs * 10000));
    }
    pProc = proc; pPid = pid; pWallMs = wallMs;
    return pct;
}

static bool setAcMaxProcessorState(int pct) {
    std::string set = "powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR"
                      " PROCTHROTTLEMAX " + std::to_string(pct);
    auto a = runHidden(set.c_str());
    auto b = runHidden("powercfg /setactive SCHEME_CURRENT");
    return a.exitCode == 0 && b.exitCode == 0;
}

// Read the AC index back. The output lists "Possible Settings" bounds first
// (their 0x00000000 would poison a naive "first 0x" parse — seen in the log),
// so take the LAST two "0x…" tokens: the index lines are always last, AC
// before DC (DC can be absent on desktops). Locale-independent.
static int queryAcMaxProcessorState() {
    auto r = runHidden("powercfg /query SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMAX");
    size_t dc = r.output.rfind("0x");
    if (dc == std::string::npos) return -1;
    size_t ac = (dc > 0) ? r.output.rfind("0x", dc - 1) : std::string::npos;
    if (ac == std::string::npos) ac = dc;
    return (int)strtoul(r.output.c_str() + ac, nullptr, 16);
}

// Apply the cap (true) or restore full boost (false); verify the readback.
static bool applyCpuState(bool capped) {
    const int pct = capped ? CPU_CAP_PCT : 100;
    bool ok = setAcMaxProcessorState(pct);
    int got = ok ? queryAcMaxProcessorState() : -1;
    if (got == pct) log("cpu: AC max processor state %d%% OK (verified)", pct);
    else            log("cpu: AC max %d%% NOT verified (cmd ok=%d, readback %d%%)",
                        pct, ok, got);
    return got == pct;
}

// Governor state — worker thread only.
struct CpuGov {
    int       want     = 0;    // last seen UI mode (0 = none yet)
    int       applied  = 0;    // last applied AC max: 0 none, 95, 100
    ULONGLONG hotSince = 0, coldSince = 0;
};
static CpuGov g_cpu;

// usage/fg are sampled ONCE per worker iteration (workerMain) and passed in —
// both queries keep delta state, so calling them twice per iteration would
// corrupt the second reading (resTick needs the same foreground signal).
static void cpuTick(int usage, int fg) {
    const int want  = g_shared.cpuMode;

    if (want != g_cpu.want) {
        g_cpu.want = want;
        g_cpu.hotSince = 0;
        // An explicit switch to AUTO applies on the first idle sample —
        // don't make the user wait out the full idle window.
        g_cpu.coldSince = (want == CPU_MODE_AUTO) ? GetTickCount() - CPU_OFF_MS : 0;
        log("cpu: mode → %s", want == CPU_MODE_AUTO ? "auto" : "off");
    }

    if (want == CPU_MODE_OFF) {
        // Stand down; only undo a cap we applied ourselves.
        if (g_cpu.applied == CPU_CAP_PCT && applyCpuState(false))
            g_cpu.applied = 100;
    } else if (usage >= 0) {
        const ULONGLONG now = GetTickCount64();
        // Busy when a heavy app owns the foreground (game) or the whole CPU
        // is loaded (compile, alt-tabbed game). Quiet is keyed on the
        // foreground signal ALONE — total load spikes with desktop background
        // noise, and one noisy sample must never veto the re-cap.
        char fgs[8];
        if (fg >= 0) snprintf(fgs, sizeof(fgs), "%d", fg);
        else         strcpy_s(fgs, "n/a");
        const bool busy = (fg >= FG_ON_PCT) || (usage >= CPU_USAGE_ON_PCT);
        if (busy) {                                // heavy work: head for boost
            if (!g_cpu.hotSince) g_cpu.hotSince = now;         // streak start
            if (g_cpu.applied != 100 && now - g_cpu.hotSince >= CPU_ON_MS) {
                log("cpu: %d%% total / %s%% fg sustained → restoring boost",
                    usage, fgs);
                if (applyCpuState(false)) {
                    g_cpu.applied = 100;
                    g_cpu.coldSince = 0;    // boost FIRED → drop the pending cap
                } else {
                    g_cpu.hotSince = now;   // failed — retry a full window later
                }
            }
        } else {
            g_cpu.hotSince = 0;                            // busy streak ended
            if (fg >= 0 && fg <= FG_OFF_PCT) {             // quiet: head for cap
                if (!g_cpu.coldSince) g_cpu.coldSince = now;   // streak start
                if (g_cpu.applied != CPU_CAP_PCT && now - g_cpu.coldSince >= CPU_OFF_MS) {
                    log("cpu: %d%% total / %s%% fg quiet sustained → capping AC max"
                        " at %d%% (Core Boost off)", usage, fgs, CPU_CAP_PCT);
                    if (applyCpuState(true)) {
                        g_cpu.applied = CPU_CAP_PCT;
                    } else {
                        g_cpu.coldSince = now;  // failed — retry a full window later
                    }
                }
            }
            // In between (mid foreground, or unknown): band — hold, and keep
            // both timers. Blips can't cancel them; only a FIRING transition
            // does. Sporadic spikes therefore can't veto the cap forever.
        }
    }

    EnterCriticalSection(&g_cs);
    g_shared.cpuUsage = usage;
    g_shared.cpuState = g_cpu.applied;
    LeaveCriticalSection(&g_cs);
}

// ───────────────────── Resolution governor ─────────────────────
// Owns the SELECTED display's resolution while enabled (opt-in). ENGAGE:
// sustained heavy GPU load (≥ ResOnPct, default RES_ON_PCT, for RES_ON_MS)
// that can be SOURCED to an application holding a borderless fullscreen
// window on that display → drop to the game resolution. RELEASE: only when that application exits
// (process handle fires) — pauses, alt-tab and menus never flip-flop.
// While NOT dropped, three-signal sustained quiet (see RES_OFF_MS) enforces
// the configured desktop resolution, so any startup/manual odd state
// converges. All display work lives on this worker thread like every other
// blocking external call (a mode switch briefly broadcasts WM_DISPLAYCHANGE).

// Restore the managed display to the configured desktop resolution, falling
// back to its registry default when the configured one is unset or
// rejected. `disp` may be nullptr (pinned display missing / nothing
// attached) — then nothing happens. Used by the worker AND the UI exit path.
static bool restoreToDefault(const LiveDisplay* disp) {
    if (!disp) return false;
    const int dw = g_shared.resDefaultW, dh = g_shared.resDefaultH;
    if (dw > 0 && dh > 0 &&
        applyDisplayMode(disp->devName, dw, dh) == DISP_CHANGE_SUCCESSFUL)
        return true;
    return restoreDisplayMode(disp->devName);
}

// Make the OS display database default for this display match the
// configured desktop resolution WITHOUT touching the current mode
// (CDS_UPDATEREGISTRY | CDS_NORESET — the documented save-without-apply).
// Why: display sleep/wake and monitor re-arrival restore the DATABASE
// default, so a stale one (an old manual 1440p setting, here) makes every
// wake come back at the wrong resolution with resized maximized windows.
// Make the OS display database default for this display match the mode it
// is CURRENTLY running (feature on, not engaged, quiet): display sleep/wake
// and monitor re-arrival restore the DATABASE default, so whatever the user
// leaves the display at becomes its wake mode. Deliberately aligned to the
// CURRENT mode — never to a configured value — so the app cannot fight the
// user's choice for a given panel (e.g. a TV they deliberately run at
// 1080p). Only exact native modes are persisted.
static bool ensureDatabaseDefault(const wchar_t* dev, bool firstCheck) {
    DEVMODEW reg{};
    reg.dmSize = sizeof(reg);
    if (!EnumDisplaySettingsExW(dev, ENUM_REGISTRY_SETTINGS, &reg, 0)) {
        log("res[%ls]: cannot read the Windows default mode", dev);
        return false;
    }
    DEVMODEW cur{};
    if (!getCurrentMode(dev, &cur)) return false;
    if (reg.dmPelsWidth == cur.dmPelsWidth &&
        reg.dmPelsHeight == cur.dmPelsHeight &&
        reg.dmDisplayFrequency == cur.dmDisplayFrequency) {
        if (firstCheck)   // one ground-truth line per session
            log("res[%ls]: Windows default mode %lux%lu @%luHz — matches"
                " the current mode",
                dev, (unsigned long)reg.dmPelsWidth,
                (unsigned long)reg.dmPelsHeight,
                (unsigned long)reg.dmDisplayFrequency);
        return true;   // already aligned — nothing to write
    }
    DEVMODEW dm{};
    if (!findNativeMode(dev, (int)cur.dmPelsWidth, (int)cur.dmPelsHeight, &dm))
        return false;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    const LONG rc = ChangeDisplaySettingsExW(dev, &dm, nullptr,
                                             CDS_UPDATEREGISTRY | CDS_NORESET,
                                             nullptr);
    if (rc == DISP_CHANGE_SUCCESSFUL) {
        log("res[%ls]: Windows default mode aligned to the current mode"
            " %lux%lu @%luHz (database had %lux%lu @%luHz)",
            dev, (unsigned long)cur.dmPelsWidth, (unsigned long)cur.dmPelsHeight,
            (unsigned long)cur.dmDisplayFrequency,
            (unsigned long)reg.dmPelsWidth, (unsigned long)reg.dmPelsHeight,
            (unsigned long)reg.dmDisplayFrequency);
        return true;
    }
    log("res[%ls]: aligning the Windows default mode failed (rc=%ld)", dev, (long)rc);
    return false;
}

// Governor state — worker thread only.
struct ResGov {
    int       want     = -1;   // last seen UI flag (-1 = not seen yet)
    int       lastW = 0, lastH = 0;        // last seen game resolution
    int       lastDefW = 0, lastDefH = 0;  // last seen desktop resolution
    bool      engaged  = false;   // display currently dropped
    bool      badFallback = false;  // driver rejected the game resolution
    bool      badDefault  = false;  // driver rejected the desktop resolution
    bool      naLogged    = false;  // "at/below game res" logged once per load period
    bool      srcLogged   = false;  // "heavy but unsourced" logged once per load period
    bool      missingLogged = false;// pinned display absent — logged once per absence
    bool      deathLogged   = false;// source-exited logged once (restore may retry)
    ULONGLONG hotSince = 0;   // sustained heavy load → drop attempt
    ULONGLONG normSince  = 0; // sustained all-quiet while quiet → enforce desktop res
    // Database-default alignment (wake fix) + engaged drift guard state.
    ULONGLONG dbSync      = 0;   // last database-default check
    ULONGLONG lastDriftFix = 0;  // last mid-game drift re-apply attempt
    bool      driftLogged  = false;
    // Source tracking: the app we dropped FOR. Its lifetime is the ONLY
    // release trigger. The HANDLE (not the pid) survives pid reuse.
    HANDLE    srcProcess = nullptr;
    DWORD     srcPid     = 0;
    char      srcName[64] = "?";
};
static ResGov g_res;

static void endSourceTracking() {
    if (g_res.srcProcess) { CloseHandle(g_res.srcProcess); g_res.srcProcess = nullptr; }
    g_res.srcPid = 0;
    g_res.srcName[0] = '\0';
    g_res.deathLogged = false;
}

// UI-side synchronous restore (IDM_EXIT). Pure GDI — no subprocess, safe on
// the UI thread, same pattern as resetClocks/applyCpuState at exit. Also
// clears the worker's engagement state: at Exit the worker is on its way
// out (quitEvent is set right after the message loop), and leaving it set
// would make the shutdown path double-restore.
static void restoreResolutionNow() {
    if (!g_shared.resEngaged) return;
    std::vector<LiveDisplay> scratch;
    const LiveDisplay* disp = resolveDisplay(g_shared.resDisp, scratch);
    if (!disp) { log("res: exit — target display not present, nothing to restore"); return; }
    if (restoreToDefault(disp)) {
        g_shared.resEngaged = 0;
        g_res.engaged = false;
        endSourceTracking();
        log("res[%ls]: exit → desktop resolution restored", disp->devName);
    } else {
        log("res[%ls]: exit → restore FAILED", disp->devName);
    }
}

static void resTick(int util, int fg) {
    const int want  = g_shared.resAuto;
    const int tw    = g_shared.resTargetW,  th   = g_shared.resTargetH;
    const int dw    = g_shared.resDefaultW, dh   = g_shared.resDefaultH;
    const int onPct = g_shared.resOnPct;   // engage threshold (user-tunable)

    // One-time visibility: if the governor ever sits silent, this line says
    // whether util/fg were even valid when it was supposed to act.
    static bool firstLogged = false;
    if (!firstLogged) {
        firstLogged = true;
        log("resTick: first pass — want=%d util=%d fg=%d cpu=%d pin='%ls'",
            want, util, fg, g_shared.cpuUsage, g_shared.resDisp);
    }

    // Reconcile against the UI's settings (mirrors cpuTick's mode handling).
    if (want != g_res.want || tw != g_res.lastW || th != g_res.lastH ||
        dw != g_res.lastDefW || dh != g_res.lastDefH) {
        const bool first = (g_res.want < 0);
        if (!first)
            log("res: config → %s, game %dx%d, desktop %dx%d",
                want ? "on" : "off", tw, th, dw, dh);
        const bool wasEngaged = g_res.engaged;
        g_res.want = want;
        g_res.lastW = tw;    g_res.lastH = th;
        g_res.lastDefW = dw; g_res.lastDefH = dh;
        g_res.badFallback = false; g_res.badDefault = false;
        g_res.naLogged = false; g_res.srcLogged = false;
        g_res.hotSince = g_res.normSince = 0;
        if (wasEngaged && !want) {
            // Feature off while dropped: hand the display back at the
            // configured desktop resolution (or registry default).
            std::vector<LiveDisplay> scratch;
            if (restoreToDefault(resolveDisplay(g_shared.resDisp, scratch))) {
                g_res.engaged = false;
                endSourceTracking();
                log("res: feature off → desktop resolution restored");
            }
        } else if (wasEngaged) {
            // Game resolution changed while dropped: re-apply in place (one
            // mode switch) instead of restore-then-re-engage (two). The
            // source keeps running — the drop survives.
            std::vector<LiveDisplay> scratch;
            const LiveDisplay* disp = resolveDisplay(g_shared.resDisp, scratch);
            LONG rc = disp ? applyDisplayMode(disp->devName, tw, th)
                           : DISP_CHANGE_FAILED;
            if (rc == DISP_CHANGE_SUCCESSFUL) {
                log("res: game resolution changed → re-applied %dx%d", tw, th);
            } else {
                restoreToDefault(disp);   // never leave it dropped and wrong
                g_res.engaged = false;
                endSourceTracking();
                log("res: game resolution changed → re-apply failed (rc=%ld), desktop restored", rc);
            }
        }
    }

    if (!want || util < 0) return;   // off, or no GPU reading: never act blind

    const ULONGLONG now = GetTickCount64();
    // GPU-quiet drives the normalization window; only GPU noise (something
    // heavy still running) resets it. Foreground spikes are checked at FIRE
    // time instead — a user reading the desktop must not postpone the
    // restore (v1.8.1: the old triple-gate reset made restores wait for a
    // completely still 30s the user never provides).
    const bool utilQuiet = util <= RES_OFF_PCT;

    if (g_res.engaged) {
        // HOLD. The sourced application's lifetime is the only release
        // trigger: pausing, alt-tab, loading screens, fps dips — nothing
        // else flips the resolution back (that was the flip-flop bug).
        const bool sourceAlive = g_res.srcProcess &&
            WaitForSingleObject(g_res.srcProcess, 0) != WAIT_OBJECT_0;
        if (sourceAlive) {
            // Drift guard: a display sleep/wake (or monitor re-arrival)
            // re-applies the OS database default and can reset the runtime
            // mid-game. Engaged means the display IS at game res — enforce.
            if (tw > 0 && !g_res.badFallback &&
                now - g_res.lastDriftFix >= RES_ON_MS) {
                std::vector<LiveDisplay> scratch;
                const LiveDisplay* disp = resolveDisplay(g_shared.resDisp, scratch);
                if (disp) {
                    DEVMODEW cur{};
                    if (getCurrentMode(disp->devName, &cur) &&
                        ((int)cur.dmPelsWidth != tw || (int)cur.dmPelsHeight != th)) {
                        g_res.lastDriftFix = now;
                        if (applyDisplayMode(disp->devName, tw, th)
                            == DISP_CHANGE_SUCCESSFUL) {
                            if (!g_res.driftLogged) {
                                g_res.driftLogged = true;
                                log("res[%ls]: mode drifted to %lux%lu"
                                    " (display wake?) → game resolution"
                                    " re-applied %dx%d",
                                    disp->devName,
                                    (unsigned long)cur.dmPelsWidth,
                                    (unsigned long)cur.dmPelsHeight, tw, th);
                            }
                        }
                    } else {
                        g_res.driftLogged = false;   // clean again
                    }
                }
            }
            return;   // stay dropped — ignore util/fg/CPU entirely
        }

        std::vector<LiveDisplay> scratch;
        const LiveDisplay* disp = resolveDisplay(g_shared.resDisp, scratch);
        if (!disp) {
            if (!g_res.missingLogged) {
                g_res.missingLogged = true;
                log("res: target display %ls not present — restore paused"
                    " until it reappears", g_shared.resDisp);
            }
            return;   // retry next tick
        }
        g_res.missingLogged = false;
        if (!g_res.deathLogged) {
            g_res.deathLogged = true;
            log("res[%ls]: source %s (pid %lu) exited → restoring desktop"
                " resolution %dx%d",
                disp->devName, g_res.srcName[0] ? g_res.srcName : "process",
                g_res.srcPid, dw, dh);
        }
        if (restoreToDefault(disp)) {
            g_res.engaged = false;
            endSourceTracking();
            g_res.normSince = 0;
        }   // else: retry next tick (engaged stays true)
        return;
    }

    // Keep the OS display database aligned with whatever the pinned display
    // is actually running (feature owns the display while on): sleep/wake
    // and monitor re-arrival restore the DATABASE default, so a stale one
    // makes every wake come back wrong. Throttled; silent when aligned.
    if (want && now - g_res.dbSync >= 300000) {
        const bool firstCheck = (g_res.dbSync == 0);
        g_res.dbSync = now;
        std::vector<LiveDisplay> scratch;
        const LiveDisplay* disp = resolveDisplay(g_shared.resDisp, scratch);
        if (disp) ensureDatabaseDefault(disp->devName, firstCheck);
    }

    // Quiet state: two independent watches — sourced-heavy→drop,
    // quiet→enforce desktop res.
    if (util >= onPct) {
        g_res.normSince = 0;                       // load resets the enforce window
        if (!g_res.hotSince) g_res.hotSince = now;
        if (now - g_res.hotSince < RES_ON_MS) return;
        if (g_res.badFallback || g_res.naLogged || g_res.srcLogged) {
            g_res.hotSince = now;   // already refused this load period — retry silently
            return;
        }

        std::vector<LiveDisplay> scratch;
        const LiveDisplay* disp = resolveDisplay(g_shared.resDisp, scratch);
        if (!disp) {
            if (!g_res.missingLogged) {
                g_res.missingLogged = true;
                if (g_shared.resDisp[0])
                    log("res: target display %ls not present — not engaging"
                        " (paused until it reappears, or re-pick in the menu)",
                        g_shared.resDisp);
                else
                    log("res: no displays attached — not engaging");
            }
            g_res.hotSince = now;
            return;
        }
        g_res.missingLogged = false;

        DEVMODEW cur{};
        if (!getCurrentMode(disp->devName, &cur)) {
            log("res[%ls]: cannot read the current mode — not engaging", disp->devName);
            g_res.hotSince = now;   // retry a full window later
            return;
        }
        if ((int)cur.dmPelsWidth <= tw) {
            // Target display already at/below the game resolution — nothing
            // to drop. A TRUE-exclusive fullscreen game changes the mode
            // itself and lands here: exclusive stays self-managed.
            g_res.naLogged = true;
            log("res[%ls]: at %dx%d ≤ game %dx%d — nothing to drop",
                disp->devName, (int)cur.dmPelsWidth, (int)cur.dmPelsHeight, tw, th);
            g_res.hotSince = now;
            return;
        }

        // Source the load: is any SUBSTANTIAL graphics consumer holding a
        // borderless fullscreen on the target display? Deliberately NOT
        // "the single top consumer" — background Chrome/WebGL can out-rank
        // a fullscreen game (observed live: chrome 89% vs game 78%); the
        // game is the one FILLING the screen, so the window decides.
        std::vector<PidUtil> per;
        if (!queryGfxUtilByPid(per)) {
            g_res.srcLogged = true;
            log("res: GPU Engine counters unavailable — cannot source the"
                " load, not dropping");
            g_res.hotSince = now;
            return;
        }
        std::sort(per.begin(), per.end(),
                  [](const PidUtil& a, const PidUtil& b) { return a.pct > b.pct; });
        HMONITOR hmon = monitorForDevice(disp->devName);
        if (!hmon) { g_res.hotSince = now; return; }
        const int srcMin = std::min(GFX_SRC_MIN_PCT, onPct / 2);
        DWORD srcPid = 0; int srcPct = 0;
        char srcCls[64] = "?"; RECT srcRect{};
        for (const auto& pu : per) {
            if (pu.pct < srcMin) break;   // sorted desc — the rest is noise
            char cls[64] = "?";
            RECT wr{};
            if (!pidHasBorderlessFullscreenOn(pu.pid, hmon, cls, sizeof(cls), &wr))
                continue;
            srcPid = pu.pid; srcPct = pu.pct;
            strncpy_s(srcCls, cls, _TRUNCATE);
            srcRect = wr;
            break;
        }
        if (!srcPid) {
            g_res.srcLogged = true;
            if (per.empty()) {
                log("res: GPU %d%% but no graphics consumer found (compute-only"
                    " load — AI inference?) — not dropping", util);
            } else {
                char tops[128] = "";
                const size_t n = std::min<size_t>(per.size(), 3);
                for (size_t i = 0; i < n; i++) {
                    char nm[48] = "?";
                    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                            FALSE, per[i].pid);
                    if (ph) { logProcessName(ph, nm, sizeof(nm)); CloseHandle(ph); }
                    char piece[72];
                    snprintf(piece, sizeof(piece), "%s%s %d%%",
                             i ? ", " : "", nm, per[i].pct);
                    strncat_s(tops, piece, _TRUNCATE);
                }
                log("res: GPU %d%% but none of the loaded processes (%s) is"
                    " the borderless-fullscreen foreground app on %ls —"
                    " background compute/render, not the played game — not dropping",
                    util, tops, disp->devName);
            }
            g_res.hotSince = now;
            return;
        }

        // Lifetime handle: survives pid reuse, signals on exit.
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, srcPid);
        if (!h) {
            log("res: cannot open source pid %lu — cannot track it, not dropping", srcPid);
            g_res.hotSince = now;
            return;
        }
        char exe[64];
        logProcessName(h, exe, sizeof(exe));

        log("res[%ls]: GPU %d%% sustained %ds sourced to %s (pid %lu, %d%% gfx,"
            " borderless fullscreen '%s' %ldx%ld) → game resolution %dx%d → %dx%d",
            disp->devName, util, RES_ON_MS / 1000, exe, srcPid, srcPct,
            srcCls,
            (long)(srcRect.right - srcRect.left),
            (long)(srcRect.bottom - srcRect.top),
            (int)cur.dmPelsWidth, (int)cur.dmPelsHeight, tw, th);
        LONG rc = applyDisplayMode(disp->devName, tw, th);
        if (rc == DISP_CHANGE_SUCCESSFUL) {
            g_res.engaged    = true;
            g_res.srcProcess = h;
            g_res.srcPid     = srcPid;
            strncpy_s(g_res.srcName, exe, _TRUNCATE);
            g_res.hotSince  = 0;
            g_res.normSince = 0;
        } else {
            CloseHandle(h);
            g_res.hotSince = now;   // retry a full window later
            if (rc == DISP_CHANGE_BADMODE) {
                // Permanent for this target: stop retrying until the setting
                // changes (each retry would flicker the display for nothing).
                g_res.badFallback = true;
                log("res[%ls]: %dx%d rejected by the driver (DISP_CHANGE_BADMODE)"
                    " — suspended until the game-resolution setting changes",
                    disp->devName, tw, th);
            } else {
                log("res[%ls]: drop failed (rc=%ld) — retrying next window",
                    disp->devName, rc);
            }
        }
        return;
    }

    // Not heavy.
    g_res.hotSince = 0;
    g_res.naLogged = false;
    g_res.srcLogged = false;

    // 60s quiet-window telemetry: WHY the 30s normalization window keeps
    // missing is invisible in per-tick lines (they sample every 10th tick).
    // One line per minute: util-quiet fraction, the maxima, and how many
    // times GPU noise zeroed a running window.
    {
        static int          mxU = -1, mxF = -1, q = 0, tot = 0, rs = 0;
        static ULONGLONG    t0 = 0;
        tot++;
        if (util > mxU) mxU = util;
        if (fg > mxF) mxF = fg;
        if (utilQuiet) q++;
        else if (g_res.normSince) rs++;   // GPU noise killed a running window
        ULONGLONG n = GetTickCount64();
        if (!t0) t0 = n;
        if (n - t0 >= 60000) {
            log("resTick: 60s — util-quiet %d/%d ticks, maxUtil=%d%% maxFg=%d%%"
                " cpu=%d%%, window-resets=%d",
                q, tot, mxU, mxF, g_shared.cpuUsage, rs);
            mxU = -1; mxF = -1; q = 0; tot = 0; rs = 0; t0 = n;
        }
    }

    if (!utilQuiet) { g_res.normSince = 0; return; }

    if (!g_res.normSince) g_res.normSince = now;
    if (now - g_res.normSince < RES_OFF_MS) return;
    if (g_res.badDefault || dw <= 0 || dh <= 0) return;
    // Fire only into a non-gaming foreground: a windowed game (low GPU util,
    // high foreground CPU) must not get its resolution yanked, and neither
    // should a user deliberately working at a non-default resolution. The
    // window stays expired — fire on the first calm tick.
    if (fg < 0 || fg > RES_OFF_FG_PCT ||
        g_shared.cpuUsage < 0 || g_shared.cpuUsage >= CPU_USAGE_ON_PCT)
        return;

    // Sustained quiet while not dropped: enforce the configured desktop
    // resolution. This is what makes the startup resolution irrelevant.
    if (!g_res.normSince) g_res.normSince = now;
    if (now - g_res.normSince < RES_OFF_MS) return;
    if (g_res.badDefault || dw <= 0 || dh <= 0) return;

    std::vector<LiveDisplay> scratch;
    const LiveDisplay* disp = resolveDisplay(g_shared.resDisp, scratch);
    if (!disp) {
        if (!g_res.missingLogged) {
            g_res.missingLogged = true;
            if (g_shared.resDisp[0]) {
                std::wstring seen;
                for (const auto& d : scratch)
                    seen += L"[" + d.shortName + L" " +
                            std::to_wstring((unsigned)d.w) + L"x" +
                            std::to_wstring((unsigned)d.h) + L"] ";
                log("res: target display %ls not present — worker sees %u"
                    " display(s), ccd=%ld: %ls— enforcement paused until it"
                    " reappears",
                    g_shared.resDisp, (unsigned)scratch.size(),
                    g_enumCcdOk, seen.c_str());
            } else {
                log("res: no displays attached — desktop-resolution"
                    " enforcement paused");
            }
        }
        g_res.normSince = now;
        return;
    }
    g_res.missingLogged = false;

    DEVMODEW cur{};
    if (!getCurrentMode(disp->devName, &cur)) { g_res.normSince = now; return; }
    if (cur.dmPelsWidth == (DWORD)dw && cur.dmPelsHeight == (DWORD)dh) {
        g_res.normSince = 0;   // already at the desktop resolution
        return;
    }
    log("res[%ls]: quiet, at %dx%d ≠ desktop default %dx%d → applying"
        " desktop resolution",
        disp->devName, (int)cur.dmPelsWidth, (int)cur.dmPelsHeight, dw, dh);
    LONG rc = applyDisplayMode(disp->devName, dw, dh);
    if (rc == DISP_CHANGE_SUCCESSFUL) {
        g_res.normSince = 0;
    } else {
        g_res.normSince = now;
        if (rc == DISP_CHANGE_BADMODE) {
            g_res.badDefault = true;
            log("res[%ls]: desktop %dx%d rejected by the driver"
                " (DISP_CHANGE_BADMODE) — suspended until the setting changes",
                disp->devName, dw, dh);
        } else {
            log("res[%ls]: apply desktop resolution failed (rc=%ld)"
                " — retrying next window", disp->devName, rc);
        }
    }
}

// ───────────────────────── Tray icon management ─────────────────────────
// Icon creation + Shell_NotifyIcon are worker-owned (they can block on a
// wedged explorer). The UI only requests a refresh via requestTrayRefresh().

// ───────────────────────── Menu ─────────────────────────

struct Preset {
    UINT         cmdId;
    int          temp;       // 0=off, -1=∞, >0=target
    const wchar_t* label;
};

static const Preset g_presets[] = {
    { IDM_OFF,  0,  L"Off (manual)"        },
    { IDM_T60, 60,  L"60\00B0C"            },
    { IDM_T65, 65,  L"65\00B0C"            },
    { IDM_T70, 70,  L"70\00B0C"            },
    { IDM_T75, 75,  L"75\00B0C"            },
    { IDM_T80, 80,  L"80\00B0C"            },
    { IDM_T85, 85,  L"85\00B0C"            },
    { IDM_T90, 90,  L"90\00B0C"            },
    { IDM_INF, -1,  L"\u221E (max power)"   },
};

// Fallback resolution presets (table-driven like g_presets — extend the
// table, not if-branches). Only ever applied when the current desktop is
// LARGER, so entries must stay below the resolutions users actually run.
struct ResPreset {
    UINT          cmdId;
    int           w, h;
    const wchar_t* label;
};

// Game (fallback) resolution presets — applied under heavy GPU load.
static const ResPreset g_resPresets[] = {
    { IDM_RES_1440P, 2560, 1440, L"2560 \u00D7 1440" },
    { IDM_RES_1080P, 1920, 1080, L"1920 \u00D7 1080" },
};

// Desktop (rest) resolution presets — what the governor restores TO. The
// setting is learned from the target display's Windows-default mode on
// first run; these entries let the user pin it explicitly.
static const ResPreset g_defResPresets[] = {
    { IDM_RES_DEF_4K,    3840, 2160, L"3840 \u00D7 2160" },
    { IDM_RES_DEF_1440P, 2560, 1440, L"2560 \u00D7 1440" },
    { IDM_RES_DEF_1080P, 1920, 1080, L"1920 \u00D7 1080" },
};

// GPU load thresholds that arm the source hunt (persisted ResOnPct). Light
// games vsync-capped at 25-50%; saturated titles at 75-90%.
struct PctPreset {
    UINT           cmdId;
    int            pct;
    const wchar_t* label;
};

static const PctPreset g_pctPresets[] = {
    { IDM_RES_PCT_25, 25, L"25 %  (catch light games)" },
    { IDM_RES_PCT_50, 50, L"50 %" },
    { IDM_RES_PCT_66, 66, L"66 %  (default)" },
    { IDM_RES_PCT_75, 75, L"75 %" },
    { IDM_RES_PCT_90, 90, L"90 %  (saturated only)" },
};

// Live display snapshot taken at menu-open; WM_COMMAND dispatch (also on the
// UI thread) indexes this same vector, so the choice always matches what the
// user actually saw. GDI display numbering can change across reboots and
// hotplugs — that's fine: the menu is rebuilt on every open and shows the
// current truth.
static std::vector<LiveDisplay> g_menuDisplays;

static std::wstring targetLabel() {
    if (g_ctrl.targetTemp == 0)  return L"Off";
    if (g_ctrl.targetTemp == -1) return L"\u221E";
    return std::to_wstring(g_ctrl.targetTemp) + L"\u00B0C";
}

static std::wstring buildTipText() {
    std::wstring s = L"CalmDownComputer";
    if (!g_gpuName.empty()) {
        s += L" - ";
        // Convert GPU name (UTF-8/ANSI) to wide.
        int len = MultiByteToWideChar(CP_UTF8, 0, g_gpuName.c_str(),
                                      (int)g_gpuName.size(), nullptr, 0);
        std::wstring w(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, g_gpuName.c_str(),
                            (int)g_gpuName.size(), &w[0], len);
        s += w;
    }
    s += L"\nTemp: ";
    s += (g_lastTemp >= 0) ? std::to_wstring(g_lastTemp) + L"\u00B0C" : L"--";
    s += L"\nCap: ";
    s += (g_ctrl.capMhz > 0) ? std::to_wstring(g_ctrl.capMhz) + L" MHz" : L"none";
    if (g_ctrl.cliffMhz > 0 && !g_shared.resEngaged)
        s += L" (cliff " + std::to_wstring(g_ctrl.cliffMhz) + L" MHz)";
    s += L"\nTarget: " + targetLabel();
    // CPU governor — kept compact; the tray tooltip caps at 128 chars.
    if (g_shared.cpuMode == CPU_MODE_OFF)
        s += L"\nCPU: off";
    else if (g_shared.cpuState == CPU_CAP_PCT)
        s += L"\nCPU: 95% office";
    else if (g_shared.cpuState == 100)
        s += L"\nCPU: 100% boost";
    else
        s += L"\nCPU: …";
    if (g_shared.cpuUsage >= 0)
        s += L" · " + std::to_wstring(g_shared.cpuUsage) + L"% load";
    // Resolution governor — only while it owns the display (the tray
    // tooltip caps at 128 chars and is already tight; the cliff info is
    // suppressed above to make room).
    if (g_shared.resEngaged)
        s += L"\nRes: " + std::to_wstring(g_shared.resTargetW) + L"×"
           + std::to_wstring(g_shared.resTargetH) + L" (game)";
    return s;
}

static void rebuildMenu() {
    if (g_hMenu) DestroyMenu(g_hMenu);
    g_hMenu = CreatePopupMenu();

    // Status header.
    AppendMenuW(g_hMenu, MF_STRING | MF_DISABLED, 0, buildTipText().c_str());
    AppendMenuW(g_hMenu, MF_SEPARATOR, 0, nullptr);

    // Presets.
    for (const auto& p : g_presets) {
        UINT flags = MF_STRING;
        if (g_ctrl.targetTemp == p.temp)
            flags |= MF_CHECKED;
        AppendMenuW(g_hMenu, flags, p.cmdId, p.label);
    }

    AppendMenuW(g_hMenu, MF_SEPARATOR, 0, nullptr);

    // CPU boost governor toggle.
    UINT cpuAutoFlags = MF_STRING | (g_shared.cpuMode == CPU_MODE_AUTO ? MF_CHECKED : 0);
    UINT cpuOffFlags  = MF_STRING | (g_shared.cpuMode == CPU_MODE_OFF  ? MF_CHECKED : 0);
    AppendMenuW(g_hMenu, cpuAutoFlags, IDM_CPU_AUTO, L"CPU: auto (cool idle, boost on load)");
    AppendMenuW(g_hMenu, cpuOffFlags,  IDM_CPU_OFF,  L"CPU: off (leave at 100%)");

    AppendMenuW(g_hMenu, MF_SEPARATOR, 0, nullptr);

    // Resolution governor: toggle + game/desktop resolution + target display.
    AppendMenuW(g_hMenu,
                MF_STRING | (g_shared.resAuto ? MF_CHECKED : 0),
                IDM_RES_AUTO, L"Display: drop to game res on heavy GPU load");
    {
        HMENU gameSub = CreatePopupMenu();
        for (const auto& rp : g_resPresets) {
            UINT flags = MF_STRING;
            if (g_shared.resTargetW == rp.w && g_shared.resTargetH == rp.h)
                flags |= MF_CHECKED;
            AppendMenuW(gameSub, flags, rp.cmdId, rp.label);
        }
        wchar_t lbl[96];
        swprintf_s(lbl, L"Game resolution (%ux%u)",
                   (unsigned)g_shared.resTargetW, (unsigned)g_shared.resTargetH);
        // DestroyMenu(g_hMenu) at rebuild recursively destroys the submenus.
        AppendMenuW(g_hMenu, MF_POPUP, (UINT_PTR)gameSub, lbl);

        HMENU deskSub = CreatePopupMenu();
        for (const auto& rp : g_defResPresets) {
            UINT flags = MF_STRING;
            if (g_shared.resDefaultW == rp.w && g_shared.resDefaultH == rp.h)
                flags |= MF_CHECKED;
            AppendMenuW(deskSub, flags, rp.cmdId, rp.label);
        }
        swprintf_s(lbl, L"Desktop resolution (%ux%u)",
                   (unsigned)g_shared.resDefaultW, (unsigned)g_shared.resDefaultH);
        AppendMenuW(g_hMenu, MF_POPUP, (UINT_PTR)deskSub, lbl);

        HMENU dispSub = CreatePopupMenu();
        AppendMenuW(dispSub, MF_STRING | (!g_shared.resDisp[0] ? MF_CHECKED : 0),
                    IDM_RES_DISP_AUTO, L"Auto (largest display)");
        g_menuDisplays = enumDisplays();
        const LiveDisplay* pinnedDisp = nullptr;
        for (size_t i = 0; i < g_menuDisplays.size() && i < 16; i++) {
            const auto& d = g_menuDisplays[i];
            UINT flags = MF_STRING;
            if (g_shared.resDisp[0] && displayMatchesPinned(d, g_shared.resDisp)) {
                flags |= MF_CHECKED;
                pinnedDisp = &d;
            }
            AppendMenuW(dispSub, flags, IDM_RES_DISP_BASE + (UINT)i, d.label.c_str());
        }
        if (pinnedDisp)
            swprintf_s(lbl, L"Target display (%ls)", pinnedDisp->shortName.c_str());
        else if (g_shared.resDisp[0])
            swprintf_s(lbl, L"Target display (pinned, not present)");
        else
            swprintf_s(lbl, L"Target display (auto)");
        AppendMenuW(g_hMenu, MF_POPUP, (UINT_PTR)dispSub, lbl);

        HMENU pctSub = CreatePopupMenu();
        for (const auto& pp : g_pctPresets) {
            UINT flags = MF_STRING;
            if (g_shared.resOnPct == pp.pct)
                flags |= MF_CHECKED;
            AppendMenuW(pctSub, flags, pp.cmdId, pp.label);
        }
        swprintf_s(lbl, L"Engage at GPU load (%u%%)", (unsigned)g_shared.resOnPct);
        AppendMenuW(g_hMenu, MF_POPUP, (UINT_PTR)pctSub, lbl);
    }

    AppendMenuW(g_hMenu, MF_SEPARATOR, 0, nullptr);

    // Start at logon toggle.
    UINT startupFlags = MF_STRING;
    if (isStartupTaskEnabled())
        startupFlags |= MF_CHECKED;
    AppendMenuW(g_hMenu, startupFlags, IDM_STARTUP, L"Start at logon");

    AppendMenuW(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_hMenu, MF_STRING, IDM_EXIT, L"Exit");
}

static void showContextMenu(HWND hwnd) {
    rebuildMenu();
    POINT pt;
    GetCursorPos(&pt);

    // Workaround: menu won't dismiss if window isn't foreground.
    SetForegroundWindow(hwnd);

    TrackPopupMenu(g_hMenu,
                   TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hwnd, nullptr);

    PostMessage(hwnd, WM_NULL, 0, 0);
}

// ───────────────────────── Timer handler ─────────────────────────

// Clock-only regulation. Called every tick with a fresh temperature sample.
//   - above target + deadband: proportional dive, err×10MHz clamped 15–150
//                              (the emergency dive is just the clamp max)
//   - below target - deadband: after CLOCK_COOL_TICKS consecutive cool ticks,
//                              climb err×5MHz clamped 15–30; fully recovered → -rgc
//   - inside deadband:         hold (the climb counter resets — climbing
//                              requires SUSTAINED cold, not borderline cold)
// After any cap change the controller settles 1 tick (2 for ≥90MHz steps),
// so the next decision sees the step's thermal effect, not stale heat.
static void regulateTick() {
    if (!g_ctrl.isActive()) return;
    if (g_ctrl.maxClock <= 0) return;   // clock control unavailable

    // Settle window: a cap needs ~15-20s to show its thermal effect.
    // Deciding on pre-effect temperatures caused the 1800MHz overreaction.
    if (g_ctrl.settle > 0) {
        g_ctrl.settle--;
        log("eval: settle (%d ticks left) — letting the cap take effect", g_ctrl.settle);
        return;
    }

    // Cliff detection: if a climb step made the temperature JUMP, we crossed
    // a V/F voltage step — learn the pre-climb cap and never climb past it.
    if (g_ctrl.climbFromCap > 0) {
        int rise = g_ctrl.latest() - g_ctrl.climbFromTemp;
        if (rise >= CLIFF_RISE_C) {
            g_ctrl.cliffMhz = g_ctrl.climbFromCap;
            log("cliff: %d→%dMHz raised temp %d→%d°C — parking at %dMHz",
                g_ctrl.climbFromCap, g_ctrl.capMhz, g_ctrl.climbFromTemp,
                g_ctrl.latest(), g_ctrl.cliffMhz);
            g_ctrl.climbFromCap = 0;
            g_ctrl.climbChecks  = 0;
            if (g_ctrl.capMhz > g_ctrl.cliffMhz)
                g_ctrl.setCap(g_ctrl.cliffMhz);   // we know it's poison — leave NOW
        } else if (++g_ctrl.climbChecks >= 2) {
            g_ctrl.climbFromCap = 0;   // no cliff — the workload just drifted
            g_ctrl.climbChecks  = 0;
        }
    }

    double eff    = g_ctrl.effective();
    double target = static_cast<double>(g_ctrl.targetTemp);
    double err    = eff - target;          // + = too hot

    // Too hot: proportional dive, every settle window.
    if (err > DEADBAND_C) {
        g_ctrl.coolTicks = 0;
        g_ctrl.climbFromCap = 0;   // managing heat — pending cliff check moot
        int step = std::clamp(static_cast<int>(err * CLOCK_KP), CLOCK_STEP_MIN, CLOCK_STEP_MAX);
        int base = (g_ctrl.capMhz > 0) ? g_ctrl.capMhz : g_ctrl.maxClock;
        int newCap = std::max(CLOCK_MIN, base - step);
        if (g_ctrl.capMhz <= 0 || newCap < g_ctrl.capMhz) g_ctrl.setCap(newCap);
        return;
    }

    // Too cool: reluctant climb after sustained cool, also proportional.
    if (err < -DEADBAND_C) {
        if (++g_ctrl.coolTicks < CLOCK_COOL_TICKS) return;
        g_ctrl.coolTicks = 0;
        if (g_ctrl.capMhz <= 0) return;   // nothing to restore
        int step = std::clamp(static_cast<int>(-err * CLOCK_KP_UP), CLOCK_STEP_MIN, CLOCK_STEP_UP_MAX);
        int newCap = g_ctrl.capMhz + step;
        if (g_ctrl.cliffMhz > 0 && newCap > g_ctrl.cliffMhz) {
            if (g_ctrl.capMhz >= g_ctrl.cliffMhz) return;   // parked at the cliff
            newCap = g_ctrl.cliffMhz;                        // approach, don't cross
        }
        if (newCap >= g_ctrl.maxClock) { g_ctrl.setCap(0); return; }
        g_ctrl.climbFromCap  = g_ctrl.capMhz;   // arm cliff detection
        g_ctrl.climbFromTemp = g_ctrl.latest();
        g_ctrl.climbChecks   = 0;
        g_ctrl.setCap(newCap);
        return;
    }

    // Deadband: hold, and require fresh sustained cold before climbing.
    g_ctrl.coolTicks = 0;
}

// ───────────────────────── Worker thread ─────────────────────────
// Owns every blocking external call: nvidia-smi queries/commands and tray
// painting. The UI thread never blocks on the outside world.
static DWORD WINAPI workerMain(LPVOID) {
    // In-process NVML first — it makes every tick below ~free. Startup
    // discovery deliberately stays on the subprocess path (three spawns per
    // session) so queryMaxClock keeps its exact clocks.max.sm semantics.
    log("NVML: %s", nvmlEnsure()
        ? "direct in-process access active (nvidia-smi subprocess fallback retained)"
        : "unavailable — readings and commands use the nvidia-smi subprocess");

    // --- Startup discovery ---
    std::string name = queryGpuName();
    int mclk = queryMaxClock();
    int pl   = queryPowerLimit();   // informational; never regulated, restored on exit
    {
        EnterCriticalSection(&g_cs);
        g_shared.gpuName      = name;
        g_shared.maxClock     = mclk;
        g_shared.defaultPower = (pl > 0) ? pl : 270;
        g_shared.initDone     = true;
        LeaveCriticalSection(&g_cs);
    }
    g_gpuName = name;
    log("GPU: %s", name.c_str());
    log("Power limit at startup: %dW (left alone; restored on exit)", g_shared.defaultPower);
    log("Max SM clock: %sMHz", mclk > 0 ? std::to_string(mclk).c_str() : "unavailable — clock control disabled");

    // Establish a known-uncapped state: any leftover/manual -lgc lock would
    // make "cap=none" a lie and the first dive could land above the real cap.
    resetClocks();

    // One-time rename migration: delete the pre-rename logon task so it
    // stops failing at every logon while pointing at the old exe.
    if (runHidden("schtasks /query /tn CalmDownGPU /fo csv /nh").exitCode == 0) {
        auto del = runHidden("schtasks /delete /tn CalmDownGPU /f");
        log("startup: removed legacy CalmDownGPU logon task (exit=%lu)", del.exitCode);
    }

    // Cached icons for the CPU-state blink: the GPU temp icon (lastIcon)
    // alternates with a CPU-state icon (blue/purple + frozen load %) on
    // wall-clock phases. One Shell_NotifyIcon per phase change, repaint only
    // once per phase — GDI work stays out of the hot loop. (Real CPU temp is
    // unavailable: this board exposes no ACPI thermal zone, and Ryzen SMN
    // reads need a kernel driver.)
    HICON lastIcon         = nullptr;   // current GPU temp icon
    HICON cpuCool          = nullptr;   // blue   — AC max capped at 95%
    HICON cpuBoost         = nullptr;   // purple — boost restored (100%)
    std::string cpuCoolTxt, cpuBoostTxt;    // load % painted into each icon
    int   lastSeenCpuState = 0;         // to announce state changes instantly
    ULONGLONG paintPhase   = 0;         // phase the frozen load was taken in
    int       paintUsage   = -1;
    ULONGLONG lastTrayEnsure = GetTickCount64();   // periodic tray self-heal
    while (WaitForSingleObject(g_quitEvent, 0) != WAIT_OBJECT_0) {
        const ULONGLONG tickStart = GetTickCount64();   // pacing anchor
        int util = -1;
        int temp = queryTempUtil(&util);

        // Pick up commands from the UI.
        int pendC = 0;
        {
            EnterCriticalSection(&g_cs);
            pendC = g_shared.pendingCap;   g_shared.pendingCap = 0;
            LeaveCriticalSection(&g_cs);
        }
        if (pendC == -1) {
            resetClocks();
            EnterCriticalSection(&g_cs); g_shared.capDone = -1; LeaveCriticalSection(&g_cs);
        } else if (pendC > 0) {
            if (setClockCap(pendC)) {
                EnterCriticalSection(&g_cs); g_shared.capDone = pendC; LeaveCriticalSection(&g_cs);
            } else {
                EnterCriticalSection(&g_cs); g_shared.capDone = -pendC; LeaveCriticalSection(&g_cs);  // negative = failed
            }
        }
        // CPU boost governor + resolution governor — sensing and every
        // blocking external call live on this thread. usage/fg are sampled
        // once per iteration and shared by both governors.
        const int usage = queryCpuUsagePct();
        const int fg    = queryForegroundPct();
        cpuTick(usage, fg);
        resTick(util, fg);

        // Publish readings for the UI tick.
        {
            EnterCriticalSection(&g_cs);
            g_shared.temp       = temp;
            g_shared.gpuUtil    = util;
            g_shared.resEngaged = g_res.engaged ? 1 : 0;
            LeaveCriticalSection(&g_cs);
        }

        // Tray refresh (Shell_NotifyIcon can block on a wedged explorer).
        bool tray = false;
        wchar_t tip[128] = L"";
        {
            EnterCriticalSection(&g_cs);
            tray = g_shared.trayDirty;
            if (tray) wcsncpy_s(tip, g_shared.trayTip, _TRUNCATE);
            LeaveCriticalSection(&g_cs);
        }
        // Icon policy: while the CPU governor is engaged, alternate GPU temp
        // colour ↔ CPU state colour on fixed wall-clock phases — independent
        // of the 10s temp-refresh race. Fresh temp readings are always
        // cached; they become visible on the next temp phase (the tooltip
        // carries the live number in the meantime). Post only on change.
        const ULONGLONG nowMs = GetTickCount64();
        const int st = g_shared.cpuState;
        const bool stateChanged = (st != lastSeenCpuState);
        lastSeenCpuState = st;
        if (tray) {
            HICON h = createTempIcon(temp);
            if (h) {
                if (lastIcon) DestroyIcon(lastIcon);
                lastIcon = h;                 // shown on the next temp phase
            }
            EnterCriticalSection(&g_cs); g_shared.trayDirty = false; LeaveCriticalSection(&g_cs);
        }
        HICON want = lastIcon;
        if (g_shared.cpuMode == CPU_MODE_AUTO &&
            (st == CPU_CAP_PCT || st == 100)) {
            // A state change always shows its colour immediately.
            const bool cpuPhase = stateChanged || ((nowMs / TRAY_BLINK_MS) & 1);
            if (cpuPhase) {
                // Freeze the load at phase entry: one repaint per phase, not
                // one per worker iteration.
                const ULONGLONG phase = nowMs / TRAY_BLINK_MS;
                if (stateChanged || phase != paintPhase) {
                    paintPhase = phase;
                    paintUsage = g_shared.cpuUsage;
                }
                HICON*       slot    = (st == CPU_CAP_PCT) ? &cpuCool   : &cpuBoost;
                std::string* slotTxt = (st == CPU_CAP_PCT) ? &cpuCoolTxt : &cpuBoostTxt;
                const COLORREF col    = (st == CPU_CAP_PCT) ? RGB(25, 100, 230)
                                                            : RGB(150, 60, 210);
                const std::string wantTxt = tempText(paintUsage);
                if (!*slot || *slotTxt != wantTxt) {
                    HICON nh = createStatusIcon(col, wantTxt);
                    if (nh) {
                        if (*slot) DestroyIcon(*slot);
                        *slot = nh;
                        *slotTxt = wantTxt;
                    }
                }
                want = *slot;
            }
        }
        if (want && want != g_nid.hIcon) {
            if (tip[0]) wcsncpy_s(g_nid.szTip, tip, _TRUNCATE);
            g_nid.hIcon = want;
            trayRefresh();   // falls back to NIM_ADD when the icon is gone
        }
        // Periodic self-heal: even with no icon change pending, make sure the
        // icon still exists in the tray (covers a missed TaskbarCreated).
        if (nowMs - lastTrayEnsure >= TRAY_ENSURE_MS) {
            lastTrayEnsure = nowMs;
            trayRefresh();
        }
        // Exact 1 Hz pacing. The old loop slept a fixed 200ms and relied on
        // the nvidia-smi subprocess burning 0.5-1s of wall time per tick;
        // with NVML a tick takes microseconds, so that pacing would have
        // spun at ~5 ticks/s for nothing. A fixed 1s period keeps every
        // governor's wall-clock windows (10s/30s/45s) sampling at the
        // cadence their constants assume. Waiting on the quit event (not
        // Sleep) keeps Exit responsive.
        const ULONGLONG elapsed = GetTickCount64() - tickStart;
        const DWORD wait = (elapsed < WORKER_TICK_MS)
            ? static_cast<DWORD>(WORKER_TICK_MS - elapsed) : 0;
        WaitForSingleObject(g_quitEvent, wait);
    }
    // Back-door shutdown (not the Exit menu item — that restores on the UI
    // thread): never leave the display dropped.
    if (g_res.engaged) {
        std::vector<LiveDisplay> scratch;
        if (restoreToDefault(resolveDisplay(g_shared.resDisp, scratch)))
            log("res: shutdown → desktop resolution restored");
    }
    endSourceTracking();
    return 0;
}

static void onTimerTick() {
    // Snapshot the worker's latest readings.
    int temp, util;
    {
        EnterCriticalSection(&g_cs);
        temp = g_shared.temp;
        util = g_shared.gpuUtil;
        LeaveCriticalSection(&g_cs);
    }
    if (!g_shared.initDone) { log("tick: waiting for GPU discovery"); return; }

    g_lastTemp = temp;
    g_ctrl.maxClock = g_shared.maxClock;
    std::string capS = (g_ctrl.capMhz > 0) ? std::to_string(g_ctrl.capMhz) + "MHz" : "none";
    log("tick: temp=%d util=%d%% target=%d cap=%s", temp, util,
        g_ctrl.targetTemp, capS.c_str());

    // Apply the worker's confirmed clock-cap result.
    int done;
    {
        EnterCriticalSection(&g_cs);
        done = g_shared.capDone;
        g_shared.capDone = 0;
        LeaveCriticalSection(&g_cs);
    }
    if (done == -1)      { g_ctrl.capMhz = 0; log("clock: restored defaults"); }
    else if (done > 0)   g_ctrl.capMhz = done;
    else if (done < -1)  log("clock: cap %dMHz FAILED — keeping previous state", -done);

    // Display refresh is painted by the worker.
    requestTrayRefresh(buildTipText());

    if (temp < 0) {
        log("tick: failed to read temperature");
        return;  // can't regulate without valid readings
    }

    // ∞ / Off: make sure clocks are uncapped, then there is nothing to do.
    if (g_ctrl.targetTemp <= 0) {
        if (g_ctrl.capMhz > 0) g_ctrl.setCap(0);
        return;
    }

    // Active regulation (clock-only).
    g_ctrl.addSample(temp);
    regulateTick();
}

// ───────────────────────── Window procedure ─────────────────────────

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_TIMER:
        if (wp == ID_TIMER)
            onTimerTick();
        break;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
            showContextMenu(hwnd);
        break;

    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        for (const auto& p : g_presets) {
            if (id == p.cmdId) {
                g_ctrl.targetTemp = p.temp;
                g_ctrl.reset();
                saveTargetTemp(p.temp);
                log("menu: target set to %s", p.temp == -1 ? "INF" :
                    p.temp == 0 ? "OFF" : std::to_string(p.temp).c_str());

                if (p.temp <= 0 && g_ctrl.capMhz > 0) {
                    // ∞ / Off — full clocks again (power is never touched).
                    g_ctrl.setCap(0);
                }
                // Force an immediate reading for instant feedback.
                onTimerTick();
                return 0;
            }
        }
        if (id == IDM_CPU_AUTO || id == IDM_CPU_OFF) {
            bool on = (id == IDM_CPU_AUTO);
            g_shared.cpuMode = on ? CPU_MODE_AUTO : CPU_MODE_OFF;
            saveCpuAuto(on);
            log("menu: CPU governor %s", on ? "auto" : "off");
            return 0;
        }
        if (id == IDM_RES_AUTO) {
            bool on = !g_shared.resAuto;   // toggle
            g_shared.resAuto = on ? 1 : 0;
            saveResAuto(on);
            log("menu: resolution governor %s", on ? "on" : "off");
            return 0;
        }
        for (const auto& rp : g_resPresets) {
            if (id == rp.cmdId) {
                g_shared.resTargetW = rp.w;
                g_shared.resTargetH = rp.h;
                saveResFallback(rp.w, rp.h);
                log("menu: game resolution %dx%d", rp.w, rp.h);
                return 0;
            }
        }
        for (const auto& rp : g_defResPresets) {
            if (id == rp.cmdId) {
                g_shared.resDefaultW = rp.w;
                g_shared.resDefaultH = rp.h;
                saveResDefault(rp.w, rp.h);
                log("menu: desktop resolution %dx%d", rp.w, rp.h);
                return 0;
            }
        }
        for (const auto& pp : g_pctPresets) {
            if (id == pp.cmdId) {
                g_shared.resOnPct = pp.pct;
                saveResOnPct(pp.pct);
                log("menu: engage threshold → %d%% GPU", pp.pct);
                return 0;
            }
        }
        if (id == IDM_RES_DISP_AUTO) {
            EnterCriticalSection(&g_cs);
            g_shared.resDisp[0] = L'\0';
            LeaveCriticalSection(&g_cs);
            saveResDisp(L"");
            log("menu: target display → auto (largest)");
            return 0;
        }
        if (id >= IDM_RES_DISP_BASE && id < IDM_RES_DISP_BASE + 16) {
            size_t idx = id - IDM_RES_DISP_BASE;
            if (idx < g_menuDisplays.size()) {
                const auto& d = g_menuDisplays[idx];
                // Pin by the CCD friendly name (the panel's EDID product name
                // — stable and human-readable); fall back to the GDI name
                // only for nameless oddities.
                std::wstring idStr = d.shortName.empty() ? std::wstring(d.devName)
                                                         : d.shortName;
                EnterCriticalSection(&g_cs);
                wcsncpy_s(g_shared.resDisp, idStr.c_str(), _TRUNCATE);
                LeaveCriticalSection(&g_cs);
                saveResDisp(idStr.c_str());
                log("menu: target display → %ls", idStr.c_str());
            }
            return 0;
        }
        if (id == IDM_STARTUP) {
            if (isStartupTaskEnabled())
                deleteStartupTask();
            else
                createStartupTask();
            return 0;
        }
        if (id == IDM_EXIT) {
            // Restore synchronously so it lands even if the worker is gone.
            resetClocks();
            setPowerLimit(g_shared.defaultPower > 0 ? g_shared.defaultPower : 270);
            if (g_shared.cpuState == CPU_CAP_PCT)
                applyCpuState(false);   // undo the CPU boost cap
            restoreResolutionNow();     // undo the dropped display resolution
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        // Explorer (re)created the taskbar — all tray icons were destroyed;
        // re-add ours. Plain NIM_ADD is correct here: the tray is fresh, the
        // icon cannot already exist. (Guard for registration failure.)
        if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
            Shell_NotifyIconW(NIM_ADD, &g_nid);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

// ───────────────────────── WinMain ─────────────────────────

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Prevent multiple instances.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"CalmDownComputer_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    logOpen();

    // Worker thread owns all nvidia-smi + tray work (see workerMain).
    InitializeCriticalSection(&g_cs);
    g_quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual-reset
    g_shared.cpuMode = loadCpuAuto() ? CPU_MODE_AUTO : CPU_MODE_OFF;
    log("CPU governor: %s", g_shared.cpuMode == CPU_MODE_AUTO ? "auto" : "off");
    g_shared.resAuto    = loadResAuto() ? 1 : 0;
    g_shared.resTargetW = loadResW();
    g_shared.resTargetH = loadResH();
    g_shared.resOnPct   = loadResOnPct();
    // Pinned target display; empty = auto (largest current display — on
    // this machine the real monitor, never the virtual streaming ones).
    // Pins are CCD friendly names; legacy formats still resolve, EXCEPT the
    // MONITOR\<pnp>\{guid} EDID-identity format: it read the GDI monitor
    // sub-device, which served a stale node here (wrong model after display
    // churn). Retire those to auto — the user re-picks in the menu.
    {
        std::wstring disp = loadResDisp();
        if (wcsncmp(disp.c_str(), L"MONITOR\\", 8) == 0) {
            log("res: retiring EDID-format pin (%ls) — read from the unreliable"
                " GDI monitor node; re-pick Target display in the menu",
                disp.c_str());
            disp.clear();
            saveResDisp(L"");
        }
        EnterCriticalSection(&g_cs);
        wcsncpy_s(g_shared.resDisp, disp.c_str(), _TRUNCATE);
        LeaveCriticalSection(&g_cs);
    }
    // One ground-truth dump per session: what the app actually enumerates.
    {
        std::vector<LiveDisplay> all = enumDisplays();
        std::wstring list;
        for (const auto& d : all) {
            wchar_t one[160];
            swprintf_s(one, L"%ls · %ux%u%s | ", d.shortName.c_str(),
                       (unsigned)d.w, (unsigned)d.h,
                       d.primary ? L" [primary]" : L"");
            list += one;
        }
        if (list.empty()) log("res: no displays attached");
        else              log("res: displays: %ls", list.c_str());

        // And warn immediately when a pin resolves to nothing — otherwise a
        // dangling pin sits silent until the first engage attempt.
        EnterCriticalSection(&g_cs);
        wchar_t pin[128];
        wcsncpy_s(pin, g_shared.resDisp, _TRUNCATE);
        LeaveCriticalSection(&g_cs);
        if (pin[0]) {
            bool found = false;
            for (const auto& d : all)
                if (displayMatchesPinned(d, pin)) { found = true; break; }
            if (!found)
                log("res: pinned target %ls not present right now — governor"
                    " idle until it appears (re-pick in the menu if renamed)",
                    pin);
        }
    }
    // Desktop (rest) resolution: learned once from the target display's
    // Windows-default mode, then owned as a setting like everything else.
    int defW = regReadDword(REG_KEY, L"ResDefaultW", -1);
    int defH = regReadDword(REG_KEY, L"ResDefaultH", -1);
    if (defW <= 0 || defH <= 0) {
        std::vector<LiveDisplay> scratch;
        const LiveDisplay* d = resolveDisplay(g_shared.resDisp, scratch);
        if (d) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(d->devName, ENUM_REGISTRY_SETTINGS, &dm)
                && dm.dmPelsWidth && dm.dmPelsHeight) {
                defW = static_cast<int>(dm.dmPelsWidth);
                defH = static_cast<int>(dm.dmPelsHeight);
                saveResDefault(defW, defH);
                log("res: learned desktop resolution %dx%d from %ls",
                    defW, defH, d->devName);
            } else {
                log("res: could not learn the desktop resolution — pick it in"
                    " the menu (Desktop resolution)");
            }
        } else {
            log("res: target display not found at startup — desktop resolution"
                " unset, pick it in the menu");
        }
    }
    g_shared.resDefaultW = defW;
    g_shared.resDefaultH = defH;
    log("Resolution governor: %s (game %ux%u, desktop %ux%u, display %s,"
        " engage %d%%)",
        g_shared.resAuto ? "on" : "off",
        g_shared.resTargetW, g_shared.resTargetH,
        g_shared.resDefaultW, g_shared.resDefaultH,
        g_shared.resDisp[0] ? "pinned" : "auto",
        g_shared.resOnPct);
    g_worker = CreateThread(nullptr, 0, workerMain, nullptr, 0, nullptr);

    // --- Restore last target ---
    g_ctrl.targetTemp = loadTargetTemp();
    log("Restored target: %d", g_ctrl.targetTemp);

    // --- Register window class ---
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = L"CalmDownComputer";
    RegisterClassW(&wc);

    // Registered "TaskbarCreated" message: fires when Explorer's taskbar is
    // (re)created — at logon this can happen AFTER this app auto-starts.
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // Hidden tool window (not message-only: SetForegroundWindow must work).
    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"CalmDownComputer", L"CalmDownComputer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, hInst, nullptr);

    // --- Tray icon ---
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = g_hwnd;
    g_nid.uID    = IDI_APP;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // Branded icon until the first temperature reading arrives (worker takes
    // over with the dynamic temp icon). LoadIcon returns a shared icon — do
    // not destroy.
    g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    if (!g_nid.hIcon) g_nid.hIcon = createTempIcon(-1);
    wcscpy_s(g_nid.szTip, L"CalmDownComputer — starting…");
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
        log("tray: initial NIM_ADD failed (taskbar not up yet?) — TaskbarCreated/refresh will re-add");

    // --- Timer ---
    SetTimer(g_hwnd, ID_TIMER, TIMER_MS, nullptr);

    // Immediate first reading (after a short delay to let the tray settle).
    SetTimer(g_hwnd, ID_TIMER + 1, 2000, nullptr);  // one-shot-ish

    // --- Message loop ---
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // Handle our one-shot startup timer.
        if (msg.message == WM_TIMER && msg.wParam == ID_TIMER + 1) {
            KillTimer(g_hwnd, ID_TIMER + 1);
            onTimerTick();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Shutdown: stop the worker, then leave.
    SetEvent(g_quitEvent);
    if (g_worker) { WaitForSingleObject(g_worker, 3000); CloseHandle(g_worker); }
    nvmlTeardown();
    CloseHandle(g_quitEvent);
    DeleteCriticalSection(&g_cs);
    if (hMutex) CloseHandle(hMutex);
    log("CalmDownComputer exiting");
    return 0;
}
