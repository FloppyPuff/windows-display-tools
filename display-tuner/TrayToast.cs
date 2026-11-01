using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace DisplayTuner;

/// <summary>托盘气泡通知(显示约 10 秒后自动移除图标); 失败时调用方回退到消息框</summary>
internal static class TrayToast
{
    private const uint NIM_ADD = 0;
    private const uint NIM_MODIFY = 1;
    private const uint NIM_DELETE = 2;
    private const uint NIF_ICON = 0x2;
    private const uint NIF_TIP = 0x4;
    private const uint NIF_INFO = 0x10;
    private const uint NIIF_INFO = 0x1;
    private const uint NIIF_WARNING = 0x2;
    private const uint NIIF_ERROR = 0x3;

    private static bool _added;
    private static NativeMethods.NOTIFYICONDATAW _nid;
    private static Timer _hideTimer;

    private static string Truncate(string s, int max)
        => string.IsNullOrEmpty(s) || s.Length <= max ? s : s.Substring(0, max);

    // 优先使用 exe 内嵌的应用图标(ApplicationIcon), 失败回退系统默认图标
    private static IntPtr LoadAppIcon()
    {
        try
        {
            var h = NativeMethods.GetModuleHandleW(null);
            if (h != IntPtr.Zero)
            {
                var icon = NativeMethods.LoadIconW(h, (IntPtr)NativeMethods.MAINICON_RESOURCE_ID);
                if (icon != IntPtr.Zero) return icon;
            }
        }
        catch { }
        return NativeMethods.LoadIconW(IntPtr.Zero, (IntPtr)NativeMethods.IDI_APPLICATION);
    }

    /// <summary>显示一条系统通知; 返回是否成功(失败时可回退消息框)</summary>
    public static bool Show(IntPtr hwnd, string title, string msg, NoticeLevel level)
    {
        if (hwnd == IntPtr.Zero) return false;

        if (!_added)
        {
            _nid = new NativeMethods.NOTIFYICONDATAW
            {
                cbSize = Marshal.SizeOf<NativeMethods.NOTIFYICONDATAW>(),
                hWnd = hwnd,
                uID = 1,
                uFlags = NIF_ICON | NIF_TIP,
                hIcon = LoadAppIcon(),
                szTip = Truncate("显示器参数调节", 127),
            };
            if (!NativeMethods.Shell_NotifyIconW(NIM_ADD, ref _nid)) return false;
            _added = true;
        }

        var nid = _nid;
        nid.uFlags = NIF_INFO;
        nid.szInfoTitle = Truncate(title, 63);
        nid.szInfo = Truncate(msg, 255);
        nid.dwInfoFlags = level == NoticeLevel.Error ? NIIF_ERROR :
                          level == NoticeLevel.Warning ? NIIF_WARNING : NIIF_INFO;
        NativeMethods.Shell_NotifyIconW(NIM_MODIFY, ref nid);

        _hideTimer?.Dispose();
        _hideTimer = new Timer(_ => Hide(), null, 10000, Timeout.Infinite);
        return true;
    }

    public static void Hide()
    {
        if (_added)
        {
            NativeMethods.Shell_NotifyIconW(NIM_DELETE, ref _nid);
            _added = false;
        }
    }
}

internal enum NoticeLevel { Info, Warning, Error }
