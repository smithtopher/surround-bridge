// The flyout's level meters: one per active input channel and one per output channel, labelled
// by speaker (L R C LFE Ls Rs ...). The masks come from SBStatus (AV_CH_* == WAVE SPEAKER_* bits:
// channel i is the i-th set bit, lowest first).
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;

namespace SurroundBridge;

public sealed class ChannelMeter : INotifyPropertyChanged
{
    public ChannelMeter(string label) => Label = label;
    public string Label { get; }
    private double level;
    /// 0..1 (-60 dB .. 0 dB).
    public double Level
    {
        get => level;
        set
        {
            if (Math.Abs(level - value) < 0.002) return;
            level = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Level)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Rest)));
        }
    }
    /// The track-coloured cover over the unlit part (see Theme.xaml MeterFillV).
    public double Rest => 1 - level;
    public event PropertyChangedEventHandler? PropertyChanged;
}

public static class ChannelLayout
{
    private static readonly (uint Bit, string Label)[] Names =
    {
        (0x1, "L"), (0x2, "R"), (0x4, "C"), (0x8, "LFE"), (0x10, "Lb"), (0x20, "Rb"), (0x40, "Lc"), (0x80, "Rc"),
        (0x100, "Cb"), (0x200, "Ls"), (0x400, "Rs"),
    };

    /// FFmpeg's default layout for a channel count (what the core uses when it has no mask).
    public static uint Default(int channels) => channels switch
    {
        1 => 0x4, 2 => 0x3, 3 => 0x7, 4 => 0x107, 5 => 0x607, 6 => 0x60F, 7 => 0x70F, 8 => 0x63F, _ => 0,
    };

    /// (channel index, label) for each channel of `mask` that's in `active`.
    public static List<(int Index, string Label)> Channels(uint mask, uint active, int count)
    {
        var list = new List<(int, string)>();
        if (mask == 0) mask = Default(count);
        if (mask == 0) { for (int i = 0; i < count && i < 8; i++) list.Add((i, (i + 1).ToString())); return list; }
        if (active == 0) active = mask;
        int index = 0;
        for (int bit = 0; bit < 32 && index < 8; bit++)
        {
            uint b = 1u << bit;
            if ((mask & b) == 0) continue;
            if ((active & b) != 0)
            {
                string label = "Ch" + (index + 1);
                foreach (var n in Names) if (n.Bit == b) label = n.Label;
                list.Add((index, label));
            }
            index++;
        }
        return list;
    }

    /// Updates `meters` to these channels and levels (dB per channel index), rebuilding it only
    /// when the channels change, so the bars don't flicker.
    public static void Update(ObservableCollection<ChannelMeter> meters, List<(int Index, string Label)> chans, Func<int, float> db)
    {
        bool same = meters.Count == chans.Count;
        for (int i = 0; same && i < chans.Count; i++) same = meters[i].Label == chans[i].Label;
        if (!same)
        {
            meters.Clear();
            foreach (var c in chans) meters.Add(new ChannelMeter(c.Label));
        }
        for (int i = 0; i < chans.Count; i++) meters[i].Level = Math.Clamp((db(chans[i].Index) + 60) / 60.0, 0, 1);
    }
}
