// Start-up: what this run is.
//   SurroundBridge.exe                 installed copy: the tray app (a second launch opens the
//                                      running copy's flyout); anywhere else: the setup window
//   … --background                     at sign-in: the tray app without opening the flyout
//   … --first-run                      right after installing: opens the flyout
//   … --uninstall [--quiet]            from Settings ▸ Apps or the About tab
//   … --install --quiet [--desktop-shortcut] [--no-login] [--no-launch]   unattended install
//   … --snapshot <dir> [--theme light|dark]   renders the windows to PNGs and exits (tests;
//                                      set SURROUND_BRIDGE_SETTINGS to a test file: never switches on)
// Development builds (bridgecore.dll beside the exe) always run as the tray app.
using System;
using System.IO;
using System.Linq;
using System.Threading;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace SurroundBridge;

public partial class App : Application
{
    private static Mutex? instance;
    private static BridgeModel? model;
    private static TrayIcon? tray;
    private static FlyoutWindow? flyout;
    private static SettingsWindow? settings;

    public static Stream IconStream() => GetResourceStream(new Uri("pack://application:,,,/Resources/AppIcon.ico"))!.Stream;

    /// The app icon at about `px` pixels (the .ico has 16…256).
    public static ImageSource IconImage(int px)
    {
        var dec = new IconBitmapDecoder(IconStream(), BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
        return dec.Frames.OrderBy(f => Math.Abs(f.PixelWidth - px) + (f.PixelWidth < px ? 1000 : 0)).First();
    }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += (_, x) => LogCrash(x.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, x) => LogCrash(x.ExceptionObject as Exception);
        var args = e.Args;
        string? Arg(string name) { int i = Array.IndexOf(args, name); return i >= 0 && i + 1 < args.Length ? args[i + 1] : null; }
        bool Has(string name) => args.Contains(name);
        if (Arg("--theme") is { } t) Theme.Forced = t == "dark";
        Theme.Apply();
        Theme.Watch();

        if (Arg("--snapshot") is { } dir) { Snapshot.Run(dir); Shutdown(0); return; }

        if (Has("--install") && Has("--quiet"))
        {
            int rc = 0;
            try
            {
                Installer.Install(Has("--desktop-shortcut"), !Has("--no-login"));
                if (!Has("--no-launch")) Installer.LaunchInstalled();
            }
            catch (Exception ex) { LogCrash(ex); rc = 1; }
            Shutdown(rc);
            return;
        }

        if (Has("--uninstall"))
        {
            if (Has("--quiet")) { Installer.Uninstall(removeSettings: false); Shutdown(0); return; }
            var w = new SetupWindow(SetupWindow.Mode.Uninstall);
            w.Closed += (_, _) => Shutdown(0);
            w.Show();
            return;
        }

        if (!Installer.IsDevBuild && !Installer.RunningInstalled)
        {
            var w = new SetupWindow(SetupWindow.Mode.Install);
            w.Closed += (_, _) => Shutdown(0);
            w.Show();
            return;
        }

        // one copy: a second launch (Start menu, desktop) asks the running one to show itself
        instance = new Mutex(true, Installer.MutexName, out bool first);
        if (!first)
        {
            if (EventWaitHandle.TryOpenExisting(Installer.ShowEventName, out var show)) show.Set();
            Shutdown(0);
            return;
        }
        Listen(Installer.ShowEventName, () => flyout?.ShowFlyout());
        Listen(Installer.QuitEventName, Quit);

        model = new BridgeModel();
        flyout = new FlyoutWindow(model);
        tray = new TrayIcon(model, () => flyout.Toggle());
        SessionEnding += (_, _) => model.Shutdown();

        if (Has("--first-run"))
            tray.Balloon("Surround Bridge is running", "It lives here in the notification area. Click its icon to switch it on and adjust the sound.");
        if (!Has("--background")) Dispatcher.BeginInvoke(() => flyout.ShowFlyout());
    }

    /// %TEMP%\SurroundBridge\crash.log: what went wrong, for a bug report.
    public static string CrashLog => Path.Combine(Path.GetTempPath(), "SurroundBridge", "crash.log");

    private static void LogCrash(Exception? ex)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(CrashLog)!);
            File.AppendAllText(CrashLog, $"{DateTime.Now:s} Surround Bridge {Installer.Version}\n{ex}\n\n");
        }
        catch { }
    }

    /// A named event another process sets (show the flyout, quit).
    private void Listen(string name, Action a)
    {
        var ev = new EventWaitHandle(false, EventResetMode.AutoReset, name);
        var t = new Thread(() => { while (ev.WaitOne()) Dispatcher.BeginInvoke(a); }) { IsBackground = true };
        t.Start();
    }

    public static void ShowSettings()
    {
        if (model == null) return;
        if (settings == null)
        {
            settings = new SettingsWindow(model);
            settings.Closed += (_, _) => settings = null;
        }
        settings.Show();
        if (settings.WindowState == WindowState.Minimized) settings.WindowState = WindowState.Normal;
        settings.Activate();
    }

    /// Quit: restore Windows' sound output and the HDMI device first.
    public static void Quit()
    {
        model?.Shutdown();
        tray?.Dispose();
        tray = null;
        instance?.ReleaseMutex();
        instance?.Dispose();
        instance = null;
        Current.Shutdown(0);
    }
}
