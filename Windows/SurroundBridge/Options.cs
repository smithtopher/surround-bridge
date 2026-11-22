// Choices for the combo boxes (value + label), the same as the Mac app's pickers.
using System.Linq;

namespace SurroundBridge;

public sealed record Choice(object Value, string Label)
{
    public override string ToString() => Label;
}

public static class Options
{
    public static Choice[] UpmixModes { get; } = UpmixSettings.Modes.Select(m => new Choice(m, UpmixSettings.Title(m))).ToArray();
    public static Choice[] Codecs { get; } = { new(0, "Dolby Digital (AC3)"), new(1, "DTS") };
    public static Choice[] Ac3Bitrates { get; } = { new(384, "384 kb/s"), new(448, "448 kb/s (DVD)"), new(640, "640 kb/s (best)") };
    public static Choice[] DtsBitrates { get; } = { new(1509, "1509 kb/s (full rate, best)"), new(754, "754 kb/s (half rate)") };
    public static Choice[] Crossovers { get; } = new[] { 60, 80, 100, 120, 150, 200, 250 }.Select(v => new Choice(v, $"{v} Hz")).ToArray();
    public static Choice[] LfeMoves { get; } = { new(false, "Copy (main speakers keep it)"), new(true, "Move (small main speakers)") };
    public static Choice[] HighCuts { get; } =
        new[] { new Choice(0, "Off") }.Concat(new[] { 4000, 6000, 7000, 8000, 10000, 12000, 16000 }.Select(v => new Choice(v, $"{v / 1000} kHz"))).ToArray();
}
