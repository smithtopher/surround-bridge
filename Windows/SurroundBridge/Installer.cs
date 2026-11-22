// The app installs itself: the release is one exe; run from anywhere else it shows SetupWindow,
// which copies it to %LOCALAPPDATA%\Programs\Surround Bridge (per user: no administrator rights),
// adds Start menu / desktop shortcuts, the "Apps" entry (uninstall) and optionally start at login.
// SURROUND_BRIDGE_TEST_ROOT=<dir> puts everything (files, shortcuts, registry keys under
// HKCU\Software\SurroundBridgeTest) somewhere harmless: always use it in tests.
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading;
using Microsoft.Win32;

namespace SurroundBridge;

public static class Installer
{
    public const string AppName = "Surround Bridge";
    public const string ExeName = "SurroundBridge.exe";
    public const string Publisher = "smithtopher";
    public const string MutexName = @"Local\SurroundBridge";
    public const string ShowEventName = @"Local\SurroundBridge.Show";
    public const string QuitEventName = @"Local\SurroundBridge.Quit";

    public static string Version
    {
        get
        {
            var v = Assembly.GetExecutingAssembly().GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "0.0.0";
            int plus = v.IndexOf('+');
            return plus >= 0 ? v[..plus] : v;
        }
    }

    private static readonly string? TestRoot = Environment.GetEnvironmentVariable("SURROUND_BRIDGE_TEST_ROOT") is { Length: > 0 } t ? t : null;

    public static string InstallDir => Path.Combine(TestRoot ?? Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", AppName);
    public static string InstalledExe => Path.Combine(InstallDir, ExeName);
    private static string StartMenuLink => Path.Combine(TestRoot != null ? Path.Combine(TestRoot, "StartMenu") : Environment.GetFolderPath(Environment.SpecialFolder.Programs), AppName + ".lnk");
    private static string DesktopLink => Path.Combine(TestRoot != null ? Path.Combine(TestRoot, "Desktop") : Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), AppName + ".lnk");
    private static string UninstallKey => TestRoot != null ? @"Software\SurroundBridgeTest\Uninstall\SurroundBridge"
                                                           : @"Software\Microsoft\Windows\CurrentVersion\Uninstall\SurroundBridge";
    private static string RunKey => TestRoot != null ? @"Software\SurroundBridgeTest\Run" : @"Software\Microsoft\Windows\CurrentVersion\Run";

    public static string CurrentExe => Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, ExeName);

    /// A development build (bin\…: bridgecore.dll next to the exe) runs as the app, never as setup.
    public static bool IsDevBuild => File.Exists(Path.Combine(Path.GetDirectoryName(CurrentExe)!, "bridgecore.dll"));

    public static bool RunningInstalled =>
        string.Equals(Path.GetFullPath(CurrentExe), Path.GetFullPath(InstalledExe), StringComparison.OrdinalIgnoreCase);

    /// Version of the installed copy, or null.
    public static string? InstalledVersion
    {
        get
        {
            using var k = Registry.CurrentUser.OpenSubKey(UninstallKey);
            return File.Exists(InstalledExe) ? k?.GetValue("DisplayVersion") as string ?? "?" : null;
        }
    }

    public static bool LaunchAtLogin
    {
        get
        {
            using var k = Registry.CurrentUser.OpenSubKey(RunKey);
            return k?.GetValue(AppName) is string;
        }
        set
        {
            using var k = Registry.CurrentUser.CreateSubKey(RunKey);
            if (value) k.SetValue(AppName, $"\"{(RunningInstalled || IsDevBuild ? CurrentExe : InstalledExe)}\" --background");
            else k.DeleteValue(AppName, false);
        }
    }

    public static bool HasDesktopShortcut => File.Exists(DesktopLink);

    // ---------------------------------------------------------------- running copy

    /// Asks a running copy to quit (it restores the sound output first) and waits for it.
    public static bool StopRunningCopy(TimeSpan timeout)
    {
        if (!Mutex.TryOpenExisting(MutexName, out var m)) return true;
        m.Dispose();
        if (EventWaitHandle.TryOpenExisting(QuitEventName, out var quit)) { quit.Set(); quit.Dispose(); }
        var until = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < until)
        {
            if (!Mutex.TryOpenExisting(MutexName, out var still)) return true;
            still.Dispose();
            Thread.Sleep(100);
        }
        // last resort: an unresponsive installed copy
        foreach (var p in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(ExeName)))
        {
            try
            {
                if (p.Id != Environment.ProcessId && string.Equals(p.MainModule?.FileName, InstalledExe, StringComparison.OrdinalIgnoreCase))
                { p.Kill(); p.WaitForExit(3000); }
            }
            catch { }
        }
        return !Mutex.TryOpenExisting(MutexName, out _);
    }

    // ---------------------------------------------------------------- install

    public static void Install(bool desktopShortcut, bool startAtLogin)
    {
        StopRunningCopy(TimeSpan.FromSeconds(8));
        Directory.CreateDirectory(InstallDir);
        if (!RunningInstalled)
        {
            // copy beside, then swap, so a failed copy never leaves half an exe
            var tmp = InstalledExe + ".new";
            File.Copy(CurrentExe, tmp, true);
            File.Move(tmp, InstalledExe, true);
        }
        CreateShortcut(StartMenuLink);
        if (desktopShortcut) CreateShortcut(DesktopLink);
        else if (File.Exists(DesktopLink)) File.Delete(DesktopLink);

        using (var k = Registry.CurrentUser.CreateSubKey(UninstallKey))
        {
            k.SetValue("DisplayName", AppName);
            k.SetValue("DisplayVersion", Version);
            k.SetValue("Publisher", Publisher);
            k.SetValue("DisplayIcon", InstalledExe + ",0");
            k.SetValue("InstallLocation", InstallDir);
            k.SetValue("UninstallString", $"\"{InstalledExe}\" --uninstall");
            k.SetValue("QuietUninstallString", $"\"{InstalledExe}\" --uninstall --quiet");
            k.SetValue("URLInfoAbout", "https://github.com/smithtopher/surround-bridge");
            k.SetValue("EstimatedSize", (int)(new FileInfo(InstalledExe).Length / 1024), RegistryValueKind.DWord);
            k.SetValue("NoModify", 1, RegistryValueKind.DWord);
            k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            k.SetValue("InstallDate", DateTime.Now.ToString("yyyyMMdd"));
        }
        using (var k = Registry.CurrentUser.CreateSubKey(RunKey))
        {
            if (startAtLogin) k.SetValue(AppName, $"\"{InstalledExe}\" --background");
            else k.DeleteValue(AppName, false);
        }
    }

    public static void LaunchInstalled(string args = "--first-run") =>
        Process.Start(new ProcessStartInfo(InstalledExe, args) { UseShellExecute = false, WorkingDirectory = InstallDir });

    private static void CreateShortcut(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var type = Type.GetTypeFromProgID("WScript.Shell") ?? throw new InvalidOperationException("WScript.Shell unavailable");
        dynamic shell = Activator.CreateInstance(type)!;
        try
        {
            dynamic link = shell.CreateShortcut(path);
            link.TargetPath = InstalledExe;
            link.WorkingDirectory = InstallDir;
            link.IconLocation = InstalledExe + ",0";
            link.Description = "Everything your PC plays, sent to your amp as Dolby Digital or DTS surround";
            link.Save();
        }
        finally { System.Runtime.InteropServices.Marshal.FinalReleaseComObject(shell); }
    }

    // ---------------------------------------------------------------- uninstall

    /// Removes shortcuts, registry entries and (optionally) the settings, then deletes the install
    /// folder once this process has exited (it may be the installed exe itself).
    public static void Uninstall(bool removeSettings)
    {
        StopRunningCopy(TimeSpan.FromSeconds(8));
        foreach (var l in new[] { StartMenuLink, DesktopLink })
            if (File.Exists(l)) File.Delete(l);
        using (var k = Registry.CurrentUser.OpenSubKey(RunKey, true)) k?.DeleteValue(AppName, false);
        Registry.CurrentUser.DeleteSubKeyTree(UninstallKey, false);
        if (removeSettings)
        {
            var dir = Path.GetDirectoryName(Settings.FilePath)!;
            if (Environment.GetEnvironmentVariable("SURROUND_BRIDGE_SETTINGS") is { Length: > 0 }) { if (File.Exists(Settings.FilePath)) File.Delete(Settings.FilePath); }
            else if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }
        if (!Directory.Exists(InstallDir)) return;
        if (RunningInstalled)
        {
            // can't delete a running exe: a hidden cmd waits for this process to go, then removes the folder
            var cmd = $"/c for /l %i in (1,1,20) do (ping -n 2 127.0.0.1 >nul & rmdir /s /q \"{InstallDir}\" 2>nul & if not exist \"{InstallDir}\" exit)";
            Process.Start(new ProcessStartInfo("cmd.exe", cmd) { CreateNoWindow = true, UseShellExecute = false, WorkingDirectory = Path.GetTempPath() });
        }
        else
        {
            Directory.Delete(InstallDir, true);
        }
    }
}
