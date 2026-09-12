// SingleStart.cpp - 防重复启动工具（守护其他软件）
// 由 HelaRoro 和 DeepSeek V4 Flash 共同开发
//
// 功能：
//  1. 后台常驻，仅在托盘显示图标，无主窗口。
//  2. 后台进程监控：发现某程序已有实例在运行、且距上次启动 <= 2 秒内再次被启动时，
//     判定为"连点造成的重复启动"，立即结束新进程并弹托盘气泡通知。
//  3. 白名单（列表形式，可增删）、explorer.exe、程序自身永不拦截，也不触发启动提醒；
//     系统自带应用（计算器、记事本等）同样在拦截范围内。
//  4. 通知标题/内容可自定义，支持 {app}（软件名称，取自 exe 版本信息的 FileDescription）。
//  5. 托盘图标：左键单击打开设置；右键菜单可打开设置 / 退出。
//  6. 开机自启、启动时通知、其他程序启动时提醒、重复启动拦截开关、进程扫描间隔、白名单等设置保存在 HKCU 注册表。
//  7. 其他程序启动时提醒：检测到软件启动时，在屏幕上方显示无按钮的圆角提醒，随后自动淡出。
//  8. 启动出错时在程序所在目录写 error_log.txt（不可写则退回 %TEMP%），弹气泡后退出。
//
// 逻辑与 SingleStart-WinUI（WinUI 3 版）保持一致：2 秒重复窗口、可配置扫描间隔（默认 100ms，
// 范围 20–2000）、还原默认设置、开机自启在保存时统一应用。注册表键位与 WinUI 版相同，设置可共享。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <commdlg.h>   // GetOpenFileNameW（白名单"添加软件"）
#include <winver.h>    // GetFileVersionInfo 系列（读取软件名 FileDescription）
#include <dwmapi.h>    // DWMWA_CLOAKED（覆盖层主窗口检测）
#include <cwctype>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------- 常量

static const wchar_t* kMutexName      = L"Local\\SingleStart_SingleInstance";
static const wchar_t* kTrayWndClass   = L"SingleStart_TrayWndClass";
static const wchar_t* kSettingsClass  = L"SingleStart_SettingsWndClass";
static const wchar_t* kPageClass      = L"SingleStart_SettingsPageClass";
static const wchar_t* kSettingsTitle  = L"SingleStart 设置";
static const wchar_t* kRegRoot        = L"Software\\HelaRoro\\SingleStart";
static const wchar_t* kRunKey         = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValueName   = L"SingleStart";
static const wchar_t* kTrayTip        = L"SingleStart";
static const wchar_t* kDefaultTitle   = L"{app}正在启动，请稍候";
static const wchar_t* kDefaultContent = L"无需再打开{app}！";

static const DWORD kDefaultReminderTimeout = 5000; // 启动提醒默认显示时长（毫秒）
static const DWORD kReminderTimeoutMin      = 500;
static const DWORD kReminderTimeoutMax      = 60000;
static const DWORD kClickFreshMs            = 3000; // 钩子记录到的点击在此时限内才视为"本次启动的点击位置"
static const int   kOverlayW  = 360;
static const int   kOverlayH  = 48;
static const wchar_t* kReminderClass = L"SingleStart_ReminderClass";

// ---------------------------------------------------------------- 高分屏 / 大缩放
// 软件按“系统 DPI 感知”运行（app.manifest 声明 dpiAware=true，wWinMain 里 SetProcessDPIAware）。
// 字体取系统消息字体（SystemParametersInfo 返回的 lfMessageFont 已按系统 DPI 缩放），
// 但界面里所有固定像素尺寸必须随系统 DPI 一起放大，否则高分屏（125%/150%/200%…）下
// 控件、覆盖层相对放大的文字会显得偏小、文字被裁剪、控件错位，导致显示不一致。
// 这里统一提供按系统 DPI 换算的缩放，g_dpiScale 在入口处根据系统 DPI 计算。
static double g_dpiScale = 1.0; // 系统缩放比（dpi / 96）
static int Sx(int v) { return (int)(v * g_dpiScale + 0.5); }
static int OverlayW() { return Sx(kOverlayW); }
static int OverlayH() { return Sx(kOverlayH); }

static const DWORD kDupWindowMs    = 2000;   // 判定"连点重复启动"的时间窗口（毫秒），与 WinUI 版一致（原 10 秒改为 2 秒）
static const DWORD kDefaultPollIntervalMs = 100; // 进程扫描间隔默认值（毫秒），可在设置里调整
static const DWORD kPollIntervalMin = 20;        // 可设置的下限：数值越小越灵敏，但更耗 CPU
static const DWORD kPollIntervalMax = 2000;      // 可设置的上限：数值越大越省 CPU，但可能漏掉短命存根
// 间隔必须足够小，才能捕捉"重复启动后自己很快退出的第二实例"
// （例如 TIM、QQ 自带单实例检测，连点时第二个进程存活不足 150ms，间隔太大时会漏掉）

#define WM_TRAYICON   (WM_APP + 1)
#define WM_BLOCKED    (WM_APP + 2)  // 监控线程：拦截到一次重复启动
#define WM_REMINDER   (WM_APP + 3)  // 监控线程：检测到软件启动（无论是否拦截），显示覆盖层提醒
#define WM_SHOW_STARTUP (WM_APP + 4) // 启动时通知
#define IDT_DEBOUNCE  1
#define IDT_STARTUP   3
#define IDT_REMINDER  4

#define IDM_SETTINGS  500
#define IDM_EXIT      501

// 设置窗口控件 ID
#define IDC_AUTOSTART    101
#define IDC_TITLE_EDIT   102
#define IDC_CONTENT_EDIT 103
#define IDC_WL_EDIT      104  // 已弃用：白名单改用列表 IDC_WL_LIST
#define IDC_SAVE_BTN     105  // 已弃用：改为改动即自动保存
#define IDC_EXIT_BTN     106
#define IDC_STATUS       107
#define IDC_POLL_EDIT    108  // 进程扫描间隔（毫秒）
#define IDC_RESTORE_BTN  109  // 还原默认设置
#define IDC_STARTUP_NOTIFY   110 // 启动时通知
#define IDC_LAUNCH_REMINDER  111 // 其他程序启动时提醒
#define IDC_REMINDER_EDIT    112 // 提醒内容自定义
#define IDC_REMINDER_TIMEOUT_EDIT 113 // 提醒显示时长（毫秒）
#define IDC_WL_LIST          114 // 白名单列表（自绘 ListBox）
#define IDC_WL_ADD           115 // 添加软件
#define IDC_BLOCK_DUP        116 // 拦截重复启动（连点只保留第一个窗口）
#define IDC_BLOCK_NOTIFY     117 // 拦截到重复启动时气泡通知

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
    bool  startupNotify  = false;  // 启动时通知
    bool  launchReminder = true;   // 其他程序启动时提醒（默认开启）
    bool  blockDup       = true;   // 拦截重复启动（连点只保留第一个窗口，默认开启）
    bool  blockNotify    = true;   // 拦截到重复启动时气泡通知（默认开启）
    std::wstring reminderContent;  // 提醒内容模板（默认与通知标题一致）
    DWORD reminderTimeoutMs = kDefaultReminderTimeout; // 提醒显示时长（毫秒）
};
static Settings g_settings;
static CRITICAL_SECTION g_settingsLock; // 保护 g_settings（UI 线程写，监控线程读）

// 监控线程向 UI 线程投递"一次启动事件"（含进程路径，供显示软件名 / 覆盖层提醒）
struct LaunchEvent {
    DWORD pid;
    std::wstring path; // 进程全路径（监控线程尽力取到；读 VERSIONINFO 不依赖进程存活）
    std::wstring exe;  // 快照里的文件名（兜底用）
};

// 最近一次被拦截程序的显示名（主线程专用；WM_BLOCKED 去抖期间被覆盖更新，无需加锁）
static std::wstring g_pendingBlockApp;

// 低层鼠标钩子：记录最近一次左键点击的屏幕坐标。
// 注：触控在 Win10/11 上不保证注入鼠标消息，故此处只是"点击位置"的首选来源，
// 拿不到时覆盖层会改按目标程序主窗口定位（见 ShowReminder / ReminderTimerTick）。
static HHOOK g_mouseHook = NULL;
static POINT g_lastClick = { -1, -1 };
static ULONGLONG g_lastClickTick = 0;
static bool g_hasClick = false;

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

// 把模板中的 {app} 替换为软件显示名
static std::wstring ReplaceTokens(const std::wstring& tmpl, const std::wstring& app) {
    return ReplaceToken(tmpl, L"{app}", app);
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

// 读取 exe 版本信息里的 FileDescription 作为软件显示名（枚举 Translation 支持多语言资源）
static std::wstring GetFileDescription(const std::wstring& path) {
    DWORD hnd = 0;
    DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &hnd);
    if (!sz) return L"";
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), hnd, sz, buf.data())) return L"";
    struct LangCp { WORD lang, cp; };
    LangCp* langs = NULL;
    UINT n = 0;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &n) && n >= sizeof(LangCp)) {
        for (UINT i = 0; i < n / sizeof(LangCp); ++i) {
            wchar_t key[64];
            wsprintfW(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[i].lang, langs[i].cp);
            wchar_t* val = NULL;
            UINT vlen = 0;
            if (VerQueryValueW(buf.data(), key, (void**)&val, &vlen) && val && *val) return val;
        }
    }
    wchar_t* val = NULL;
    UINT vlen = 0;
    if (VerQueryValueW(buf.data(), L"\\StringFileInfo\\040904b0\\FileDescription", (void**)&val, &vlen) && val && *val)
        return val;
    return L"";
}

// 判断字符是否为 CJK 汉字（用于"名字-描述"无空格形式时，仅当后接中文才截断，避免误伤 7-Zip 这类）
static bool IsCJK(wchar_t c) {
    return (c >= 0x2E80 && c <= 0x2FFF) ||
           (c >= 0x3400 && c <= 0x4DBF) ||
           (c >= 0x4E00 && c <= 0x9FFF) ||
           (c >= 0xF900 && c <= 0xFAFF);
}

static bool IsSpaceOrTab(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'　'; // 含全角空格
}

// 全角横杠（不会出现在文件名里，遇到即视为"名字－描述"分隔）
static bool IsWideDash(wchar_t c) {
    return c == L'－' || c == L'—' || c == L'–' ||
           c == L'―' || c == L'─';
}

// 显示名只取 FileDescription 中"名字"部分：去掉横杠及后面的内容。
// 半角连字符仅在两侧有空白、或后接中文时截断（7-Zip、1Password 这类文件名型连字符保留）。
static std::wstring StripDescriptionTail(const std::wstring& desc) {
    for (size_t i = 0; i < desc.size(); ++i) {
        wchar_t c = desc[i];
        if (c == L'-') {
            bool leftSpace = (i > 0) && IsSpaceOrTab(desc[i - 1]);
            bool rightCjk = (i + 1 < desc.size()) && IsCJK(desc[i + 1]);
            if (!leftSpace && !rightCjk) continue;
        } else if (!IsWideDash(c)) {
            continue;
        }
        std::wstring r = desc.substr(0, i);
        while (!r.empty() && IsSpaceOrTab(r.back())) r.pop_back();
        return r;
    }
    return desc;
}

// 得到软件的显示名：FileDescription(去横杠后缀) -> 文件名去扩展名 -> exe 文件名去扩展名
static std::wstring GetAppDisplayName(DWORD pid, const std::wstring& path, const std::wstring& exe) {
    std::wstring p = path;
    if (p.empty() && pid) { // path 为空（监控线程没取到）时按 pid 现取
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            wchar_t buf[MAX_PATH];
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, buf, &sz)) p.assign(buf, sz);
            CloseHandle(h);
        }
    }
    if (!p.empty()) {
        std::wstring desc = GetFileDescription(p);
        if (!desc.empty()) {
            std::wstring name = StripDescriptionTail(desc);
            if (!name.empty()) return name;
        }
        std::wstring base = GetFileName(p);
        size_t dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos) base = base.substr(0, dot);
        if (!base.empty()) return base;
    }
    std::wstring e = GetFileName(exe);
    size_t dot = e.find_last_of(L'.');
    if (dot != std::wstring::npos) e = e.substr(0, dot);
    return e.empty() ? L"未知程序" : e;
}

// 按 pid 取进程全路径（供启动提醒覆盖层加载程序图标；失败返回空串）
static std::wstring QueryProcPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH];
    DWORD sz = MAX_PATH;
    std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, buf, &sz)) r.assign(buf, sz);
    CloseHandle(h);
    return r;
}

// 文字超出可用宽度时的截断规则：
// 默认模板形如“{app}正在启动，请稍候”——软件名在前、固定文案在后。因此要省略的应是
// 软件名“后面的部分”，固定文案（“正在启动”等）必须完整保留：输出形如
// “名字开头...正在启动，请稍候”。自定模板里软件名不在开头等情况下，退化为保留
// 尽量多的头部与尾部、中间省略。返回的字符串保证在不换行的宽度内可完整画出。
static std::wstring FitMiddleText(const std::wstring& s, const std::wstring& app,
                                  HDC hdc, HFONT font, int avail) {
    if (avail <= 0) return L"";
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    SIZE fsz;
    if (GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &fsz) && fsz.cx <= avail) {
        SelectObject(hdc, oldFont);
        return s; // 完全放得下，不截断
    }
    std::wstring dots = L"...";
    SIZE dsz;
    GetTextExtentPoint32W(hdc, dots.c_str(), (int)dots.size(), &dsz);
    int dotW = dsz.cx;
    if (dotW > avail) { SelectObject(hdc, oldFont); return L""; }

    // 文本以软件名开头时，软件名之后的部分视为必须完整保留的固定文案
    bool hasSuffix = !app.empty() && s.size() > app.size() && s.compare(0, app.size(), app) == 0;
    if (hasSuffix) {
        std::wstring suffix = s.substr(app.size());
        SIZE ssz;
        GetTextExtentPoint32W(hdc, suffix.c_str(), (int)suffix.size(), &ssz);
        if (ssz.cx + dotW <= avail) {
            // 固定文案整段保留；剩余宽度给软件名“开头”，省略的是名字后半部分
            int headAvail = avail - ssz.cx - dotW;
            int lo = 0, hi = (int)app.size(), best = 0;
            while (lo <= hi) {
                int mid = (lo + hi) / 2; // 保留软件名开头 mid 个字符
                if (mid == 0) { best = 0; lo = mid + 1; }
                else {
                    SIZE hsz;
                    if (GetTextExtentPoint32W(hdc, app.c_str(), mid, &hsz) && hsz.cx <= headAvail) {
                        best = mid;
                        lo = mid + 1;
                    } else hi = mid - 1;
                }
            }
            std::wstring res = app.substr(0, (size_t)best);
            if (best < (int)app.size()) res += dots;
            res += suffix;
            SelectObject(hdc, oldFont);
            return res;
        }
    }

    // 兜底（自定模板软件名不在开头等）：头部与尾部尽量保留，中间省略
    int lo = 0, hi = (int)s.size(), tailN = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2; // 保留末尾 mid 个字符
        if (mid == 0) { tailN = 0; lo = mid + 1; }
        else {
            SIZE tsz;
            if (GetTextExtentPoint32W(hdc, s.c_str() + (int)s.size() - mid, mid, &tsz) && tsz.cx <= avail) {
                tailN = mid;
                lo = mid + 1;
            } else hi = mid - 1;
        }
    }
    std::wstring tail = s.substr((size_t)s.size() - tailN);
    SIZE tw;
    GetTextExtentPoint32W(hdc, tail.c_str(), (int)tail.size(), &tw);
    int headAvail = avail - tw.cx - dotW;
    lo = 0; hi = (int)s.size() - tailN; // 头部不与尾部重叠
    int headN = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (mid == 0) { headN = 0; lo = mid + 1; }
        else {
            SIZE hsz;
            if (GetTextExtentPoint32W(hdc, s.c_str(), mid, &hsz) && hsz.cx <= headAvail) {
                headN = mid;
                lo = mid + 1;
            } else hi = mid - 1;
        }
    }
    std::wstring res = s.substr(0, (size_t)headN);
    if (headN < (int)(s.size() - tailN)) res += dots;
    res += tail;
    SelectObject(hdc, oldFont);
    return res;
}

// 是否还有"新鲜"的用户点击位置（在 kClickFreshMs 内由低层钩子记录）
static bool GetLastClickPoint(POINT& out, ULONGLONG now) {
    if (!g_hasClick) return false;
    if (now - g_lastClickTick > kClickFreshMs) return false;
    out = g_lastClick;
    return true;
}

// ---------------------------------------------------------------- 注册表

static std::wstring RegGetString(const wchar_t* name, const wchar_t* def) {
    // 先问长度再分配：原先固定 1024 字符的缓冲遇到更长的标题/内容会读取失败，
    // 于是静默回退成默认值（用户输入的长文案在重启后丢失）。
    HKEY k = NULL;
    std::wstring out;
    bool got = false;
    DWORD type = 0, sz = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegRoot, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, name, NULL, &type, NULL, &sz) == ERROR_SUCCESS &&
            type == REG_SZ && sz >= sizeof(wchar_t)) {
            std::vector<wchar_t> buf(sz / sizeof(wchar_t) + 1, 0);
            DWORD sz2 = sz;
            if (RegQueryValueExW(k, name, NULL, &type, (BYTE*)buf.data(), &sz2) == ERROR_SUCCESS) {
                out.assign(buf.data());
                got = true;
            }
        }
        RegCloseKey(k);
    }
    return got ? out : std::wstring(def ? def : L"");
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
    g_settings.startupNotify     = RegGetDword(L"StartupNotify", 0) != 0;
    g_settings.launchReminder    = RegGetDword(L"LaunchReminder", 1) != 0; // 默认开启
    // 拦截 与 “拦截后气泡通知” 拆成两个独立开关，默认均开启。
    // 兼容旧版：旧版单一开关 BlockNotify 若为 0 表示用户整体关掉了拦截，映射到两开关均关。
    DWORD oldBlock = RegGetDword(L"BlockNotify", 1);
    g_settings.blockNotify       = oldBlock != 0;
    g_settings.blockDup          = RegGetDword(L"BlockDup", oldBlock) != 0;
    // 提醒内容与通知标题是两个独立字段：默认值取 kDefaultTitle。
    // （旧行为是“留空则跟随通知标题”，但改动即自动保存会把标题固化进本字段，
    //   之后改标题不再影响提醒文案，反而不易理解，故改为独立默认值。）
    g_settings.reminderContent   = RegGetString(L"ReminderContent", kDefaultTitle);
    DWORD t = RegGetDword(L"ReminderTimeoutMs", kDefaultReminderTimeout);
    if (t < kReminderTimeoutMin) t = kReminderTimeoutMin;
    if (t > kReminderTimeoutMax) t = kReminderTimeoutMax;
    g_settings.reminderTimeoutMs = t;
    // 兼容旧版本：剥掉模板里残留的 {count}（新版已不支持）
    g_settings.titleFormat   = ReplaceToken(g_settings.titleFormat, L"{count}", L"");
    g_settings.contentFormat = ReplaceToken(g_settings.contentFormat, L"{count}", L"");
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

// 提醒内容的“实际生效值”：用户没填过（空）就回退到通知标题模板，
// 与设置窗口回填 / 运行时 ShowReminder 的取用逻辑保持一致（避免界面显示空白）。
static std::wstring EffectiveReminderContent() {
    return g_settings.reminderContent.empty() ? g_settings.titleFormat
                                              : g_settings.reminderContent;
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
static LRESULT CALLBACK ReminderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
struct ReminderInst;
static void ReminderTimerTick(ReminderInst* r);
static void SubclassWhitelistList(HWND wlList);

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

    // 白名单：从列表控件逐项读出
    std::vector<std::wstring> wl;
    HWND wlList = GetDlgItem(g_settingsPage, IDC_WL_LIST);
    int nItems = (int)SendMessageW(wlList, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < nItems; ++i) {
        int len = (int)SendMessageW(wlList, LB_GETTEXTLEN, i, 0);
        if (len <= 0 || len >= 4096) continue;
        std::wstring s;
        s.resize((size_t)len + 1);
        SendMessageW(wlList, LB_GETTEXT, i, (LPARAM)s.data());
        s.resize(wcslen(s.c_str()));
        if (!s.empty()) wl.push_back(s);
    }

    // 进程扫描间隔：读输入框并限制在合法范围
    BOOL ok = FALSE;
    DWORD poll = GetDlgItemInt(g_settingsPage, IDC_POLL_EDIT, &ok, FALSE);
    if (!ok) poll = kDefaultPollIntervalMs;
    DWORD pollRaw = poll;
    if (poll < kPollIntervalMin) poll = kPollIntervalMin;
    if (poll > kPollIntervalMax) poll = kPollIntervalMax;
    // 被夹到范围外时把实际生效值写回输入框，否则界面显示的数值与真正生效的不一致
    if (ok && poll != pollRaw) SetDlgItemInt(g_settingsPage, IDC_POLL_EDIT, (int)poll, FALSE);

    // 新功能开关。注意：禁用的编辑框 GetWindowText 仍可正常读值，
    // 故一律照读并保存，关闭->再开启时内容不会丢。
    bool startupNotify  = IsDlgButtonChecked(g_settingsPage, IDC_STARTUP_NOTIFY) == BST_CHECKED;
    bool launchReminder = IsDlgButtonChecked(g_settingsPage, IDC_LAUNCH_REMINDER) == BST_CHECKED;
    bool blockDup       = IsDlgButtonChecked(g_settingsPage, IDC_BLOCK_DUP) == BST_CHECKED;
    bool blockNotify    = IsDlgButtonChecked(g_settingsPage, IDC_BLOCK_NOTIFY) == BST_CHECKED;
    std::wstring reminderContent;
    GetWindowTextW(GetDlgItem(g_settingsPage, IDC_REMINDER_EDIT), buf, 4095);
    reminderContent = buf;
    DWORD reminderTimeout = GetDlgItemInt(g_settingsPage, IDC_REMINDER_TIMEOUT_EDIT, &ok, FALSE);
    if (!ok) reminderTimeout = kDefaultReminderTimeout;
    DWORD reminderTimeoutRaw = reminderTimeout;
    if (reminderTimeout < kReminderTimeoutMin) reminderTimeout = kReminderTimeoutMin;
    if (reminderTimeout > kReminderTimeoutMax) reminderTimeout = kReminderTimeoutMax;
    if (ok && reminderTimeout != reminderTimeoutRaw)
        SetDlgItemInt(g_settingsPage, IDC_REMINDER_TIMEOUT_EDIT, (int)reminderTimeout, FALSE);

    EnterCriticalSection(&g_settingsLock);
    g_settings.titleFormat       = title;
    g_settings.contentFormat     = content;
    g_settings.whitelist         = wl;
    g_settings.pollIntervalMs    = poll;
    g_settings.startupNotify     = startupNotify;
    g_settings.launchReminder    = launchReminder;
    g_settings.blockDup          = blockDup;
    g_settings.blockNotify       = blockNotify;
    g_settings.reminderContent   = reminderContent;
    g_settings.reminderTimeoutMs = reminderTimeout;
    LeaveCriticalSection(&g_settingsLock);

    RegSetString(L"TitleFormat", title.c_str());
    RegSetString(L"ContentFormat", content.c_str());
    RegSetMultiString(L"Whitelist", wl);
    RegSetDword(L"PollIntervalMs", poll);
    RegSetDword(L"StartupNotify", startupNotify ? 1 : 0);
    RegSetDword(L"LaunchReminder", launchReminder ? 1 : 0);
    RegSetDword(L"BlockDup", blockDup ? 1 : 0);
    RegSetDword(L"BlockNotify", blockNotify ? 1 : 0);
    RegSetString(L"ReminderContent", reminderContent.c_str());
    RegSetDword(L"ReminderTimeoutMs", reminderTimeout);

    // 应用开机自启（与 WinUI 版一致：统一在保存时应用，而非勾选时立即执行）
    ApplyAutoStart(IsDlgButtonChecked(g_settingsPage, IDC_AUTOSTART) == BST_CHECKED);

    SetStatus(L"已保存");
}

// 保存时机：关闭设置窗口（X / Alt+F4）时保存一次，退出软件时再补一次。
// 没有“保存”按钮，也不做改动即保存。

// 还原默认设置（与 WinUI 版一致：恢复默认标题/内容/白名单/扫描间隔，并关闭开机自启）
static void RestoreDefaults(HWND hwnd) {
    int r = MessageBoxW(hwnd, L"确定要还原默认设置吗？", L"还原默认设置",
                        MB_ICONQUESTION | MB_OKCANCEL);
    if (r != IDOK) return;

    EnterCriticalSection(&g_settingsLock);
    g_settings.titleFormat       = kDefaultTitle;
    g_settings.contentFormat     = kDefaultContent;
    g_settings.whitelist.clear();
    g_settings.pollIntervalMs    = kDefaultPollIntervalMs;
    g_settings.startupNotify     = false;
    g_settings.launchReminder    = true;   // 提醒功能默认开启
    g_settings.blockDup          = true;   // 拦截重复启动默认开启
    g_settings.blockNotify       = true;   // 拦截到重复启动时气泡通知默认开启
    g_settings.reminderContent   = kDefaultTitle; // 提醒内容有自己的默认值，不跟随通知标题
    g_settings.reminderTimeoutMs = kDefaultReminderTimeout;
    LeaveCriticalSection(&g_settingsLock);

    RegSetString(L"TitleFormat", kDefaultTitle);
    RegSetString(L"ContentFormat", kDefaultContent);
    RegSetMultiString(L"Whitelist", std::vector<std::wstring>());
    RegSetDword(L"PollIntervalMs", kDefaultPollIntervalMs);
    RegSetDword(L"StartupNotify", 0);
    RegSetDword(L"LaunchReminder", 1);
    RegSetDword(L"BlockDup", 1);
    RegSetDword(L"BlockNotify", 1);
    RegSetString(L"ReminderContent", kDefaultTitle);
    RegSetDword(L"ReminderTimeoutMs", kDefaultReminderTimeout);
    ApplyAutoStart(false);

    // 刷新界面
    SetWindowTextW(GetDlgItem(g_settingsPage, IDC_TITLE_EDIT), kDefaultTitle);
    SetWindowTextW(GetDlgItem(g_settingsPage, IDC_CONTENT_EDIT), kDefaultContent);
    SendMessageW(GetDlgItem(g_settingsPage, IDC_WL_LIST), LB_RESETCONTENT, 0, 0);
    SetDlgItemInt(g_settingsPage, IDC_POLL_EDIT, kDefaultPollIntervalMs, FALSE);
    SetWindowTextW(GetDlgItem(g_settingsPage, IDC_REMINDER_EDIT), kDefaultTitle);
    SetDlgItemInt(g_settingsPage, IDC_REMINDER_TIMEOUT_EDIT, kDefaultReminderTimeout, FALSE);
    CheckDlgButton(g_settingsPage, IDC_AUTOSTART, BST_UNCHECKED);
    CheckDlgButton(g_settingsPage, IDC_STARTUP_NOTIFY, BST_UNCHECKED);
    CheckDlgButton(g_settingsPage, IDC_LAUNCH_REMINDER, BST_CHECKED); // 启动提醒默认开启
    CheckDlgButton(g_settingsPage, IDC_BLOCK_DUP, BST_CHECKED); // 拦截重复启动默认开启
    CheckDlgButton(g_settingsPage, IDC_BLOCK_NOTIFY, BST_CHECKED); // 拦截到重复启动时气泡通知默认开启
    EnableWindow(GetDlgItem(g_settingsPage, IDC_REMINDER_EDIT), TRUE);
    EnableWindow(GetDlgItem(g_settingsPage, IDC_REMINDER_TIMEOUT_EDIT), TRUE);
    EnableWindow(GetDlgItem(g_settingsPage, IDC_BLOCK_NOTIFY), TRUE); // 拦截与气泡通知默认均开启
    EnableWindow(GetDlgItem(g_settingsPage, IDC_TITLE_EDIT), TRUE);
    EnableWindow(GetDlgItem(g_settingsPage, IDC_CONTENT_EDIT), TRUE);

    // 还原后立即落盘，避免用户以为已生效、实际还停在旧设置上；状态提示随后覆盖为“已还原”。
    SaveSettings();
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
    case WM_DRAWITEM:      // 自绘 ListBox（白名单）绘制消息：转发给设置窗口
    case WM_MEASUREITEM:
        // 必须返回设置窗口的处理结果（尤其 WM_MEASUREITEM 需返回 TRUE 才能让行高生效）
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    case WM_CTLCOLORSTATIC: {
        // 静态标签（字段标题 / 提示等）文字用灰色，
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
        // 设置窗口图标（标题栏 + 任务栏）
        if (g_icon) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_icon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_icon);
        }
        g_settingsPage = CreateWindowExW(0, kPageClass, L"",
                                         WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                         0, 0, 0, 0, hwnd, NULL, g_hInst, NULL);
        HWND page = g_settingsPage;
        const int M = Sx(14);
        // 逻辑像素宽度，实际像素由 mkCtl 里的 Sx() 统一按系统缩放换算。
        // 注意不要再写成 Sx(280)：mkCtl 已经换算过，重复换算会得到 缩放² 的宽度，
        // 高分屏（125%/150%/200%）下控件会宽出窗口被截断。
        // 所有控件统一这个宽度（输入框、白名单列表、按钮都一样宽）。
        const int cw = 280;
        int x = M, y = Sx(8);

        NONCLIENTMETRICSW ncm;
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        HFONT f = g_font;

        auto mkCtl = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                         int w, int h, HMENU id) -> HWND {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                     x, y, Sx(w), Sx(h), page, id, g_hInst, NULL);
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
            return c;
        };

        // 开机自动启动
        mkCtl(L"BUTTON", L"开机自动启动", BS_AUTOCHECKBOX, cw, 24, (HMENU)IDC_AUTOSTART);
        y += Sx(24) + Sx(10);

        // 启动时通知
        mkCtl(L"BUTTON", L"本软件启动时通知", BS_AUTOCHECKBOX, cw, 24, (HMENU)IDC_STARTUP_NOTIFY);
        y += Sx(24) + Sx(10);

        // 其他程序启动时提醒
        mkCtl(L"BUTTON", L"其他软件启动时提醒", BS_AUTOCHECKBOX, cw, 24, (HMENU)IDC_LAUNCH_REMINDER);
        y += Sx(24) + Sx(10);

        // 提醒内容（仅开启时可编辑）
        mkCtl(L"STATIC", L"提醒内容", 0, cw, 20, NULL);
        y += Sx(20) + Sx(4);
        mkCtl(L"EDIT", L"", ES_AUTOHSCROLL, cw, 26, (HMENU)IDC_REMINDER_EDIT);
        y += Sx(26) + Sx(4);
        mkCtl(L"STATIC", L"{app} 会被替换为软件名称，\\n 表示换行。", 0, cw, 32, NULL);
        y += Sx(32) + Sx(12);

        // 提醒显示时长
        mkCtl(L"STATIC", L"提醒显示时长（毫秒）", 0, cw, 20, NULL);
        y += Sx(20) + Sx(4);
        mkCtl(L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER, cw, 26, (HMENU)IDC_REMINDER_TIMEOUT_EDIT);
        y += Sx(26) + Sx(10);

        // 拦截重复启动 与 拦截后气泡通知：两个独立开关（默认均开启）
        mkCtl(L"BUTTON", L"拦截重复启动", BS_AUTOCHECKBOX, cw, 24, (HMENU)IDC_BLOCK_DUP);
        y += Sx(24) + Sx(10);
        // 气泡通知控制下方标题/内容的可编辑性
        mkCtl(L"BUTTON", L"拦截到重复启动时气泡通知", BS_AUTOCHECKBOX, cw, 24, (HMENU)IDC_BLOCK_NOTIFY);
        y += Sx(24) + Sx(10);

        // 通知标题
        mkCtl(L"STATIC", L"通知标题", 0, cw, 20, NULL);
        y += Sx(20) + Sx(4);
        mkCtl(L"EDIT", L"", ES_AUTOHSCROLL, cw, 26, (HMENU)IDC_TITLE_EDIT);
        y += Sx(26) + Sx(10);

        // 通知内容
        mkCtl(L"STATIC", L"通知内容", 0, cw, 20, NULL);
        y += Sx(20) + Sx(4);
        mkCtl(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
              cw, 90, (HMENU)IDC_CONTENT_EDIT);
        y += Sx(90) + Sx(4);
        mkCtl(L"STATIC", L"支持 {app}，\\n 表示换行。", 0, cw, 32, NULL);
        y += Sx(32) + Sx(12);

        // 白名单（列表形式，自带滚动条，独立于页面滚动）
        mkCtl(L"STATIC", L"白名单", 0, cw, 20, NULL);
        y += Sx(20) + Sx(4);
        // LBS_HASSTRINGS 必须加：owner-draw 列表没有它，LB_ADDSTRING 存的是被截断的 item data
        // 而非字符串，会导致列表项乱码且 LB_GETTEXT 读不回来（保存白名单时丢数据）
        mkCtl(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | LBS_NOTIFY,
              cw, 150, (HMENU)IDC_WL_LIST);
        y += Sx(150) + Sx(4);
        // 与列表同宽，保持整个白名单区块左对齐、右端齐平
        CreateWindowExW(0, L"BUTTON", L"添加软件…", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        x, y, Sx(cw), Sx(30), page, (HMENU)IDC_WL_ADD, g_hInst, NULL);
        SendMessageW(GetDlgItem(page, IDC_WL_ADD), WM_SETFONT, (WPARAM)f, TRUE);
        y += Sx(30) + Sx(6);
        mkCtl(L"STATIC", L"列表内的程序不会拦截或提醒，右键或长按删除。", 0, cw, 32, NULL);
        y += Sx(32) + Sx(12);

        // 进程扫描间隔
        mkCtl(L"STATIC", L"进程扫描间隔（毫秒）", 0, cw, 20, NULL);
        y += Sx(20) + Sx(4);
        mkCtl(L"EDIT", L"100", ES_AUTOHSCROLL | ES_NUMBER, cw, 26, (HMENU)IDC_POLL_EDIT);
        y += Sx(26) + Sx(18);

        // 按钮（每个独占一行，与输入框/列表同宽）。没有“保存”按钮：关闭设置窗口时保存。
        CreateWindowExW(0, L"BUTTON", L"退出软件", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        x, y, Sx(cw), Sx(30), page, (HMENU)IDC_EXIT_BTN, g_hInst, NULL);
        SendMessageW(GetDlgItem(page, IDC_EXIT_BTN), WM_SETFONT, (WPARAM)f, TRUE);
        y += Sx(30) + Sx(10);

        CreateWindowExW(0, L"BUTTON", L"还原默认设置", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        x, y, Sx(cw), Sx(30), page, (HMENU)IDC_RESTORE_BTN, g_hInst, NULL);
        SendMessageW(GetDlgItem(page, IDC_RESTORE_BTN), WM_SETFONT, (WPARAM)f, TRUE);
        y += Sx(30) + Sx(10);

        // 状态提示（最后一行）
        mkCtl(L"STATIC", L"", 0, cw, 20, (HMENU)IDC_STATUS);
        y += Sx(20) + Sx(8);
        g_contentHeight = y;

        // 载入当前设置
        CheckDlgButton(page, IDC_AUTOSTART, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(page, IDC_STARTUP_NOTIFY, g_settings.startupNotify ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(page, IDC_LAUNCH_REMINDER, g_settings.launchReminder ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(page, IDC_BLOCK_DUP, g_settings.blockDup ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(page, IDC_BLOCK_NOTIFY, g_settings.blockNotify ? BST_CHECKED : BST_UNCHECKED);
        SetWindowTextW(GetDlgItem(page, IDC_REMINDER_EDIT), EffectiveReminderContent().c_str());
        SetDlgItemInt(page, IDC_REMINDER_TIMEOUT_EDIT, (int)g_settings.reminderTimeoutMs, FALSE);
        SetWindowTextW(GetDlgItem(page, IDC_TITLE_EDIT), g_settings.titleFormat.c_str());
        SetWindowTextW(GetDlgItem(page, IDC_CONTENT_EDIT), g_settings.contentFormat.c_str());
        SubclassWhitelistList(GetDlgItem(page, IDC_WL_LIST)); // 先子类再填充，确保删除按钮命中
        for (auto& w : g_settings.whitelist)
            SendMessageW(GetDlgItem(page, IDC_WL_LIST), LB_ADDSTRING, 0, (LPARAM)w.c_str());
        SetDlgItemInt(page, IDC_POLL_EDIT, (int)g_settings.pollIntervalMs, FALSE);
        // 提醒内容/时长仅在"其他程序启动时提醒"开启时可编辑
        bool remindOn = g_settings.launchReminder;
        EnableWindow(GetDlgItem(page, IDC_REMINDER_EDIT), remindOn);
        EnableWindow(GetDlgItem(page, IDC_REMINDER_TIMEOUT_EDIT), remindOn);
        // 拦截关闭时，“拦截气泡通知”选项及标题/内容一并禁用
        EnableWindow(GetDlgItem(page, IDC_BLOCK_NOTIFY), g_settings.blockDup);
        EnableWindow(GetDlgItem(page, IDC_TITLE_EDIT), g_settings.blockDup && g_settings.blockNotify);
        EnableWindow(GetDlgItem(page, IDC_CONTENT_EDIT), g_settings.blockDup && g_settings.blockNotify);
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
        case SB_LINEUP:      pos -= Sx(20); break;
        case SB_LINEDOWN:    pos += Sx(20); break;
        case SB_PAGEUP:      pos -= (rc.bottom - Sx(20)); break;
        case SB_PAGEDOWN:    pos += (rc.bottom - Sx(20)); break;
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
        // 光标悬停在白名单列表上时，滚轮只滚动列表，不触发整页滚动
        POINT pt;
        GetCursorPos(&pt); // 用实时光标位置，避免 GetMessagePos 的 16 位坐标在多屏/高分下溢出
        HWND wlList = g_settingsPage ? GetDlgItem(g_settingsPage, IDC_WL_LIST) : NULL;
        if (wlList) {
            RECT rc;
            GetWindowRect(wlList, &rc);
            if (PtInRect(&rc, pt)) {
                SendMessageW(wlList, WM_MOUSEWHEEL, wp, lp);
                return 0;
            }
        }
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        g_scrollPos -= delta / WHEEL_DELTA * Sx(30);
        ApplyScroll(hwnd);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        // 最小宽度不小于整行控件所需宽度（左右边距 + cw），否则拉窄后控件右端会被切掉
        mmi->ptMinTrackSize.x = Sx(330);
        mmi->ptMinTrackSize.y = Sx(220);
        mmi->ptMaxTrackSize.y = GetSystemMetrics(SM_CYFULLSCREEN); // 允许拉到满屏高度，减少滚动
        return 0;
    }

    // 自绘 ListBox（白名单）：行高固定、逐行绘制
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lp;
        if (mi->CtlType == ODT_LISTBOX) mi->itemHeight = Sx(24);
        return TRUE;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlType != ODT_LISTBOX) break;
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        if (dis->itemState & ODS_SELECTED) {
            FillRect(hdc, &rc, (HBRUSH)(COLOR_HIGHLIGHT + 1));
            SetTextColor(hdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
        } else {
            FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        }
        SetBkMode(hdc, TRANSPARENT);
        wchar_t tbuf[512] = { 0 };
        if (dis->itemID != (UINT)-1)
            SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)tbuf);
        RECT tr = rc;
        tr.left += 4;
        tr.right -= 4;
        DrawTextW(hdc, tbuf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        // 各控件只在勾选/输入时调整相互的可用状态，真正的落盘在关闭设置窗口时进行
        case IDC_EXIT_BTN:
            if (g_settingsPage) SaveSettings(); // 退出软件前把未保存的改动落盘
            DestroyWindow(hwnd);
            PostQuitMessage(0);
            break;
        case IDC_RESTORE_BTN:
            RestoreDefaults(hwnd);
            break;
        case IDC_LAUNCH_REMINDER:
            if (HIWORD(wp) == BN_CLICKED) {
                bool on = IsDlgButtonChecked(g_settingsPage, IDC_LAUNCH_REMINDER) == BST_CHECKED;
                EnableWindow(GetDlgItem(g_settingsPage, IDC_REMINDER_EDIT), on);
                EnableWindow(GetDlgItem(g_settingsPage, IDC_REMINDER_TIMEOUT_EDIT), on);
            }
            break;
        case IDC_BLOCK_DUP: // 关闭拦截时，拦截气泡通知选项及其文案一并禁用
            if (HIWORD(wp) == BN_CLICKED) {
                bool dup  = IsDlgButtonChecked(g_settingsPage, IDC_BLOCK_DUP) == BST_CHECKED;
                bool noti = IsDlgButtonChecked(g_settingsPage, IDC_BLOCK_NOTIFY) == BST_CHECKED;
                EnableWindow(GetDlgItem(g_settingsPage, IDC_BLOCK_NOTIFY), dup);
                EnableWindow(GetDlgItem(g_settingsPage, IDC_TITLE_EDIT), dup && noti);
                EnableWindow(GetDlgItem(g_settingsPage, IDC_CONTENT_EDIT), dup && noti);
            }
            break;
        case IDC_BLOCK_NOTIFY:
            if (HIWORD(wp) == BN_CLICKED) {
                bool on  = IsDlgButtonChecked(g_settingsPage, IDC_BLOCK_NOTIFY) == BST_CHECKED;
                bool dup = IsDlgButtonChecked(g_settingsPage, IDC_BLOCK_DUP) == BST_CHECKED;
                EnableWindow(GetDlgItem(g_settingsPage, IDC_TITLE_EDIT), dup && on); // 与创建时的判定保持一致
                EnableWindow(GetDlgItem(g_settingsPage, IDC_CONTENT_EDIT), dup && on);
            }
            break;
        case IDC_WL_ADD: {
            wchar_t fname[MAX_PATH] = { 0 };
            OPENFILENAMEW ofn;
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0\0";
            ofn.lpstrFile = fname;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                std::wstring name = Lower(GetFileName(fname));
                if (!name.empty()) {
                    SendMessageW(GetDlgItem(g_settingsPage, IDC_WL_LIST), LB_ADDSTRING, 0, (LPARAM)name.c_str());
                }
            }
            break;
        }
        }
        return 0;

    case WM_CLOSE:
        if (g_settingsPage) SaveSettings(); // 关闭设置窗口即保存（无“保存”按钮）
        ShowWindow(hwnd, SW_HIDE);          // 关闭 = 隐藏，再次打开直接显示
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
                                    WS_MAXIMIZEBOX | WS_VSCROLL | WS_CLIPCHILDREN,
                                    CW_USEDEFAULT, CW_USEDEFAULT, Sx(340), Sx(660),
                                    NULL, NULL, g_hInst, NULL);
    if (g_settingsWnd) {
        ShowWindow(g_settingsWnd, SW_SHOW);
        SetForegroundWindow(g_settingsWnd);
        UpdateWindow(g_settingsWnd);
    }
}

// ---------------------------------------------------------------- 白名单列表（子类）

static WNDPROC g_origWlProc = NULL;
// 最近一次由 WM_RBUTTONDOWN 删除白名单项的时间：用于忽略紧随其后的 WM_CONTEXTMENU，防止重复删除下一项
static ULONGLONG g_wlRightDeleteTick = 0;

// 子类白名单 ListBox：右键或触控长按点中某行则删除该行；其余交给基类处理，并抢焦点让滚轮作用于列表。
// 触控长按在系统里通常等价于右键（注入 WM_RBUTTONDOWN，部分驱动只发 WM_CONTEXTMENU），两条通道都处理并去重。
static LRESULT CALLBACK WhitelistSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        SetFocus(hwnd); // 让随后的滚轮消息发到列表
        break;
    case WM_RBUTTONDOWN: { // 右键 / 触控长按注入的右键：命中即删
        POINT pt;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        LRESULT res = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
        if (res != LB_ERR && HIWORD(res) == 0) { // HIWORD==0 表示点在项上，空白处不删
            g_wlRightDeleteTick = GetTickCount64();
            SendMessageW(hwnd, LB_DELETESTRING, (int)LOWORD(res), 0);
            return 0;
        }
        break;
    }
    case WM_CONTEXTMENU: { // 兜底通道：某些长按只发 WM_CONTEXTMENU
        POINT pt;
        if ((short)LOWORD(lp) == -1) GetCursorPos(&pt); // 键盘触发（Shift+F10）时无坐标，用光标位置
        else { pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp); }
        POINT c = pt;
        ScreenToClient(hwnd, &c);
        LRESULT res = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(c.x, c.y));
        if (res != LB_ERR && HIWORD(res) == 0) {
            if (GetTickCount64() - g_wlRightDeleteTick >= 600) { // 刚删过的不再删，防误删下一项
                SendMessageW(hwnd, LB_DELETESTRING, (int)LOWORD(res), 0);
            }
            return 0;
        }
        break;
    }
    case WM_NCDESTROY: {
        // 恢复原窗口过程并把消息交给它收尾。注意先在局部变量里留一份：
        // 置空 g_origWlProc 之后再传给 CallWindowProcW 会传入 NULL（原过程收不到 WM_NCDESTROY）。
        WNDPROC orig = g_origWlProc;
        g_origWlProc = NULL;
        if (orig) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)orig);
            return CallWindowProcW(orig, hwnd, msg, wp, lp);
        }
        break;
    }
    }
    if (!g_origWlProc) return DefWindowProcW(hwnd, msg, wp, lp);
    return CallWindowProcW(g_origWlProc, hwnd, msg, wp, lp);
}

static void SubclassWhitelistList(HWND wlList) {
    if (!wlList) return;
    g_origWlProc = (WNDPROC)SetWindowLongPtrW(wlList, GWLP_WNDPROC, (LONG_PTR)WhitelistSubclassProc);
}

// ---------------------------------------------------------------- 启动提醒覆盖层

// 一条覆盖层提醒的状态。一次“软件启动”对应一条，各条独立窗口互不影响，
// 可同时弹出多条（例如连续打开两个不同的软件）。同一软件的连续启动复用同一条。
struct ReminderInst {
    HWND hwnd = NULL;
    std::wstring key;   // 去重键：exe 全路径小写（取不到路径时用显示名）
    DWORD targetPid = 0;
    std::wstring text;
    std::wstring app;   // 软件显示名（截断时据此保留“正在启动”等固定文案）
    DWORD timeoutMs = kDefaultReminderTimeout;
    ULONGLONG shownTick = 0;
    int alpha = 255;
    int phase = 0; // 0=IDLE 1=STEADY 2=FADE
    bool anchoredToClick = false;
    bool anchoredToWnd = false;
    int tickCount = 0;
    HFONT font = NULL;
    HICON icon = NULL;  // 被启动软件的程序图标（SHGetFileInfo 取得，左侧展示）
};
static const int kRemPhaseIdle = 0, kRemPhaseSteady = 1, kRemPhaseFade = 2;
static std::vector<ReminderInst*> g_rems; // 当前显示中的各条提醒（主线程专用）

static ReminderInst* FindReminderByKey(const std::wstring& key) {
    for (auto* r : g_rems) if (r->key == key) return r;
    return NULL;
}

// 枚举回调：目标 pid 是否已有可见主窗口（用于窗口定位 / 判定“软件完全打开”）
static BOOL CALLBACK FindMainWndCB(HWND h, LPARAM lp) {
    struct FindCtx { DWORD pid; RECT rc; bool found; };
    FindCtx* ctx = (FindCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != ctx->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (IsIconic(h)) return TRUE;
    RECT rc;
    if (!GetWindowRect(h, &rc)) return TRUE;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    ctx->rc = rc;
    ctx->found = true;
    return FALSE; // 找到一个即停
}

static bool FindVisibleMainWindow(DWORD pid, RECT& out) {
    struct FindCtx { DWORD pid; RECT rc; bool found; };
    FindCtx ctx = { pid, {}, false };
    EnumWindows(FindMainWndCB, (LPARAM)&ctx);
    if (ctx.found) out = ctx.rc;
    return ctx.found;
}

// 目标进程是否已退出（短命存根 / 被拦截杀掉的第二实例）
static bool TargetProcessExited(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return true;
    DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_OBJECT_0;
}

// 把某条覆盖层放到锚点上方 8px、水平居中的位置，并钳制在所在显示器工作区内
static void PositionReminder(ReminderInst* r, POINT anchor) {
    if (!r || !r->hwnd) return;
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    int x = anchor.x - OverlayW() / 2;
    int y = anchor.y - Sx(8) - OverlayH();
    if (x < wa.left) x = wa.left;
    if (x + OverlayW() > wa.right) x = wa.right - OverlayW();
    if (y < wa.top) y = anchor.y + Sx(8); // 上方放不下则移到锚点下方
    if (y + OverlayH() > wa.bottom) y = wa.bottom - OverlayH();
    SetWindowPos(r->hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// 登记覆盖层窗口类（类已存在时会被系统忽略）
static void RegisterReminderClass() {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ReminderProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = kReminderClass;
    RegisterClassExW(&wc);
}

// 创建一条覆盖层提醒（独立窗口），挂入 g_rems 后返回
static ReminderInst* CreateReminder() {
    RegisterReminderClass();
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                kReminderClass, L"", WS_POPUP,
                                0, 0, OverlayW(), OverlayH(), NULL, NULL, g_hInst, NULL);
    if (!hwnd) return NULL;
    HRGN rgn = CreateRoundRectRgn(0, 0, OverlayW() + 1, OverlayH() + 1, Sx(16), Sx(16));
    SetWindowRgn(hwnd, rgn, TRUE); // 区域句柄所有权移交窗口，无需删除
    ReminderInst* r = new ReminderInst();
    r->hwnd = hwnd;
    NONCLIENTMETRICSW ncm;
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    r->font = CreateFontIndirectW(&ncm.lfMessageFont);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)r);
    g_rems.push_back(r);
    return r;
}

// 释放一条提醒：从列表移除并回收图标/字体（在窗口 WM_NCDESTROY 里调用）
static void RemoveReminder(ReminderInst* r) {
    if (!r) return;
    for (size_t i = 0; i < g_rems.size(); ++i) {
        if (g_rems[i] == r) { g_rems.erase(g_rems.begin() + i); break; }
    }
    if (r->icon) DestroyIcon(r->icon);
    if (r->font) DeleteObject(r->font);
    delete r;
}

static LRESULT CALLBACK ReminderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ReminderInst* r = (ReminderInst*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT; // 点击穿透，只作通知
    case WM_TIMER:
        ReminderTimerTick(r);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        RemoveReminder(r); // 窗口销毁时回收本条实例（此后不再访问 r / hwnd）
        return 0;
    case WM_PAINT: {
        if (!r) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        HDC mdc = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, OverlayW(), OverlayH());
        HBITMAP oldBmp = (HBITMAP)SelectObject(mdc, bmp);
        // 先填满整个窗口（避免 layered 窗口未绘制区域露出黑色），再沿圆角路径描边：
        // 路径几何与窗口圆角区域一致，四角描边完整，也不会产生黑色边缘
        RECT rc = { 0, 0, OverlayW(), OverlayH() };
        HBRUSH bg = CreateSolidBrush(RGB(255, 251, 225)); // 浅黄底
        FillRect(mdc, &rc, bg);
        DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(210, 170, 70));
        HPEN oldPn = (HPEN)SelectObject(mdc, pen);
        HBRUSH oldBr = (HBRUSH)SelectObject(mdc, GetStockObject(NULL_BRUSH)); // 仅描边不填充
        BeginPath(mdc);
        RoundRect(mdc, 1, 1, OverlayW() - 1, OverlayH() - 1, Sx(16), Sx(16));
        EndPath(mdc);
        StrokePath(mdc);
        SelectObject(mdc, oldPn);
        SelectObject(mdc, oldBr);
        DeleteObject(pen);
        SetBkMode(mdc, TRANSPARENT);
        SetTextColor(mdc, RGB(60, 60, 60));
        HFONT f = r->font ? r->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT oldFont = (HFONT)SelectObject(mdc, f);

        // 左侧绘制被启动软件的程序图标（无图标时省略）
        int iconSize = Sx(18);
        int iconRight = Sx(14);
        bool hasIcon = (r->icon != NULL);
        if (hasIcon) {
            DrawIconEx(mdc, Sx(14), (OverlayH() - iconSize) / 2, r->icon,
                       iconSize, iconSize, 0, NULL, DI_NORMAL);
            iconRight += iconSize + Sx(10); // 图标右侧与文字之间留间距
        }
        int right = OverlayW() - Sx(14);

        SIZE tsz;
        GetTextExtentPoint32W(mdc, r->text.c_str(), (int)r->text.size(), &tsz);
        RECT tr;
        if (tsz.cx <= right - iconRight) {
            // 整段放得下：图标+文字作为一组居中摆放，避免短软件名时右侧大片空白
            int gap = hasIcon ? Sx(10) : 0;
            int groupW = (hasIcon ? iconSize : 0) + gap + tsz.cx;
            int textLeft = (OverlayW() - groupW) / 2 + (hasIcon ? iconSize + gap : 0);
            if (textLeft < iconRight) textLeft = iconRight;
            tr = { textLeft, 0, right, OverlayH() };
            DrawTextW(mdc, r->text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
            // 放不下时：省略的是软件名“后面的部分”，完整保留“正在启动”等固定文案，
            // 输出形如“名字开头...正在启动，请稍候”。
            int avail = right - iconRight;
            std::wstring disp = FitMiddleText(r->text, r->app, mdc, f, avail);
            tr = { iconRight, 0, right, OverlayH() };
            DrawTextW(mdc, disp.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(mdc, oldFont);
        BitBlt(hdc, 0, 0, OverlayW(), OverlayH(), mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 单窗口定时器（50ms）：每 3 tick（≈150ms）轮询目标主窗口/进程存活/超时，驱动淡出
static void ReminderTimerTick(ReminderInst* r) {
    if (!r || !r->hwnd) return;
    ULONGLONG now = GetTickCount64();
    ++r->tickCount;
    if (r->tickCount % 3 == 0) {
        RECT wr;
        bool hasWnd = FindVisibleMainWindow(r->targetPid, wr);
        // 第②级定位：没拿到点击位置时，主窗口一出现就贴到窗口上沿
        if (hasWnd && !r->anchoredToClick && !r->anchoredToWnd) {
            POINT a = { (wr.left + wr.right) / 2, wr.top };
            PositionReminder(r, a);
            r->anchoredToWnd = true;
        }
        if (hasWnd || (now - r->shownTick >= r->timeoutMs) ||
            TargetProcessExited(r->targetPid))
            r->phase = kRemPhaseFade;
    }
    if (r->phase == kRemPhaseFade) {
        r->alpha -= 10;
        if (r->alpha < 0) r->alpha = 0;
        SetLayeredWindowAttributes(r->hwnd, 0, (BYTE)r->alpha, LWA_ALPHA);
        if (r->alpha == 0) {
            KillTimer(r->hwnd, IDT_REMINDER);
            HWND h = r->hwnd;
            DestroyWindow(h); // WM_NCDESTROY 里 RemoveReminder 回收 r，此后不再访问 r
        }
    }
}

// 弹出（或刷新）一条覆盖层提醒。不同软件各占一条、可同时存在互不影响；
// 同一软件（同 exe 路径）在上一条还没消失时再次启动，则复用同一条刷新位置与内容。
static void ShowReminder(DWORD pid, const std::wstring& text, const std::wstring& app,
                         DWORD timeoutMs, const std::wstring& exePath) {
    std::wstring key = exePath.empty() ? Lower(app) : Lower(exePath);
    ReminderInst* r = FindReminderByKey(key);
    if (r) {
        if (r->icon) { DestroyIcon(r->icon); r->icon = NULL; }
    } else {
        r = CreateReminder();
        if (!r) return;
    }
    ULONGLONG now = GetTickCount64();
    r->key = key;
    r->targetPid = pid;
    r->text = text;
    r->app = app;
    r->timeoutMs = timeoutMs;
    r->shownTick = now;
    r->alpha = 255;
    r->phase = kRemPhaseSteady;
    r->tickCount = 0;
    r->anchoredToClick = false;
    r->anchoredToWnd = false;

    // 左侧展示程序的图标：按本次 exe 路径读取
    if (!exePath.empty()) {
        SHFILEINFOW sfi;
        ZeroMemory(&sfi, sizeof(sfi));
        if (SHGetFileInfoW(exePath.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_SMALLICON))
            r->icon = sfi.hIcon;
    }

    POINT pt;
    if (GetLastClickPoint(pt, now)) {
        // 第①级定位：用户点击位置上方
        r->anchoredToClick = true;
        PositionReminder(r, pt);
    } else {
        // 第③级定位（兜底）：屏幕底部中央；主窗口出现后由定时器接管重定位
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(mon, &mi);
        POINT anchor = { (mi.rcWork.left + mi.rcWork.right) / 2,
                         mi.rcWork.bottom - OverlayH() - Sx(60) };
        PositionReminder(r, anchor);
    }
    SetLayeredWindowAttributes(r->hwnd, 0, 255, LWA_ALPHA);
    ShowWindow(r->hwnd, SW_SHOW);
    SetTimer(r->hwnd, IDT_REMINDER, 50, NULL);
}

// 低层鼠标钩子：记录最近一次左键点击（供覆盖层第①级定位）
static LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_LBUTTONDOWN) {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lp;
        g_lastClick = ms->pt;
        g_lastClickTick = GetTickCount64();
        g_hasClick = true;
    }
    return CallNextHookEx(NULL, code, wp, lp);
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
    case WM_BLOCKED: {
        // 拦截到重复启动：记录显示名，重置去抖定时器，把一次爆发合并成一条通知
        LaunchEvent* e = (LaunchEvent*)lp;
        if (e) {
            g_pendingBlockApp = GetAppDisplayName(e->pid, e->path, e->exe);
            delete e;
        }
        if (g_debounceTimer) KillTimer(hwnd, g_debounceTimer);
        g_debounceTimer = (UINT)SetTimer(hwnd, IDT_DEBOUNCE, 700, NULL);
        return 0;
    }
    case WM_REMINDER: {
        // 检测到软件启动：显示覆盖层提醒（无论是否被拦截）
        LaunchEvent* e = (LaunchEvent*)lp;
        if (e) {
            EnterCriticalSection(&g_settingsLock);
            bool remind = g_settings.launchReminder;
            std::wstring contentFmt = EffectiveReminderContent(); // 未自定义则用通知标题
            DWORD to = g_settings.reminderTimeoutMs;
            LeaveCriticalSection(&g_settingsLock);
            if (remind) {
                std::wstring app = GetAppDisplayName(e->pid, e->path, e->exe);
                std::wstring txt = ReplaceTokens(contentFmt, app);
                // 覆盖层左侧要显示程序图标，需要给到进程的 exe 路径
                std::wstring iconPath = e->path.empty() ? QueryProcPath(e->pid) : e->path;
                ShowReminder(e->pid, ConvertEscapes(txt), app, to, iconPath);
            }
            delete e;
        }
        return 0;
    }
    case WM_SHOW_STARTUP:
        SetTimer(hwnd, IDT_STARTUP, 1500, NULL); // 稍作延迟，避免刚启动系统未就绪时气泡被吞
        return 0;
    case WM_TIMER:
        if (wp == IDT_DEBOUNCE) {
            if (g_debounceTimer) {
                KillTimer(hwnd, g_debounceTimer);
                g_debounceTimer = 0;
            }
            std::wstring titleFmt, contentFmt;
            GetFormats(titleFmt, contentFmt);
            std::wstring title   = ReplaceTokens(titleFmt, g_pendingBlockApp);
            std::wstring content = ConvertEscapes(ReplaceTokens(contentFmt, g_pendingBlockApp));
            ShowBalloon(title.c_str(), content.c_str());
        } else if (wp == IDT_STARTUP) {
            KillTimer(hwnd, IDT_STARTUP);
            ShowBalloon(L"SingleStart 已启动",
                        L"SingleStart 正在后台运行。");
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_SETTINGS) OpenSettings();
        else if (LOWORD(wp) == IDM_EXIT) { if (g_settingsPage) SaveSettings(); PostQuitMessage(0); }
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
    // 先取一次基准快照：否则第一次扫描时 prev 为空，本软件启动前就在运行的所有进程
    // 都会被当成“刚启动”——既会误杀同名的既有实例（如已开着的两个记事本），
    // 也会在每个同类进程上弹一条启动提醒。
    std::map<DWORD, ProcEntry> prev = EnumProcs();
    // “最近一次由用户点击图标启动”的时刻。进程退出后仍保留一段时间，
    // 让 calc.exe 这类“启动即退出、转交真程序”的存根也能被识别为重复启动。
    std::map<std::wstring, ULONGLONG> lastLaunch;

    for (;;) {
        Sleep(GetPollIntervalMs());
        ULONGLONG now = GetTickCount64();
        auto cur = EnumProcs();
        if (cur.empty()) continue; // 快照失败：跳过本轮且不更新基准，否则下一轮会把所有进程当成新启动
        EnterCriticalSection(&g_settingsLock);
        bool blockDup    = g_settings.blockDup;    // 拦截重复启动开关（false 时不杀第二实例）
        bool blockNotify = g_settings.blockNotify; // 拦截到重复启动时气泡通知开关
        LeaveCriticalSection(&g_settingsLock);

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

            // 取进程全路径（在拦截之前取：读 VERSIONINFO 只依赖磁盘文件，进程死不死无影响）
            std::wstring fullPath;
            {
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (h) {
                    wchar_t pb[MAX_PATH];
                    DWORD psz = MAX_PATH;
                    if (QueryFullProcessImageNameW(h, 0, pb, &psz)) fullPath.assign(pb, psz);
                    CloseHandle(h);
                }
            }

            // 启动提醒：只要检测到软件被用户拉起就提醒（无论是否重复 / 是否被拦截）。
            // 白名单在前面 IsExempt 时已排除。
            EnterCriticalSection(&g_settingsLock);
            bool remind = g_settings.launchReminder;
            LeaveCriticalSection(&g_settingsLock);
            if (remind)
                PostMessageW(g_trayWnd, WM_REMINDER, 0,
                             (LPARAM)new LaunchEvent{ pid, fullPath, exe });

            // 去重键优先用全路径：不同目录下的同名程序（各自的 setup.exe / Launcher.exe）
            // 不是彼此的“第二实例”，不应互相拦截；取不到路径时退回文件名。
            std::wstring exeLower = Lower(exe);
            std::wstring key = fullPath.empty() ? exeLower : Lower(fullPath);
            auto it = lastLaunch.find(key);
            ULONGLONG last = (it != lastLaunch.end()) ? it->second : 0;

            if (last != 0 && (now - last) <= kDupWindowMs) {
                // 强保证"至少一个存活"：杀新进程前先确认还有同名实例在运行。
                // 若原实例已退出（关闭窗口 / 崩溃 / 存根已转交），则放行本次启动，
                // 让它成为存活的实例，避免"拦截后一个都不剩"。
                bool survivor = false;
                for (auto& k2 : cur) {
                    if (k2.first != pid && Lower(k2.second.exe) == exeLower) { survivor = true; break; }
                }
                if (survivor && blockDup) {
                    // 判定为连点造成的重复启动：结束新进程（是否弹气泡由 blockNotify 另行决定）
                    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                    BOOL killed = FALSE;
                    if (h) {
                        killed = TerminateProcess(h, 1);
                        CloseHandle(h);
                    }
                    if (killed && blockNotify)
                        PostMessageW(g_trayWnd, WM_BLOCKED, 0,
                                     (LPARAM)new LaunchEvent{ pid, fullPath, exe });
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
    // 计算系统缩放（系统 DPI / 96），据此放大所有固定像素布局，保证高分屏/大缩放显示一致
    {
        HDC sdc = GetDC(NULL);
        int dpi = GetDeviceCaps(sdc, LOGPIXELSX);
        ReleaseDC(NULL, sdc);
        if (dpi <= 0) dpi = 96;
        g_dpiScale = dpi / 96.0;
    }

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

    // 低层鼠标钩子：记录用户点击位置（供启动提醒覆盖层定位）
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, hInst, 0);

    // 启动时通知：软件自身启动完成后弹气泡
    if (g_settings.startupNotify) PostMessageW(g_trayWnd, WM_SHOW_STARTUP, 0, 0);

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
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    CloseHandle(hMutex);
    if (g_settingsPage) SaveSettings(); // 设置窗口还开着时，退出前把改动落盘
    if (g_settingsWnd) DestroyWindow(g_settingsWnd);
    return (int)msg.wParam;
}
