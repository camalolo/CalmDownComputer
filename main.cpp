// CalmDownGPU - Windows tray app that regulates GPU temperature via power limiting.
// Uses nvidia-smi to query temperature and set power limits.
// Build: see CMakeLists.txt
//
// Key design decisions:
// - Single-threaded: everything runs on the UI thread. nvidia-smi calls are fast
//   (<2s) and the app has no visible window, so brief blocking is acceptable.
// - Hidden commands: uses CreateProcess with CREATE_NO_WINDOW to avoid console popups.
// - Control algorithm: weighted-average + proportional steps with:
//   • Emergency brake: immediate power cut when temp exceeds target by ≥2°C
//   • Cooldown: 20s settling time after each power change
//   • Deadband: ±1.5°C — once stable, power stays put

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

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

// ───────────────────────── Logging ─────────────────────────

static char g_logPath[MAX_PATH] = {};

static void log(const char* fmt, ...) {
    if (!g_logPath[0]) return;
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
    GetTempPathA(MAX_PATH, g_logPath);
    strcat_s(g_logPath, "CalmDownGPU.log");
    // Write a session separator.
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") == 0 && f) {
        fprintf(f, "\n══════════════════════════════════════════\n");
        fclose(f);
        log("CalmDownGPU starting");
    }
}

// ───────────────────────── Constants ─────────────────────────

#define WM_TRAYICON     (WM_USER + 1)
#define ID_TIMER         1
#define TIMER_MS         10000       // 10 seconds
#define HISTORY_LEN      6           // rolling window: 6 × 10s = 60s
#define DEADBAND_C       1.5         // ±1.5°C: don't adjust within this band
#define MIN_SAMPLES_EVAL 3           // need at least this many samples before evaluating
#define COOLDOWN_TICKS   2           // after a power change, skip eval this many ticks (20s settling)
#define STEP_KP          2.5         // proportional gain: step = |error| × KP
#define STEP_MIN         5           // minimum step in watts
#define STEP_MAX         30          // maximum step in watts

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

    char chunk[512];
    DWORD n = 0;
    while (ReadFile(hRead, chunk, sizeof(chunk), &n, nullptr) && n > 0)
        res.output.append(chunk, n);

    WaitForSingleObject(pi.hProcess, 15000);
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

// Returns watts, or -1 on failure.
static int queryPowerLimit() {
    auto r = runHidden("nvidia-smi --query-gpu=power.limit --format=csv,noheader,nounits -i 0");
    auto s = trim(r.output);
    if (s.empty()) return -1;
    try { return static_cast<int>(std::stof(s)); }
    catch (...) { return -1; }
}

// Returns {minWatts, maxWatts}; falls back to {100, 400} on failure.
static std::pair<int,int> queryPowerRange() {
    auto r = runHidden(
        "nvidia-smi --query-gpu=power.min_limit,power.max_limit "
        "--format=csv,noheader,nounits -i 0");
    auto s = trim(r.output);
    float lo = 0, hi = 0;
    // Output looks like "100.00, 270.00"
    if (sscanf_s(s.c_str(), "%f, %f", &lo, &hi) == 2 && hi > 0)
        return { static_cast<int>(lo), static_cast<int>(hi) };
    return { 100, 400 };
}

// Set power limit. Returns true on success.
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
//   2. Push into rolling history (kept across power changes — old data ages out naturally).
//   3. If history has enough samples AND average is outside the deadband,
//      compute a proportional step and apply it via nvidia-smi.
//   A cooldown timer prevents evaluating too soon after a change (thermal lag).

struct Controller {
    int  targetTemp    = 0;     // 0 = off, -1 = max power, >0 = target °C
    int  powerMin      = 100;   // watts
    int  powerMax      = 400;
    int  currentPower  = 150;   // last-known applied power limit

    std::deque<int> history;
    int  cooldown      = 0;     // ticks to skip after a power change

    void reset() { history.clear(); cooldown = 0; }

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

    // Immediate safety brake: if the latest reading exceeds the target,
    // cut power immediately without waiting for cooldown or enough samples.
    // Returns new power or -1 if no change.
    int emergencyBrake() {
        if (!isActive()) return -1;
        if (history.empty()) return -1;
        int cur = latest();
        int overshoot = cur - targetTemp;
        if (overshoot < 2) return -1;  // only brake on significant overshoot

        int step = std::max(STEP_MIN, overshoot * 3);
        step = std::min(step, STEP_MAX);
        int next = currentPower - step;
        next = std::max(powerMin, next);
        if (next >= currentPower) return -1;
        log("BRAKE: temp=%d overshoot=%d → step=%dW → power %dW → %dW",
            cur, overshoot, step, currentPower, next);
        cooldown = COOLDOWN_TICKS;
        return next;
    }

    // Returns the new power to apply, or -1 if no change is needed.
    int evaluate() {
        if (!isActive())                   return -1;
        if ((int)history.size() < MIN_SAMPLES_EVAL) return -1;
        if (cooldown > 0) {
            cooldown--;
            log("eval: cooldown active (%d ticks left), skipping", cooldown + 1);
            return -1;
        }

        double avg = weightedAvg();
        int    cur = latest();
        // Use the higher of weighted-avg and latest reading so we react
        // quickly when temps are climbing, and vice-versa when dropping.
        double effective = (cur > avg) ? (avg + cur) / 2.0 : avg;
        double error = effective - static_cast<double>(targetTemp);

        // Deadband — temperature is close enough; leave power alone.
        if (std::abs(error) <= DEADBAND_C) {
            log("eval: eff=%.1f (avg=%.1f cur=%d) target=%d error=%+.1f → deadband, no change (power=%dW)",
                effective, avg, cur, targetTemp, error, currentPower);
            return -1;
        }

        // Proportional step: bigger corrections when far off, smaller when close.
        int step = static_cast<int>(std::abs(error) * STEP_KP);
        step = std::max(STEP_MIN, std::min(STEP_MAX, step));

        int next = currentPower + (error > 0 ? -step : step);
        next = std::max(powerMin, std::min(powerMax, next));

        if (next == currentPower) return -1;
        log("eval: eff=%.1f (avg=%.1f cur=%d) target=%d error=%+.1f → step=%dW → power %dW → %dW",
            effective, avg, cur, targetTemp, error, step, currentPower, next);
        cooldown = COOLDOWN_TICKS;
        return next;
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

// ───────────────────────── Tray icon management ─────────────────────────

static void setTrayIcon(int temp) {
    HICON hNew = createTempIcon(temp);
    if (!hNew) return;

    g_nid.hIcon = hNew;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);

    if (g_hIcon) DestroyIcon(g_hIcon);
    g_hIcon = hNew;
}

static void setTrayTip(const std::wstring& tip) {
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

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
    s += L"  |  Power: ";
    s += std::to_wstring(g_ctrl.currentPower) + L"W";
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

static void onTimerTick() {
    int temp = queryTemp();
    g_lastTemp = temp;
    log("tick: temp=%d target=%d power=%dW", temp, g_ctrl.targetTemp, g_ctrl.currentPower);

    // Always update display.
    setTrayIcon(temp);
    setTrayTip(buildTipText());

    if (temp < 0) {
        log("tick: failed to read temperature");
        return;  // can't regulate without valid readings
    }

    // ∞ mode: push power to max once.
    if (g_ctrl.targetTemp == -1) {
        if (g_ctrl.currentPower != g_ctrl.powerMax) {
            if (setPowerLimit(g_ctrl.powerMax))
                g_ctrl.currentPower = g_ctrl.powerMax;
        }
        return;
    }

    // Active regulation.
    if (g_ctrl.isActive()) {
        g_ctrl.addSample(temp);

        // Immediate brake if over target — bypasses cooldown.
        int next = g_ctrl.emergencyBrake();

        // Normal proportional control if no brake was needed.
        if (next < 0)
            next = g_ctrl.evaluate();

        if (next >= 0) {
            if (setPowerLimit(next)) {
                g_ctrl.currentPower = next;
                log("tick: power changed to %dW", next);
            }
        }
    }
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

                if (p.temp == -1) {
                    // Infinity — immediately max out power.
                    if (setPowerLimit(g_ctrl.powerMax))
                        g_ctrl.currentPower = g_ctrl.powerMax;
                } else if (p.temp == 0) {
                    // Off — stop regulating, leave power as-is.
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
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_hIcon) DestroyIcon(g_hIcon);
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

    // --- Query GPU info on startup ---
    g_gpuName = queryGpuName();
    log("GPU: %s", g_gpuName.c_str());

    auto [lo, hi] = queryPowerRange();
    g_ctrl.powerMin = lo;
    g_ctrl.powerMax = hi;
    log("Power range: %d-%dW", lo, hi);

    int pl = queryPowerLimit();
    g_ctrl.currentPower = (pl > 0) ? pl : hi;
    log("Current power limit: %dW", g_ctrl.currentPower);

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
    g_nid.hIcon  = createTempIcon(-1);  // placeholder
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

    if (hMutex) CloseHandle(hMutex);
    log("CalmDownGPU exiting");
    return 0;
}
