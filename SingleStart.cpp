// SingleStart.cpp - 防重复启动工具（守护其他软件）
// 由 HelaRoro 和 DeepSeek V4 Flash 共同开发
//
// 功能：
//  1. 后台常驻，仅在托盘显示图标，无主窗口。
//  2. 后台进程监控：发现某程序已有实例在运行、且距上次启动 <= 2 秒内再次被启动时，
//     判定为"连点造成的重复启动"，立即结束新进程并弹托盘气泡通知。
//  3. 白名单（设置里每行一个程序名）、explorer.exe、程序自身永不拦截；
//     系统自带应用（计算器、记事本等）同样在拦截范围内。
//  4. 通知标题/内容可自定义，支持 {count}（拦截次数）、{app}（最近被拦截的程序名）。
//  5. 托盘图标：左键单击打开设置；右键菜单可打开设置 / 退出。
//  6. 开机自启、进程扫描间隔、白名单等设置保存在 HKCU 注册表。
//  7. 启动出错时在程序所在目录写 error_log.txt（不可写则退回 %TEMP%），弹气泡后退出。
//
// 逻辑与 SingleStart-WinUI（WinUI 3 版）保持一致：2 秒重复窗口、可配置扫描间隔（默认 100ms，
// 范围 20–2000）、还原默认设置、开机自启在保存时统一应用。注册表键位与 WinUI 版相同，设置可共享。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------- 常量

static const wchar_t* kMutexName      = L"Local\\SingleStart_SingleInstance";
static const wchar_t* kTrayWndClass   = L"SingleStart_TrayWndClass";
static const wchar_t* kSettingsClass  = L"SingleStart_SettingsWndClass";
static const wchar_t* kPageClass      = L"SingleStart_SettingsPageClass";
static const wchar_t* kSettingsTitle  = L"SingleStart - 设置";
static const wchar_t* kRegRoot        = L"Software\\HelaRoro\\SingleStart";
static const wchar_t* kRunKey         = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValueName   = L"SingleStart";
static const wchar_t* kTrayTip        = L"SingleStart（防重复启动）";
static const wchar_t* kDefaultTitle   = L"已拦截{count}次重复启动";
static const wchar_t* kDefaultContent = L"桌面里的软件双击打开，任务栏里的软件单击打开。不要重复打开软件！";

static const DWORD kDupWindowMs    = 2000;   // 判定"连点重复启动"的时间窗口（毫秒），与 WinUI 版一致（原 10 秒改为 2 秒）
static const DWORD kDefaultPollIntervalMs = 100; // 进程扫描间隔默认值（毫秒），可在设置里调整
static const DWORD kPollIntervalMin = 20;        // 可设置的下限：数值越小越灵敏，但更耗 CPU
static const DWORD kPollIntervalMax = 2000;      // 可设置的上限：数值越大越省 CPU，但可能漏掉短命存根
// 间隔必须足够小，才能捕捉"重复启动后自己很快退出的第二实例"
// （例如 TIM、QQ 自带单实例检测，连点时第二个进程存活不足 150ms，间隔太大时会漏掉）

#define WM_TRAYICON   (WM_APP + 1)
#define WM_BLOCKED    (WM_APP + 2)  // 监控线程：拦截到一次重复启动
#define IDT_DEBOUNCE  1

#define IDM_SETTINGS  500
#define IDM_EXIT      501

// 设置窗口控件 ID
#define IDC_AUTOSTART    101
#define IDC_TITLE_EDIT   102
#define IDC_CONTENT_EDIT 103
#define IDC_WL_EDIT      104
#define IDC_SAVE_BTN     105
#define IDC_EXIT_BTN     106
#define IDC_STATUS       107
#define IDC_POLL_EDIT    108  // 进程扫描间隔（毫秒）
#define IDC_RESTORE_BTN  109  // 还原默认设置

// ---------------------------------------------------------------- 全局状态

static HINSTANCE g_hInst         = NULL;
static HWND      g_trayWnd       = NULL;
static HWND      g_settingsWnd   = NULL;
static HWND      g_settingsPage  = NULL;
static HICON     g_icon          = NULL;
static bool      g_trayAdded     = false;
static UINT      g_msgTaskbarCreated = 0; // 资源管理器重启通知（TaskbarCreated 广播）
static UINT      g_debounceTimer = 0;
static const GUID kIconGuid =
    { 0x7a2f5b3c, 0x1e4d, 0x4f6a, { 0x9b, 0x8c, 0x3d, 0x2e, 0x1f, 0x0a, 0x5b, 0x6c } };

static int       g_contentHeight = 0;
static int       g_scrollPos     = 0;
static HFONT     g_font          = NULL;
static std::wstring g_ownExeLower;
static std::wstring g_lastErrorLogPath;

struct Settings {
    std::wstring titleFormat;
    std::wstring contentFormat;
    std::vector<std::wstring> whitelist; // 白名单：程序文件名（小写）
    DWORD pollIntervalMs = kDefaultPollIntervalMs; // 进程扫描间隔（毫秒），监控线程每轮读取
};
static Settings g_settings;
static CRITICAL_SECTION g_settingsLock; // 保护 g_settings（UI 线程写，监控线程读）

// 监控线程与 UI 线程共享的拦截计数
struct MonitorShared {
    CRITICAL_SECTION cs;
    LONG  blockedCount;
    wchar_t lastApp[MAX_PATH];
};
static MonitorShared g_mon;

// ---------------------------------------------------------------- 工具函数

template <size_t N>
static void SetStr(wchar_t (&dst)[N], const wchar_t* src) {
    wcsncpy(dst, src, N - 1);
    dst[N - 1] = L'\0';
}

static std::wstring Lower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = (wchar_t)towlower(c);
    return r;
}

static std::wstring GetFileName(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? p : p.substr(pos + 1);
}

static std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring p(buf, n);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos);
    return p;
}

static std::wstring ReplaceToken(const std::wstring& tmpl, const std::wstring& token,
                                 const std::wstring& rep) {
    std::wstring out = tmpl;
    size_t pos = 0;
    while ((pos = out.find(token, pos)) != std::wstring::npos) {
        out.replace(pos, token.size(), rep);
        pos += rep.size();
    }
    return out;
}

// 把 {count} / {app} 替换为实际值
static std::wstring ReplaceTokens(const std::wstring& tmpl, int count, const std::wstring& app) {
    std::wstring out = ReplaceToken(tmpl, L"{count}", std::to_wstring(count));
    return ReplaceToken(out, L"{app}", app);
}

// 把内容里字面的 \n 转成换行
static std::wstring ConvertEscapes(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size() && s[i + 1] == L'n') {
            out += L"\r\n";
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

// ---------------------------------------------------------------- 注册表

static std::wstring RegGetString(const wchar_t* name, const wchar_t* def) {
    HKEY k = NULL;
    wchar_t buf[1024];
    ZeroMemory(buf, sizeof(buf));
    DWORD sz = sizeof(buf), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegRoot, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, name, NULL, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && type == REG_SZ) {
            RegCloseKey(k);
            return std::wstring(buf);
        }
        RegCloseKey(k);
    }
    return std::wstring(def ? def : L"");
}

static void RegSetString(const wchar_t* name, const wchar_t* val) {
    HKEY k = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegRoot, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static DWORD RegGetDword(const wchar_t* name, DWORD def) {
    HKEY k = NULL;
    DWORD val = def, sz = sizeof(val), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegRoot, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (!(RegQueryValueExW(k, name, NULL, &type, (BYTE*)&val, &sz) == ERROR_SUCCESS && type == REG_DWORD))
            val = def;
        RegCloseKey(k);
    }
    return val;
}

static void RegSetDword(const wchar_t* name, DWORD val) {
    HKEY k = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegRoot, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        RegCloseKey(k);
    }
}

static std::vector<std::wstring> RegGetMultiString(const wchar_t* name) {
    std::vector<std::wstring> out;
    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegRoot, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD type = 0, sz = 0;
        if (RegQueryValueExW(k, name, NULL, &type, NULL, &sz) == ERROR_SUCCESS &&
            type == REG_MULTI_SZ && sz >= 2) {
            std::vector<wchar_t> buf(sz / sizeof(wchar_t) + 1, 0);
            RegQueryValueExW(k, name, NULL, &type, (BYTE*)buf.data(), &sz);
            const wchar_t* p = buf.data();
            while (*p) {
                out.push_back(p);
                p += wcslen(p) + 1;
            }
        }
        RegCloseKey(k);
    }
    return out;
}

static void RegSetMultiString(const wchar_t* name, const std::vector<std::wstring>& vals) {
    std::wstring data;
    for (auto& v : vals) { data += v; data += L'\0'; }
    data += L'\0';
    if (vals.empty()) data += L'\0'; // 空列表也需双结尾符才是合法的 REG_MULTI_SZ
    HKEY k = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegRoot, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_MULTI_SZ, (const BYTE*)data.c_str(),
                       (DWORD)(data.size() * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static void LoadSettings() {
    g_settings.titleFormat   = RegGetString(L"TitleFormat", kDefaultTitle);
    g_settings.contentFormat = RegGetString(L"ContentFormat", kDefaultContent);
    g_settings.whitelist     = RegGetMultiString(L"Whitelist");
    DWORD p = RegGetDword(L"PollIntervalMs", kDefaultPollIntervalMs);
    if (p < kPollIntervalMin) p = kPollIntervalMin;
    if (p > kPollIntervalMax) p = kPollIntervalMax;
    g_settings.pollIntervalMs = p;
}

// 当前进程扫描间隔（毫秒）：UI 线程保存时写入，监控线程每轮读取，改动即时生效
static DWORD GetPollIntervalMs() {
    EnterCriticalSection(&g_settingsLock);
    DWORD p = g_settings.pollIntervalMs;
    LeaveCriticalSection(&g_settingsLock);
    if (p < kPollIntervalMin) p = kPollIntervalMin;
    if (p > kPollIntervalMax) p = kPollIntervalMax;
    return p;
}

// 通知标题/内容模板（托盘线程弹通知时读取；与 WinUI 版 SettingsManager.Formats() 对应）
static void GetFormats(std::wstring& title, std::wstring& content) {
    EnterCriticalSection(&g_settingsLock);
    title   = g_settings.titleFormat;
    content = g_settings.contentFormat;
    LeaveCriticalSection(&g_settingsLock);
}

// ---------------------------------------------------------------- 开机自启

static bool IsAutoStartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD type = 0, sz = 0;
        LONG r = RegQueryValueExW(k, kRunValueName, NULL, &type, NULL, &sz);
        RegCloseKey(k);
        return r == ERROR_SUCCESS;
    }
    return false;
}

static void ApplyAutoStart(bool enable) {
    HKEY k = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exe[MAX_PATH];
        if (GetModuleFileNameW(NULL, exe, MAX_PATH)) {
            std::wstring val = L"\"" + std::wstring(exe) + L"\"";
            RegSetValueExW(k, kRunValueName, 0, REG_SZ, (const BYTE*)val.c_str(),
                           (DWORD)((val.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(k, kRunValueName);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- 托盘气泡

static void AddTrayIcon() {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_GUID;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.guidItem = kIconGuid;
    nid.hIcon = g_icon;
    SetStr(nid.szTip, kTrayTip);
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    if (!ok) return;
    g_trayAdded = true;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void RemoveTrayIcon() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    nid.uFlags = NIF_GUID;
    nid.guidItem = kIconGuid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

// 显示临时气泡通知（Win11 下为横幅，默认不进通知中心）
static void ShowBalloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO | NIF_GUID; // 图标以 NIF_GUID 注册，NIM_MODIFY 必须同样带上 GUID 才能定位到图标
    nid.guidItem = kIconGuid;
    nid.dwInfoFlags = NIIF_INFO;
    SetStr(nid.szInfoTitle, title);
    SetStr(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------- 错误处理

static std::wstring GetErrorLogPath() {
    std::wstring path = GetExeDir() + L"\\error_log.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return path;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"SingleStart_error_log.txt";
}

static void WriteErrorLog(const std::wstring& msg) {
    std::wstring path = GetErrorLogPath();
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD fileSize = GetFileSize(h, NULL);
    if (fileSize == 0) { // 空文件先写 UTF-16 BOM，方便记事本识别
        const wchar_t bom = 0xFEFF;
        DWORD w = 0;
        WriteFile(h, &bom, sizeof(bom), &w, NULL);
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[64];
    wsprintfW(ts, L"[%04d-%02d-%02d %02d:%02d:%02d] ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = std::wstring(ts) + msg + L"\r\n";
    DWORD w = 0;
    WriteFile(h, line.c_str(), (DWORD)(line.size() * sizeof(wchar_t)), &w, NULL);
    CloseHandle(h);
    g_lastErrorLogPath = path;
}

// ---------------------------------------------------------------- 窗口过程声明

static LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static void RegisterTrayClass() {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = kTrayWndClass;
    RegisterClassExW(&wc);
}

static void RegisterSettingsClass() {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = kSettingsClass;
    RegisterClassExW(&wc);
}

static void RegisterPageClass() {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PageProc; // 页面窗口必须用 PageProc：把控件消息转发给设置窗口并处理底色
    wc.hInstance = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kPageClass;
    RegisterClassExW(&wc);
}

// 启动出错：写日志 -> 弹气泡 -> 退出
static void ShowStartupError(const std::wstring& msg) {
    WriteErrorLog(msg);

    RegisterTrayClass();
    HWND wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayWndClass, L"SingleStart",
                               WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, g_hInst, NULL);
    HICON icon = g_icon ? g_icon : LoadIconW(NULL, IDI_APPLICATION);

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = wnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_INFO;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = icon;
    nid.dwInfoFlags = NIIF_ERROR;
    SetStr(nid.szInfoTitle, L"SingleStart 启动错误");
    std::wstring text = L"启动失败：" + msg + L"\r\n详细记录：" + g_lastErrorLogPath + L"\r\n软件即将退出。";
    SetStr(nid.szInfo, text.c_str());

    bool added = Shell_NotifyIconW(NIM_ADD, &nid);
    if (added) Sleep(8000); // 给气泡留出显示时间
    if (added) Shell_NotifyIconW(NIM_DELETE, &nid);
    if (wnd) DestroyWindow(wnd);
    ExitProcess(1);
}

// ---------------------------------------------------------------- 设置窗口

static void SetStatus(const std::wstring& text) {
    if (!g_settingsPage) return;
    HWND s = GetDlgItem(g_settingsPage, IDC_STATUS);
    if (s) SetWindowTextW(s, text.c_str());
}

static void SaveSettings() {
    if (!g_settingsPage) return;
    wchar_t buf[4096];

    std::wstring title, content;
    GetWindowTextW(GetDlgItem(g_settingsPage, IDC_TITLE_EDIT), buf, 4095);
    title = buf;
    GetWindowTextW(GetDlgItem(g_settingsPage, IDC_CONTENT_EDIT), buf, 4095);
    content = buf;

    // 白名单：按行拆分成程序名列表
    std::vector<std::wstring> wl;
    GetWindowTextW(GetDlgItem(g_settingsPage, IDC_WL_EDIT), buf, 4095);
    std::wstring line;
    for (wchar_t ch : std::wstring(buf)) {
        if (ch == L'\r' || ch == L'\n') {
            if (!line.empty()) { wl.push_back(line); line.clear(); }
        } else {
            line += ch;
        }
    }
    if (!line.empty()) wl.push_back(line);

    // 进程扫描间隔：读输入框并限制在合法范围
    BOOL ok = FALSE;
    DWORD poll = GetDlgItemInt(g_settingsPage, IDC_POLL_EDIT, &ok, FALSE);
    if (!ok) poll = kDefaultPollIntervalMs;
    if (poll < kPollIntervalMin) poll = kPollIntervalMin;
    if (poll > kPollIntervalMax) poll = kPollIntervalMax;

    EnterCriticalSection(&g_settingsLock);
    g_settings.titleFormat    = title;
    g_settings.contentFormat  = content;
    g_settings.whitelist      = wl;
    g_settings.pollIntervalMs = poll;
    LeaveCriticalSection(&g_settingsLock);

    RegSetString(L"TitleFormat", title.c_str());
    RegSetString(L"ContentFormat", content.c_str());
    RegSetMultiString(L"Whitelist", wl);
    RegSetDword(L"PollIntervalMs", poll);

    // 应用开机自启（与 WinUI 版一致：统一在保存时应用，而非勾选时立即执行）
    ApplyAutoStart(IsDlgButtonChecked(g_settingsPage, IDC_AUTOSTART) == BST_CHECKED);

    SetStatus(L"已保存");
}

// 还原默认设置（与 WinUI 版一致：恢复默认标题/内容/白名单/扫描间隔，并关闭开机自启）
static void RestoreDefaults(HWND hwnd) {
    int r = MessageBoxW(hwnd, L"现在的设置将会被默认设置覆盖！确定还原？", L"还原默认设置",
                        MB_ICONQUESTION | MB_OKCANCEL);
    if (r != IDOK) return;

    EnterCriticalSection(&g_settingsLock);
    g_settings.titleFormat    = kDefaultTitle;
    g_settings.contentFormat  = kDefaultContent;
    g_settings.whitelist.clear();
    g_settings.pollIntervalMs = kDefaultPollIntervalMs;
    LeaveCriticalSection(&g_settingsLock);

    RegSetString(L"TitleFormat", kDefaultTitle);
    RegSetString(L"ContentFormat", kDefaultContent);
    RegSetMultiString(L"Whitelist", std::vector<std::wstring>());
    RegSetDword(L"PollIntervalMs", kDefaultPollIntervalMs);
    ApplyAutoStart(false);

    // 刷新界面
    SetWindowTextW(GetDlgItem(g_settingsPage, IDC_TITLE_EDIT), kDefaultTitle);
    SetWindowTextW(GetDlgItem(g_settingsPage, IDC_CONTENT_EDIT), kDefaultContent);
    SetWindowTextW(GetDlgItem(g_settingsPage, IDC_WL_EDIT), L"");
    SetDlgItemInt(g_settingsPage, IDC_POLL_EDIT, kDefaultPollIntervalMs, FALSE);
    CheckDlgButton(g_settingsPage, IDC_AUTOSTART, BST_UNCHECKED);
    SetStatus(L"已还原默认设置");
}

// 页面滚动：把整页子窗口按当前滚动位置摆放
static void ApplyScroll(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int maxScroll = g_contentHeight - rc.bottom;
    if (maxScroll < 0) maxScroll = 0;
    if (g_scrollPos > maxScroll) g_scrollPos = maxScroll;
    if (g_scrollPos < 0) g_scrollPos = 0;
    SCROLLINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = g_contentHeight - 1;
    si.nPage = rc.bottom;
    si.nPos = g_scrollPos;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    if (g_settingsPage) {
        int w = rc.right;
        if (w < 1) w = 1;
        SetWindowPos(g_settingsPage, NULL, 0, -g_scrollPos, w, g_contentHeight, SWP_NOZORDER);
    }
}

// 页面窗口：把控件的 WM_COMMAND / WM_NOTIFY 转发给设置窗口，并处理静态文字底色
static LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        SendMessageW(GetParent(hwnd), WM_COMMAND, wp, lp);
        return 0;
    case WM_NOTIFY:
        SendMessageW(GetParent(hwnd), WM_NOTIFY, wp, lp);
        return 0;
    case WM_CTLCOLORSTATIC: {
        // 静态标签（通知标题格式 / 提示 / 关于等）文字用灰色，
        // 背景与页面底色一致，避免文字以系统默认色（纯白）绘制而显得突兀
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_settingsPage = CreateWindowExW(0, kPageClass, L"",
                                         WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                         0, 0, 0, 0, hwnd, NULL, g_hInst, NULL);
        HWND page = g_settingsPage;
        const int M = 14;
        const int cw = 360;
        int x = M, y = 8;

        NONCLIENTMETRICSW ncm;
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        HFONT f = g_font;

        auto mkCtl = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                         int w, int h, HMENU id) -> HWND {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                     x, y, w, h, page, id, g_hInst, NULL);
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
            return c;
        };

        // 开机自启（最上方，勾选框）
        mkCtl(L"BUTTON", L"开机自动启动", BS_AUTOCHECKBOX, cw, 24, (HMENU)IDC_AUTOSTART);
        y += 24 + 18;

        // 通知标题自定义
        mkCtl(L"STATIC", L"通知标题自定义", 0, cw, 20, NULL);
        y += 20 + 4;
        mkCtl(L"EDIT", L"", ES_AUTOHSCROLL, cw, 26, (HMENU)IDC_TITLE_EDIT);
        y += 26 + 4;
        mkCtl(L"STATIC", L"标题中的 {count} 会被替换为实际拦截次数，{app} 为最近被拦截的程序名。",
              0, cw, 44, NULL);
        y += 44 + 12;

        // 通知内容自定义
        mkCtl(L"STATIC", L"通知内容自定义", 0, cw, 20, NULL);
        y += 20 + 4;
        mkCtl(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
              cw, 90, (HMENU)IDC_CONTENT_EDIT);
        y += 90 + 4;
        mkCtl(L"STATIC", L"内容中的 {count}、{app} 同理；输入 \\n 表示换行。",
              0, cw, 32, NULL);
        y += 32 + 12;

        // 白名单
        mkCtl(L"STATIC", L"白名单", 0, cw, 20, NULL);
        y += 20 + 4;
        mkCtl(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
              cw, 120, (HMENU)IDC_WL_EDIT);
        y += 120 + 4;
        mkCtl(L"STATIC", L"每行一个程序名，如 notepad.exe。留空表示不设白名单。",
              0, cw, 20, NULL);
        y += 20 + 18;

        // 进程扫描间隔（毫秒）
        mkCtl(L"STATIC", L"进程扫描间隔（毫秒）", 0, cw, 20, NULL);
        y += 20 + 4;
        mkCtl(L"EDIT", L"100", ES_AUTOHSCROLL | ES_NUMBER, cw, 26, (HMENU)IDC_POLL_EDIT);
        y += 26 + 4;
        mkCtl(L"STATIC", L"数值越小越灵敏（可捕捉“启动即退出”的短命第二实例），但更耗 CPU；越大越省 CPU。默认 100。",
              0, cw, 40, NULL);
        y += 40 + 18;

        // 按钮（每个独占一行，宽度一致）
        CreateWindowExW(0, L"BUTTON", L"保存设置", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        x, y, cw, 30, page, (HMENU)IDC_SAVE_BTN, g_hInst, NULL);
        SendMessageW(GetDlgItem(page, IDC_SAVE_BTN), WM_SETFONT, (WPARAM)f, TRUE);
        y += 30 + 10;

        CreateWindowExW(0, L"BUTTON", L"退出软件", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        x, y, cw, 30, page, (HMENU)IDC_EXIT_BTN, g_hInst, NULL);
        SendMessageW(GetDlgItem(page, IDC_EXIT_BTN), WM_SETFONT, (WPARAM)f, TRUE);
        y += 30 + 10;

        CreateWindowExW(0, L"BUTTON", L"还原默认设置", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        x, y, cw, 30, page, (HMENU)IDC_RESTORE_BTN, g_hInst, NULL);
        SendMessageW(GetDlgItem(page, IDC_RESTORE_BTN), WM_SETFONT, (WPARAM)f, TRUE);
        y += 30 + 10;

        // 状态提示
        mkCtl(L"STATIC", L"", 0, cw, 20, (HMENU)IDC_STATUS);
        y += 20 + 10;

        // 底部说明（最后一行）
        mkCtl(L"STATIC", L"由 HelaRoro 和 DeepSeek V4 Flash 共同开发", 0, cw, 20, NULL);
        y += 20 + 8;
        g_contentHeight = y;

        // 载入当前设置
        CheckDlgButton(page, IDC_AUTOSTART, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
        SetWindowTextW(GetDlgItem(page, IDC_TITLE_EDIT), g_settings.titleFormat.c_str());
        SetWindowTextW(GetDlgItem(page, IDC_CONTENT_EDIT), g_settings.contentFormat.c_str());
        std::wstring wlText;
        for (auto& w : g_settings.whitelist) { wlText += w; wlText += L"\r\n"; }
        SetWindowTextW(GetDlgItem(page, IDC_WL_EDIT), wlText.c_str());
        SetDlgItemInt(page, IDC_POLL_EDIT, g_settings.pollIntervalMs, FALSE);
        ApplyScroll(hwnd);
        return 0;
    }

    case WM_SIZE:
        if (g_settingsPage) ApplyScroll(hwnd);
        return 0;

    case WM_VSCROLL: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int maxScroll = g_contentHeight - rc.bottom;
        if (maxScroll < 0) maxScroll = 0;
        int pos = g_scrollPos;
        switch (LOWORD(wp)) {
        case SB_TOP:         pos = 0; break;
        case SB_BOTTOM:      pos = maxScroll; break;
        case SB_LINEUP:      pos -= 20; break;
        case SB_LINEDOWN:    pos += 20; break;
        case SB_PAGEUP:      pos -= (rc.bottom - 20); break;
        case SB_PAGEDOWN:    pos += (rc.bottom - 20); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = HIWORD(wp); break;
        }
        if (pos < 0) pos = 0;
        if (pos > maxScroll) pos = maxScroll;
        g_scrollPos = pos;
        ApplyScroll(hwnd);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        g_scrollPos -= delta / WHEEL_DELTA * 30;
        ApplyScroll(hwnd);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 380;
        mmi->ptMinTrackSize.y = 220;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SAVE_BTN:
            SaveSettings();
            break;
        case IDC_EXIT_BTN:
            DestroyWindow(hwnd);
            PostQuitMessage(0);
            break;
        case IDC_RESTORE_BTN:
            RestoreDefaults(hwnd);
            break;
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE); // 关闭 = 隐藏，再次打开直接显示
        return 0;

    case WM_DESTROY:
        g_settingsWnd = NULL;
        g_settingsPage = NULL;
        if (g_font) {
            DeleteObject(g_font);
            g_font = NULL;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 把设置窗口带到前台（避免开在其它窗口后面，用户以为没反应）
static void BringSettingsToFront() {
    if (!g_settingsWnd) return;
    if (IsIconic(g_settingsWnd)) ShowWindow(g_settingsWnd, SW_RESTORE);
    ShowWindow(g_settingsWnd, SW_SHOW);
    SetForegroundWindow(g_settingsWnd);
    SetWindowPos(g_settingsWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(g_settingsWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

static void OpenSettings() {
    if (g_settingsWnd) {
        BringSettingsToFront();
        return;
    }
    RegisterSettingsClass();
    RegisterPageClass();
    g_settingsWnd = CreateWindowExW(0, kSettingsClass, kSettingsTitle,
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                                    WS_VSCROLL | WS_CLIPCHILDREN,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 410, 660,
                                    NULL, NULL, g_hInst, NULL);
    if (g_settingsWnd) {
        ShowWindow(g_settingsWnd, SW_SHOW);
        SetForegroundWindow(g_settingsWnd);
        UpdateWindow(g_settingsWnd);
    }
}

// ---------------------------------------------------------------- 托盘窗口

static void ShowTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"打开设置");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 资源管理器重启后会广播 TaskbarCreated，托盘图标随之丢失，需要重新添加
    if (g_msgTaskbarCreated != 0 && msg == g_msgTaskbarCreated) {
        RemoveTrayIcon();
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_TRAYICON: {
        // 真实托盘回调的 lParam 高 16 位可能带标志（如 0x00010202），
        // 鼠标消息码在低 16 位，必须用 LOWORD 提取
        UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONUP) {
            OpenSettings();
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        }
        break;
    }
    case WM_BLOCKED:
        // 拦截到重复启动：重置去抖定时器，把一次爆发合并成一条通知
        if (g_debounceTimer) KillTimer(hwnd, g_debounceTimer);
        g_debounceTimer = (UINT)SetTimer(hwnd, IDT_DEBOUNCE, 700, NULL);
        break;
    case WM_TIMER:
        if (wp == IDT_DEBOUNCE) {
            if (g_debounceTimer) {
                KillTimer(hwnd, g_debounceTimer);
                g_debounceTimer = 0;
            }
            EnterCriticalSection(&g_mon.cs);
            LONG count = g_mon.blockedCount;
            std::wstring app = g_mon.lastApp;
            LeaveCriticalSection(&g_mon.cs);
            std::wstring titleFmt, contentFmt;
            GetFormats(titleFmt, contentFmt);
            std::wstring title   = ReplaceTokens(titleFmt, (int)count, app);
            std::wstring content = ConvertEscapes(ReplaceTokens(contentFmt, (int)count, app));
            ShowBalloon(title.c_str(), content.c_str());
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_SETTINGS) OpenSettings();
        else if (LOWORD(wp) == IDM_EXIT) PostQuitMessage(0);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 进程监控

struct ProcEntry {
    DWORD pid;
    DWORD parentPid;
    std::wstring exe; // 文件名（如 notepad.exe），来自快照 szExeFile，不逐个 OpenProcess 取全路径
};

static std::map<DWORD, ProcEntry> EnumProcs() {
    std::map<DWORD, ProcEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            // 直接用快照里的文件名（szExeFile），不再逐个 OpenProcess + 查全路径，大幅降低 CPU 占用
            out[pe.th32ProcessID] = { pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile };
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// 是否豁免（不拦截）：程序自身、explorer.exe、白名单。
// 注意：不再豁免整个 %WINDIR% —— 系统自带应用（计算器 calc.exe、记事本 notepad.exe 等）
// 也需要拦截，否则用户连点计算器/记事本不会被拦到。若某程序不想拦截，加入白名单即可。
// 系统后台进程不会被误杀，因为下面的监控只处理”由用户点击图标拉起”的进程。
static bool IsExempt(const std::wstring& exeName) {
    std::wstring key = Lower(exeName);
    if (key == g_ownExeLower) return true;
    if (key == L"explorer.exe") return true; // 资源管理器本体：多开文件窗口是正常需求，永不拦截
    EnterCriticalSection(&g_settingsLock);
    bool inWl = false;
    for (auto& w : g_settings.whitelist) {
        if (Lower(w) == key) { inWl = true; break; }
    }
    LeaveCriticalSection(&g_settingsLock);
    return inWl;
}

// 监控线程：周期性扫描进程，拦截"短时间内的重复启动"
static DWORD WINAPI MonitorThread(LPVOID) {
    std::map<DWORD, ProcEntry> prev;
    // “最近一次由用户点击图标启动”的时刻。进程退出后仍保留一段时间，
    // 让 calc.exe 这类“启动即退出、转交真程序”的存根也能被识别为重复启动。
    std::map<std::wstring, ULONGLONG> lastLaunch;

    for (;;) {
        Sleep(GetPollIntervalMs());
        ULONGLONG now = GetTickCount64();
        auto cur = EnumProcs();

        for (auto& kv : cur) {
            DWORD pid = kv.first;
            const std::wstring& exe = kv.second.exe;
            if (prev.count(pid)) continue; // 非新出现的进程
            if (IsExempt(exe)) continue;

            // 只处理“用户点击图标”发起的启动：
            //   - 桌面快捷方式 / 任务栏 / 文件管理器双击 -> 父进程是 explorer.exe
            //   - 应用商店(UWP)程序（如新版计算器） -> 由 ApplicationFrameHost.exe 拉起
            // 其余一律忽略，避免误杀浏览器等软件自己派生的同名子进程（如 msedge、claude 的多进程）。
            // 父进程直接用快照里的文件名判断（父进程长期存在，必在快照中），与 WinUI 版一致。
            auto pit = cur.find(kv.second.parentPid);
            if (pit == cur.end()) continue;
            std::wstring parentName = Lower(pit->second.exe);
            if (parentName != L"explorer.exe" && parentName != L"applicationframehost.exe") continue;

            std::wstring key = Lower(exe);
            auto it = lastLaunch.find(key);
            ULONGLONG last = (it != lastLaunch.end()) ? it->second : 0;

            if (last != 0 && (now - last) <= kDupWindowMs) {
                // 强保证"至少一个存活"：杀新进程前先确认还有同名实例在运行。
                // 若原实例已退出（关闭窗口 / 崩溃 / 存根已转交），则放行本次启动，
                // 让它成为存活的实例，避免"拦截后一个都不剩"。
                bool survivor = false;
                for (auto& k2 : cur) {
                    if (k2.first != pid && Lower(k2.second.exe) == key) { survivor = true; break; }
                }
                if (survivor) {
                    // 判定为连点造成的重复启动：结束新进程
                    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                    BOOL killed = FALSE;
                    if (h) {
                        killed = TerminateProcess(h, 1);
                        CloseHandle(h);
                    }
                    if (killed) {
                        EnterCriticalSection(&g_mon.cs);
                        g_mon.blockedCount++;
                        wcsncpy(g_mon.lastApp, GetFileName(exe).c_str(), MAX_PATH - 1);
                        g_mon.lastApp[MAX_PATH - 1] = 0;
                        LeaveCriticalSection(&g_mon.cs);
                        PostMessageW(g_trayWnd, WM_BLOCKED, 0, 0);
                    }
                }
                lastLaunch[key] = now; // 刷新时间，2 秒内连续第 3 次点击也能拦住
            } else {
                lastLaunch[key] = now;
            }
        }

        // 清理过期的启动记录（保留 2 倍窗口，给“启动即退出”的存根留出识别余地）
        for (auto it = lastLaunch.begin(); it != lastLaunch.end();) {
            if (now - it->second > (ULONGLONG)kDupWindowMs * 2) it = lastLaunch.erase(it);
            else ++it;
        }
        prev = cur;
    }
    return 0;
}

// ---------------------------------------------------------------- 入口

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;
    SetProcessDPIAware();

    // 启用 ComCtl32 v6 视觉样式（现代按钮/编辑框外观；需配合 app.manifest 声明 v6 依赖）
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // 守护者自身单实例
    HANDLE hMutex = CreateMutexW(NULL, TRUE, kMutexName);
    if (!hMutex) {
        ShowStartupError(L"创建单实例互斥体失败");
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 0;
    }

    LoadSettings();
    {
        wchar_t buf[MAX_PATH];
        if (GetModuleFileNameW(NULL, buf, MAX_PATH)) g_ownExeLower = Lower(GetFileName(buf));
    }
    InitializeCriticalSection(&g_settingsLock);
    InitializeCriticalSection(&g_mon.cs);
    ZeroMemory(&g_mon.lastApp, sizeof(g_mon.lastApp));

    RegisterTrayClass();
    // 用真实但不显示的顶层窗口承载托盘图标（比 message-only 更可靠地接收托盘回调）
    g_trayWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kTrayWndClass, L"SingleStart",
                                WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_trayWnd) {
        ShowStartupError(L"创建托盘窗口失败");
        return 1;
    }

    g_icon = LoadIconW(hInst, MAKEINTRESOURCE(1));
    if (!g_icon) g_icon = LoadIconW(NULL, IDI_APPLICATION);

    // 注册资源管理器重启通知，用于重启后重新添加托盘图标
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    AddTrayIcon();
    if (!g_trayAdded) {
        ShowStartupError(L"添加托盘图标失败");
        return 1;
    }

    HANDLE hThread = CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
    if (!hThread) {
        ShowStartupError(L"创建监控线程失败");
        return 1;
    }
    CloseHandle(hThread); // 线程持续运行，句柄无需保留

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveTrayIcon();
    CloseHandle(hMutex);
    if (g_settingsWnd) DestroyWindow(g_settingsWnd);
    return (int)msg.wParam;
}
