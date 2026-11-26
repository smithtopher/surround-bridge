// The app's model (port of the Mac app's BridgeModel.swift): settings, on/off, switching Windows'
// default output to the virtual cable and back, device watching, status.
// The only place that calls the C bridge, always on `queue` (one thread), so the UI never waits
// for a device to switch formats. Everything else runs on the UI thread.
using System;
using System.Collections.Concurrent;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;

namespace SurroundBridge;

public enum BridgeState { Off, Starting, Running, NoVirtualDevice, NoOutput, Failed }

public sealed record OutputDevice(string Uid, string Name, bool IsHdmi)
{
    public string Label => Name + (IsHdmi ? " (HDMI)" : "");
    // the themed ComboBox's selection box shows ToString(), not DisplayMemberPath
    public override string ToString() => Label;
}

/// One thread running actions in order (the Mac app's serial DispatchQueue).
internal sealed class SerialQueue
{
    private readonly BlockingCollection<Action> work = new();
    public SerialQueue(string name)
    {
        var t = new Thread(() => { foreach (var a in work.GetConsumingEnumerable()) a(); }) { IsBackground = true, Name = name };
        t.Start();
    }
    public void Post(Action a) => work.Add(a);
    public void Send(Action a)
    {
        using var done = new ManualResetEventSlim();
        work.Add(() => { try { a(); } finally { done.Set(); } });
        done.Wait();
    }
}

public sealed class BridgeModel : INotifyPropertyChanged
{
    public static BridgeModel Shared { get; private set; } = null!;

    public const string VirtualInputName = "CABLE Output";   // VB-Audio Virtual Cable: what we capture
    public const string VirtualOutputName = "CABLE Input";   // ... and what apps play into

    private readonly Settings s;
    private readonly IntPtr bridge;
    private readonly SerialQueue queue = new("surround-bridge");
    private readonly Dispatcher ui;
    private readonly DispatcherTimer timer;
    private readonly DispatcherTimer deviceDebounce;
    private readonly Native.DeviceChangeCallback deviceCallback;   // kept alive: native code holds it
    private int generation;                                       // invalidates a start that finished after a stop
    private static readonly bool Debug = Environment.GetEnvironmentVariable("SURROUND_BRIDGE_DEBUG") != null;

    public event PropertyChangedEventHandler? PropertyChanged;
    private void Changed([CallerMemberName] string? n = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
    private void ChangedAll() => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(string.Empty));

    /// autostart false: never switches on by itself (snapshots, tests).
    public BridgeModel(bool autostart = true)
    {
        Shared = this;
        ui = Dispatcher.CurrentDispatcher;
        s = Settings.Load();
        bridge = Native.sb_bridge_create();
        PushUpmix();
        RefreshDevices();
        if (autostart) RestoreOutputAfterCrash();

        deviceDebounce = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(600) };
        deviceDebounce.Tick += (_, _) => { deviceDebounce.Stop(); DevicesChanged(); };
        deviceCallback = _ => ui.BeginInvoke(() => { deviceDebounce.Stop(); deviceDebounce.Start(); });
        Native.pc_set_device_change_callback(deviceCallback, IntPtr.Zero);
        SystemEvents.PowerModeChanged += (_, e) =>
        {
            if (e.Mode == PowerModes.Resume) ui.BeginInvoke(() => { RefreshDevices(); RestartIfOn(); });
        };

        timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        timer.Tick += (_, _) => Tick();
        timer.Start();

        if (!autostart) s.enabled = false;
        else if (Environment.GetEnvironmentVariable("SURROUND_BRIDGE_AUTOSTART") != null) s.enabled = true;
        if (s.enabled) Apply();
    }

    // ------------------------------------------------------------------ settings

    public bool Enabled
    {
        get => s.enabled || previewOn;
        set { if (s.enabled == value) return; s.enabled = value; s.Save(); Changed(); Apply(); }
    }

    public string OutputUid
    {
        get => s.outputUID;
        set
        {
            if (value == null || s.outputUID == value) return;
            s.outputUID = value;
            if (Outputs.FirstOrDefault(o => o.Uid == value) is { } o) s.outputName = o.Name;
            s.Save();
            Changed();
            RestartIfOn();
        }
    }

    /// 0 Dolby Digital, 1 DTS, 2 multichannel PCM (HDMI receivers; SB_CODEC_*)
    public int Codec
    {
        get => s.codec;
        set { if (s.codec == value) return; s.codec = value; s.Save(); Changed(); Changed(nameof(IsDts)); Changed(nameof(IsAc3)); Changed(nameof(IsPcm)); Changed(nameof(HasBitrate)); RestartIfOn(); }
    }
    public bool IsDts => s.codec == 1;
    public bool IsAc3 => s.codec == 0;
    public bool IsPcm => s.codec == 2;
    public bool HasBitrate => !IsPcm;
    private string CodecName => IsPcm ? "PCM" : IsDts ? "DTS" : "Dolby Digital";

    public int Ac3Bitrate
    {
        get => s.ac3Bitrate;
        set { if (s.ac3Bitrate == value) return; s.ac3Bitrate = value; s.Save(); Changed(); RestartIfOn(); }
    }

    public int DtsBitrate
    {
        get => s.dtsBitrate;
        set { if (s.dtsBitrate == value) return; s.dtsBitrate = value; s.Save(); Changed(); RestartIfOn(); }
    }

    /// Point Windows' sound output at the cable while it's on (and back afterwards).
    public bool AutoSwitchOutput
    {
        get => s.autoSwitchOutput;
        set
        {
            if (s.autoSwitchOutput == value) return;
            s.autoSwitchOutput = value;
            s.Save();
            Changed();
            if (State == BridgeState.Running) { if (value) SwitchOutputToBridge(); else RestoreOutput(); }
        }
    }

    public bool LaunchAtLogin
    {
        get => Installer.LaunchAtLogin;
        set { Installer.LaunchAtLogin = value; Changed(); }
    }

    // ------------------------------------------------------------------ upmix

    public UpmixSettings Upmix
    {
        get => s.upmix;
        private set
        {
            if (s.upmix == value) return;
            var old = s.upmix;
            s.upmix = value;
            s.Save();
            PushUpmix();
            RaiseUpmix();
            if (value.Enabled != old.Enabled) RestartIfOn();   // the encoder layout changes
        }
    }

    private void RaiseUpmix()
    {
        foreach (var n in new[] { nameof(Upmix), nameof(UpmixMode), nameof(UpmixBlurb), nameof(UpmixEnabled), nameof(SurroundSlider),
                                  nameof(CenterSlider), nameof(WidthSlider), nameof(LfeSlider), nameof(CrossoverHz), nameof(LfeMove),
                                  nameof(BassDb), nameof(TrebleDb), nameof(TrebleLabel), nameof(RearDelayMs), nameof(RearHighcutHz), nameof(SurroundLabel), nameof(CenterLabel),
                                  nameof(WidthLabel), nameof(LfeLabel), nameof(BassLabel), nameof(RearDelayLabel), nameof(StatusLine) })
            Changed(n);
    }

    /// A preset sets the sliders; Custom keeps them.
    public UpmixMode UpmixMode
    {
        get => s.upmix.Mode;
        set
        {
            var u = s.upmix with { Mode = value };
            if (value != UpmixMode.Custom) u = u.WithPreset(value);
            Upmix = u;
        }
    }
    public string UpmixBlurb => UpmixSettings.Blurb(s.upmix.Mode);
    public bool UpmixEnabled => s.upmix.Enabled;

    /// Moving a slider turns a preset into Custom.
    private void Edit(Func<UpmixSettings, UpmixSettings> f)
    {
        var u = f(s.upmix);
        if (u.Mode != UpmixMode.Off) u = u with { Mode = UpmixMode.Custom };
        Upmix = u;
    }

    // dB sliders run -20..+6; the left end means off (-40 dB)
    private static double DbIn(double v) => Math.Max(-20, v);
    private static double DbOut(double v) => v <= -20 ? -40 : Math.Round(v * 2) / 2;

    public double SurroundSlider { get => DbIn(s.upmix.SurroundDb); set { if (DbOut(value) != s.upmix.SurroundDb) Edit(u => u with { SurroundDb = DbOut(value) }); } }
    public double CenterSlider { get => s.upmix.Center; set { var v = Math.Round(value * 20) / 20; if (v != s.upmix.Center) Edit(u => u with { Center = v }); } }
    public double WidthSlider { get => s.upmix.Width; set { var v = Math.Round(value * 20) / 20; if (v != s.upmix.Width) Edit(u => u with { Width = v }); } }
    public double LfeSlider { get => DbIn(s.upmix.LfeDb); set { if (DbOut(value) != s.upmix.LfeDb) Edit(u => u with { LfeDb = DbOut(value) }); } }
    public int CrossoverHz { get => s.upmix.CrossoverHz; set { if (value != s.upmix.CrossoverHz) Edit(u => u with { CrossoverHz = value }); } }
    public bool LfeMove { get => s.upmix.LfeMove; set { if (value != s.upmix.LfeMove) Edit(u => u with { LfeMove = value }); } }
    // tone: not part of the presets, so changing it keeps the mode
    public double BassDb { get => s.upmix.BassDb; set { var v = Math.Round(value * 2) / 2; if (v != s.upmix.BassDb) Upmix = s.upmix with { BassDb = v }; } }
    public double TrebleDb { get => s.upmix.TrebleDb; set { var v = Math.Round(value * 2) / 2; if (v != s.upmix.TrebleDb) Upmix = s.upmix with { TrebleDb = v }; } }
    public double RearDelayMs { get => s.upmix.RearDelayMs; set { var v = Math.Round(value); if (v != s.upmix.RearDelayMs) Edit(u => u with { RearDelayMs = v }); } }
    public int RearHighcutHz { get => s.upmix.RearHighcutHz; set { if (value != s.upmix.RearHighcutHz) Edit(u => u with { RearHighcutHz = value }); } }

    private static string Db(double v) => v <= -20 ? "Off" : $"{v:+0.0;-0.0;0.0} dB";
    public string SurroundLabel => Db(s.upmix.SurroundDb);
    public string LfeLabel => Db(s.upmix.LfeDb);
    public string CenterLabel => s.upmix.Center < 0.05 ? "Phantom" : s.upmix.Center > 0.95 ? "Locked" : $"{s.upmix.Center * 100:0}%";
    public string WidthLabel => Math.Abs(s.upmix.Width - 0.5) < 0.01 ? "As recorded" : $"{s.upmix.Width * 100:0}%";
    public string BassLabel => s.upmix.BassDb < 0.25 ? "Off" : $"+{s.upmix.BassDb:0.0} dB";
    public string TrebleLabel => s.upmix.TrebleDb < 0.25 ? "Off" : $"+{s.upmix.TrebleDb:0.0} dB";
    public string RearDelayLabel => s.upmix.RearDelayMs < 0.5 ? "Off" : $"{s.upmix.RearDelayMs:0} ms";

    private void PushUpmix()
    {
        var u = s.upmix.Engine;
        var b = bridge;
        queue.Post(() => Native.sb_bridge_set_upmix(b, ref u));
    }

    // ------------------------------------------------------------------ status

    private BridgeState state = BridgeState.Off;
    public BridgeState State
    {
        get => state;
        private set
        {
            if (state == value) return;
            state = value;
            if (Debug) Console.Error.WriteLine($"[bridge] state: {state} — {StatusLine}");
            ChangedAll();
        }
    }
    private string failure = "";
    public bool IsRunning => state == BridgeState.Running;

    public OutputDevice[] Outputs { get; private set; } = Array.Empty<OutputDevice>();
    public string? VirtualDeviceUid { get; private set; }      // the cable's capture side
    private string? cableRenderUid;                            // the cable's playback side ("CABLE Input")
    private Native.SBStatus status;
    public string? LastError { get; private set; }

    public OutputDevice? SelectedOutput =>
        Outputs.FirstOrDefault(o => o.Uid == s.outputUID) ??
        (s.outputName.Length > 0 ? Outputs.FirstOrDefault(o => o.Name == s.outputName) : null);

    public bool HasOutputs => Outputs.Length > 0;

    /// Short line for the menu: what's happening now.
    public string StatusLine => state switch
    {
        BridgeState.Off => "Off",
        BridgeState.Starting => "Starting…",
        BridgeState.Running => $"{CodecName} {(status.out_channels > 0 ? (status.out_channels >= 8 ? "7.1" : status.out_channels >= 6 ? "5.1" : "stereo")
                                  : s.upmix.Enabled || status.channels >= 6 ? "5.1" : "stereo")} → "
                               + $"{SelectedOutput?.Name ?? "output"} · ~{Math.Round(status.latency_ms)} ms",
        BridgeState.NoVirtualDevice => "The virtual audio cable isn't installed",
        BridgeState.NoOutput => $"{(s.outputName.Length > 0 ? s.outputName : "The HDMI/optical output")} isn't connected",
        _ => failure,
    };

    public Brush DotBrush => state switch
    {
        BridgeState.Running => new SolidColorBrush(Color.FromRgb(0x2E, 0xB8, 0x5C)),
        BridgeState.Starting => new SolidColorBrush(Color.FromRgb(0xE8, 0xB3, 0x1A)),
        BridgeState.Off => new SolidColorBrush(Color.FromRgb(0x8A, 0x8A, 0x8E)),
        _ => new SolidColorBrush(Color.FromRgb(0xF0, 0x8A, 0x24)),
    };

    /// The flyout's meters: the input channels in use (2 while a 5.1 cable carries stereo) and
    /// what goes out (2, 5.1 or 7.1), by speaker.
    public ObservableCollection<ChannelMeter> InMeters { get; } = new();
    public ObservableCollection<ChannelMeter> OutMeters { get; } = new();

    private unsafe void UpdateMeters()
    {
        if (state != BridgeState.Running) { InMeters.Clear(); OutMeters.Clear(); return; }
        var st = status;
        float[] inDb = new float[8], outDb = new float[8];
        for (int i = 0; i < 8; i++) { inDb[i] = st.peak_db[i]; outDb[i] = st.out_peak_db[i]; }
        ChannelLayout.Update(InMeters, ChannelLayout.Channels(st.in_mask, st.in_active, st.channels), i => inDb[i]);
        ChannelLayout.Update(OutMeters, ChannelLayout.Channels(st.out_mask, 0, st.out_channels), i => outDb[i]);
    }

    /// Faster status while the menu is open (meters); once a second otherwise.
    public bool FastStatus { set => timer.Interval = TimeSpan.FromMilliseconds(value ? 100 : 1000); }

    // ------------------------------------------------------------------ devices

    public void RefreshDevices()
    {
        var outs = Native.Outputs();
        cableRenderUid = outs.FirstOrDefault(o => o.Name.Contains(VirtualOutputName, StringComparison.OrdinalIgnoreCase))?.Uid;
        Outputs = outs.Where(o => o.IsHdmi || o.HasBitstream).Select(o => new OutputDevice(o.Uid, o.Name, o.IsHdmi)).ToArray();
        VirtualDeviceUid = Native.Inputs().FirstOrDefault(i => i.Name.Contains(VirtualInputName, StringComparison.OrdinalIgnoreCase))?.Uid;
        // first run: pick the only (or first) digital output
        if (s.outputUID.Length == 0 && Outputs.Length > 0)
        {
            s.outputUID = Outputs[0].Uid;
            s.outputName = Outputs[0].Name;
            s.Save();
        }
        Changed(nameof(Outputs));
        Changed(nameof(HasOutputs));
        Changed(nameof(OutputUid));
        Changed(nameof(VirtualDeviceUid));
    }

    /// A TV's endpoint id can change when it re-sends its EDID, so a missing output is looked up
    /// by name; the bridge restarts when it comes back.
    private void DevicesChanged()
    {
        var had = SelectedOutput;
        RefreshDevices();
        if (SelectedOutput is { } o && o.Uid != s.outputUID) { s.outputUID = o.Uid; s.Save(); Changed(nameof(OutputUid)); }
        if (!s.enabled) return;
        switch (state)
        {
            case BridgeState.Running when SelectedOutput == null || had?.Uid != SelectedOutput.Uid:
                RestartIfOn();
                break;
            case BridgeState.NoOutput or BridgeState.NoVirtualDevice or BridgeState.Failed:
                if (SelectedOutput != null && VirtualDeviceUid != null) Apply();
                break;
        }
    }

    // ------------------------------------------------------------------ on / off

    private void RestartIfOn()
    {
        if (!s.enabled) return;
        StopBridge(Apply);
    }

    private void Apply()
    {
        if (s.enabled) Start(); else StopBridge(null);
    }

    private void Start()
    {
        LastError = null;
        if (VirtualDeviceUid is not { } input) { State = BridgeState.NoVirtualDevice; return; }
        if (SelectedOutput is not { } output) { State = BridgeState.NoOutput; return; }
        State = BridgeState.Starting;
        int gen = ++generation;
        var cfg = new Native.SBConfig
        {
            codec = IsPcm ? Native.SB_CODEC_PCM : IsDts ? Native.SB_CODEC_DTS : Native.SB_CODEC_AC3,
            ac3_bitrate_kbps = s.ac3Bitrate,
            dts_bitrate_kbps = s.dtsBitrate,
        };
        var b = bridge;
        queue.Post(() =>
        {
            cfg.input_uid = Native.AllocUtf8(input);
            cfg.output_uid = Native.AllocUtf8(output.Uid);
            int rc;
            string message;
            Native.SBStatus st;
            unsafe
            {
                byte* err = stackalloc byte[512];
                err[0] = 0;
                rc = Native.sb_bridge_start(b, ref cfg, err, 512);
                message = Native.Str(err, 512);
            }
            System.Runtime.InteropServices.Marshal.FreeCoTaskMem(cfg.input_uid);
            System.Runtime.InteropServices.Marshal.FreeCoTaskMem(cfg.output_uid);
            Native.sb_bridge_status(b, out st);
            ui.BeginInvoke(() =>
            {
                if (gen != generation || !s.enabled) return;
                if (rc == 0)
                {
                    status = st;
                    State = BridgeState.Running;
                    SwitchOutputToBridge();
                }
                else
                {
                    failure = message.Length > 0 ? message : "Could not start";
                    State = BridgeState.Failed;
                    ChangedAll();
                }
            });
        });
    }

    private void StopBridge(Action? then)
    {
        generation++;
        RestoreOutput();
        var b = bridge;
        queue.Post(() =>
        {
            Native.sb_bridge_stop(b);   // gives the HDMI device back (format, exclusive access)
            ui.BeginInvoke(() =>
            {
                if (!s.enabled) State = BridgeState.Off;
                then?.Invoke();
            });
        });
    }

    /// Snapshots only: show a state's UI without doing anything.
    internal void PreviewState(BridgeState st) { previewOn = false; State = st; UpdateMeters(); Changed(nameof(Enabled)); }
    private bool previewOn;   // snapshots: the switch drawn on, without switching anything on

    /// Snapshots only: running, with these meters (dB per channel).
    internal unsafe void PreviewMeters(int inCh, uint inMask, uint inActive, int outCh, uint outMask, float[] inDb, float[] outDb)
    {
        var st = new Native.SBStatus { running = 1, channels = inCh, in_mask = inMask, in_active = inActive,
                                       out_channels = outCh, out_mask = outMask, latency_ms = 120 };
        for (int i = 0; i < 8; i++) { st.peak_db[i] = i < inDb.Length ? inDb[i] : -120; st.out_peak_db[i] = i < outDb.Length ? outDb[i] : -120; }
        status = st;
        previewOn = true;
        State = BridgeState.Running;
        UpdateMeters();
        Changed(nameof(Enabled));
        Changed(nameof(StatusLine));
    }

    /// Quit / log off / uninstall: synchronous, so the device and Windows' output are restored before exit.
    public void Shutdown()
    {
        timer.Stop();
        Native.pc_set_device_change_callback(null, IntPtr.Zero);
        RestoreOutput();
        var b = bridge;
        queue.Send(() => Native.sb_bridge_stop(b));
    }

    // ------------------------------------------------------------------ Windows' sound output

    /// While on, apps must play into the cable (the HDMI device is ours, exclusively). What was
    /// selected before is remembered, also on disk in case the app is killed.
    private void SwitchOutputToBridge()
    {
        if (!s.autoSwitchOutput || cableRenderUid is not { } cable) return;
        for (int role = 0; role < 2; role++)
        {
            var cur = Native.DefaultOutput(role);
            if (cur != null && cur != cable)
            {
                if (role == 0) s.restoreOutputUID = cur; else s.restoreCommsOutputUID = cur;
            }
            Native.SetDefaultOutput(role, cable);
        }
        s.Save();
    }

    private void RestoreOutput()
    {
        if (cableRenderUid is { } cable)
        {
            // only if it's still pointing at us (the user may have picked something else meanwhile)
            if (Native.DefaultOutput(0) == cable && s.restoreOutputUID is { } a) Native.SetDefaultOutput(0, a);
            if (Native.DefaultOutput(1) == cable && s.restoreCommsOutputUID is { } c) Native.SetDefaultOutput(1, c);
        }
        if (s.restoreOutputUID != null || s.restoreCommsOutputUID != null)
        {
            s.restoreOutputUID = s.restoreCommsOutputUID = null;
            s.Save();
        }
    }

    /// A previous run was killed while on: put the output back unless we're turning on again.
    private void RestoreOutputAfterCrash()
    {
        if (!s.enabled && s.restoreOutputUID != null) RestoreOutput();
    }

    // ------------------------------------------------------------------ status

    private void Tick()
    {
        if (state != BridgeState.Running) return;
        var b = bridge;
        queue.Post(() =>
        {
            Native.sb_bridge_status(b, out var st);
            ui.BeginInvoke(() =>
            {
                if (state != BridgeState.Running) return;
                status = st;
                UpdateMeters();
                Changed(nameof(StatusLine));
                if (st.running == 0) RestartIfOn();   // the pipeline stopped by itself (device gone)
                if (Debug)
                    Console.Error.WriteLine($"[bridge] {StatusLine} | buffer {st.buffer_ms:0.0} ms, drift {st.drift_ppm:+0;-0} ppm, " +
                                            $"overruns {st.overruns}, jumps {st.jumps}, upmix {st.upmixing}");
            });
        });
    }
}
