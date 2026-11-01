using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Interop;
using Wpf.Ui.Controls;

namespace DisplayTuner;

public partial class MainWindow : FluentWindow
{
    private List<DisplayItem> _displays = new();
    private List<ModeEntry> _modes = new();
    private List<ResolutionItem> _resolutions = new();
    private ModeEntry _current;

    private bool _updatingUi;        // 程序化更新控件时抑制联动事件
    private bool _awaitingConfirm;   // 状态机: 已切换, 等待用户确认(期间禁用"应用")
    private ModeEntry _pendingTarget; // 本次已切换的目标模式
    private ModeEntry _pendingOld;    // 切换前的模式(用于恢复)
    private ConfirmWindow _confirmWindow;

    public MainWindow()
    {
        InitializeComponent();
        Loaded += (_, _) => ReloadDisplays();
    }

    // ---------- 枚举与联动 ----------

    private IntPtr Handle => new WindowInteropHelper(this).Handle;

    private void ReloadDisplays()
    {
        var keep = (DisplayCombo.SelectedItem as DisplayItem)?.Device;
        _displays = DisplayService.GetDisplays();

        _updatingUi = true;
        try
        {
            DisplayCombo.ItemsSource = _displays;
            if (_displays.Count == 0)
            {
                DisplayCombo.SelectedIndex = -1;
                ResolutionList.ItemsSource = null;
                FrequencyList.ItemsSource = null;
                CurrentModeText.Text = "当前模式：—";
                SetStatus("未检测到活动的物理显示器(可能显示器已关闭或处于远程会话)", NoticeLevel.Error);
                return;
            }
            int idx = keep != null ? _displays.FindIndex(d => d.Device == keep) : 0;
            DisplayCombo.SelectedIndex = idx >= 0 ? idx : 0;
        }
        finally
        {
            _updatingUi = false;
        }
        LoadDisplay(DisplayCombo.SelectedItem as DisplayItem);
    }

    private void LoadDisplay(DisplayItem d)
    {
        if (d == null) return;

        _modes = DisplayService.GetModes(d.Device);
        _resolutions = DisplayService.GroupByResolution(_modes);
        _current = DisplayService.ReadCurrent(d.Device);

        _updatingUi = true;
        try
        {
            ResolutionList.ItemsSource = _resolutions;
            if (_current != null)
            {
                int i = _resolutions.FindIndex(r => r.Width == _current.Width && r.Height == _current.Height);
                ResolutionList.SelectedIndex = i >= 0 ? i : (_resolutions.Count > 0 ? 0 : -1);
            }
            else
            {
                ResolutionList.SelectedIndex = _resolutions.Count > 0 ? 0 : -1;
            }
            FillFrequencyList(ResolutionList.SelectedItem as ResolutionItem, autoSelect: true);
            UpdateCurrentText(d);
        }
        finally
        {
            _updatingUi = false;
        }
        SetStatus("就绪", NoticeLevel.Info);
    }

    private void UpdateCurrentText(DisplayItem d)
    {
        CurrentModeText.Text = _current != null
            ? $"当前模式：{_current.Width} × {_current.Height} @ {_current.Frequency}Hz"
            : "当前模式：读取失败";
    }

    private void FillFrequencyList(ResolutionItem res, bool autoSelect)
    {
        FrequencyList.ItemsSource = res?.Frequencies;
        if (!autoSelect || res == null || res.Frequencies.Count == 0) return;

        int idx = 0; // 默认选最高
        if (_current != null && _current.Width == res.Width && _current.Height == res.Height)
        {
            int i = res.Frequencies.IndexOf(_current.Frequency);
            if (i >= 0) idx = i; // 当前分辨率默认选当前刷新率
        }
        FrequencyList.SelectedIndex = idx;
    }

    private void DisplayCombo_Changed(object sender, RoutedEventArgs e)
    {
        if (_updatingUi || _awaitingConfirm) return;
        LoadDisplay(DisplayCombo.SelectedItem as DisplayItem);
    }

    private void ResolutionList_Changed(object sender, RoutedEventArgs e)
    {
        if (_updatingUi || _awaitingConfirm) return;
        FillFrequencyList(ResolutionList.SelectedItem as ResolutionItem, autoSelect: true);
    }

    private void Refresh_Click(object sender, RoutedEventArgs e)
    {
        if (_awaitingConfirm) return;
        ReloadDisplays();
    }

    // ---------- 应用流程 ----------

    private void Apply_Click(object sender, RoutedEventArgs e)
    {
        if (_awaitingConfirm) return;

        var d = DisplayCombo.SelectedItem as DisplayItem;
        var res = ResolutionList.SelectedItem as ResolutionItem;
        if (d == null || res == null || FrequencyList.SelectedItem == null)
        {
            SetStatus("请先选择分辨率和刷新率", NoticeLevel.Warning);
            return;
        }
        uint freq = (uint)FrequencyList.SelectedItem;

        var target = DisplayService.FindMode(_modes, res.Width, res.Height, freq);
        if (target == null)
        {
            SetStatus("所选模式不在该显示器的模式列表中", NoticeLevel.Error);
            return;
        }
        _current = DisplayService.ReadCurrent(d.Device);
        if (_current == null)
        {
            SetStatus("无法读取当前模式, 已中止", NoticeLevel.Error);
            return;
        }
        if (_current.SameMode(target))
        {
            SetStatus("当前已是该模式，无需切换", NoticeLevel.Info);
            return;
        }

        var old = _current;

        // 1. 预检(不改变任何设置)
        int r = DisplayService.TestMode(d.Device, target);
        if (r != NativeMethods.DISP_CHANGE_SUCCESSFUL)
        {
            Fail(d, "预检失败: " + DisplayService.ErrorText(r) + "，未做任何更改");
            return;
        }

        // 2. 应用(暂不写注册表)
        r = DisplayService.ApplyMode(d.Device, target, persist: false);
        if (r != NativeMethods.DISP_CHANGE_SUCCESSFUL)
        {
            Fail(d, "切换失败: " + DisplayService.ErrorText(r));
            return;
        }

        // 3. 重新枚举验证(刷新率 ±1Hz 容差: 系统会把 60Hz 归一化为 59.94Hz 回读为 59)
        if (!DisplayService.VerifyApplied(d.Device, target))
        {
            int rb = DisplayService.ApplyMode(d.Device, old, persist: false);
            _current = DisplayService.ReadCurrent(d.Device);
            UpdateCurrentText(d);
            Fail(d, "切换未达目标, 已回滚原刷新率" +
                    (rb == NativeMethods.DISP_CHANGE_SUCCESSFUL ? "" : "(回滚失败, 请手动检查 设置-系统-屏幕-高级显示)"));
            return;
        }

        // 4. 成功: 进入等待确认, 弹出 15 秒倒计时确认窗口
        _awaitingConfirm = true;
        _pendingTarget = target;
        _pendingOld = old;
        ApplyButton.IsEnabled = false;
        _current = target;
        UpdateCurrentText(d);
        SetStatus($"已切换到 {target.Width} × {target.Height} @ {target.Frequency}Hz，请在 15 秒内确认", NoticeLevel.Info);
        Toast("切换成功", $"已切换到 {target.Width} × {target.Height} @ {target.Frequency}Hz，请在 15 秒内确认，超时将自动恢复", NoticeLevel.Info);

        try
        {
            _confirmWindow = new ConfirmWindow(target, old) { Owner = this };
            _confirmWindow.Decision += Keep => HandleDecision(d, Keep);
            _confirmWindow.Show();
        }
        catch
        {
            // 确认窗口无法显示时立即恢复原模式, 避免切换后的设置悬空无人确认
            _confirmWindow = null;
            _awaitingConfirm = false;
            DisplayService.ApplyMode(d.Device, old, persist: true);
            ApplyButton.IsEnabled = true;
            _current = DisplayService.ReadCurrent(d.Device);
            UpdateCurrentText(d);
            Fail(d, "确认窗口无法显示, 已自动恢复原刷新率");
        }
    }

    /// <summary>处理确认结果: 保持(持久化) 或 恢复原模式</summary>
    private void HandleDecision(DisplayItem d, bool keep)
    {
        if (!_awaitingConfirm) return;
        _awaitingConfirm = false;

        string dev = d.Device;
        if (keep)
        {
            int r = DisplayService.ApplyMode(dev, _pendingTarget, persist: true);
            bool ok = r == NativeMethods.DISP_CHANGE_SUCCESSFUL && DisplayService.VerifyApplied(dev, _pendingTarget);
            if (ok)
            {
                SetStatus($"已保持 {_pendingTarget.Width} × {_pendingTarget.Height} @ {_pendingTarget.Frequency}Hz，并写入系统显示设置", NoticeLevel.Info);
                Toast("已确认", "已保持当前设置，并写入系统显示设置(重启后仍生效)", NoticeLevel.Info);
            }
            else
            {
                SetStatus("写入系统设置失败: " + DisplayService.ErrorText(r) + " (本次会话仍生效, 重启后可能恢复)", NoticeLevel.Warning);
                Toast("部分失败", "写入系统显示设置失败，重启后可能恢复原设置", NoticeLevel.Warning);
            }
        }
        else
        {
            int r = DisplayService.ApplyMode(dev, _pendingOld, persist: true);
            bool ok = r == NativeMethods.DISP_CHANGE_SUCCESSFUL && DisplayService.VerifyApplied(dev, _pendingOld);
            if (ok)
            {
                SetStatus($"已恢复 {_pendingOld.Width} × {_pendingOld.Height} @ {_pendingOld.Frequency}Hz", NoticeLevel.Info);
                Toast("已恢复", $"已恢复原设置 {_pendingOld.Width} × {_pendingOld.Height} @ {_pendingOld.Frequency}Hz", NoticeLevel.Info);
            }
            else
            {
                SetStatus("恢复失败，请手动检查 设置-系统-屏幕-高级显示", NoticeLevel.Error);
                Toast("恢复失败", "自动恢复失败，请手动检查 设置-系统-屏幕-高级显示", NoticeLevel.Error);
            }
        }

        _confirmWindow = null;
        ApplyButton.IsEnabled = true;

        // 重新枚举刷新界面(保持当前选择显示器)
        ReloadDisplays();
    }

    private void Fail(DisplayItem d, string message)
    {
        SetStatus(message, NoticeLevel.Error);
        Toast("切换失败", message, NoticeLevel.Error);
    }

    // ---------- 通知与状态 ----------

    private void SetStatus(string text, NoticeLevel level)
    {
        StatusText.Text = text;
    }

    private void Toast(string title, string msg, NoticeLevel level)
    {
        if (Handle != IntPtr.Zero && TrayToast.Show(Handle, title, msg, level)) return;
        // 托盘通知不可用时回退消息框(注意 Wpf.Ui.Controls 中同名类型, 需完全限定)
        System.Windows.MessageBox.Show(this, msg, title, System.Windows.MessageBoxButton.OK,
            level == NoticeLevel.Error ? System.Windows.MessageBoxImage.Error :
            level == NoticeLevel.Warning ? System.Windows.MessageBoxImage.Warning : System.Windows.MessageBoxImage.Information);
    }

    protected override void OnClosing(System.ComponentModel.CancelEventArgs e)
    {
        // 等待确认期间关闭主窗口: 先恢复原模式再退出
        if (_awaitingConfirm)
        {
            var d = DisplayCombo.SelectedItem as DisplayItem;
            if (d != null && _pendingOld != null)
                DisplayService.ApplyMode(d.Device, _pendingOld, persist: true);
            _awaitingConfirm = false;
        }
        TrayToast.Hide();
        base.OnClosing(e);
    }
}
