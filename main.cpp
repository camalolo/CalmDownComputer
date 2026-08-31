// CalmDownComputer - Windows tray app that keeps the whole machine cool.
// GPU: regulates temperature by clock capping (nvidia-smi -lgc).
// CPU: caps the AC "Maximum processor state" at 95% while idle — on Ryzen,
//      anything below 100% disables Core Performance Boost (5600X: pinned at
//      3.7GHz base instead of 4.65GHz boost, much cooler under office load) —
//      and restores 100% as soon as a sustained load (a game) shows up.
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

#include <windows.h>
#include <shellapi.h>

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
    fprintf(f, "%02d:%02d:%02d ",
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

// ───────────────────────── nvidia-smi wrappers ─────────────────────────

// Returns °C, or -1 on failure.
static int queryTemp() {
    auto r = runHidden("nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits -i 0");
    auto s = trim(r.output);
    if (s.empty()) { log("queryTemp: empty response (exit=%lu)", r.exitCode); return -1; }
    try { int t = std::stoi(s); return t; }
    catch (...) { log("queryTemp: parse failed: %s", s.c_str()); return -1; }
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

static void cpuTick() {
    const int usage = queryCpuUsagePct();
    const int fg    = queryForegroundPct();
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
    if (g_ctrl.cliffMhz > 0)
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
    while (WaitForSingleObject(g_quitEvent, 0) != WAIT_OBJECT_0) {
        int temp = queryTemp();

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
        // CPU boost governor — sensing + powercfg live on this thread.
        cpuTick();

        // Publish readings for the UI tick.
        {
            EnterCriticalSection(&g_cs);
            g_shared.temp = temp;
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
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        }
        Sleep(200);   // light pacing; the queries above take ~0.5-1s themselves
    }
    return 0;
}

static void onTimerTick() {
    // Snapshot the worker's latest readings.
    int temp;
    {
        EnterCriticalSection(&g_cs);
        temp = g_shared.temp;
        LeaveCriticalSection(&g_cs);
    }
    if (!g_shared.initDone) { log("tick: waiting for GPU discovery"); return; }

    g_lastTemp = temp;
    g_ctrl.maxClock = g_shared.maxClock;
    std::string capS = (g_ctrl.capMhz > 0) ? std::to_string(g_ctrl.capMhz) + "MHz" : "none";
    log("tick: temp=%d target=%d cap=%s", temp, g_ctrl.targetTemp, capS.c_str());

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
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
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
    Shell_NotifyIconW(NIM_ADD, &g_nid);

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
    CloseHandle(g_quitEvent);
    DeleteCriticalSection(&g_cs);
    if (hMutex) CloseHandle(hMutex);
    log("CalmDownComputer exiting");
    return 0;
}
