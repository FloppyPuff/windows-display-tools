using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

namespace DisplayTuner;

/// <summary>一台活动显示器(WPF 绑定要求使用属性, 不能使用字段)</summary>
public sealed class DisplayItem
{
    public string Device { get; set; }       // \\.\DISPLAY5
    public string AdapterName { get; set; }  // NVIDIA GeForce RTX 4070 Laptop GPU
    public string MonitorName { get; set; }  // 显示器型号(可能为空或通用名)
    public string Label { get; set; }        // 展示用名称

    public override string ToString() => Label;
}

/// <summary>一个显示模式(分辨率 + 刷新率 + 色深)</summary>
public sealed class ModeEntry
{
    public uint Width { get; set; }
    public uint Height { get; set; }
    public uint Frequency { get; set; }
    public uint BitsPerPel { get; set; }

    /// <summary>NTSC 类刷新率会被系统归一化(请求 60Hz 实际生效 59.94Hz 且回读为 59),
    /// 因此刷新率 ±1Hz 视为同一模式, 分辨率必须精确一致</summary>
    public bool SameMode(ModeEntry other) =>
        other != null && Width == other.Width && Height == other.Height &&
        Math.Abs((int)Frequency - (int)other.Frequency) <= 1;
}

/// <summary>一个分辨率及其可用刷新率列表(降序)</summary>
public sealed class ResolutionItem
{
    public uint Width { get; set; }
    public uint Height { get; set; }
    public string Label { get; set; }
    public List<uint> Frequencies { get; set; }

    public override string ToString() => Label;
}

public static class DisplayService
{
    // 虚拟/间接显示适配器(远程控制软件等), 不参与调节
    private static readonly string[] VirtualKeywords =
        { "idddriver", "indirect display", "virtual display", "sunflower" };

    private static bool IsVirtualAdapter(string deviceString)
    {
        if (string.IsNullOrEmpty(deviceString)) return false;
        var s = deviceString.ToLowerInvariant();
        return VirtualKeywords.Any(k => s.Contains(k));
    }

    private static string BuildLabel(DisplayItem d)
    {
        if (!string.IsNullOrEmpty(d.MonitorName) &&
            d.MonitorName != "Generic PnP Monitor" && d.MonitorName != "通用即插即用监视器")
            return d.MonitorName;
        if (!string.IsNullOrEmpty(d.AdapterName)) return d.AdapterName;
        return d.Device;
    }

    /// <summary>枚举所有活动的物理显示器(过滤虚拟适配器)</summary>
    public static List<DisplayItem> GetDisplays()
    {
        var list = new List<DisplayItem>();
        uint i = 0;
        while (true)
        {
            var dd = new NativeMethods.DISPLAY_DEVICEW
            { cb = Marshal.SizeOf<NativeMethods.DISPLAY_DEVICEW>() };
            if (!NativeMethods.EnumDisplayDevicesW(null, i, ref dd, 0)) break;
            if ((dd.StateFlags & NativeMethods.DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0 &&
                !IsVirtualAdapter(dd.DeviceString))
            {
                var item = new DisplayItem { Device = dd.DeviceName, AdapterName = dd.DeviceString };
                var md = new NativeMethods.DISPLAY_DEVICEW
                { cb = Marshal.SizeOf<NativeMethods.DISPLAY_DEVICEW>() };
                if (NativeMethods.EnumDisplayDevicesW(dd.DeviceName, 0, ref md, 0) &&
                    !string.IsNullOrEmpty(md.DeviceString))
                    item.MonitorName = md.DeviceString;
                item.Label = BuildLabel(item);
                list.Add(item);
            }
            i++;
        }
        return list;
    }

    /// <summary>枚举某显示器的全部模式(按 分辨率+刷新率 去重, 同组合保留与当前色深一致或更高的色深)</summary>
    public static List<ModeEntry> GetModes(string device)
    {
        var raw = new List<ModeEntry>();
        int idx = 0;
        while (true)
        {
            var dm = new NativeMethods.DEVMODEW
            { dmSize = (ushort)Marshal.SizeOf<NativeMethods.DEVMODEW>() };
            if (!NativeMethods.EnumDisplaySettingsW(device, idx, ref dm)) break;
            idx++;
            raw.Add(new ModeEntry
            {
                Width = dm.dmPelsWidth,
                Height = dm.dmPelsHeight,
                Frequency = dm.dmDisplayFrequency,
                BitsPerPel = dm.dmBitsPerPel,
            });
        }

        uint curBpp = ReadCurrent(device)?.BitsPerPel ?? 32;
        return raw
            .GroupBy(m => (m.Width, m.Height, m.Frequency))
            .Select(g => g.OrderByDescending(m => m.BitsPerPel == curBpp)
                         .ThenByDescending(m => m.BitsPerPel)
                         .First())
            .ToList();
    }

    /// <summary>读取当前模式; 失败返回 null</summary>
    public static ModeEntry ReadCurrent(string device)
    {
        var dm = new NativeMethods.DEVMODEW
        { dmSize = (ushort)Marshal.SizeOf<NativeMethods.DEVMODEW>() };
        if (!NativeMethods.EnumDisplaySettingsW(device, NativeMethods.ENUM_CURRENT_SETTINGS, ref dm))
            return null;
        return new ModeEntry
        {
            Width = dm.dmPelsWidth,
            Height = dm.dmPelsHeight,
            Frequency = dm.dmDisplayFrequency,
            BitsPerPel = dm.dmBitsPerPel,
        };
    }

    /// <summary>把模式列表整理为 分辨率(面积降序) -> 刷新率(降序) 的层级</summary>
    public static List<ResolutionItem> GroupByResolution(List<ModeEntry> modes)
    {
        return modes
            .GroupBy(m => (m.Width, m.Height))
            .OrderByDescending(g => (ulong)g.Key.Width * g.Key.Height)
            .ThenByDescending(g => g.Key.Width)
            .Select(g => new ResolutionItem
            {
                Width = g.Key.Width,
                Height = g.Key.Height,
                Label = g.Key.Width + " × " + g.Key.Height,
                Frequencies = g.Select(m => m.Frequency).Distinct().OrderByDescending(f => f).ToList(),
            })
            .ToList();
    }

    public static ModeEntry FindMode(List<ModeEntry> modes, uint w, uint h, uint freq)
        => modes.FirstOrDefault(m => m.Width == w && m.Height == h && m.Frequency == freq);

    private static NativeMethods.DEVMODEW BuildDevMode(ModeEntry m)
    {
        var dm = new NativeMethods.DEVMODEW
        {
            dmSize = (ushort)Marshal.SizeOf<NativeMethods.DEVMODEW>(),
            dmPelsWidth = m.Width,
            dmPelsHeight = m.Height,
            dmDisplayFrequency = m.Frequency,
            dmBitsPerPel = m.BitsPerPel,
            dmFields = NativeMethods.DM_PELSWIDTH | NativeMethods.DM_PELSHEIGHT |
                       NativeMethods.DM_DISPLAYFREQUENCY | NativeMethods.DM_BITSPERPEL,
        };
        return dm;
    }

    /// <summary>CDS_TEST 预检: 只校验模式可用性, 不改变任何显示设置</summary>
    public static int TestMode(string device, ModeEntry m)
    {
        var dm = BuildDevMode(m);
        return NativeMethods.ChangeDisplaySettingsExW(device, ref dm, IntPtr.Zero,
            NativeMethods.CDS_TEST, IntPtr.Zero);
    }

    /// <summary>应用模式; persist=true 时写入系统显示设置(重启后保持)</summary>
    public static int ApplyMode(string device, ModeEntry m, bool persist)
    {
        var dm = BuildDevMode(m);
        uint flags = persist ? NativeMethods.CDS_UPDATEREGISTRY : 0;
        return NativeMethods.ChangeDisplaySettingsExW(device, ref dm, IntPtr.Zero, flags, IntPtr.Zero);
    }

    /// <summary>重新枚举验证当前模式是否已生效(刷新率容差 ±1Hz, 见 SameMode 说明)</summary>
    public static bool VerifyApplied(string device, ModeEntry expected)
    {
        var now = ReadCurrent(device);
        return now != null && expected != null &&
               now.Width == expected.Width && now.Height == expected.Height &&
               Math.Abs((int)now.Frequency - (int)expected.Frequency) <= 1;
    }

    public static string ErrorText(int code)
    {
        switch (code)
        {
            case NativeMethods.DISP_CHANGE_SUCCESSFUL: return "成功";
            case -4: return "切换参数错误(BADFLAGS)";
            case -5: return "切换参数无效(BADPARAM)";
            case -2: return "显示器不支持该模式(BADMODE)";
            case -1: return "显示驱动切换失败(FAILED)";
            case -3: return "无法写入系统显示设置(NOTUPDATED)";
            case -6: return "多显示器配置错误(BADDUALVIEW)";
            case 1: return "需要重启电脑才能生效(RESTART)";
            default: return "未知错误(" + code + ")";
        }
    }
}
