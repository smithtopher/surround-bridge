// Stereo upmix settings: the same modes, presets and ranges as the Mac app's Upmix.swift (and
// Surround Player). The engine does the work (core/pc_upmix.c). Tone (bass and treble boost) is a
// listening preference kept out of the presets, so a loudness curve survives mode changes (Mac 1.2.0).
using System.Text.Json.Serialization;

namespace SurroundBridge;

public enum UpmixMode { Off = 0, Music, Movies, Wide, Custom }

public sealed record UpmixSettings
{
    public UpmixMode Mode { get; init; } = UpmixMode.Music;
    public double SurroundDb { get; init; } = -3;     // rear level, -40 = rears off
    public double Center { get; init; } = 0.4;        // 0 phantom centre .. 1 locked to the centre speaker
    public double Width { get; init; } = 0.55;        // 0.5 = as recorded
    public double LfeDb { get; init; } = -6;          // -40 = no LFE
    public int CrossoverHz { get; init; } = 100;
    public bool LfeMove { get; init; }                // false: copy bass to the sub; true: move it
    public double BassDb { get; init; }               // tone: not part of the presets
    public double TrebleDb { get; init; }
    public double RearDelayMs { get; init; } = 10;
    public int RearHighcutHz { get; init; } = 7000;   // 0 = off

    [JsonIgnore] public bool Enabled => Mode != UpmixMode.Off;

    public static readonly UpmixMode[] Modes = { UpmixMode.Off, UpmixMode.Music, UpmixMode.Movies, UpmixMode.Wide, UpmixMode.Custom };

    public static string Title(UpmixMode m) => m switch
    {
        UpmixMode.Off => "Off",
        UpmixMode.Music => "Music",
        UpmixMode.Movies => "Movies & TV",
        UpmixMode.Wide => "Wide",
        _ => "Custom",
    };

    public static string Blurb(UpmixMode m) => m switch
    {
        UpmixMode.Off => "Stereo stays stereo.",
        UpmixMode.Music => "Gentle room sound in the rears; vocals stay between the front speakers.",
        UpmixMode.Movies => "Dialogue locked to the centre speaker, ambience behind you.",
        UpmixMode.Wide => "Bigger, more enveloping sound with stronger rears and a wider front.",
        _ => "Your own settings.",
    };

    /// The sliders' values for a preset (the mode is left alone).
    public UpmixSettings WithPreset(UpmixMode p) => p switch
    {
        UpmixMode.Music => this with { SurroundDb = -3, Center = 0.4, Width = 0.55, LfeDb = -6, CrossoverHz = 100, LfeMove = false, RearDelayMs = 10, RearHighcutHz = 7000 },
        UpmixMode.Movies => this with { SurroundDb = -4, Center = 0.9, Width = 0.5, LfeDb = -3, CrossoverHz = 120, LfeMove = false, RearDelayMs = 15, RearHighcutHz = 7000 },
        UpmixMode.Wide => this with { SurroundDb = 0, Center = 0.5, Width = 0.8, LfeDb = -3, CrossoverHz = 100, LfeMove = false, RearDelayMs = 5, RearHighcutHz = 12000 },
        _ => this,
    };

    [JsonIgnore] internal Native.PCUpmix Engine => new()
    {
        enabled = Enabled ? 1 : 0,
        include_video = 1,
        surround_db = (float)SurroundDb,
        center = (float)Center,
        width = (float)Width,
        lfe_db = (float)LfeDb,
        crossover_hz = CrossoverHz,
        lfe_move = LfeMove ? 1 : 0,
        bass_db = (float)BassDb,
        rear_delay_ms = (float)RearDelayMs,
        rear_highcut_hz = RearHighcutHz,
        treble_db = (float)TrebleDb,
    };
}
