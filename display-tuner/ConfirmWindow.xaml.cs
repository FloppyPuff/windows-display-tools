using System;
using System.ComponentModel;
using System.Windows;
using System.Windows.Threading;
using Wpf.Ui.Controls;

namespace DisplayTuner;

/// <summary>
/// 15 秒倒计时确认窗口: 未确认(或按 Esc/X)自动恢复原模式, 防黑屏保险。
/// Decision 事件: true = 保持, false = 恢复; 只触发一次。
/// </summary>
public partial class ConfirmWindow : FluentWindow
{
    public event Action<bool> Decision;

    private readonly DispatcherTimer _timer;
    private int _remaining = 15;
    private bool _decided;

    public ConfirmWindow(ModeEntry target, ModeEntry old)
    {
        InitializeComponent();

        DescText.Text = $"已切换到 {target.Width} × {target.Height} @ {target.Frequency}Hz。";
        OldText.Text = $"请在 15 秒内确认保持，超时将自动恢复 {old.Width} × {old.Height} @ {old.Frequency}Hz。";
        UpdateCountdown();

        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _timer.Tick += (_, _) =>
        {
            if (_decided) return;
            _remaining--;
            if (_remaining <= 0) Decide(false);   // 超时 = 恢复
            else UpdateCountdown();
        };

        Loaded += (_, _) => { Activate(); _timer.Start(); };
    }

    private void UpdateCountdown()
    {
        CountdownText.Text = _remaining + " 秒后自动恢复";
    }

    private void Decide(bool keep)
    {
        if (_decided) return;
        _decided = true;
        _timer.Stop();
        Decision?.Invoke(keep);
        Close();
    }

    private void Keep_Click(object sender, RoutedEventArgs e) => Decide(true);

    private void Revert_Click(object sender, RoutedEventArgs e) => Decide(false);

    protected override void OnClosing(CancelEventArgs e)
    {
        // 点 X / Alt+F4 / Esc 视为恢复
        if (!_decided)
        {
            _decided = true;
            _timer.Stop();
            Decision?.Invoke(false);
        }
        base.OnClosing(e);
    }
}
