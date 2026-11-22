// --snapshot <dir>: renders the flyout, every Settings tab and the setup windows to PNGs, so the
// UI can be checked without clicking through it. The model never switches on here.
using System;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace SurroundBridge;

public static class Snapshot
{
    public static void Run(string dir)
    {
        Directory.CreateDirectory(dir);
        var model = new BridgeModel(autostart: false);
        string theme = Theme.Dark ? "dark" : "light";

        var flyout = new FlyoutWindow(model);
        Save(flyout, Path.Combine(dir, $"flyout-{theme}.png"));
        model.PreviewState(BridgeState.NoVirtualDevice);   // the first-run notice
        Save(flyout, Path.Combine(dir, $"flyout-setup-{theme}.png"));
        model.PreviewState(BridgeState.Off);
        flyout.Close();

        var settings = new SettingsWindow(model);
        string[] tabs = { "general", "upmix", "about" };
        for (int i = 0; i < tabs.Length; i++)
        {
            settings.Tabs.SelectedIndex = i;
            if (i == 1)
            {
                Show(settings);
                foreach (var ex in Find<Expander>(settings)) ex.IsExpanded = true;
            }
            Save(settings, Path.Combine(dir, $"settings-{tabs[i]}-{theme}.png"));
        }
        settings.Close();

        foreach (var m in new[] { SetupWindow.Mode.Install, SetupWindow.Mode.Uninstall })
        {
            var w = new SetupWindow(m);
            Save(w, Path.Combine(dir, $"setup-{m.ToString().ToLowerInvariant()}-{theme}.png"));
            w.Close();
        }
        model.Shutdown();
    }

    private static void Show(Window w)
    {
        w.ShowActivated = false;
        w.Left = -20000;   // off screen
        w.Top = -20000;
        w.Show();
        w.UpdateLayout();
        // let bindings and templates settle
        w.Dispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
    }

    private static void Save(Window w, string path)
    {
        Show(w);
        var root = (FrameworkElement)w.Content;
        double width = root.ActualWidth + root.Margin.Left + root.Margin.Right;
        double height = root.ActualHeight + root.Margin.Top + root.Margin.Bottom;
        double scale = 1.5;
        var bmp = new RenderTargetBitmap((int)Math.Ceiling(width * scale), (int)Math.Ceiling(height * scale),
                                         96 * scale, 96 * scale, PixelFormats.Pbgra32);
        var bg = new DrawingVisual();
        using (var dc = bg.RenderOpen())
        {
            var brush = w.Background is SolidColorBrush b && b.Color.A > 0 ? b : (Brush)Application.Current.Resources["Bg"];
            dc.DrawRectangle(brush, null, new Rect(0, 0, width, height));
        }
        bmp.Render(bg);
        bmp.Render(root);
        var enc = new PngBitmapEncoder();
        enc.Frames.Add(BitmapFrame.Create(bmp));
        using var f = File.Create(path);
        enc.Save(f);
        Console.WriteLine("wrote " + path);
    }

    private static System.Collections.Generic.IEnumerable<T> Find<T>(DependencyObject root) where T : DependencyObject
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
        {
            var c = VisualTreeHelper.GetChild(root, i);
            if (c is T t) yield return t;
            foreach (var x in Find<T>(c)) yield return x;
        }
    }
}
