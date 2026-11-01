using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Windows;
using Wpf.Ui.Appearance;
using Wpf.Ui.Controls;

namespace DisplayTuner;

public partial class App : Application
{
    private Mutex _mutex;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // 兜底: 未处理异常时显示错误信息而不是让程序静默退出
        DispatcherUnhandledException += (_, args) =>
        {
            System.Windows.MessageBox.Show("程序发生未处理的异常：\n" + args.Exception.Message,
                "显示器参数调节", System.Windows.MessageBoxButton.OK, System.Windows.MessageBoxImage.Error);
            args.Handled = true;
        };

        // 跟随系统深浅色主题(失败不致命, 默认浅色)
        try
        {
            var sys = ApplicationThemeManager.GetSystemTheme();
            ApplicationThemeManager.Apply(sys == SystemTheme.Dark ? ApplicationTheme.Dark : ApplicationTheme.Light);
        }
        catch { }

        // 诊断模式: --noop 输出完整清单到 %TEMP% 后直接退出, 不改动任何设置
        if (e.Args.Contains("--noop"))
        {
            WriteNoopReport();
            Shutdown();
            return;
        }

        // 单实例: 重复启动时激活已有窗口并退出
        _mutex = new Mutex(true, @"Local\DisplayTunerSingleInstance", out bool createdNew);
        if (!createdNew)
        {
            var hwnd = NativeMethods.FindWindowW(null, "显示器参数调节");
            if (hwnd != IntPtr.Zero)
            {
                NativeMethods.ShowWindow(hwnd, NativeMethods.SW_RESTORE);
                NativeMethods.SetForegroundWindow(hwnd);
            }
            Shutdown();
            return;
        }

        var window = new MainWindow();
        MainWindow = window;
        window.Show();
    }

    private static void WriteNoopReport()
    {
        var sb = new StringBuilder();
        sb.AppendLine("[显示器参数调节 - 诊断报告]");

        var displays = DisplayService.GetDisplays();
        if (displays.Count == 0)
            sb.AppendLine("未检测到活动的物理显示器");

        foreach (var d in displays)
        {
            sb.AppendLine();
            sb.AppendLine($"显示器: {d.Label}  [{d.Device}] (适配器: {d.AdapterName})");
            var cur = DisplayService.ReadCurrent(d.Device);
            sb.AppendLine(cur != null
                ? $"  当前模式: {cur.Width} × {cur.Height} @ {cur.Frequency}Hz (色深 {cur.BitsPerPel})"
                : "  当前模式: 读取失败");

            var modes = DisplayService.GetModes(d.Device);
            var groups = DisplayService.GroupByResolution(modes);
            sb.AppendLine($"  分辨率 {groups.Count} 组, 模式 {modes.Count} 个:");
            foreach (var g in groups)
                sb.AppendLine($"    {g.Label}: {string.Join(", ", g.Frequencies.Select(f => f + "Hz"))}");
        }

        var path = Path.Combine(Path.GetTempPath(), "displaytuner_debug.txt");
        try
        {
            File.WriteAllText(path, sb.ToString(), new UTF8Encoding(true));
        }
        catch { }
    }
}
