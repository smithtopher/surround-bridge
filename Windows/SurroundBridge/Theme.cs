// Light / dark colours (Theme.xaml's DynamicResources), following Windows' app mode, plus the
// dark title bar for normal windows. The accent is the icon's indigo.
using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using Microsoft.Win32;

namespace SurroundBridge;

public static class Theme
{
    public static bool Dark { get; private set; }
    public static bool? Forced;   // --theme light|dark (snapshots)

    public static bool SystemDark()
    {
        using var k = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
        return k?.GetValue("AppsUseLightTheme") is int v && v == 0;
    }

    public static void Apply()
    {
        Dark = Forced ?? SystemDark();
        var r = Application.Current.Resources;
        void Set(string key, string hex) => r[key] = new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex));
        if (Dark)
        {
            Set("Bg", "#202020"); Set("Card", "#2B2B2B"); Set("CardBorder", "#3A3A3A");
            Set("Text", "#F2F2F2"); Set("SubText", "#A9A9B2");
            Set("ControlBg", "#333336"); Set("ControlBorder", "#48484D"); Set("ControlHover", "#3E3E44");
            Set("Track", "#4A4A50"); Set("SwitchBorder", "#B0B0B8"); Set("ThumbOuter", "#45454B");
            Set("Accent", "#8D89FF"); Set("AccentHover", "#A19EFF"); Set("AccentText", "#14141A");
            Set("NoticeBg", "#3A2F20"); Set("NoticeBorder", "#6A5233"); Set("Popup", "#2F2F33");
            Set("FlyoutBg", "#282828");
        }
        else
        {
            Set("Bg", "#F3F3F3"); Set("Card", "#FFFFFF"); Set("CardBorder", "#E3E3E6");
            Set("Text", "#1B1B1F"); Set("SubText", "#5E5F68");
            Set("ControlBg", "#FBFBFC"); Set("ControlBorder", "#D3D3D9"); Set("ControlHover", "#EFEFF3");
            Set("Track", "#D9D9E0"); Set("SwitchBorder", "#6E6E76"); Set("ThumbOuter", "#FFFFFF");
            Set("Accent", "#5B57D6"); Set("AccentHover", "#4C48C4"); Set("AccentText", "#FFFFFF");
            Set("NoticeBg", "#FFF4E5"); Set("NoticeBorder", "#F3CF9C"); Set("Popup", "#FFFFFF");
            Set("FlyoutBg", "#F9F9F9");
        }
        foreach (Window w in Application.Current.Windows) TitleBar(w);
    }

    /// Follow Windows switching between light and dark while running.
    public static void Watch()
    {
        SystemEvents.UserPreferenceChanged += (_, e) =>
        {
            if (e.Category == UserPreferenceCategory.General && Forced == null && SystemDark() != Dark)
                Application.Current.Dispatcher.BeginInvoke(Apply);
        };
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    /// Dark title bar on dark theme (DWMWA_USE_IMMERSIVE_DARK_MODE), Windows 10 20H1+ / 11.
    public static void TitleBar(Window w)
    {
        var h = new WindowInteropHelper(w).Handle;
        if (h == IntPtr.Zero) { w.SourceInitialized += (_, _) => TitleBar(w); return; }
        int on = Dark ? 1 : 0;
        DwmSetWindowAttribute(h, 20, ref on, sizeof(int));
    }
}
