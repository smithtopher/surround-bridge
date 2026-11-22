using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;

namespace SurroundBridge;

public partial class FlyoutWindow : Window
{
    private readonly BridgeModel model;
    private DateTime hiddenAt;
    private Action? noticeAction;

    public FlyoutWindow(BridgeModel model)
    {
        InitializeComponent();
        this.model = model;
        DataContext = model;
        AppIcon.Source = App.IconImage(64);
        model.PropertyChanged += (_, e) => { if (e.PropertyName is "" or null or nameof(BridgeModel.State) or nameof(BridgeModel.StatusLine)) UpdateNotice(); };
        CableSetup.Shared.PropertyChanged += (_, _) => UpdateNotice();
        UpdateNotice();
        Deactivated += (_, _) => HideFlyout();
        KeyDown += (_, e) => { if (e.Key == Key.Escape) HideFlyout(); };
    }

    /// Plain-language help for whatever is stopping it.
    private void UpdateNotice()
    {
        string? text = null, button = null;
        noticeAction = null;
        switch (model.State)
        {
            case BridgeState.NoVirtualDevice:
                var cable = CableSetup.Shared;
                text = cable.Message;
                if (cable.Phase == CablePhase.Idle) { button = "Install VB-Audio Virtual Cable…"; noticeAction = cable.Install; }
                else if (cable.Phase == CablePhase.Failed) { button = "Open VB-Audio's website"; noticeAction = CableSetup.OpenWebsite; }
                break;
            case BridgeState.NoOutput:
                text = "Connect your TV, receiver or soundbar (HDMI or optical) and switch it on; Surround Bridge starts by itself when it appears.";
                break;
            case BridgeState.Failed:
                text = "It couldn't start. Check that nothing else is using the HDMI/optical output exclusively, then turn it off and on again.";
                break;
        }
        NoticeBox.Visibility = text == null ? Visibility.Collapsed : Visibility.Visible;
        NoticeText.Text = text ?? "";
        NoticeButton.Visibility = button == null ? Visibility.Collapsed : Visibility.Visible;
        NoticeButton.Content = button ?? "";
    }

    private void Notice_Click(object sender, RoutedEventArgs e) => noticeAction?.Invoke();

    public void Toggle()
    {
        // a click on the tray icon first deactivates (hides) an open flyout: don't reopen it
        if (IsVisible) { HideFlyout(); return; }
        if ((DateTime.UtcNow - hiddenAt).TotalMilliseconds < 300) return;
        ShowFlyout();
    }

    public void ShowFlyout()
    {
        model.FastStatus = true;
        Show();
        UpdateLayout();
        PlaceNearTray();
        Activate();
        OnOff.Focus();
    }

    public void HideFlyout()
    {
        if (!IsVisible) return;
        hiddenAt = DateTime.UtcNow;
        model.FastStatus = false;
        Hide();
    }

    [StructLayout(LayoutKind.Sequential)] private struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO { public int cbSize; public RECT rcMonitor, rcWork; public uint dwFlags; }
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromPoint(POINT p, uint flags);
    [DllImport("user32.dll")] private static extern bool GetMonitorInfo(IntPtr m, ref MONITORINFO mi);

    /// Above the taskbar's notification area on the screen with the pointer (wherever the taskbar is).
    private void PlaceNearTray()
    {
        GetCursorPos(out var cur);
        var mi = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
        GetMonitorInfo(MonitorFromPoint(cur, 2), ref mi);
        var dpi = VisualTreeHelper.GetDpi(this);
        double sx = dpi.DpiScaleX, sy = dpi.DpiScaleY;
        RECT w = mi.rcWork, m = mi.rcMonitor;
        double width = ActualWidth, height = ActualHeight;
        double left, top;
        if (w.Bottom < m.Bottom) { left = w.Right / sx - width; top = w.Bottom / sy - height; }          // taskbar at the bottom
        else if (w.Top > m.Top) { left = w.Right / sx - width; top = w.Top / sy; }                       // top
        else if (w.Left > m.Left) { left = w.Left / sx; top = w.Bottom / sy - height; }                  // left
        else { left = w.Right / sx - width; top = w.Bottom / sy - height; }                              // right, or auto-hide
        Left = left;   // the window's own 12 px margin (for the shadow) keeps it off the edges
        Top = top;
    }

    private void Settings_Click(object sender, RoutedEventArgs e)
    {
        HideFlyout();
        App.ShowSettings();
    }

    private void Quit_Click(object sender, RoutedEventArgs e) => App.Quit();
}
