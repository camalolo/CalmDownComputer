// CalmDownGPU - Windows tray app that regulates GPU temperature by clock capping.
// Uses nvidia-smi to query temperature and lock core clocks (-lgc).
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
//   • Above target: -30MHz per tick (first engagement -150MHz)
//   • ≥6°C over target: emergency dive, -150MHz per tick
//   • Below target: +15MHz only after ~60s of sustained cool
//   The power limit is left alone (read once at startup, restored on exit).

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
    dirW += L"CalmDownGPU.log";
    WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), -1, g_logPath, MAX_PATH, nullptr, nullptr);

    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") != 0 || !f) {
        GetTempPathA(MAX_PATH, g_logPath);
        strcat_s(g_logPath, "CalmDownGPU.log");
    } else {
        fclose(f);
    }
    // Write a session separator.
    f = nullptr;
    if (fopen_s(&f, g_logPath, "a") == 0 && f) {
        fprintf(f, "\n══════════════════════════════════════════\n");
        fclose(f);
    }
    log("CalmDownGPU starting → log: %s", g_logPath);
}

// ───────────────────────── Constants ─────────────────────────

#define WM_TRAYICON     (WM_USER + 1)
#define ID_TIMER         1
#define TIMER_MS         10000       // 10 seconds
#define HISTORY_LEN      6           // rolling window: 6 × 10s = 60s
#define DEADBAND_C       1.5         // ±1.5°C: don't adjust within this band

// Clock-only control constants. The card enforces power THROUGH clocks
// anyway; clock caps apply instantly, verify against clocks.sm under load,
// and give continuous authority all the way down (no 100W floor).
#define CLOCK_STEP_DOWN        30    // MHz per tick while above target
#define CLOCK_STEP_DOWN_FIRST  150   // first dive from uncapped: get near the cliff fast
#define CLOCK_STEP_UP          15    // MHz per restore step (reluctant climb)
#define CLOCK_COOL_TICKS       6     // consecutive cool ticks before a raise (~60s)
#define CLOCK_EMERGENCY_STEP   150   // MHz dive per tick when ≥6°C over target
#define CLOCK_MIN              300   // never cap below this (usability floor)
#define CLOCK_LOW              210   // low end of the -lgc range (idle clock, MHz)

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
    IDM_STARTUP = 2500,   // Start at logon toggle
    IDM_EXIT    = 3000,
};

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

static HICON createTempIcon(int temp) {
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

    // Background colour by temperature severity.
    COLORREF bg;
    if (temp < 0)        bg = RGB(70, 70, 70);     // grey — error
    else if (temp < 60)  bg = RGB(30, 140, 70);    // green
    else if (temp < 70)  bg = RGB(80, 170, 50);    // light-green
    else if (temp < 75)  bg = RGB(180, 170, 30);   // yellow
    else if (temp < 80)  bg = RGB(210, 130, 30);   // orange
    else if (temp < 85)  bg = RGB(210, 80, 30);    // dark-orange
    else                 bg = RGB(200, 40, 40);    // red

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

    // Temperature text.
    std::string text;
    if (temp < 0)        text = "--";
    else if (temp > 99)  text = "99";
    else                 text = std::to_string(temp);

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

// ───────────────────────── Scheduled task helpers ─────────────────────────

static const char* TASK_NAME = "CalmDownGPU";

// Returns true if the "CalmDownGPU" scheduled task exists.
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

static const wchar_t* REG_KEY = L"Software\\CalmDownGPU";

static void saveTargetTemp(int temp) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"TargetTemp", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&temp), sizeof(temp));
        RegCloseKey(hKey);
    }
}

static int loadTargetTemp() {
    HKEY hKey;
    int temp = 0;  // default: Off
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 0, size = sizeof(val);
        if (RegQueryValueExW(hKey, L"TargetTemp", nullptr, nullptr,
                             reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS)
            temp = static_cast<int>(val);
        RegCloseKey(hKey);
    }
    return temp;
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

    void reset() { history.clear(); coolTicks = 0; settle = 0; }

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
        int old = (capMhz > 0) ? capMhz : maxClock;
        settle = 2;   // never act again until the cap's thermal effect is visible
        if (mhz > 0) { enqueueCap(mhz); log("clock: %d → %dMHz", old, mhz); }
        else         { enqueueCap(-1);  log("clock: %d → uncapped (-rgc)", old); }
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
    std::wstring s = L"CalmDownGPU";
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
    s += L"\nClock cap: ";
    s += (g_ctrl.capMhz > 0) ? std::to_wstring(g_ctrl.capMhz) + L" MHz" : L"none";
    s += L"\nTarget: " + targetLabel();
    if (g_ctrl.isActive() && !g_ctrl.history.empty())
        s += L"  (avg " + std::to_wstring(static_cast<int>(g_ctrl.weightedAvg())) + L"\u00B0C)";
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
//   - ≥6°C over target:        emergency dive, -150MHz per tick, no gating
//   - above target + deadband: steady dive, -30MHz per tick
//                              (first engagement from uncapped: -150MHz)
//   - below target - deadband: after CLOCK_COOL_TICKS consecutive cool ticks
//                              +15MHz; fully recovered → -rgc
//   - inside deadband:         hold (the climb counter resets — climbing
//                              requires SUSTAINED cold, not borderline cold)
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

    double eff    = g_ctrl.effective();
    double target = static_cast<double>(g_ctrl.targetTemp);
    int    cur    = g_ctrl.latest();

    // Emergency dive.
    if (cur >= target + 6) {
        int newCap = (g_ctrl.capMhz > 0 ? g_ctrl.capMhz : g_ctrl.maxClock) - CLOCK_EMERGENCY_STEP;
        newCap = std::max(CLOCK_MIN, newCap);
        if (g_ctrl.capMhz <= 0 || newCap < g_ctrl.capMhz) g_ctrl.setCap(newCap);
        return;
    }

    // Too hot: steady dive, every tick.
    if (eff > target + DEADBAND_C) {
        g_ctrl.coolTicks = 0;
        int base = (g_ctrl.capMhz > 0) ? g_ctrl.capMhz : g_ctrl.maxClock;
        int step = (g_ctrl.capMhz > 0) ? CLOCK_STEP_DOWN : CLOCK_STEP_DOWN_FIRST;
        int newCap = std::max(CLOCK_MIN, base - step);
        if (g_ctrl.capMhz <= 0 || newCap < g_ctrl.capMhz) g_ctrl.setCap(newCap);
        return;
    }

    // Too cool: reluctant climb after sustained cool.
    if (eff < target - DEADBAND_C) {
        if (++g_ctrl.coolTicks < CLOCK_COOL_TICKS) return;
        g_ctrl.coolTicks = 0;
        if (g_ctrl.capMhz <= 0) return;   // nothing to restore
        int newCap = g_ctrl.capMhz + CLOCK_STEP_UP;
        g_ctrl.setCap(newCap >= g_ctrl.maxClock ? 0 : newCap);
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

    HICON lastIcon = nullptr;
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
        if (tray) {
            HICON h = createTempIcon(temp);
            if (h) {
                if (tip[0]) wcsncpy_s(g_nid.szTip, tip, _TRUNCATE);
                g_nid.hIcon = h;
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
                if (lastIcon) DestroyIcon(lastIcon);
                lastIcon = h;
            }
            EnterCriticalSection(&g_cs); g_shared.trayDirty = false; LeaveCriticalSection(&g_cs);
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
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"CalmDownGPU_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    logOpen();

    // Worker thread owns all nvidia-smi + tray work (see workerMain).
    InitializeCriticalSection(&g_cs);
    g_quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual-reset
    g_worker = CreateThread(nullptr, 0, workerMain, nullptr, 0, nullptr);

    // --- Restore last target ---
    g_ctrl.targetTemp = loadTargetTemp();
    log("Restored target: %d", g_ctrl.targetTemp);

    // --- Register window class ---
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = L"CalmDownGPU";
    RegisterClassW(&wc);

    // Hidden tool window (not message-only: SetForegroundWindow must work).
    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"CalmDownGPU", L"CalmDownGPU",
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
    wcscpy_s(g_nid.szTip, L"CalmDownGPU — starting…");
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
    log("CalmDownGPU exiting");
    return 0;
}
