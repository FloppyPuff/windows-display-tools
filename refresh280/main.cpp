// refresh280.cpp — 一键切换显示器刷新率到 280Hz
// 技术方案: Win32 API (EnumDisplaySettingsW / ChangeDisplaySettingsExW), MinGW 编译
//
// 程序行为:
//   1. 双击启动, 立即把活动显示器切换到 280Hz(优先保持当前分辨率与色深)
//   2. 通过 Windows 系统通知(托盘气泡/通知中心)提示切换成功
//   3. 弹出确认窗口: [保持 280Hz] 或 [恢复原刷新率], 需用户点击确认
//   4. 保险措施: 15 秒内未确认则自动恢复原刷新率(防止切换后显示器异常显示)
//   5. 确认"保持"后才会把 280Hz 写入系统显示设置(重启后仍生效)
//
// 设计要点(经三轮审查与实测加固, 并按"笔记本+外接 2K@280 显示器"环境优化):
//   - 事务式切换: 先 CDS_TEST 逐台预检(只校验不改变设置) -> 逐台应用 -> 逐台重新枚举验证;
//     任一台未达标自动回滚已应用的显示器
//     (注: 不使用 CDS_NORESET 批量暂存——该标志仅在配合 CDS_UPDATEREGISTRY 时有效,
//      单独使用会返回 DISP_CHANGE_BADPARAM; 配合它使用会提前写注册表,
//      破坏"用户确认后才持久化"的安全设计)
//   - 恢复/持久化逐台校验返回值, 失败明确提示, 不做虚假成功提示
//   - 只切换支持 280Hz 的显示器, 不支持的(如笔记本内屏)保持原刷新率并明确提示
//   - 显示友好名称(显示器型号/适配器名)而非 \\.\DISPLAYn
//   - 过滤虚拟显示适配器(向日葵 OrayIddDriver/ToDesk 等), 远程会话不误判
//   - 个别显示器读取失败时跳过并提示, 不再整体中止; 全部失败才报错
//   - 状态机防重入: 确认/回滚只允许执行一次
//   - 单实例: 重复启动激活已有窗口并直接退出; 互斥量句柄在退出时释放
//   - 托盘通知不可用时回退到普通消息框
//   - 高 DPI 适配: 界面尺寸与字体按系统 DPI 缩放(Manifest 声明 PerMonitorV2)
//   - 诊断模式(--noop)报告写入 %TEMP%, 避免 exe 位于受保护目录时写入失败

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cwchar>
#include <cwctype>
#include <cstring>

static HINSTANCE g_hInst = NULL;
static HWND g_hwndHidden = NULL;
static HWND g_hwndConfirm = NULL;
static HWND g_hwndStatic = NULL;
static HFONT g_hFont = NULL;
static HANDLE g_hMutex = NULL;
static NOTIFYICONDATAW g_nid = {};
static bool g_trayVisible = false;
static int g_countdown = 15;
static int g_dpi = 96;

static const UINT WM_APP_TRAY = WM_APP + 1;
static const UINT TIMER_COUNTDOWN = 1;
static const UINT TIMER_EXIT = 2;

enum class SwitchState
{
    Idle,                 // 初始/枚举中
    AwaitingConfirmation, // 已切换成功, 等待用户确认(15 秒倒计时)
    Keeping,              // 用户确认保持, 正在持久化
    Reverting,            // 正在恢复原刷新率(取消/超时/失败回滚)
    Finished              // 流程结束, 等待通知展示后退出
};

static SwitchState g_state = SwitchState::Idle;

struct ModeInfo
{
    std::wstring dev;   // 如 \\.\DISPLAY5
    DWORD w, h, freq, bpp;
};

// 每台显示器的诊断/计划信息
struct DisplayPlan
{
    std::wstring dev;        // 设备名, 如 \\.\DISPLAY5
    std::wstring name;       // 适配器名, 如 NVIDIA GeForce RTX 4070 Laptop GPU
    std::wstring monitor;    // 显示器型号, 如 ASUS VG279QM(可能为空或通用名)
    bool readOk = false;
    ModeInfo cur = {};
    bool already280 = false;
    bool hasTarget = false;
    ModeInfo target = {};
};

static std::vector<DisplayPlan> g_plans;
static std::vector<std::wstring> g_filteredVirtual;  // 被忽略的虚拟显示适配器
static std::vector<ModeInfo> g_oldModes;  // 仅含实际要切换的显示器(用于恢复)
static std::vector<ModeInfo> g_newModes;  // 目标 280Hz 模式
static std::wstring g_desc;               // 目标描述(显示器名+模式, 多台用顿号连接)
static std::wstring g_oldDesc;            // 原模式描述
static std::wstring g_skippedDesc;        // 不支持 280Hz 而被跳过的显示器
static std::wstring g_readFailDesc;       // 读取当前模式失败的显示器

// ---------- 工具函数 ----------

static int GetSystemDpi()
{
    HDC hdc = GetDC(NULL);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc) ReleaseDC(NULL, hdc);
    return dpi;
}

static int S(int v)  // 按 DPI 缩放尺寸
{
    return MulDiv(v, g_dpi, 96);
}

static HFONT CreateUiFont()
{
    // 9pt 微软雅黑, 随 DPI 缩放; 失败时回退系统默认 GUI 字体
    HFONT f = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    return f ? f : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static std::wstring ToLower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// 虚拟/间接显示适配器(远程控制软件等安装), 不是真实显示器, 不参与切换
static bool IsVirtualDisplayAdapter(const wchar_t* deviceString)
{
    static const wchar_t* kws[] = {
        L"idddriver", L"indirect display", L"virtual display", L"sunflower"
    };
    std::wstring s = ToLower(deviceString);
    for (auto& k : kws)
        if (s.find(k) != std::wstring::npos) return true;
    return false;
}

// 通过二级枚举获取显示器型号(如 ASUS VG279QM)
static std::wstring GetMonitorName(const wchar_t* devName)
{
    DISPLAY_DEVICEW dd;
    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(devName, 0, &dd, 0) && dd.DeviceString[0])
        return dd.DeviceString;
    return std::wstring();
}

static std::wstring ModeDesc(const ModeInfo& m)
{
    return std::to_wstring(m.w) + L"x" + std::to_wstring(m.h) +
           L" @ " + std::to_wstring(m.freq) + L"Hz";
}

// 友好名称: 优先显示器型号, 型号未知/通用名时用适配器名, 最后用设备名
static std::wstring DisplayLabel(const DisplayPlan& p)
{
    if (!p.monitor.empty() &&
        p.monitor != L"Generic PnP Monitor" && p.monitor != L"通用即插即用监视器")
        return p.monitor;
    if (!p.name.empty()) return p.name;
    return p.dev;
}

static std::wstring LabelForDev(const std::wstring& dev)
{
    for (auto& p : g_plans)
        if (p.dev == dev) return DisplayLabel(p);
    return dev;
}

static std::wstring JoinWithLabels(const std::vector<ModeInfo>& v)
{
    std::wstring s;
    for (size_t i = 0; i < v.size(); i++)
    {
        if (i) s += L"、";
        s += LabelForDev(v[i].dev) + L" (" + ModeDesc(v[i]) + L")";
    }
    return s;
}

static bool GetAttachedDisplays(std::vector<DisplayPlan>& plans)
{
    DISPLAY_DEVICEW dd;
    DWORD i = 0;
    while (true)
    {
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) break;
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
        {
            if (IsVirtualDisplayAdapter(dd.DeviceString))
            {
                g_filteredVirtual.push_back(std::wstring(dd.DeviceName) + L" (" + dd.DeviceString + L")");
            }
            else
            {
                DisplayPlan p;
                p.dev = dd.DeviceName;
                p.name = dd.DeviceString;
                p.monitor = GetMonitorName(dd.DeviceName);
                plans.push_back(p);
            }
        }
        i++;
    }
    return !plans.empty();
}

static bool GetCurrentMode(const wchar_t* dev, DEVMODEW& dm)
{
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    return EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &dm) != FALSE;
}

// 查找 280Hz 模式: 评分制, 依次优先 分辨率+色深完全相同 > 分辨率相同 > 像素面积最大
static bool Find280Mode(const wchar_t* dev, const DEVMODEW& cur, DEVMODEW& out)
{
    DEVMODEW dm;
    bool found = false;
    unsigned long long best = 0;
    int i = 0;
    while (true)
    {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(dev, i, &dm)) break;
        i++;
        if (dm.dmDisplayFrequency != 280) continue;
        bool sameRes = dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight;
        bool sameBpp = dm.dmBitsPerPel == cur.dmBitsPerPel;
        unsigned long long score = (unsigned long long)dm.dmPelsWidth * dm.dmPelsHeight;
        if (sameRes) score += 0x100000000ULL;
        if (sameRes && sameBpp) score += 0x200000000ULL;
        if (!found || score > best)
        {
            out = dm;
            best = score;
            found = true;
        }
    }
    return found;
}

static void AnalyzeDisplays()
{
    for (auto& p : g_plans)
    {
        DEVMODEW cur;
        if (!GetCurrentMode(p.dev.c_str(), cur)) { p.readOk = false; continue; }
        p.readOk = true;
        p.cur = { p.dev, cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency, cur.dmBitsPerPel };
        if (cur.dmDisplayFrequency == 280) { p.already280 = true; continue; }
        DEVMODEW t;
        if (Find280Mode(p.dev.c_str(), cur, t))
        {
            p.hasTarget = true;
            p.target = { p.dev, t.dmPelsWidth, t.dmPelsHeight, t.dmDisplayFrequency, t.dmBitsPerPel };
        }
    }
}

static std::wstring ErrorText(LONG r)
{
    switch (r)
    {
    case DISP_CHANGE_BADMODE:     return L"显示器不支持该显示模式";
    case DISP_CHANGE_FAILED:      return L"显示驱动切换失败";
    case DISP_CHANGE_RESTART:     return L"需要重启电脑才能生效";
    case DISP_CHANGE_NOTUPDATED:  return L"无法写入系统显示设置";
    case DISP_CHANGE_BADFLAGS:    return L"切换参数错误";
    case DISP_CHANGE_BADPARAM:    return L"切换参数无效";
    case DISP_CHANGE_BADDUALVIEW: return L"多显示器配置错误";
    default:                      return L"未知错误";
    }
}

// ---------- 模式应用(事务式) ----------

static LONG ChangeMode(const ModeInfo& m, DWORD flags)
{
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = m.w;
    dm.dmPelsHeight = m.h;
    dm.dmDisplayFrequency = m.freq;
    dm.dmBitsPerPel = m.bpp;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    return ChangeDisplaySettingsExW(m.dev.c_str(), &dm, NULL, flags, NULL);
}

// 重新枚举验证每台设备的当前刷新率是否达到期望值
static bool VerifyFreqs(const std::vector<ModeInfo>& modes, const std::vector<bool>& toCheck,
                        std::wstring& failList)
{
    bool allOk = true;
    for (size_t i = 0; i < modes.size() && i < toCheck.size(); i++)
    {
        if (!toCheck[i]) continue;
        DEVMODEW cur;
        if (!GetCurrentMode(modes[i].dev.c_str(), cur) || cur.dmDisplayFrequency != modes[i].freq)
        {
            allOk = false;
            failList += LabelForDev(modes[i].dev) + L" ";
        }
    }
    return allOk;
}

// 事务式应用: 可选 CDS_TEST 预检 -> 逐台应用 -> 逐台验证; 失败时若提供 rollback 则恢复原模式
// anyApplied 表示是否实际应用过(预检即失败时全程未做任何更改)
static bool ApplyAndVerify(const std::vector<ModeInfo>& modes, DWORD flags, bool preTest,
                           const std::vector<ModeInfo>* rollback, bool& anyApplied,
                           std::wstring& failList)
{
    failList.clear();
    anyApplied = false;

    // 1. 预检: 逐台测试模式是否可用(不改变任何显示设置)
    if (preTest)
    {
        for (size_t i = 0; i < modes.size(); i++)
        {
            LONG r = ChangeMode(modes[i], CDS_TEST);
            if (r != DISP_CHANGE_SUCCESSFUL)
                failList += LabelForDev(modes[i].dev) + L"(" + ErrorText(r) + L") ";
        }
        if (!failList.empty())
            return false;  // 未应用任何变更, 无需回滚
    }

    // 2. 逐台应用
    std::vector<bool> applied(modes.size(), false);
    for (size_t i = 0; i < modes.size(); i++)
    {
        LONG r = ChangeMode(modes[i], flags);
        if (r == DISP_CHANGE_SUCCESSFUL)
        {
            applied[i] = true;
            anyApplied = true;
        }
        else
        {
            failList += LabelForDev(modes[i].dev) + L"(" + ErrorText(r) + L") ";
        }
    }

    // 3. 重新枚举验证实际结果
    std::wstring vFail;
    if (!VerifyFreqs(modes, applied, vFail))
        failList += L"未达目标: " + vFail;

    // 4. 失败时回滚(调用方保证此处 flags 为 0、注册表未被改动)
    if (!failList.empty() && anyApplied && rollback && !rollback->empty())
    {
        for (size_t i = 0; i < rollback->size(); i++)
            ChangeMode((*rollback)[i], 0);
        std::vector<bool> allTrue(rollback->size(), true);
        std::wstring rbFail;
        if (!VerifyFreqs(*rollback, allTrue, rbFail))
            failList += L"回滚亦失败: " + rbFail;
    }
    return failList.empty();
}

// ---------- 通知与退出 ----------

// 优先使用 exe 内嵌的应用图标(资源 ID 1), 失败回退系统默认图标
static HICON LoadAppIcon()
{
    HICON h = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    return h ? h : LoadIconW(NULL, IDI_APPLICATION);
}

// 返回托盘图标是否添加成功; 失败时调用方回退到 MessageBox
static bool ShowToast(const wchar_t* title, const wchar_t* msg, DWORD flags)
{
    if (!g_hwndHidden) return false;

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwndHidden;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = LoadAppIcon();
    lstrcpynW(g_nid.szTip, L"280Hz 刷新率切换", sizeof(g_nid.szTip) / sizeof(WCHAR));
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) return false;
    g_trayVisible = true;

    NOTIFYICONDATAW ni = g_nid;
    ni.uFlags = NIF_INFO;
    lstrcpynW(ni.szInfoTitle, title, sizeof(ni.szInfoTitle) / sizeof(WCHAR));
    lstrcpynW(ni.szInfo, msg, sizeof(ni.szInfo) / sizeof(WCHAR));
    ni.dwInfoFlags = flags;
    Shell_NotifyIconW(NIM_MODIFY, &ni);  // 气泡失败不致命, 托盘图标仍在
    return true;
}

// 统一通知入口: 优先托盘气泡, 失败回退消息框
static void NotifyUser(const wchar_t* title, const wchar_t* msg, DWORD flags)
{
    if (ShowToast(title, msg, flags)) return;

    UINT mbFlags = MB_TOPMOST | MB_SETFOREGROUND;
    if (flags & NIIF_ERROR)        mbFlags |= MB_ICONERROR;
    else if (flags & NIIF_WARNING) mbFlags |= MB_ICONWARNING;
    else                           mbFlags |= MB_ICONINFORMATION;
    MessageBoxW(NULL, msg, title, mbFlags);
}

// 通知 + 退出调度: 气泡有展示时间则等 8 秒; 消息框已阻塞展示过则直接退出
static void CleanupAndExit();
static void FinishWithNotify(const wchar_t* title, const wchar_t* msg, DWORD flags)
{
    NotifyUser(title, msg, flags);
    if (g_trayVisible && g_hwndHidden)
        SetTimer(g_hwndHidden, TIMER_EXIT, 8000, NULL);
    else
        CleanupAndExit();
}

static void CleanupAndExit()
{
    if (g_hwndHidden)
    {
        KillTimer(g_hwndHidden, TIMER_COUNTDOWN);
        KillTimer(g_hwndHidden, TIMER_EXIT);
    }
    if (g_trayVisible)
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayVisible = false;
    }
    if (g_hwndConfirm) { DestroyWindow(g_hwndConfirm); g_hwndConfirm = NULL; }
    if (g_hwndHidden) { DestroyWindow(g_hwndHidden); g_hwndHidden = NULL; }
    if (g_hFont) { DeleteObject(g_hFont); g_hFont = NULL; }
    if (g_hMutex) { CloseHandle(g_hMutex); g_hMutex = NULL; }
    g_state = SwitchState::Finished;
    PostQuitMessage(0);
}

// ---------- 确认窗口与流程 ----------

static void UpdateCountdownText()
{
    if (!g_hwndStatic) return;
    std::wstring text = L"已切换到 " + g_desc + L"。\r\n请在 " + std::to_wstring(g_countdown) +
                        L" 秒内确认保持，超时将自动恢复 " + g_oldDesc + L"。";
    if (!g_skippedDesc.empty())
        text += L"\r\n以下显示器不支持 280Hz，保持原刷新率: " + g_skippedDesc;
    if (!g_readFailDesc.empty())
        text += L"\r\n无法读取以下显示器，保持原样: " + g_readFailDesc;
    SetWindowTextW(g_hwndStatic, text.c_str());
}

static void OnKeep()
{
    if (g_state != SwitchState::AwaitingConfirmation) return;
    g_state = SwitchState::Keeping;
    if (g_hwndHidden) KillTimer(g_hwndHidden, TIMER_COUNTDOWN);
    if (g_hwndConfirm) { DestroyWindow(g_hwndConfirm); g_hwndConfirm = NULL; }

    std::wstring failList;
    bool dummy = false;
    if (ApplyAndVerify(g_newModes, CDS_UPDATEREGISTRY, false, nullptr, dummy, failList))
        FinishWithNotify(L"已确认", L"已保持 280Hz，并写入系统显示设置", NIIF_INFO);
    else
    {
        std::wstring msg = L"部分显示器写入系统设置失败(重启后可能恢复原刷新率): " + failList;
        FinishWithNotify(L"部分失败", msg.c_str(), NIIF_WARNING);
    }
    g_state = SwitchState::Finished;
}

static void OnRevert(bool timedOut)
{
    if (g_state != SwitchState::AwaitingConfirmation) return;
    g_state = SwitchState::Reverting;
    if (g_hwndHidden) KillTimer(g_hwndHidden, TIMER_COUNTDOWN);
    if (g_hwndConfirm) { DestroyWindow(g_hwndConfirm); g_hwndConfirm = NULL; }

    std::wstring failList;
    bool dummy = false;
    if (ApplyAndVerify(g_oldModes, CDS_UPDATEREGISTRY, false, nullptr, dummy, failList))
    {
        std::wstring msg = timedOut
            ? L"15 秒内未确认，已自动恢复 " + g_oldDesc
            : L"已恢复 " + g_oldDesc;
        FinishWithNotify(L"已恢复原刷新率", msg.c_str(), NIIF_INFO);
    }
    else
    {
        std::wstring msg = L"自动恢复失败，请手动检查 设置-系统-屏幕-高级显示: " + failList;
        FinishWithNotify(L"恢复失败", msg.c_str(), NIIF_ERROR);
    }
    g_state = SwitchState::Finished;
}

static LRESULT CALLBACK ConfirmProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HFONT f = g_hFont ? g_hFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        g_hwndStatic = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE, S(18), S(10), S(424), S(96), hwnd, NULL, g_hInst, NULL);
        HWND bKeep = CreateWindowExW(0, L"BUTTON", L"保持 280Hz",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(70), S(112), S(130), S(36), hwnd, (HMENU)IDOK, g_hInst, NULL);
        HWND bRev = CreateWindowExW(0, L"BUTTON", L"恢复原刷新率",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, S(250), S(112), S(130), S(36), hwnd, (HMENU)IDCANCEL, g_hInst, NULL);
        SendMessageW(g_hwndStatic, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(bKeep, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(bRev, WM_SETFONT, (WPARAM)f, TRUE);
        UpdateCountdownText();
        return 0;
    }
    case WM_COMMAND:
        // IDOK = 保持; IDCANCEL = 恢复(Esc 也会触发 IDCANCEL)
        switch (LOWORD(wp))
        {
        case IDOK:     OnKeep();        return 0;
        case IDCANCEL: OnRevert(false); return 0;
        }
        break;
    case WM_CLOSE:      // 点 X / Alt+F4 视为取消
        OnRevert(false);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK HiddenProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TIMER:
        if (wp == TIMER_COUNTDOWN)
        {
            if (g_state != SwitchState::AwaitingConfirmation) return 0;
            g_countdown--;
            if (g_countdown <= 0) OnRevert(true);
            else UpdateCountdownText();
            return 0;
        }
        if (wp == TIMER_EXIT)
        {
            CleanupAndExit();
            return 0;
        }
        break;
    case WM_APP_TRAY:
        if (LOWORD(lp) == NIN_BALLOONUSERCLICK && g_hwndConfirm)
            SetForegroundWindow(g_hwndConfirm);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void MessageLoop()
{
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        // 对话框消息翻译: Tab 切换焦点, Enter 触发默认按钮, Esc 触发 IDCANCEL
        if (g_hwndConfirm && IsDialogMessageW(g_hwndConfirm, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// ---------- 主流程 ----------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    g_hInst = hInst;
    g_dpi = GetSystemDpi();
    g_hFont = CreateUiFont();

    // 诊断模式: --noop 只输出计划, 不实际切换
    bool noop = false;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv)
        {
            for (int i = 1; i < argc; i++)
                if (wcscmp(argv[i], L"--noop") == 0) noop = true;
            LocalFree(argv);
        }
    }

    WNDCLASSW wcHidden = {};
    wcHidden.lpfnWndProc = HiddenProc;
    wcHidden.hInstance = hInst;
    wcHidden.lpszClassName = L"Refresh280Hidden";
    RegisterClassW(&wcHidden);

    WNDCLASSW wcConfirm = {};
    wcConfirm.lpfnWndProc = ConfirmProc;
    wcConfirm.hInstance = hInst;
    wcConfirm.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcConfirm.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wcConfirm.hIcon = LoadAppIcon();
    wcConfirm.lpszClassName = L"Refresh280Confirm";
    RegisterClassW(&wcConfirm);

    g_hwndHidden = CreateWindowExW(0, L"Refresh280Hidden", L"",
        WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwndHidden) return 1;

    // 单实例保护: 重复启动时激活已有窗口并直接退出(不再弹第二次通知)
    g_hMutex = CreateMutexW(NULL, FALSE, L"Local\\Refresh280SingleInstance");
    if (g_hMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND hPrev = FindWindowW(L"Refresh280Confirm", NULL);
        if (!hPrev) hPrev = FindWindowW(L"Refresh280Hidden", NULL);
        if (hPrev)
        {
            ShowWindow(hPrev, SW_SHOW);
            SetForegroundWindow(hPrev);
            FlashWindow(hPrev, TRUE);
        }
        CloseHandle(g_hMutex);
        g_hMutex = NULL;
        DestroyWindow(g_hwndHidden);
        g_hwndHidden = NULL;
        return 0;
    }

    // 枚举活动显示器并分析每台的状态
    if (!GetAttachedDisplays(g_plans))
    {
        FinishWithNotify(L"切换失败",
            L"未检测到活动的物理显示器(可能显示器已关闭或处于远程会话)", NIIF_ERROR);
        MessageLoop();
        return 0;
    }
    AnalyzeDisplays();

    if (noop)
    {
        // 报告写入 %TEMP%, 避免 exe 位于受保护目录(如 Program Files)时写入失败
        wchar_t tempDir[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, tempDir);
        std::wstring reportPath = (n > 0 && n < MAX_PATH)
            ? std::wstring(tempDir) + L"refresh280_debug.txt"
            : L"refresh280_debug.txt";

        std::wstring report = L"\xFEFF[280Hz 切换 - 诊断报告]\r\n报告文件: " + reportPath + L"\r\n";
        if (!g_filteredVirtual.empty())
        {
            report += L"已忽略的虚拟显示适配器(远程控制软件等):\r\n";
            for (auto& v : g_filteredVirtual)
                report += L"  " + v + L"\r\n";
        }
        for (auto& p : g_plans)
        {
            report += L"显示器: " + DisplayLabel(p) + L"  [" + p.dev + L"] (" + p.name + L")\r\n";
            if (!p.readOk)
                report += L"  状态: 读取当前模式失败(将跳过)\r\n";
            else if (p.already280)
                report += L"  状态: 已是 280Hz, 当前 " + ModeDesc(p.cur) + L"\r\n";
            else if (p.hasTarget)
                report += L"  状态: 计划切换 " + ModeDesc(p.cur) + L" -> " + ModeDesc(p.target) + L"\r\n";
            else
                report += L"  状态: 当前 " + ModeDesc(p.cur) + L", 无 280Hz 模式, 将跳过\r\n";
        }

        HANDLE hf = CreateFileW(reportPath.c_str(),
            GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(hf, report.data(), (DWORD)(report.size() * sizeof(WCHAR)), &written, NULL);
            CloseHandle(hf);
        }
        CleanupAndExit();
        return 0;
    }

    // 组装计划: 需要切换的、不支持的(跳过)、读取失败的(跳过并提示)
    for (auto& p : g_plans)
    {
        if (!p.readOk)
        {
            g_readFailDesc += DisplayLabel(p) + L" ";
            continue;
        }
        if (p.hasTarget)
        {
            g_oldModes.push_back(p.cur);
            g_newModes.push_back(p.target);
        }
        else if (!p.already280)
        {
            g_skippedDesc += DisplayLabel(p) + L" ";
        }
    }

    if (g_newModes.empty())
    {
        if (!g_readFailDesc.empty())
        {
            std::wstring msg = L"无法读取以下显示器当前模式，未做任何更改: " + g_readFailDesc;
            FinishWithNotify(L"无法判断", msg.c_str(), NIIF_WARNING);
        }
        else if (!g_skippedDesc.empty())
        {
            std::wstring msg = L"未检测到支持 280Hz 的显示器，请确认外接显示器已连接并开启: " + g_skippedDesc;
            FinishWithNotify(L"切换失败", msg.c_str(), NIIF_ERROR);
        }
        else
        {
            std::wstring alreadyDesc;
            for (auto& p : g_plans)
                if (p.already280) alreadyDesc += DisplayLabel(p) + L" ";
            std::wstring msg = L"以下显示器已是 280Hz，无需切换: " + alreadyDesc;
            FinishWithNotify(L"刷新率切换", msg.c_str(), NIIF_INFO);
        }
        MessageLoop();
        return 0;
    }

    g_desc = JoinWithLabels(g_newModes);
    g_oldDesc = JoinWithLabels(g_oldModes);

    // 事务式切换(预检 -> 应用 -> 验证), 失败自动回滚, 且不写注册表
    std::wstring failList;
    bool anyApplied = false;
    if (!ApplyAndVerify(g_newModes, 0, true, &g_oldModes, anyApplied, failList))
    {
        std::wstring msg = (anyApplied ? L"切换失败，已回滚原刷新率: " : L"切换失败，未做任何更改: ") + failList;
        FinishWithNotify(L"切换失败", msg.c_str(), NIIF_ERROR);
        MessageLoop();
        return 0;
    }

    // 全部切换成功: 系统通知 + 确认窗口 + 15 秒倒计时保险
    g_state = SwitchState::AwaitingConfirmation;
    std::wstring msg = L"已切换到 " + g_desc + L"，请在 15 秒内确认，超时将自动恢复";
    NotifyUser(L"切换成功", msg.c_str(), NIIF_INFO);

    g_countdown = 15;
    g_hwndConfirm = CreateWindowExW(WS_EX_TOPMOST, L"Refresh280Confirm", L"280Hz 切换确认",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, S(460), S(216), NULL, NULL, hInst, NULL);
    if (g_hwndConfirm)
    {
        ShowWindow(g_hwndConfirm, SW_SHOW);
        SetForegroundWindow(g_hwndConfirm);
        SetTimer(g_hwndHidden, TIMER_COUNTDOWN, 1000, NULL);
    }
    else
    {
        OnRevert(false);  // 窗口创建失败也走恢复流程
    }

    MessageLoop();
    return 0;
}
