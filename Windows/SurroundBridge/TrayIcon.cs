// The notification-area icon: click for the flyout, right-click for a short menu. The icon is
// dimmed while the bridge is off; its tooltip is the status line.
using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using WF = System.Windows.Forms;

namespace SurroundBridge;

public sealed class TrayIcon : IDisposable
{
    private readonly WF.NotifyIcon icon = new();
    private readonly BridgeModel model;
    private readonly Icon onIcon, offIcon;
    private readonly WF.ToolStripMenuItem toggleItem;

    public TrayIcon(BridgeModel model, Action showFlyout)
    {
        this.model = model;
        var size = WF.SystemInformation.SmallIconSize;
        using (var s = App.IconStream()) onIcon = new Icon(s, size);
        offIcon = Dimmed(onIcon);

        var menu = new WF.ContextMenuStrip();
        toggleItem = new WF.ToolStripMenuItem("Turn on", null, (_, _) => model.Enabled = !model.Enabled);
        menu.Items.Add(toggleItem);
        menu.Items.Add(new WF.ToolStripMenuItem("Settings…", null, (_, _) => App.ShowSettings()));
        menu.Items.Add(new WF.ToolStripSeparator());
        menu.Items.Add(new WF.ToolStripMenuItem("Quit Surround Bridge", null, (_, _) => App.Quit()));
        menu.Opening += (_, _) => toggleItem.Text = model.Enabled ? "Turn off" : "Turn on";
        icon.ContextMenuStrip = menu;
        icon.MouseClick += (_, e) => { if (e.Button == WF.MouseButtons.Left) showFlyout(); };

        model.PropertyChanged += OnModel;
        Update();
        icon.Visible = true;
    }

    private void OnModel(object? s, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is "" or null or nameof(BridgeModel.State) or nameof(BridgeModel.StatusLine) or nameof(BridgeModel.Enabled)) Update();
    }

    private void Update()
    {
        icon.Icon = model.IsRunning ? onIcon : offIcon;
        var tip = "Surround Bridge — " + model.StatusLine;
        icon.Text = tip.Length > 127 ? tip[..126] + "…" : tip;
    }

    public void Balloon(string title, string text) => icon.ShowBalloonTip(5000, title, text, WF.ToolTipIcon.None);

    [DllImport("user32.dll")] private static extern bool DestroyIcon(IntPtr h);

    private static Icon Dimmed(Icon src)
    {
        using var bmp = src.ToBitmap();
        using var outBmp = new Bitmap(bmp.Width, bmp.Height, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(outBmp))
        {
            // greyscale at 65% opacity
            var cm = new ColorMatrix(new[]
            {
                new[] { 0.30f, 0.30f, 0.30f, 0, 0 },
                new[] { 0.59f, 0.59f, 0.59f, 0, 0 },
                new[] { 0.11f, 0.11f, 0.11f, 0, 0 },
                new[] { 0f, 0, 0, 0.65f, 0 },
                new[] { 0f, 0, 0, 0, 1 },
            });
            using var ia = new ImageAttributes();
            ia.SetColorMatrix(cm);
            g.DrawImage(bmp, new Rectangle(0, 0, bmp.Width, bmp.Height), 0, 0, bmp.Width, bmp.Height, GraphicsUnit.Pixel, ia);
        }
        var h = outBmp.GetHicon();
        var icon = (Icon)Icon.FromHandle(h).Clone();
        DestroyIcon(h);
        return icon;
    }

    public void Dispose()
    {
        model.PropertyChanged -= OnModel;
        icon.Visible = false;
        icon.Dispose();
        onIcon.Dispose();
        offIcon.Dispose();
    }
}
