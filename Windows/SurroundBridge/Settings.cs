// Persisted settings: %APPDATA%\Surround Bridge\settings.json (same keys as the Mac app's
// UserDefaults where they mean the same). SURROUND_BRIDGE_SETTINGS=<file> points somewhere else:
// always use that in tests, never the user's own file.
using System;
using System.IO;
using System.Text.Json;

namespace SurroundBridge;

public sealed class Settings
{
    public bool enabled { get; set; }
    public string outputUID { get; set; } = "";
    public string outputName { get; set; } = "";
    public int codec { get; set; }                  // 0 AC3, 1 DTS
    public int ac3Bitrate { get; set; } = 640;
    public int dtsBitrate { get; set; } = 1509;
    public bool autoSwitchOutput { get; set; } = true;
    public UpmixSettings upmix { get; set; } = new();
    // the default outputs before the bridge switched them to the cable (restored when it's off,
    // or at the next launch after a crash)
    public string? restoreOutputUID { get; set; }
    public string? restoreCommsOutputUID { get; set; }

    public static string FilePath =>
        Environment.GetEnvironmentVariable("SURROUND_BRIDGE_SETTINGS") is { Length: > 0 } p ? p
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Surround Bridge", "settings.json");

    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true };

    public static Settings Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath), Json) ?? new Settings();
        }
        catch { /* unreadable: start from defaults (the file is rewritten on the next change) */ }
        return new Settings();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            var tmp = FilePath + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(this, Json));
            File.Move(tmp, FilePath, true);
        }
        catch { /* best effort */ }
    }
}
