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
//     弹出时同时播放"快速渐显 + 上滑回弹"动画（点击位置在屏幕顶部时改为下滑），
//     整条提醒是自绘的抗锯齿卡片，并跟随系统深浅色。
//  8. 可选（设置里开关，需先开启"其他软件启动时提醒"，默认关闭）：目标软件窗口真正显示后，
//     提醒移动到该窗口的位置、放大到窗口大小覆盖其上，然后渐隐消失。
//  9. 设置界面与提醒卡片都跟随系统明暗自动切换；提醒卡片为较不透明的半透明卡片，
//     圆角/描边全部走逐像素 alpha 抗锯齿（不使用系统模糊材质，原因见 ReminderColors 注释）。
// 10. 启动出错时在程序所在目录写 error_log.txt（不可写则退回 %TEMP%），弹气泡后退出。
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
#include <dwmapi.h>    // DWMWA_CLOAKED（覆盖层主窗口检测）/ 系统背景材质
#include <uxtheme.h>   // SetWindowTheme（设置界面深色模式）
#include <objidl.h>    // GDI+ 需要 IStream
#include <gdiplus.h>   // 覆盖层抗锯齿绘制（圆角、文字、图标）
#include <mmsystem.h>  // timeBeginPeriod：动画期间提高定时器精度，否则 16ms 定时器实际约 31ms
#include <cmath>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus")
#pragma comment(lib, "uxtheme")
#endif

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

// 覆盖层动画参数（时间单位毫秒，位移单位逻辑像素）
static const int kRemTimerMs     = 16;  // 有动画时的时钟周期（≈60fps），动画才顺滑
static const int kRemIdleTickMs  = 100; // 没有动画（稳定/等待）时的时钟周期：省电，不必 60fps 空转
static const int kRemPollMs      = 160; // 稳定阶段轮询目标窗口的间隔（按时间算，与时钟周期无关）
static const int kRemWaitPollMs  = 500; // 隐形等待阶段的轮询间隔
static const int kRemExeScanMs   = 1000;// 等待阶段"按 exe 名找窗口"的频率（要遍历进程，降频调用）
static const int kPopMs          = 260; // 弹出位移时长
static const int kPopFadeMs      = 120; // 弹出期间的渐显时长（比渐隐快）
static const int kPopOffset      = 20;  // 弹出起点与终点的位移距离
static const int kPopBack        = 145; // 回弹强度（百分比，145≈7% 过冲，越大回弹越明显）
static const int kFadeMs         = 480; // 渐隐时长（超时/目标进程退出，与原版速度接近）
static const int kCoverMs        = 380; // 移动到目标窗口并放大的时长
static const int kCoverFadeMs    = 260; // 覆盖到位后的渐隐时长
static const int kCoverStepTicks = 1;   // 覆盖动画每帧都出（配合下方的快速绘制，跑满 60fps）
static const int kCoverWaitMs    = 20000; // 提醒超时消失后，仍继续等目标窗口出现的时长
static const int kCoverIconGrow  = 350;   // 视觉引导动画末尾图标放大到百分之多少（350 = 3.5 倍）

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

// 判定"连点重复启动"的时间窗口（毫秒）：可在设置里调整，默认 2 秒（与 WinUI 版一致）
static const DWORD kDefaultDupWindowMs = 2000;
static const DWORD kDupWindowMin       = 500;
static const DWORD kDupWindowMax       = 10000;
static const DWORD kDefaultPollIntervalMs = 100; // 进程扫描间隔默认值（毫秒），可在设置里调整
static const DWORD kPollIntervalMin = 20;        // 可设置的下限：数值越小越灵敏，但更耗 CPU
static const DWORD kPollIntervalMax = 2000;      // 可设置的上限：数值越大越省 CPU，但可能漏掉短命存根
// 间隔必须足够小，才能捕捉"重复启动后自己很快退出的第二实例"
// （例如 TIM、QQ 自带单实例检测，连点时第二个进程存活不足 150ms，间隔太大时会漏掉）

#define WM_TRAYICON   (WM_APP + 1)
#define WM_BLOCKED    (WM_APP + 2)  // 监控线程：拦截到一次重复启动
#define WM_REMINDER   (WM_APP + 3)  // 监控线程：检测到软件启动（无论是否拦截），显示覆盖层提醒
#define WM_SHOW_STARTUP (WM_APP + 4) // 启动时通知
#define IDT_STARTUP   3
#define IDT_THEME     5

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
#define IDC_COVER_WINDOW     118 // 视觉引导动画：提醒移动到目标软件窗口并覆盖（依赖"其他软件启动时提醒"）
#define IDC_DUP_WINDOW_EDIT  119 // 重复启动判定时间（毫秒）
// 分组标题（纯文字 STATIC，用正常文字色，与灰色说明文字区分开）；区间用于 WM_CTLCOLORSTATIC 判断
#define IDC_SEC_FIRST        130
#define IDC_SEC_STARTUP      130 // 启动与通知
#define IDC_SEC_REMINDER     131 // 启动提醒弹窗
#define IDC_SEC_BLOCK        132 // 重复启动拦截
#define IDC_SEC_WHITELIST    133 // 白名单
#define IDC_SEC_OTHER        134 // 其他
#define IDC_SEC_LAST         134

// ---------------------------------------------------------------- 全局状态

static HINSTANCE g_hInst         = NULL;
static HWND      g_trayWnd       = NULL;
static HWND      g_settingsWnd   = NULL;
static HWND      g_settingsPage  = NULL;
static HICON     g_icon          = NULL;
static bool      g_trayAdded     = false;
static UINT      g_msgTaskbarCreated = 0; // 资源管理器重启通知（TaskbarCreated 广播）
static ULONGLONG g_lastBlockBalloonTick = 0; // 上一次"拦截气泡"的时间：同一波连点只弹一条通知
static const GUID kIconGuid =
    { 0x7a2f5b3c, 0x1e4d, 0x4f6a, { 0x9b, 0x8c, 0x3d, 0x2e, 0x1f, 0x0a, 0x5b, 0x6c } };

static int       g_contentHeight = 0;
static int       g_scrollPos     = 0;
static HFONT     g_font          = NULL;
static std::wstring g_ownExeLower;
static std::wstring g_lastErrorLogPath;

// 设置页控件登记表：按这张表统一排版（表内顺序 = 页面上到下）
static const int kPageCtlW = 280; // 页面控件的逻辑宽度（输入框/列表/按钮统一宽度）
static const int kPageIndent = 12; // 分组内选项相对分组标题的缩进
struct PageItem { HWND hwnd; int indent; int h; int gapAfter; };
static std::vector<PageItem> g_pageItems;
static void LayoutPage(HWND host);

struct Settings {
    std::wstring titleFormat;
    std::wstring contentFormat;
    std::vector<std::wstring> whitelist; // 白名单：程序文件名（小写）
    DWORD pollIntervalMs = kDefaultPollIntervalMs; // 进程扫描间隔（毫秒），监控线程每轮读取
    bool  startupNotify  = false;  // 启动时通知
    bool  launchReminder = true;   // 其他程序启动时提醒（默认开启）
    bool  blockDup       = true;   // 拦截重复启动（连点只保留第一个窗口，默认开启）
    bool  blockNotify    = true;   // 拦截到重复启动时气泡通知（默认开启）
    bool  coverWindow    = false;  // 目标软件窗口出现后，提醒移动到该窗口并覆盖（默认关闭）
    DWORD dupWindowMs = kDefaultDupWindowMs; // 重复启动判定时间（毫秒），监控线程每轮读取
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

// 低层鼠标钩子：记录最近一次左键点击的屏幕坐标。
// 注：触控在 Win10/11 上不保证注入鼠标消息，故此处只是"点击位置"的首选来源，
// 拿不到时覆盖层会改按目标程序主窗口定位（见 ShowReminder / ReminderTimerTick）。
static HHOOK g_mouseHook = NULL;
static POINT g_lastClick = { -1, -1 };
static ULONGLONG g_lastClickTick = 0;
static bool g_hasClick = false;

// ---------------------------------------------------------------- 深色模式（跟随系统）
// 系统"应用模式"开关保存在 HKCU\...\Themes\Personalize\AppsUseLightTheme（0=深色，1=浅色）。
// 设置界面靠 uxtheme 的深色主题把标准控件一起变暗（该接口未文档化，按序号动态取，
// 老系统取不到就整体保持浅色，功能不受影响）；覆盖层提醒是自绘的，任何系统都能跟随深色。

typedef BOOL (WINAPI *PFN_AllowDarkModeForWindow)(HWND, BOOL);
typedef int  (WINAPI *PFN_SetPreferredAppMode)(int);
typedef void (WINAPI *PFN_FlushMenuThemes)(void);

static bool  g_sysDark     = false; // 系统当前是否为深色
static bool  g_darkUiReady = false; // 设置界面深色是否可用（uxtheme 深色接口 + Win10 1809+）
static DWORD g_winBuild    = 0;     // 系统内部版本号（判断亚克力/云母等接口是否可用）
static PFN_AllowDarkModeForWindow g_pAllowDarkModeForWindow = NULL;
static PFN_SetPreferredAppMode    g_pSetPreferredAppMode    = NULL;
static PFN_FlushMenuThemes        g_pFlushMenuThemes        = NULL;

// 设置界面配色（浅色 / 深色两套；g_uiDarkBuilt 记录画刷是按哪套建的）
struct UiTheme {
    COLORREF pageBg, text, subText, fieldBg, fieldText, selBg, selText, sep;
    HBRUSH   brPage, brField, brSel, brSep;
};
static UiTheme g_ui;
static bool    g_uiBuilt     = false;
static bool    g_uiDarkBuilt = false;

static void ApplySettingsTheme(HWND hwnd);
static void RefreshReminderTheme();

// GetVersionEx 会被兼容性清单改写，这里改用 ntdll!RtlGetVersion 取真实版本号
static DWORD GetWindowsBuild() {
    // 布局必须与 RTL_OSVERSIONINFOW 完全一致：5 个 ULONG 之后紧跟 128 个 WCHAR。
    // （多插字段会让 RtlGetVersion 写入的字符串错位；这里只读 build，但别留下这个坑。）
    struct Osvi {
        ULONG size, major, minor, build, platform;
        wchar_t csd[128];
    };
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (nt) {
        typedef LONG (WINAPI *PFN_RtlGetVersion)(void*);
        PFN_RtlGetVersion f = (PFN_RtlGetVersion)GetProcAddress(nt, "RtlGetVersion");
        if (f) {
            Osvi v;
            ZeroMemory(&v, sizeof(v));
            v.size = sizeof(v);
            if (f(&v) == 0 && v.build) return v.build;
        }
    }
    return 0;
}

static bool SystemUsesDarkTheme() {
    HKEY k = NULL;
    DWORD v = 1, sz = sizeof(v), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"AppsUseLightTheme", NULL, &type, (BYTE*)&v, &sz) != ERROR_SUCCESS ||
            type != REG_DWORD)
            v = 1; // 读不到按浅色处理
        RegCloseKey(k);
    }
    return v == 0;
}

static void InitDarkModeSupport() {
    if (!g_winBuild) g_winBuild = GetWindowsBuild();
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (ux) {
        g_pAllowDarkModeForWindow = (PFN_AllowDarkModeForWindow)GetProcAddress(ux, MAKEINTRESOURCEA(133));
        g_pSetPreferredAppMode    = (PFN_SetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        g_pFlushMenuThemes        = (PFN_FlushMenuThemes)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    }
    g_darkUiReady = (g_winBuild >= 17763) && g_pAllowDarkModeForWindow && g_pSetPreferredAppMode;
    if (g_darkUiReady) g_pSetPreferredAppMode(1); // PreferredAppMode::AllowDark
    g_sysDark = SystemUsesDarkTheme();
}

// 按当前明暗重建配色画刷（明暗没变就直接返回）
static void BuildUiTheme() {
    bool dark = g_sysDark && g_darkUiReady;
    if (g_uiBuilt && dark == g_uiDarkBuilt) return;
    if (g_ui.brPage)  { DeleteObject(g_ui.brPage);  g_ui.brPage  = NULL; }
    if (g_ui.brField) { DeleteObject(g_ui.brField); g_ui.brField = NULL; }
    if (g_ui.brSel)   { DeleteObject(g_ui.brSel);   g_ui.brSel   = NULL; }
    if (g_ui.brSep)   { DeleteObject(g_ui.brSep);   g_ui.brSep   = NULL; }
    if (dark) {
        g_ui.pageBg    = RGB(32, 32, 32);
        g_ui.text      = RGB(240, 240, 240);
        g_ui.subText   = RGB(155, 155, 155);
        g_ui.fieldBg   = RGB(45, 45, 45);
        g_ui.fieldText = RGB(238, 238, 238);
        g_ui.selBg     = RGB(38, 79, 120);
        g_ui.selText   = RGB(255, 255, 255);
        g_ui.sep       = RGB(72, 72, 72);
    } else {
        g_ui.pageBg    = GetSysColor(COLOR_BTNFACE);
        g_ui.text      = GetSysColor(COLOR_WINDOWTEXT);
        g_ui.subText   = GetSysColor(COLOR_GRAYTEXT);
        g_ui.fieldBg   = GetSysColor(COLOR_WINDOW);
        g_ui.fieldText = GetSysColor(COLOR_WINDOWTEXT);
        g_ui.selBg     = GetSysColor(COLOR_HIGHLIGHT);
        g_ui.selText   = GetSysColor(COLOR_HIGHLIGHTTEXT);
        g_ui.sep       = RGB(214, 214, 214);
    }
    g_ui.brPage  = CreateSolidBrush(g_ui.pageBg);
    g_ui.brField = CreateSolidBrush(g_ui.fieldBg);
    g_ui.brSel   = CreateSolidBrush(g_ui.selBg);
    g_ui.brSep   = CreateSolidBrush(g_ui.sep);
    g_uiDarkBuilt = dark;
    g_uiBuilt = true;
}

static BOOL CALLBACK ThemeChildCB(HWND child, LPARAM lp) {
    bool dark = lp != 0;
    if (g_pAllowDarkModeForWindow) g_pAllowDarkModeForWindow(child, dark ? TRUE : FALSE);
    SetWindowTheme(child, dark ? L"DarkMode_Explorer" : NULL, NULL);
    return TRUE;
}

// 给窗口及其全部子控件套用/撤销深色主题（EnumChildWindows 会递归到所有后代）
static void ApplyWindowDarkTheme(HWND top, bool dark) {
    if (!top || !g_darkUiReady) return;
    if (g_pAllowDarkModeForWindow) g_pAllowDarkModeForWindow(top, dark ? TRUE : FALSE);
    SetWindowTheme(top, dark ? L"DarkMode_Explorer" : NULL, NULL);
    EnumChildWindows(top, ThemeChildCB, dark ? 1 : 0);
}

// 标题栏深色（Win10 1809+ 用属性 20，更早的预览版用 19）
static void ApplyTitleBarTheme(HWND hwnd, bool dark) {
    if (!hwnd || g_winBuild < 17763) return;
    BOOL v = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &v, sizeof(v))))
        DwmSetWindowAttribute(hwnd, 19, &v, sizeof(v));
}

// 系统明暗可能变化时调用（WM_SETTINGCHANGE / 定时兜底）：真的变了才重刷
static void OnSystemThemeMaybeChanged() {
    bool dark = SystemUsesDarkTheme();
    if (dark == g_sysDark) return;
    g_sysDark = dark;
    if (g_settingsWnd) ApplySettingsTheme(g_settingsWnd);
    RefreshReminderTheme();
}

// 套用设置窗口主题（配色 + 控件主题 + 标题栏）并重画
static void ApplySettingsTheme(HWND hwnd) {
    if (!hwnd) return;
    BuildUiTheme();
    bool dark = g_sysDark && g_darkUiReady;
    ApplyWindowDarkTheme(hwnd, dark);
    ApplyTitleBarTheme(hwnd, dark);
    if (dark && g_pFlushMenuThemes) g_pFlushMenuThemes();
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

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
    // 目标窗口覆盖：新功能，默认关闭
    g_settings.coverWindow       = RegGetDword(L"CoverWindow", 0) != 0;
    DWORD dw = RegGetDword(L"DupWindowMs", kDefaultDupWindowMs);
    if (dw < kDupWindowMin) dw = kDupWindowMin;
    if (dw > kDupWindowMax) dw = kDupWindowMax;
    g_settings.dupWindowMs = dw;
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

// 当前"重复启动判定时间"（毫秒）：监控线程每轮读取，设置改动即时生效
static DWORD GetDupWindowMs() {
    EnterCriticalSection(&g_settingsLock);
    DWORD v = g_settings.dupWindowMs;
    LeaveCriticalSection(&g_settingsLock);
    if (v < kDupWindowMin) v = kDupWindowMin;
    if (v > kDupWindowMax) v = kDupWindowMax;
    return v;
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
    wc.hbrBackground = NULL; // 底色由 PageProc 的 WM_ERASEBKGND 按当前明暗填充（深色模式需要）
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
    // 覆盖开关被禁用（没开提醒）时也照读照存，重新开启提醒后不会丢
    bool coverWindow    = IsDlgButtonChecked(g_settingsPage, IDC_COVER_WINDOW) == BST_CHECKED;
    // 重复启动判定时间：读输入框并夹到合法范围（夹取后写回界面，避免显示值与生效值不一致）
    BOOL okDup = FALSE;
    DWORD dupWindow = GetDlgItemInt(g_settingsPage, IDC_DUP_WINDOW_EDIT, &okDup, FALSE);
    if (!okDup) dupWindow = kDefaultDupWindowMs;
    DWORD dupWindowRaw = dupWindow;
    if (dupWindow < kDupWindowMin) dupWindow = kDupWindowMin;
    if (dupWindow > kDupWindowMax) dupWindow = kDupWindowMax;
    if (okDup && dupWindow != dupWindowRaw)
        SetDlgItemInt(g_settingsPage, IDC_DUP_WINDOW_EDIT, (int)dupWindow, FALSE);
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
    g_settings.coverWindow       = coverWindow;
    g_settings.dupWindowMs       = dupWindow;
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
    RegSetDword(L"CoverWindow", coverWindow ? 1 : 0);
    RegSetDword(L"DupWindowMs", dupWindow);
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
    g_settings.coverWindow       = false;  // 覆盖目标窗口是新功能，默认关闭
    g_settings.dupWindowMs       = kDefaultDupWindowMs;
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
    RegSetDword(L"CoverWindow", 0);
    RegSetDword(L"DupWindowMs", kDefaultDupWindowMs);
    RegSetString(L"ReminderContent", kDefaultTitle);
    RegSetDword(L"ReminderTimeoutMs", kDefaultReminderTimeout);
    ApplyAutoStart(false);

    // 刷新界面
    SetWindowTextW(GetDlgItem(g_settingsPage, IDC_TITLE_EDIT), kDefaultTitle);
    SetWindowTextW(GetDlgItem(g_settingsPage, IDC_CONTENT_EDIT), kDefaultContent);
    SendMessageW(GetDlgItem(g_settingsPage, IDC_WL_LIST), LB_RESETCONTENT, 0, 0);
    SetDlgItemInt(g_settingsPage, IDC_POLL_EDIT, kDefaultPollIntervalMs, FALSE);
    SetDlgItemInt(g_settingsPage, IDC_DUP_WINDOW_EDIT, kDefaultDupWindowMs, FALSE);
    SetWindowTextW(GetDlgItem(g_settingsPage, IDC_REMINDER_EDIT), kDefaultTitle);
    SetDlgItemInt(g_settingsPage, IDC_REMINDER_TIMEOUT_EDIT, kDefaultReminderTimeout, FALSE);
    CheckDlgButton(g_settingsPage, IDC_AUTOSTART, BST_UNCHECKED);
    CheckDlgButton(g_settingsPage, IDC_STARTUP_NOTIFY, BST_UNCHECKED);
    CheckDlgButton(g_settingsPage, IDC_LAUNCH_REMINDER, BST_CHECKED); // 启动提醒默认开启
    CheckDlgButton(g_settingsPage, IDC_BLOCK_DUP, BST_CHECKED); // 拦截重复启动默认开启
    CheckDlgButton(g_settingsPage, IDC_BLOCK_NOTIFY, BST_CHECKED); // 拦截到重复启动时气泡通知默认开启
    CheckDlgButton(g_settingsPage, IDC_COVER_WINDOW, BST_UNCHECKED); // 覆盖目标窗口默认关闭
    EnableWindow(GetDlgItem(g_settingsPage, IDC_REMINDER_EDIT), TRUE);
    EnableWindow(GetDlgItem(g_settingsPage, IDC_REMINDER_TIMEOUT_EDIT), TRUE);
    EnableWindow(GetDlgItem(g_settingsPage, IDC_COVER_WINDOW), TRUE); // 提醒默认开启，故覆盖开关可用
    EnableWindow(GetDlgItem(g_settingsPage, IDC_BLOCK_NOTIFY), TRUE); // 拦截与气泡通知默认均开启
    EnableWindow(GetDlgItem(g_settingsPage, IDC_DUP_WINDOW_EDIT), TRUE); // 拦截默认开启，故判定时间可编辑
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

// 按登记表把页面控件从上到下摆一遍（分组内选项按 indent 缩进）
static void LayoutPage(HWND host) {
    if (!g_settingsPage) return;
    const int M = Sx(14);
    int y = Sx(8);
    for (size_t i = 0; i < g_pageItems.size(); ++i) {
        const PageItem& it = g_pageItems[i];
        if (!it.hwnd) continue;
        int x = M + Sx(it.indent);
        int w = Sx(kPageCtlW) - Sx(it.indent);
        ShowWindow(it.hwnd, SW_SHOWNA);
        SetWindowPos(it.hwnd, NULL, x, y, w, Sx(it.h), SWP_NOZORDER | SWP_NOACTIVATE);
        y += Sx(it.h) + Sx(it.gapAfter);
    }
    g_contentHeight = y;
    if (host) ApplyScroll(host);
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
    case WM_ERASEBKGND: { // 类背景画刷为 NULL，底色由这里按当前明暗填充
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_ui.brPage);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        // 静态标签（字段标题 / 提示等）文字用灰色，
        // 背景与页面底色一致，避免文字以系统默认色绘制而显得突兀；
        // 分组标题用正常文字色，才像"标题"而不是提示。
        HDC hdc = (HDC)wp;
        int id = ((HWND)lp != NULL) ? GetDlgCtrlID((HWND)lp) : 0;
        bool isHeader = (id >= IDC_SEC_FIRST && id <= IDC_SEC_LAST);
        SetTextColor(hdc, isHeader ? g_ui.text : g_ui.subText);
        SetBkColor(hdc, g_ui.pageBg);
        return (LRESULT)g_ui.brPage;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        // 输入框 / 列表底色：深色时跟着变暗（不依赖系统控件的深色主题）
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, g_ui.fieldText);
        SetBkColor(hdc, g_ui.fieldBg);
        return (LRESULT)g_ui.brField;
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, g_ui.pageBg);
        return (LRESULT)g_ui.brPage;
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
        // 逻辑像素宽度，实际像素由布局时统一按系统缩放换算。
        // 注意不要再写成 Sx(280)：布局里已经换算过，重复换算会得到 缩放² 的宽度，
        // 高分屏（125%/150%/200%）下控件会宽出窗口被截断。
        // 所有控件统一这个宽度（输入框、白名单列表、按钮都一样宽）。

        NONCLIENTMETRICSW ncm;
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        HFONT f = g_font;

        // 控件一律先建在 (0,0)，位置交给 LayoutPage 统一排版：
        // 整页按"分组标题 + 分割线 + 缩进的选项"分成若干组，indent 是缩进（0 或 kPageIndent），
        // gapAfter 是它与下一个控件之间的逻辑间距。
        g_pageItems.clear();
        auto mkCtl = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                         int h, HMENU id, int indent, int gapAfter) -> HWND {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                     0, 0, Sx(kPageCtlW), Sx(h), page, id, g_hInst, NULL);
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
            PageItem it;
            it.hwnd = c;
            it.indent = indent;
            it.h = h;
            it.gapAfter = gapAfter;
            g_pageItems.push_back(it);
            return c;
        };
        auto secTitle = [&](const wchar_t* text, HMENU id) { // 分组标题（正常文字色）
            mkCtl(L"STATIC", text, 0, 22, id, 0, 2);
        };
        auto secLine = [&]() {                              // 分组分割线（自绘）
            mkCtl(L"STATIC", L"", SS_OWNERDRAW, 10, NULL, 0, 10);
        };

        // ---- 基本
        secTitle(L"基本", (HMENU)IDC_SEC_STARTUP);
        secLine();
        mkCtl(L"BUTTON", L"开机自动启动", BS_AUTOCHECKBOX, 24, (HMENU)IDC_AUTOSTART, kPageIndent, 10);
        mkCtl(L"BUTTON", L"本软件启动时通知", BS_AUTOCHECKBOX, 24, (HMENU)IDC_STARTUP_NOTIFY, kPageIndent, 18);

        // ---- 拦截（放在提醒之前）
        secTitle(L"拦截", (HMENU)IDC_SEC_BLOCK);
        secLine();
        // 拦截重复启动 与 拦截后气泡通知：两个独立开关（默认均开启）
        mkCtl(L"BUTTON", L"拦截重复启动", BS_AUTOCHECKBOX, 24, (HMENU)IDC_BLOCK_DUP, kPageIndent, 10);
        // 气泡通知控制下方标题/内容的可编辑性
        mkCtl(L"BUTTON", L"拦截到重复启动时气泡通知", BS_AUTOCHECKBOX, 24, (HMENU)IDC_BLOCK_NOTIFY, kPageIndent, 10);
        // 重复启动判定时间（在这个时间窗口内再次启动才算重复；仅在开启拦截时可用）
        mkCtl(L"STATIC", L"重复启动判定时间（毫秒）", 0, 20, NULL, kPageIndent, 4);
        mkCtl(L"EDIT", L"2000", ES_AUTOHSCROLL | ES_NUMBER, 26, (HMENU)IDC_DUP_WINDOW_EDIT, kPageIndent, 10);
        // 通知标题
        mkCtl(L"STATIC", L"通知标题", 0, 20, NULL, kPageIndent, 4);
        mkCtl(L"EDIT", L"", ES_AUTOHSCROLL, 26, (HMENU)IDC_TITLE_EDIT, kPageIndent, 10);
        // 通知内容
        mkCtl(L"STATIC", L"通知内容", 0, 20, NULL, kPageIndent, 4);
        mkCtl(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
              90, (HMENU)IDC_CONTENT_EDIT, kPageIndent, 4);
        mkCtl(L"STATIC", L"支持 {app}，\\n 表示换行。", 0, 32, NULL, kPageIndent, 18);

        // ---- 提醒
        secTitle(L"提醒", (HMENU)IDC_SEC_REMINDER);
        secLine();
        // 其他程序启动时提醒
        mkCtl(L"BUTTON", L"其他软件启动时提醒", BS_AUTOCHECKBOX, 24, (HMENU)IDC_LAUNCH_REMINDER, kPageIndent, 10);
        // 提醒内容（仅开启时可编辑）
        mkCtl(L"STATIC", L"提醒内容", 0, 20, NULL, kPageIndent, 4);
        mkCtl(L"EDIT", L"", ES_AUTOHSCROLL, 26, (HMENU)IDC_REMINDER_EDIT, kPageIndent, 4);
        mkCtl(L"STATIC", L"{app} 会被替换为软件名称，\\n 表示换行。", 0, 32, NULL, kPageIndent, 12);
        // 提醒显示时长
        mkCtl(L"STATIC", L"提醒显示时长（毫秒）", 0, 20, NULL, kPageIndent, 4);
        mkCtl(L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER, 26, (HMENU)IDC_REMINDER_TIMEOUT_EDIT, kPageIndent, 10);
        // 视觉引导动画（依赖"其他软件启动时提醒"，未开启时置灰不可选）
        mkCtl(L"BUTTON", L"视觉引导动画", BS_AUTOCHECKBOX, 24, (HMENU)IDC_COVER_WINDOW, kPageIndent, 18);

        // ---- 白名单（列表形式，自带滚动条，独立于页面滚动）
        secTitle(L"白名单", (HMENU)IDC_SEC_WHITELIST);
        secLine();
        // LBS_HASSTRINGS 必须加：owner-draw 列表没有它，LB_ADDSTRING 存的是被截断的 item data
        // 而非字符串，会导致列表项乱码且 LB_GETTEXT 读不回来（保存白名单时丢数据）
        mkCtl(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | LBS_NOTIFY,
              150, (HMENU)IDC_WL_LIST, kPageIndent, 4);
        // 与列表同宽，保持整个白名单区块左对齐、右端齐平
        mkCtl(L"BUTTON", L"添加软件…", BS_PUSHBUTTON, 30, (HMENU)IDC_WL_ADD, kPageIndent, 6);
        mkCtl(L"STATIC", L"列表内的程序不会拦截或提醒，右键或长按删除。", 0, 32, NULL, kPageIndent, 18);

        // ---- 高级
        secTitle(L"高级", (HMENU)IDC_SEC_OTHER);
        secLine();
        // 进程扫描间隔
        mkCtl(L"STATIC", L"进程扫描间隔（毫秒）", 0, 20, NULL, kPageIndent, 4);
        mkCtl(L"EDIT", L"100", ES_AUTOHSCROLL | ES_NUMBER, 26, (HMENU)IDC_POLL_EDIT, kPageIndent, 18);
        // 按钮（每个独占一行，与输入框/列表同宽）。没有“保存”按钮：关闭设置窗口时保存。
        mkCtl(L"BUTTON", L"退出软件", BS_PUSHBUTTON, 30, (HMENU)IDC_EXIT_BTN, kPageIndent, 10);
        mkCtl(L"BUTTON", L"还原默认设置", BS_PUSHBUTTON, 30, (HMENU)IDC_RESTORE_BTN, kPageIndent, 20);

        // 状态提示（最后一行）
        mkCtl(L"STATIC", L"", 0, 20, (HMENU)IDC_STATUS, 0, 8);

        LayoutPage(hwnd);

        // 载入当前设置
        CheckDlgButton(page, IDC_AUTOSTART, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(page, IDC_STARTUP_NOTIFY, g_settings.startupNotify ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(page, IDC_LAUNCH_REMINDER, g_settings.launchReminder ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(page, IDC_BLOCK_DUP, g_settings.blockDup ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(page, IDC_BLOCK_NOTIFY, g_settings.blockNotify ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(page, IDC_COVER_WINDOW, g_settings.coverWindow ? BST_CHECKED : BST_UNCHECKED);
        SetWindowTextW(GetDlgItem(page, IDC_REMINDER_EDIT), EffectiveReminderContent().c_str());
        SetDlgItemInt(page, IDC_REMINDER_TIMEOUT_EDIT, (int)g_settings.reminderTimeoutMs, FALSE);
        SetWindowTextW(GetDlgItem(page, IDC_TITLE_EDIT), g_settings.titleFormat.c_str());
        SetWindowTextW(GetDlgItem(page, IDC_CONTENT_EDIT), g_settings.contentFormat.c_str());
        SubclassWhitelistList(GetDlgItem(page, IDC_WL_LIST)); // 先子类再填充，确保删除按钮命中
        for (auto& w : g_settings.whitelist)
            SendMessageW(GetDlgItem(page, IDC_WL_LIST), LB_ADDSTRING, 0, (LPARAM)w.c_str());
        SetDlgItemInt(page, IDC_POLL_EDIT, (int)g_settings.pollIntervalMs, FALSE);
        SetDlgItemInt(page, IDC_DUP_WINDOW_EDIT, (int)g_settings.dupWindowMs, FALSE);
        // 提醒内容/时长仅在"其他程序启动时提醒"开启时可编辑；覆盖开关同样依赖它
        bool remindOn = g_settings.launchReminder;
        EnableWindow(GetDlgItem(page, IDC_REMINDER_EDIT), remindOn);
        EnableWindow(GetDlgItem(page, IDC_REMINDER_TIMEOUT_EDIT), remindOn);
        EnableWindow(GetDlgItem(page, IDC_COVER_WINDOW), remindOn);
        // 拦截关闭时，"拦截气泡通知"选项、判定时间及标题/内容一并禁用
        EnableWindow(GetDlgItem(page, IDC_BLOCK_NOTIFY), g_settings.blockDup);
        EnableWindow(GetDlgItem(page, IDC_DUP_WINDOW_EDIT), g_settings.blockDup);
        EnableWindow(GetDlgItem(page, IDC_TITLE_EDIT), g_settings.blockDup && g_settings.blockNotify);
        EnableWindow(GetDlgItem(page, IDC_CONTENT_EDIT), g_settings.blockDup && g_settings.blockNotify);
        // 跟随系统明暗（深色时把标准控件一起换成深色主题）
        SetTimer(hwnd, IDT_THEME, 1000, NULL); // 兜底轮询：系统切换明暗时 WM_SETTINGCHANGE 偶尔收不到
        ApplySettingsTheme(hwnd);
        return 0;
    }

    case WM_ERASEBKGND: { // 客户区被页面子窗口盖住，这里只兜底避免拉伸时露白
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_ui.brPage);
        return 1;
    }

    case WM_SETTINGCHANGE:
        // 系统切换浅色/深色时会广播 WM_SETTINGCHANGE，lParam 为 "ImmersiveColorSet"
        if (lp && !IsBadStringPtrW((LPCWSTR)lp, 64) &&
            _wcsicmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0)
            OnSystemThemeMaybeChanged();
        return 0;

    case WM_THEMECHANGED:
        OnSystemThemeMaybeChanged();
        return 0;

    case WM_TIMER:
        if (wp == IDT_THEME) {
            OnSystemThemeMaybeChanged();
            return 0;
        }
        break;

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
        if (dis->CtlType == ODT_STATIC) {
            // 分组分割线：先铺页面底色（避免滚动时留下残影），再在竖直中线画一条 1px 细线
            RECT rc = dis->rcItem;
            FillRect(dis->hDC, &rc, g_ui.brPage);
            int y = rc.top + (rc.bottom - rc.top) / 2;
            RECT line = { rc.left, y, rc.right, y + 1 };
            FillRect(dis->hDC, &line, g_ui.brSep);
            return TRUE;
        }
        if (dis->CtlType != ODT_LISTBOX) break;
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        if (dis->itemState & ODS_SELECTED) {
            FillRect(hdc, &rc, g_ui.brPage); // 先铺底，避免选中行两侧留白
            FillRect(hdc, &rc, g_ui.brSel);
            SetTextColor(hdc, g_ui.selText);
        } else {
            FillRect(hdc, &rc, g_ui.brField);
            SetTextColor(hdc, g_ui.fieldText);
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
                // 覆盖功能依附于提醒功能：提醒关闭时置灰不可勾选（勾选状态保留）
                EnableWindow(GetDlgItem(g_settingsPage, IDC_COVER_WINDOW), on);
            }
            break;
        case IDC_BLOCK_DUP: // 关闭拦截时，拦截气泡通知选项及其文案一并禁用
            if (HIWORD(wp) == BN_CLICKED) {
                bool dup  = IsDlgButtonChecked(g_settingsPage, IDC_BLOCK_DUP) == BST_CHECKED;
                bool noti = IsDlgButtonChecked(g_settingsPage, IDC_BLOCK_NOTIFY) == BST_CHECKED;
                EnableWindow(GetDlgItem(g_settingsPage, IDC_BLOCK_NOTIFY), dup);
                EnableWindow(GetDlgItem(g_settingsPage, IDC_DUP_WINDOW_EDIT), dup);
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
        KillTimer(hwnd, IDT_THEME);
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
//
// 一条提醒 = 一个分层窗口（WS_EX_LAYERED + UpdateLayeredWindow 逐像素 alpha）：
//  - 只有逐像素 alpha 才能画出抗锯齿的圆角与描边（窗口区域 SetWindowRgn 是二值裁剪，
//    角上必然有锯齿），窗口整体的不透明度则由 BLENDFUNCTION 的 SourceConstantAlpha
//    给出，于是"位移/缩放"和"渐显渐隐"可以同时播放、互不干扰；
//  - 卡片内容用 GDI+ 画进 32 位 DIB（圆角走 GraphicsPath、文字走 AntiAliasGridFit）；
//  - 背景优先申请系统模糊材质（Win11 系统背景材质 / Win10 亚克力），申请不到就退回
//    较不透明的纯色，保证任何系统上都看得清。

struct ReminderInst {
    HWND hwnd = NULL;
    std::wstring key;   // 去重键：exe 全路径小写（取不到路径时用显示名）
    DWORD targetPid = 0;
    std::wstring text;
    std::wstring app;   // 软件显示名（截断时据此保留“正在启动”等固定文案）
    DWORD timeoutMs = kDefaultReminderTimeout;
    ULONGLONG shownTick = 0;
    int  phase = 0;     // kRemPhase*
    bool anchoredToClick = false;
    bool anchoredToWnd = false;
    int  tickCount = 0;
    HFONT font = NULL;
    HICON icon = NULL;               // 被启动软件的程序图标（SHGetFileInfo 取得，左侧展示）
    Gdiplus::Bitmap* iconBmp = NULL; // 同一个图标的 GDI+ 位图（自绘用，保住 alpha 通道）
    Gdiplus::Font*   gpFont  = NULL; // 由上面的 LOGFONT 生成的 GDI+ 字体（缓存，别每帧重建）

    // 几何与动画（物理像素 / 屏幕坐标）
    int  x = 0, y = 0, w = 0, h = 0;
    int  radius = 0;         // 圆角半径（覆盖动画里会随尺寸收窄）
    int  alpha = 255;        // 当前整体不透明度
    int  baseAlpha = 255;    // 稳定状态的不透明度
    bool popFromBelow = true;// true：起点更低（向上滑入）；false：起点更高（向下滑入）
    int  popY0 = 0;          // 弹出动画起点 y
    int  popY1 = 0;          // 弹出动画终点 y
    int  fadeMs = kFadeMs;   // 本次渐隐时长
    ULONGLONG phaseTick = 0; // 当前阶段开始时刻
    RECT coverFrom = { 0, 0, 0, 0 };
    RECT coverTo   = { 0, 0, 0, 0 };
    HWND targetWnd = NULL;   // 覆盖动画跟随的目标窗口

    // 绘制资源
    HDC     memDC = NULL;
    HBITMAP dib = NULL, oldDib = NULL;
    void*   bits = NULL;
    int     surfW = 0, surfH = 0; // 实际分配的 DIB 尺寸（只增不减，避免缩放动画每帧重建）
    bool    dirty = true;

    // 视觉引导动画：提醒超时消失后仍继续等目标窗口，窗口一出现就补播这段动画
    std::wstring exeName;            // 目标 exe 文件名（小写，用于目标进程已退出时按名字找窗口）
    std::wstring exePath;            // 目标 exe 全路径（小写，优先按它精确匹配窗口）
    ULONGLONG   lastPollTick = 0;    // 上一次轮询目标窗口的时刻（轮询按时间算，与时钟频率解耦）
    ULONGLONG   lastExeScan  = 0;    // 上一次"按 exe 名找窗口"的时刻（这条路径较贵，单独降频）
    bool        pendingCover = false;// 弹出动画结束后立刻进入"覆盖目标窗口"动画
    bool        coverDone = false;   // 本次提醒是否已经播放过覆盖动画
    ULONGLONG   waitUntil = 0;       // 等待目标窗口的截止时刻（0 = 不在等待）
    // 补播动画必须回到"隐藏前的位置"，否则弹出位置会跑到窗口那边去
    int  restoreX = 0, restoreY = 0;
    bool restoreFromBelow = true;
    // "视觉引导动画"进度 0..1：小窗放大去覆盖目标窗口时，图标一边往中间聚、一边放大，
    // 文字同时淡出；进度与小窗宽高用的是同一条缓动曲线，所以两者始终同步。
    double coverT = 0.0;
};
static const int kRemPhaseIdle = 0, kRemPhasePopIn = 1, kRemPhaseSteady = 2,
                 kRemPhaseCover = 3, kRemPhaseFade = 4, kRemPhaseWait = 5;
static std::vector<ReminderInst*> g_rems; // 当前显示中的各条提醒（主线程专用）

static ReminderInst* FindReminderByKey(const std::wstring& key) {
    for (auto* r : g_rems) if (r->key == key) return r;
    return NULL;
}

// 窗口的可见外框：优先用 DWM 的 EXTENDED_FRAME_BOUNDS（不含 Win10/11 那圈看不见的
// 调整边框），这样"贴窗口"和"覆盖窗口"都不会差出几个像素（避免错位）
static RECT VisibleWindowRect(HWND h) {
    RECT ext;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext))) &&
        ext.right > ext.left && ext.bottom > ext.top)
        return ext;
    RECT rc = { 0, 0, 0, 0 };
    GetWindowRect(h, &rc);
    return rc;
}

// 枚举回调：目标 pid 是否已有可见主窗口（用于窗口定位 / 判定“软件完全打开” / 覆盖跟随）
struct FindWndCtx { DWORD pid; HWND hwnd; RECT rc; bool found; };
static BOOL CALLBACK FindMainWndCB(HWND h, LPARAM lp) {
    FindWndCtx* ctx = (FindWndCtx*)lp;
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
    ctx->rc = VisibleWindowRect(h);
    ctx->hwnd = h;
    ctx->found = true;
    return FALSE; // 找到一个即停
}

static bool FindVisibleMainWindow(DWORD pid, RECT& out, HWND* outWnd = NULL) {
    FindWndCtx ctx = { pid, NULL, { 0, 0, 0, 0 }, false };
    EnumWindows(FindMainWndCB, (LPARAM)&ctx);
    if (ctx.found) {
        out = ctx.rc;
        if (outWnd) *outWnd = ctx.hwnd;
    }
    return ctx.found;
}

// 覆盖动画每一帧都要重新确认目标窗口（窗口被移动/缩放时不能让提醒停在旧位置上）
static bool GetTargetWindowRect(HWND h, RECT& out) {
    if (!h || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h)) return false;
    RECT rc = VisibleWindowRect(h);
    if (rc.right <= rc.left || rc.bottom <= rc.top) return false;
    out = rc;
    return true;
}

// 按 exe 找可见主窗口：目标进程是个"启动即退出的存根"、真程序窗口稍后才出现时用。
// 优先按全路径精确匹配（同名不同目录的程序不会被认错），路径不可用时才退回文件名。
struct FindExeCtx { const std::wstring* exe; const std::wstring* path; HWND hwnd; RECT rc; bool found; };
static BOOL CALLBACK FindByExeCB(HWND h, LPARAM lp) {
    FindExeCtx* ctx = (FindExeCtx*)lp;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    RECT rc;
    if (!GetWindowRect(h, &rc)) return TRUE;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return TRUE;
    std::wstring path = QueryProcPath(pid);
    if (path.empty()) return TRUE;
    std::wstring pathLower = Lower(path);
    if (ctx->path && !ctx->path->empty()) {
        if (pathLower != *ctx->path) return TRUE; // 有全路径：只认完全一致的那个程序
    } else if (Lower(GetFileName(path)) != *ctx->exe) {
        return TRUE;
    }
    ctx->rc = VisibleWindowRect(h);
    ctx->hwnd = h;
    ctx->found = true;
    return FALSE; // 找到一个即停
}

static bool FindWindowByExeName(const std::wstring& exeLower, const std::wstring& pathLower,
                                RECT& out, HWND* outWnd) {
    // 第一轮：按全路径精确找（避免把别的目录里的同名程序当成目标）
    if (!pathLower.empty()) {
        FindExeCtx ctx = { &exeLower, &pathLower, NULL, { 0, 0, 0, 0 }, false };
        EnumWindows(FindByExeCB, (LPARAM)&ctx);
        if (ctx.found) {
            out = ctx.rc;
            if (outWnd) *outWnd = ctx.hwnd;
            return true;
        }
    }
    // 第二轮：退回按文件名（存根退出后真程序换了目录、或拿不到原始路径的情况）
    if (exeLower.empty()) return false;
    FindExeCtx ctx2 = { &exeLower, NULL, NULL, { 0, 0, 0, 0 }, false };
    EnumWindows(FindByExeCB, (LPARAM)&ctx2);
    if (ctx2.found) {
        out = ctx2.rc;
        if (outWnd) *outWnd = ctx2.hwnd;
    }
    return ctx2.found;
}

// 目标进程是否已退出（短命存根 / 被拦截杀掉的第二实例）
static bool TargetProcessExited(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return true;
    DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_OBJECT_0;
}

// 计算提醒该出现在哪：锚点上方 8px、水平居中，并钳制在所在显示器工作区内。
// placedBelow=true 表示上方放不下（点击位置贴着屏幕顶部），于是改放到了锚点下方，
// 弹出动画的方向也要跟着反过来（从上往下）。
static RECT ComputeReminderRect(POINT anchor, int w, int h, bool& placedBelow) {
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    int x = anchor.x - w / 2;
    int y = anchor.y - Sx(8) - h;
    placedBelow = false;
    if (x < wa.left) x = wa.left;
    if (x + w > wa.right) x = wa.right - w;
    if (y < wa.top) { y = anchor.y + Sx(8); placedBelow = true; } // 上方放不下则移到锚点下方
    if (y + h > wa.bottom) y = wa.bottom - h;
    RECT rc = { x, y, x + w, y + h };
    return rc;
}

// ---------------------------------------------------------------- 覆盖层配色

// 覆盖层配色（跟随系统明暗）。这里不使用任何系统模糊/亚克力/云母材质：
// 实测材质的绘制范围是"整块窗口矩形"，与逐像素 alpha 的透明区域无关，
// 一旦生效就会从卡片圆角外的透明像素里透出来（表现为白色直角底），
// 而且分层窗口采样不到背后内容、根本得不到真模糊。
// 逐像素 alpha 才能做出抗锯齿圆角 + 渐隐动画，所以采用半透明纯色底
// （按需求"透明度不要很高"：浅色 98%、深色 96%）。
struct RemTheme {
    COLORREF bg, border, text;
    int bgAlpha;
    int borderAlpha;
};
static RemTheme ReminderColors() {
    RemTheme t;
    if (g_sysDark) {
        t.bg          = RGB(28, 28, 30);
        t.border      = RGB(140, 140, 145); // 灰色描边：和浅色版一样是实描边（之前 alpha 只有 170，深色下几乎看不见）
        t.text        = RGB(240, 240, 240);
        t.bgAlpha     = 246;
        t.borderAlpha = 255;
    } else {
        t.bg          = RGB(255, 251, 225); // 浅黄底（与旧版一致）
        t.border      = RGB(210, 170, 70);
        t.text        = RGB(60, 60, 60);
        t.bgAlpha     = 250;
        t.borderAlpha = 255;
    }
    return t;
}

// ---------------------------------------------------------------- 覆盖层绘制

// 覆盖层要能在"整块窗口那么大"的尺寸上逐帧重绘（"移动到目标窗口并覆盖"的动画），
// 所以背景不能每帧都丢给 GDI+ 的路径抗锯齿去填几百万像素（那样只能跑到 20~30fps）：
//   - 纯色底与 4 条直边用连续的 DWORD 填充（顺序写内存，最快，且轴对齐的边不需要抗锯齿）；
//   - 只有 4 个圆角需要抗锯齿，按"半径 + 配色"预渲染成 4 张小角块缓存起来，每帧直接拷贝；
//   - 文字与图标面积很小，仍然交给 GDI+ 抗锯齿。

// 预乘 alpha 的 32 位像素（BGRA，与 UpdateLayeredWindow 的 AC_SRC_ALPHA 要求一致）
static DWORD PremulColor(COLORREF c, int a) {
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    DWORD r = (DWORD)(GetRValue(c) * a / 255);
    DWORD g = (DWORD)(GetGValue(c) * a / 255);
    DWORD b = (DWORD)(GetBValue(c) * a / 255);
    return ((DWORD)a << 24) | (r << 16) | (g << 8) | b;
}

// 用同一个颜色快速填一块矩形（32bpp 预乘 alpha）。
// 先填第一行，其余行整行 memcpy：memcpy 是向量化的，整屏尺寸下比逐像素循环快好几倍。
static void FillPixels(void* bits, int stride, int x, int y, int w, int h, DWORD pm) {
    if (!bits || w <= 0 || h <= 0) return;
    BYTE* base = (BYTE*)bits + (size_t)y * stride + (size_t)x * 4;
    DWORD* first = (DWORD*)base;
    for (int xx = 0; xx < w; ++xx) first[xx] = pm;
    for (int yy = 1; yy < h; ++yy)
        memcpy(base + (size_t)yy * stride, base, (size_t)w * 4);
}

// 圆角矩形路径（角块与整体绘制共用同一条几何，保证形状完全一致）
static void AddRoundRectPath(Gdiplus::GraphicsPath& path, float x, float y,
                             float w, float h, float rad) {
    float d = rad * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d < 1.0f) d = 1.0f;
    path.StartFigure();
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// 4 个圆角的预渲染角块（配色/半径不变时一直复用，只在换主题时重建一次）
struct CornerTiles {
    bool     valid = false;
    int      radius = 0;
    COLORREF bg = 0, border = 0;
    int      bgAlpha = 0, borderAlpha = 0;
    int      size = 0;              // 每块边长
    std::vector<DWORD> px[4];       // 左上 / 右上 / 左下 / 右下
};
static CornerTiles g_cornerTiles;

static void BuildCornerTiles(int radius, COLORREF bg, int bgAlpha,
                            COLORREF border, int borderAlpha) {
    if (radius < 2) radius = 2;
    if (g_cornerTiles.valid && g_cornerTiles.radius == radius &&
        g_cornerTiles.bg == bg && g_cornerTiles.bgAlpha == bgAlpha &&
        g_cornerTiles.border == border && g_cornerTiles.borderAlpha == borderAlpha)
        return;

    const int R = radius;
    const int S = 3 * R + 2; // 临时画布：两个圆角 + 中间直边都放得下
    const int T = R + 1;     // 每块角块边长（含抗锯齿边缘）
    if (T > S) return;

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!dc) return;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = S;
    bi.bmiHeader.biHeight      = -S;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) {
        DeleteDC(dc);
        return;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(dc, bmp);
    memset(bits, 0, (size_t)S * S * 4);
    {
        // 画一次"和大卡片几何完全一致"的圆角矩形，再抠出四个角
        Gdiplus::Bitmap gb(S, S, S * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Gdiplus::Graphics g(&gb);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::GraphicsPath path;
        AddRoundRectPath(path, 0.5f, 0.5f, (float)S - 1.0f, (float)S - 1.0f, (float)R);
        Gdiplus::SolidBrush bgBrush(Gdiplus::Color((BYTE)bgAlpha, GetRValue(bg),
                                                   GetGValue(bg), GetBValue(bg)));
        g.FillPath(&bgBrush, &path);
        Gdiplus::Pen pen(Gdiplus::Color((BYTE)borderAlpha, GetRValue(border),
                                        GetGValue(border), GetBValue(border)), 1.0f);
        g.DrawPath(&pen, &path);
    }
    g_cornerTiles.size = T;
    for (int c = 0; c < 4; ++c) {
        int sx = (c == 1 || c == 3) ? S - T : 0;
        int sy = (c == 2 || c == 3) ? S - T : 0;
        g_cornerTiles.px[c].assign((size_t)T * T, 0);
        for (int y = 0; y < T; ++y)
            memcpy(&g_cornerTiles.px[c][(size_t)y * T],
                   (BYTE*)bits + (size_t)(sy + y) * S * 4 + (size_t)sx * 4,
                   (size_t)T * 4);
    }
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);

    g_cornerTiles.radius      = radius;
    g_cornerTiles.bg          = bg;
    g_cornerTiles.bgAlpha     = bgAlpha;
    g_cornerTiles.border      = border;
    g_cornerTiles.borderAlpha = borderAlpha;
    g_cornerTiles.valid       = true;
}

// 分配（必要时扩容）32 位 DIB 画布。只增不减：覆盖动画每帧尺寸都在变，
// 每帧重建位图会很浪费。
static void EnsureSurface(ReminderInst* r, int needW, int needH) {
    if (needW < 1) needW = 1;
    if (needH < 1) needH = 1;
    if (r->memDC && r->surfW >= needW && r->surfH >= needH) return;
    if (r->memDC) {
        if (r->oldDib) SelectObject(r->memDC, r->oldDib);
        if (r->dib) DeleteObject(r->dib);
        DeleteDC(r->memDC);
        r->memDC = NULL; r->dib = NULL; r->oldDib = NULL; r->bits = NULL;
    }
    int w = (needW > r->surfW) ? needW : r->surfW;
    int h = (needH > r->surfH) ? needH : r->surfH;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    HDC screen = GetDC(NULL);
    r->memDC = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!r->memDC) return;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h; // 负数=自上而下，与 GDI+ 坐标一致
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    r->dib = CreateDIBSection(r->memDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!r->dib) {
        DeleteDC(r->memDC);
        r->memDC = NULL;
        return;
    }
    r->bits  = bits;
    r->oldDib = (HBITMAP)SelectObject(r->memDC, r->dib);
    r->surfW = w;
    r->surfH = h;
    r->dirty = true;
}

// 把卡片画进 DIB：抗锯齿圆角背景 + 描边 + 程序图标 + 文本
static void RenderReminderContent(ReminderInst* r) {
    if (!r || !r->bits || r->w < 1 || r->h < 1) return;
    const int W = r->w, H = r->h;
    const int stride = r->surfW * 4;
    RemTheme th = ReminderColors();

    int radius = (r->radius > 0) ? r->radius : Sx(16);
    int maxR = (W < H ? W : H) / 2;
    if (radius > maxR) radius = maxR;
    if (radius < 2) radius = 2;

    // ---- 背景：整块纯色 + 4 条直边 + 4 个预渲染角块（见本节开头的性能说明）
    FillPixels(r->bits, stride, 0, 0, W, H, PremulColor(th.bg, th.bgAlpha));
    const DWORD pmBorder = PremulColor(th.border, th.borderAlpha);
    FillPixels(r->bits, stride, radius, 0, W - 2 * radius, 1, pmBorder);
    FillPixels(r->bits, stride, radius, H - 1, W - 2 * radius, 1, pmBorder);
    FillPixels(r->bits, stride, 0, radius, 1, H - 2 * radius, pmBorder);
    FillPixels(r->bits, stride, W - 1, radius, 1, H - 2 * radius, pmBorder);

    BuildCornerTiles(radius, th.bg, th.bgAlpha, th.border, th.borderAlpha);
    const int T = g_cornerTiles.size;
    if (g_cornerTiles.valid && T <= W && T <= H) {
        for (int c = 0; c < 4; ++c) {
            int dx = (c == 1 || c == 3) ? W - T : 0;
            int dy = (c == 2 || c == 3) ? H - T : 0;
            const DWORD* src = g_cornerTiles.px[c].data();
            for (int y = 0; y < T; ++y)
                memcpy((BYTE*)r->bits + (size_t)(dy + y) * stride + (size_t)dx * 4,
                       src + (size_t)y * T, (size_t)T * 4);
        }
    }

    // ---- 文字与图标：面积小，交给 GDI+
    // 文本布局仍用 GDI 量宽度（沿用原有的截断/居中规则），只是画的时候交给 GDI+
    HFONT f = r->font ? r->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT oldFont = (HFONT)SelectObject(r->memDC, f);

    int iconSize  = Sx(18);
    int iconLeft  = Sx(14);
    int iconRight = Sx(14);
    bool hasIcon  = (r->iconBmp != NULL);
    if (hasIcon) iconRight += iconSize + Sx(10);
    int right = W - Sx(14);
    if (right < iconRight) right = iconRight;

    SIZE tsz = { 0, 0 };
    GetTextExtentPoint32W(r->memDC, r->text.c_str(), (int)r->text.size(), &tsz);
    std::wstring disp;
    int textLeft = iconRight;
    if (tsz.cx <= right - iconRight) {
        // 整段放得下：图标+文字作为一组居中摆放，避免短软件名时右侧大片空白
        int gap = hasIcon ? Sx(10) : 0;
        int groupW = (hasIcon ? iconSize : 0) + gap + tsz.cx;
        int groupLeft = (W - groupW) / 2;
        if (groupLeft < Sx(10)) groupLeft = Sx(10);
        if (hasIcon) iconLeft = groupLeft;
        textLeft = groupLeft + (hasIcon ? iconSize + gap : 0);
        if (textLeft < iconRight) textLeft = iconRight;
        disp = r->text;
    } else {
        // 放不下时：省略的是软件名“后面的部分”，完整保留“正在启动”等固定文案，
        // 输出形如“名字开头...正在启动，请稍候”。
        disp = FitMiddleText(r->text, r->app, r->memDC, f, right - iconRight);
    }
    SelectObject(r->memDC, oldFont);
    if (!hasIcon && disp.empty()) {
        r->dirty = false;
        return;
    }

    // GDI+ 字体由同一个 LOGFONT 生成（字号/字形与 GDI 一致），缓存起来避免每帧重建。
    // 必须在把 DIB 从 DC 里撤出之前创建：Font(HDC, LOGFONT) 要用 DC 的 DPI 信息。
    if (!r->gpFont && !disp.empty()) {
        LOGFONTW lf;
        ZeroMemory(&lf, sizeof(lf));
        if (GetObjectW(f, sizeof(lf), &lf) == 0) lf.lfHeight = -Sx(12);
        Gdiplus::Font* pf = new Gdiplus::Font(r->memDC, &lf);
        if (pf->GetLastStatus() == Gdiplus::Ok) r->gpFont = pf;
        else delete pf;
    }

    // GDI 与 GDI+ 不要同时持有同一块 DIB：画之前把位图从 DC 里撤出，画完再选回去。
    // （两者同时引用同一块内存时，个别驱动上可能出现花屏/撕裂。）
    if (r->dib) SelectObject(r->memDC, r->oldDib);
    {
    Gdiplus::Bitmap gbmp(W, H, stride, PixelFormat32bppPARGB, (BYTE*)r->bits);
    Gdiplus::Graphics g(&gbmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    // 视觉引导动画进度：0 = 普通小窗；1 = 已经盖住目标窗口。
    // 图标从"左侧原始位置/原始大小"过渡到"整块中央、放大到 kCoverIconGrow%"，
    // 文字同步淡出。进度用的是与小窗宽高完全相同的缓动值，所以始终同步。
    double ct = r->coverT;
    if (ct < 0.0) ct = 0.0;
    if (ct > 1.0) ct = 1.0;

    if (hasIcon) {
        int baseSize  = iconSize;
        int baseLeft  = iconLeft;
        int baseTop   = (H - baseSize) / 2;
        double grow   = 1.0 + ((double)kCoverIconGrow / 100.0 - 1.0) * ct;
        int sz = (int)(baseSize * grow + 0.5);
        int cap = H / 4; // 目标窗口很扁/很矮时别把图标放得过大
        if (cap > baseSize && sz > cap) sz = cap;
        if (sz < 1) sz = 1;
        double cx = baseLeft + baseSize / 2.0;
        double cy = baseTop + baseSize / 2.0;
        int tx = (int)(cx + (W / 2.0 - cx) * ct + 0.5);
        int ty = (int)(cy + (H / 2.0 - cy) * ct + 0.5);
        Gdiplus::Rect dst(tx - sz / 2, ty - sz / 2, sz, sz);
        g.DrawImage(r->iconBmp, dst, 0, 0, (INT)r->iconBmp->GetWidth(),
                    (INT)r->iconBmp->GetHeight(), Gdiplus::UnitPixel);
    }
    if (!disp.empty() && ct < 1.0) {
        if (r->gpFont) {
            // GenericTypographic：去掉默认格式自带的那圈内边距，位置才好算准
            const Gdiplus::StringFormat* fmt = Gdiplus::StringFormat::GenericTypographic();
            int textA = (int)(255.0 * (1.0 - ct) + 0.5); // 文字随进度淡出
            if (textA > 0) {
                Gdiplus::SolidBrush tb(Gdiplus::Color((BYTE)textA, GetRValue(th.text),
                                                      GetGValue(th.text), GetBValue(th.text)));
                Gdiplus::REAL lh = r->gpFont->GetHeight(&g);
                Gdiplus::REAL ty = ((Gdiplus::REAL)H - lh) / 2.0f;
                g.DrawString(disp.c_str(), (INT)disp.size(), r->gpFont,
                             Gdiplus::PointF((Gdiplus::REAL)textLeft, ty), fmt, &tb);
            }
        }
    }
    } // gbmp / g 在这里析构：此后 GDI+ 不再持有这块 DIB
    if (r->dib) r->oldDib = (HBITMAP)SelectObject(r->memDC, r->dib);
    r->dirty = false;
}

// 把 DIB 按当前矩形与整体不透明度送上屏幕（逐像素 alpha）
static void PresentReminder(ReminderInst* r) {    if (!r || !r->hwnd || !r->memDC) return;
    if (r->dirty) RenderReminderContent(r);
    HDC screen = GetDC(NULL);
    if (!screen) return;
    POINT dst = { r->x, r->y };
    SIZE  sz  = { r->w, r->h };
    POINT src = { 0, 0 };
    int a = r->alpha;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = (BYTE)a;
    bf.AlphaFormat         = AC_SRC_ALPHA;
    UpdateLayeredWindow(r->hwnd, screen, &dst, &sz, r->memDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

// 缓动：末尾带轻微回弹（弹出动画用；back 越大过冲越明显）
static double EaseOutBack(double t, double back) {
    double u = t - 1.0;
    return 1.0 + (back + 1.0) * u * u * u + back * u * u;
}

// 缓动：平滑进出、不回弹（覆盖动画用）
static double EaseInOutCubic(double t) {
    if (t < 0.5) return 4.0 * t * t * t;
    double u = -2.0 * t + 2.0;
    return 1.0 - (u * u * u) / 2.0;
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

// ---------------------------------------------------------------- 动画时钟
// 为什么不再用 SetTimer：普通定时器受系统时钟粒度（约 15.6ms）和"定时器合并"（timer
// coalescing）影响，即使把 uToleranceDelay 设为 0，请求 16ms 实测也只有 ~22~31ms 一跳
// （也就是 30~45 帧）。这里改用 Win10 1803+ 官方推荐的高精度可等待定时器
// （CREATE_WAITABLE_TIMER_HIGH_RESOLUTION），直接在主线程消息循环里等它，按 16ms 推进动画。

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static HANDLE g_animTimer  = NULL;
static volatile LONG g_animActive = 0; // 时钟是否已武装（有提醒在显示）
static int  g_animPeriodMs = 0;        // 当前时钟周期（16=有动画，100=仅轮询）
static bool g_animTimerHiRes = false;  // 是否成功拿到高精度可等待定时器
static bool g_remHiRes = false;         // 是否已把系统时钟精度提到 1ms

// 系统时钟精度只在"真的在放动画、且退回的是普通定时器"时提高；
// 稳定/等待阶段不提高、动画结束立刻恢复，避免无谓唤醒费电。
static void ReminderHiResSet(bool on) {
    if (on == g_remHiRes) return;
    g_remHiRes = on;
    if (on) timeBeginPeriod(1); else timeEndPeriod(1);
}

static void AnimClockInit() {
    if (g_animTimer) return;
    g_animTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    g_animTimerHiRes = (g_animTimer != NULL);
    if (!g_animTimer) g_animTimer = CreateWaitableTimerW(NULL, FALSE, NULL); // 老系统退回普通定时器
}

// 现在需要多快的时钟：有弹出/覆盖/渐隐动画就 16ms，否则 100ms 足够
// （稳定/等待阶段只是每隔一两百毫秒轮询一次目标窗口）。
static int DesiredClockPeriod() {
    for (size_t i = 0; i < g_rems.size(); ++i) {
        int p = g_rems[i]->phase;
        if (p == kRemPhasePopIn || p == kRemPhaseCover || p == kRemPhaseFade) return kRemTimerMs;
    }
    return kRemIdleTickMs;
}

// 武装时钟。幂等：周期没变就不重设（SetWaitableTimer 的 due=0 会立刻触发，
// 每帧都重设会让消息循环空转）。
static void AnimClockArm() {
    AnimClockInit();
    int period = DesiredClockPeriod();
    bool reactivate = (InterlockedExchange(&g_animActive, 1) == 0);
    if (g_animTimer && (reactivate || period != g_animPeriodMs)) {
        LARGE_INTEGER due;
        due.QuadPart = 0; // 立即触发，之后每 period 毫秒一次
        SetWaitableTimer(g_animTimer, &due, period, NULL, NULL, FALSE);
    }
    g_animPeriodMs = period; // 没有可等待定时器时，消息循环的兜底超时也按这个周期
    // 高精度定时器自带精度，不需要提系统精度；只有退回普通定时器且正在放动画时才提
    ReminderHiResSet(!g_animTimerHiRes && period == kRemTimerMs);
}

// 没有提醒要动了就撤掉时钟，彻底安静下来
static void AnimClockDisarm() {
    InterlockedExchange(&g_animActive, 0);
    g_animPeriodMs = 0;
    ReminderHiResSet(false);
    if (g_animTimer) CancelWaitableTimer(g_animTimer);
}

// 推进所有在显示的提醒一帧。窗口可能在动画里被销毁，所以先取句柄快照、
// 每次再用 GWLP_USERDATA 换取（此刻仍有效的）实例指针。
static void ReminderTickAll() {
    if (g_rems.empty()) return;
    std::vector<HWND> wins;
    wins.reserve(g_rems.size());
    for (size_t i = 0; i < g_rems.size(); ++i) wins.push_back(g_rems[i]->hwnd);
    for (size_t i = 0; i < wins.size(); ++i) {
        HWND h = wins[i];
        if (!h || !IsWindow(h)) continue;
        ReminderInst* r = (ReminderInst*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (r) ReminderTimerTick(r);
    }
    // 阶段可能刚变过（动画结束 / 进入等待）：按需在 16ms 与 100ms 之间切换时钟频率。
    // 周期没变时 AnimClockArm 是空操作；提醒全都消失时 RemoveReminder 已撤掉时钟。
    if (!g_rems.empty()) AnimClockArm();
}

// 创建一条覆盖层提醒（独立窗口），挂入 g_rems 后返回
static ReminderInst* CreateReminder() {    RegisterReminderClass();
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                kReminderClass, L"", WS_POPUP,
                                0, 0, OverlayW(), OverlayH(), NULL, NULL, g_hInst, NULL);
    if (!hwnd) return NULL;
    // 不再用 SetWindowRgn 切圆角：区域裁剪是二值的，角上一定有锯齿。
    // 圆角与描边由逐像素 alpha 的自绘内容给出（见 RenderReminderContent）。
    ReminderInst* r = new ReminderInst();
    r->hwnd = hwnd;
    r->radius = Sx(16);
    NONCLIENTMETRICSW ncm;
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    r->font = CreateFontIndirectW(&ncm.lfMessageFont);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)r);
    g_rems.push_back(r);
    return r;
}

// 释放一条提醒：从列表移除并回收绘制资源（在窗口 WM_NCDESTROY 里调用）
static void RemoveReminder(ReminderInst* r) {
    if (!r) return;
    for (size_t i = 0; i < g_rems.size(); ++i) {
        if (g_rems[i] == r) { g_rems.erase(g_rems.begin() + i); break; }
    }
    if (r->gpFont) delete r->gpFont;
    if (r->memDC) {
        if (r->oldDib) SelectObject(r->memDC, r->oldDib);
        if (r->dib) DeleteObject(r->dib);
        DeleteDC(r->memDC);
    }
    if (r->iconBmp) delete r->iconBmp;
    if (r->icon) DestroyIcon(r->icon);
    if (r->font) DeleteObject(r->font);
    if (g_rems.empty()) AnimClockDisarm(); // 最后一条也消失了：停掉动画时钟
    delete r;
}

// 系统明暗变化时，让所有在显示的提醒按新配色重绘
static void RefreshReminderTheme() {
    g_cornerTiles.valid = false; // 配色变了，角块缓存作废
    for (auto* r : g_rems) {
        if (!r || !r->hwnd) continue;
        r->dirty = true;
        PresentReminder(r);
    }
}

static LRESULT CALLBACK ReminderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ReminderInst* r = (ReminderInst*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT; // 点击穿透，只作通知
    case WM_ERASEBKGND:
        return 1; // 内容全部由 UpdateLayeredWindow 提供，擦背景只会闪
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        RemoveReminder(r); // 窗口销毁时回收本条实例（此后不再访问 r / hwnd）
        return 0;
    case WM_PAINT: { // 分层窗口不靠 WM_PAINT 出内容，这里只是把无效区标记为已处理
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 进入渐隐阶段
static void StartReminderFade(ReminderInst* r, int ms) {
    r->phase = kRemPhaseFade;
    r->phaseTick = GetTickCount64();
    r->fadeMs = ms > 0 ? ms : kFadeMs;
}

// 进入"移动到目标窗口并覆盖"阶段
static void StartReminderCover(ReminderInst* r, HWND target, const RECT& wr) {
    r->phase = kRemPhaseCover;
    r->phaseTick = GetTickCount64();
    r->coverFrom.left = r->x;  r->coverFrom.top = r->y;
    r->coverFrom.right = r->x + r->w; r->coverFrom.bottom = r->y + r->h;
    r->coverTo = wr;
    r->targetWnd = target;
    r->anchoredToWnd = true;
    r->coverDone = true; // 引导动画已经播过，之后超时消失就不必再等目标窗口了
    // 目标尺寸可能很大，先把画布按目标大小备好，动画过程中就不必反复重建位图
    EnsureSurface(r, wr.right - wr.left, wr.bottom - wr.top);
}

// 每 16ms 一次：推进弹出/覆盖/渐隐动画，并（按需）轮询目标窗口
static void ReminderTimerTick(ReminderInst* r) {
    if (!r || !r->hwnd) return;
    ULONGLONG now = GetTickCount64();
    ++r->tickCount;

    if (r->phase == kRemPhasePopIn) {
        // ② 弹出：位移走"回弹"曲线，渐显走更短的时长（渐显比位移先完成）
        double t = (double)(now - r->phaseTick) / (double)kPopMs;
        if (t > 1.0) t = 1.0;
        double e = EaseOutBack(t, kPopBack / 100.0);
        r->y = (int)(r->popY0 + (r->popY1 - r->popY0) * e + (e >= 0 ? 0.5 : -0.5));
        double ft = (double)(now - r->phaseTick) / (double)kPopFadeMs;
        if (ft > 1.0) ft = 1.0;
        r->alpha = (int)(r->baseAlpha * ft + 0.5);
        if (t >= 1.0) {
            r->y = r->popY1;
            r->alpha = r->baseAlpha;
            r->phase = kRemPhaseSteady;
            r->phaseTick = now;
            // 补播的引导动画：弹出结束后立刻接着去覆盖目标窗口
            if (r->pendingCover) {
                r->pendingCover = false;
                RECT tw = { 0, 0, 0, 0 };
                HWND hw = r->targetWnd;
                if (!hw || !GetTargetWindowRect(hw, tw)) {
                    hw = NULL;
                    if (!FindVisibleMainWindow(r->targetPid, tw, &hw))
                        FindWindowByExeName(r->exeName, r->exePath, tw, &hw);
                }
                if (hw) StartReminderCover(r, hw, tw);
            }
        }
        PresentReminder(r);
        return;
    }

    if (r->phase == kRemPhaseSteady) {
        // 按时间轮询（与时钟频率无关）：稳定阶段本来就不需要 60fps
        if (now - r->lastPollTick < (ULONGLONG)kRemPollMs) return;
        r->lastPollTick = now;
        RECT wr;
        HWND hw = NULL;
        bool hasWnd = FindVisibleMainWindow(r->targetPid, wr, &hw);
        // 第②级定位：没拿到点击位置时，主窗口一出现就贴到窗口上沿
        // （开启"覆盖目标窗口"时不在这里贴，交给下面的覆盖动画一次到位，避免动画起点错位）
        EnterCriticalSection(&g_settingsLock);
        bool cover = g_settings.coverWindow;
        LeaveCriticalSection(&g_settingsLock);
        if (hasWnd && !cover && !r->anchoredToClick && !r->anchoredToWnd) {
            POINT a = { (wr.left + wr.right) / 2, wr.top };
            bool below = false;
            RECT rc = ComputeReminderRect(a, r->w, r->h, below);
            r->x = rc.left;
            r->y = rc.top;
            r->anchoredToWnd = true;
            PresentReminder(r);
        }
        if (hasWnd) {
            if (cover) {
                // ④ 目标窗口已经显示：移动到窗口位置并放大到窗口大小，覆盖其上
                StartReminderCover(r, hw, wr);
                return;
            }
            StartReminderFade(r, kFadeMs); // 窗口已出现，提醒没用了
        } else if (now - r->shownTick >= r->timeoutMs || TargetProcessExited(r->targetPid)) {
            StartReminderFade(r, kFadeMs); // 超时或进程已退出
        }
        return;
    }

    if (r->phase == kRemPhaseCover) {
        if (r->tickCount % kCoverStepTicks != 0) return;
        // 每帧重新取一次目标窗口矩形：窗口在这期间被拖动/缩放时，提醒不会停在旧位置上
        RECT tw;
        if (GetTargetWindowRect(r->targetWnd, tw)) r->coverTo = tw;
        double t = (double)(now - r->phaseTick) / (double)kCoverMs;
        if (t > 1.0) t = 1.0;
        double e = EaseInOutCubic(t); // 平滑进出、不回弹
        const RECT& a = r->coverFrom;
        const RECT& b = r->coverTo;
        int nx = (int)(a.left   + (b.left   - a.left)   * e + 0.5);
        int ny = (int)(a.top    + (b.top    - a.top)    * e + 0.5);
        int nw = (int)((a.right  - a.left) + ((b.right  - b.left) - (a.right - a.left)) * e + 0.5);
        int nh = (int)((a.bottom - a.top)  + ((b.bottom - b.top)  - (a.bottom - a.top))  * e + 0.5);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        r->x = nx;
        r->y = ny;
        if (nw != r->w || nh != r->h) {
            r->w = nw;
            r->h = nh;
            EnsureSurface(r, nw, nh);
            r->dirty = true;
        }
        // 图标/文字动画跟着同一条缓动曲线走（与小窗宽高变化完全同步）
        if (e != r->coverT) {
            r->coverT = e;
            r->dirty = true;
        }
        // 圆角半径保持恒定：一方面放大过程中尺度变化已经很明显，另一方面角块缓存
        // 是按下半径缓存的，半径每帧都变会导致每帧重建角块（得不偿失）。
        if (t >= 1.0) {
            r->x = b.left;
            r->y = b.top;
            StartReminderFade(r, kCoverFadeMs); // 到位后直接渐隐消失
        }
        PresentReminder(r);
        return;
    }

    if (r->phase == kRemPhaseFade) {
        double t = (double)(now - r->phaseTick) / (double)r->fadeMs;
        if (t > 1.0) t = 1.0;
        r->alpha = (int)(r->baseAlpha * (1.0 - t) + 0.5);
        if (t >= 1.0) {
            r->alpha = 0;
            PresentReminder(r);
            // 还欠着"视觉引导动画"没播（目标窗口迟迟没出现）：先不销毁，
            // 隐形地继续等目标窗口，窗口一出现就补播这段动画。
            // 等待总时长以第一次进入等待时算起（waitUntil 只在此处设置一次），
            // 免得"窗口一闪而过"导致反复等下去。
            EnterCriticalSection(&g_settingsLock);
            bool wantCover = g_settings.coverWindow;
            LeaveCriticalSection(&g_settingsLock);
            if (wantCover && !r->coverDone) {
                if (r->waitUntil == 0) {
                    r->waitUntil = now + kCoverWaitMs;
                    // 记住隐藏前的位置与弹出方向：补播动画要从原位置原地弹出，
                    // 不能改用目标窗口的位置（否则看起来就是"错位"）
                    r->restoreX = r->x;
                    r->restoreY = r->y;
                    r->restoreFromBelow = r->popFromBelow;
                }
                if (now < r->waitUntil) {
                    r->phase = kRemPhaseWait;
                    r->phaseTick = now;
                    r->tickCount = 0;
                    r->lastPollTick = 0; // 进入等待后立刻轮询一次
                    r->lastExeScan = 0;
                    return;
                }
            }
            HWND h = r->hwnd;
            DestroyWindow(h); // WM_NCDESTROY 里 RemoveReminder 回收 r，此后不再访问 r
            return;
        }
        PresentReminder(r);
        return;
    }

    if (r->phase == kRemPhaseWait) {
        // 隐形等待目标窗口：一出现就从"隐藏前的原位置"重新弹出，然后接着播覆盖动画
        if (now - r->lastPollTick < (ULONGLONG)kRemWaitPollMs) return;
        r->lastPollTick = now;
        RECT wr;
        HWND hw = NULL;
        bool hasWnd = FindVisibleMainWindow(r->targetPid, wr, &hw);
        if (!hasWnd && now - r->lastExeScan >= (ULONGLONG)kRemExeScanMs) {
            // 目标进程已退出（存根）时按 exe 找：这条路径要遍历进程，降频到 1 秒一次
            r->lastExeScan = now;
            hasWnd = FindWindowByExeName(r->exeName, r->exePath, wr, &hw);
        }
        if (hasWnd) {
            r->targetWnd = hw;
            r->coverTo = wr;
            r->pendingCover = true;
            r->w = OverlayW();
            r->h = OverlayH();
            EnsureSurface(r, r->w, r->h);
            r->dirty = true;
            // 原地弹出：终点点就是隐藏前那一帧的位置，位移方向也沿用当时的方向
            r->popFromBelow = r->restoreFromBelow;
            r->popY1 = r->restoreY;
            r->popY0 = r->restoreY + (r->popFromBelow ? Sx(kPopOffset) : -Sx(kPopOffset));
            r->x = r->restoreX;
            r->y = r->popY0;
            r->alpha = 0;
            r->baseAlpha = 255;
            r->phase = kRemPhasePopIn;
            r->phaseTick = now;
            r->shownTick = now;
            r->tickCount = 0;
            PresentReminder(r);
            return;
        }
        if (now >= r->waitUntil) { // 等够时间还没出现：安静收掉
            HWND h = r->hwnd;
            DestroyWindow(h);
        }
        return;
    }
}

// 取程序图标：优先用 PrivateExtractIconsW 直接取一张较大的图标（小窗里缩到 18px 显示、
// 视觉引导动画里要放大到几十像素，用 shell 的 32x32 放大会糊），失败再退回 shell 大图标。
static HICON LoadAppIcon(const std::wstring& path) {
    if (path.empty()) return NULL;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef UINT (WINAPI *PFN_PrivateExtractIconsW)(LPCWSTR, int, int, int, HICON*, UINT*, UINT, UINT);
        PFN_PrivateExtractIconsW f =
            (PFN_PrivateExtractIconsW)GetProcAddress(u32, "PrivateExtractIconsW");
        if (f) {
            int want = Sx(64);
            if (want < 32) want = 32;
            HICON ic = NULL;
            UINT id = 0;
            if (f(path.c_str(), 0, want, want, &ic, &id, 1, 0) == 1 && ic) return ic;
        }
    }
    SHFILEINFOW sfi;
    ZeroMemory(&sfi, sizeof(sfi));
    if (SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_LARGEICON))
        return sfi.hIcon;
    return NULL;
}

// 显示一条覆盖层提醒。每个软件最多只占一条：同一条还没消失时再次启动（连点、反复点）
// 直接忽略，让"最开始那条"继续播完；不同软件各有各的实例，可以同时显示互不影响。
static void ShowReminder(DWORD pid, const std::wstring& text, const std::wstring& app,
                         DWORD timeoutMs, const std::wstring& exePath,
                         const std::wstring& exeName) {
    std::wstring key = exePath.empty() ? Lower(app) : Lower(exePath);
    if (FindReminderByKey(key)) return; // 已有该软件的提醒：忽略本次，不重播
    ReminderInst* r = CreateReminder();
    if (!r) return;
    ULONGLONG now = GetTickCount64();
    r->key = key;
    r->targetPid = pid;
    r->text = text;
    r->app = app;
    r->timeoutMs = timeoutMs;
    r->shownTick = now;
    r->tickCount = 0;
    r->anchoredToClick = false;
    r->anchoredToWnd = false;
    r->targetWnd = NULL;
    r->baseAlpha = 255;
    r->fadeMs = kFadeMs;
    r->radius = Sx(16);
    r->w = OverlayW();
    r->h = OverlayH();
    // 目标 exe（小写）：进程"启动即退出"时靠它继续找真程序的窗口。
    // 有全路径就优先按全路径精确匹配，避免误认其他目录里的同名程序。
    r->exeName = Lower(exeName.empty() ? GetFileName(exePath) : exeName);
    r->exePath = Lower(exePath);
    r->lastPollTick = 0;
    r->lastExeScan = 0;
    r->pendingCover = false;
    r->coverDone = false;
    r->waitUntil = 0;
    r->coverT = 0.0;

    // 左侧展示程序的图标：按本次 exe 路径读取
    r->icon = LoadAppIcon(exePath);
    if (r->icon) r->iconBmp = Gdiplus::Bitmap::FromHICON(r->icon);
    r->dirty = true;

    // 落点：① 用户点击位置上方；③ 兜底为屏幕底部中央（主窗口出现后交给定时器重定位）
    POINT anchor;
    bool placedBelow = false;
    if (GetLastClickPoint(anchor, now)) {
        r->anchoredToClick = true; // 第①级定位
    } else {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(mon, &mi);
        anchor.x = (mi.rcWork.left + mi.rcWork.right) / 2;
        anchor.y = mi.rcWork.bottom - OverlayH() - Sx(60);
    }
    RECT rc = ComputeReminderRect(anchor, r->w, r->h, placedBelow);

    // ② 弹出动画：默认从下往上滑入（起点比终点低 20px），
    //    只有在"点击位置贴着屏幕顶部、提醒被放到锚点下方"时才改成从上往下滑，
    //    否则向上滑会滑出屏幕外。
    EnsureSurface(r, r->w, r->h);
    r->popFromBelow = !placedBelow;
    r->popY1 = rc.top;
    r->popY0 = rc.top + (r->popFromBelow ? Sx(kPopOffset) : -Sx(kPopOffset));
    r->x = rc.left;
    r->y = r->popY0;
    r->alpha = 0;
    r->phase = kRemPhasePopIn;
    r->phaseTick = now;

    // 逐像素 alpha 的窗口：先把第一帧（半透明、alpha=0）送到屏幕上再显示，
    // 避免先显示出一个系统底色的空窗口
    PresentReminder(r);
    ShowWindow(r->hwnd, SW_SHOWNA);
    AnimClockArm(); // 交给动画时钟按 ~16ms 推进（弹出/覆盖/渐隐都在里面）
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
    // 系统切换浅色/深色（lParam 为 "ImmersiveColorSet"）：设置界面与提醒卡片一起跟着换
    if (msg == WM_SETTINGCHANGE && lp && !IsBadStringPtrW((LPCWSTR)lp, 64) &&
        _wcsicmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) {
        OnSystemThemeMaybeChanged();
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
        // 拦截到重复启动：第一次拦下就立刻弹气泡（原来要等连点结束、防抖 700ms 才弹，
        // 通知会迟到，且显示的是最后一次点击的软件名）。
        // 同一波连点（落在判定时间窗口内）只弹一条，避免连点刷屏。
        LaunchEvent* e = (LaunchEvent*)lp;
        if (e) {
            ULONGLONG now = GetTickCount64();
            DWORD gap = GetDupWindowMs();
            if (g_lastBlockBalloonTick == 0 || now - g_lastBlockBalloonTick >= (ULONGLONG)gap) {
                g_lastBlockBalloonTick = now;
                std::wstring app = GetAppDisplayName(e->pid, e->path, e->exe);
                std::wstring titleFmt, contentFmt;
                GetFormats(titleFmt, contentFmt);
                std::wstring title   = ReplaceTokens(titleFmt, app);
                std::wstring content = ConvertEscapes(ReplaceTokens(contentFmt, app));
                ShowBalloon(title.c_str(), content.c_str());
            }
            delete e;
        }
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
                ShowReminder(e->pid, ConvertEscapes(txt), app, to, iconPath, e->exe);
            }
            delete e;
        }
        return 0;
    }
    case WM_SHOW_STARTUP:
        SetTimer(hwnd, IDT_STARTUP, 1500, NULL); // 稍作延迟，避免刚启动系统未就绪时气泡被吞
        return 0;
    case WM_TIMER:
        if (wp == IDT_STARTUP) {
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
        DWORD dupWindow  = g_settings.dupWindowMs; // 重复启动判定时间窗口（毫秒，设置里可调）
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

            if (last != 0 && (now - last) <= dupWindow) {
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
            if (now - it->second > (ULONGLONG)dupWindow * 2) it = lastLaunch.erase(it);
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

    // 深色模式支持（uxtheme 深色接口 + 系统当前明暗），要在建窗口前准备好
    InitDarkModeSupport();
    BuildUiTheme();

    // GDI+：覆盖层提醒的圆角/文字/图标都靠它做抗锯齿
    ULONG_PTR gdipToken = 0;
    {
        Gdiplus::GdiplusStartupInput gdipInput;
        Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, NULL);
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

    AnimClockInit(); // 动画时钟（有提醒显示时才武装，平时完全静默）

    // 消息循环：同时等"动画时钟"和"窗口消息"。动画时钟到点就推进一帧提醒动画，
    // 这样动画节奏由高精度可等待定时器决定，不受 SetTimer 的系统粒度/定时器合并影响。
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    int exitCode = 0;
    for (;;) {
        // 正常情况下等"动画时钟 + 窗口消息"；万一系统连可等待定时器都建不出来，
        // 就退化成"动画期间 16ms 超时轮询"，功能不受影响。
        DWORD waitCount = g_animTimer ? 1 : 0;
        HANDLE* waitHandles = g_animTimer ? &g_animTimer : NULL;
        DWORD timeout = INFINITE;
        if (!g_animTimer && InterlockedCompareExchange(&g_animActive, 0, 0) != 0)
            timeout = (DWORD)(g_animPeriodMs > 0 ? g_animPeriodMs : kRemTimerMs);
        DWORD w = MsgWaitForMultipleObjectsEx(waitCount, waitHandles, timeout, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
        bool animTick = g_animTimer ? (w == WAIT_OBJECT_0) : (w == WAIT_TIMEOUT);
        if (animTick) { // 动画帧
            if (InterlockedCompareExchange(&g_animActive, 0, 0) != 0) ReminderTickAll();
            continue;
        }
        if (w == (g_animTimer ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0)) { // 有窗口消息
            bool quit = false;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { quit = true; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (quit) { exitCode = (int)msg.wParam; break; }
            continue;
        }
        break; // 等待失败（极少见），直接退出循环
    }

    RemoveTrayIcon();
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_animTimer) { CancelWaitableTimer(g_animTimer); CloseHandle(g_animTimer); g_animTimer = NULL; }
    CloseHandle(hMutex);
    if (g_settingsPage) SaveSettings(); // 设置窗口还开着时，退出前把改动落盘
    if (g_settingsWnd) DestroyWindow(g_settingsWnd);
    if (gdipToken) Gdiplus::GdiplusShutdown(gdipToken);
    return exitCode;
}
