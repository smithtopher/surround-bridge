// P/Invoke bindings for bridgecore.dll (core/include/bridge.h, bridge_win.h, pc_types.h).
// Struct layouts must match the C headers exactly (default x64 packing).
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace SurroundBridge;

internal static unsafe class Native
{
    private const string Dll = "bridgecore.dll";

    public const int SB_CODEC_AC3 = 0, SB_CODEC_DTS = 1;

    [StructLayout(LayoutKind.Sequential)]
    public struct PCDevice
    {
        public fixed byte uid[256];
        public fixed byte name[256];
        public int output_channels;
        public int is_hdmi;
        public int is_digital;
        public int has_compressed_formats;
        public int is_default;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PCUpmix
    {
        public int enabled;
        public int include_video;
        public float surround_db;
        public float center;
        public float width;
        public float lfe_db;
        public int crossover_hz;
        public int lfe_move;
        public float bass_db;
        public float rear_delay_ms;
        public int rear_highcut_hz;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SBConfig
    {
        public IntPtr input_uid;        // UTF-8 (the bridge copies the strings)
        public IntPtr input_file;
        public int input_realtime;
        public IntPtr output_uid;
        public IntPtr dump_path;
        public int codec;
        public int ac3_bitrate_kbps;
        public int dts_bitrate_kbps;
        public double target_buffer_ms;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SBStatus
    {
        public int running;
        public int upmixing;
        public fixed byte input[256];
        public fixed byte output[512];
        public double buffer_ms;
        public double drift_ppm;
        public double latency_ms;
        public long overruns;
        public long jumps;
        public double target_ms;
        public fixed float peak_db[8];
        public int channels;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void DeviceChangeCallback(IntPtr ctx);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern IntPtr sb_bridge_create();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void sb_bridge_destroy(IntPtr b);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int sb_bridge_start(IntPtr b, ref SBConfig cfg, byte* err, int errlen);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void sb_bridge_stop(IntPtr b);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void sb_bridge_set_upmix(IntPtr b, ref PCUpmix u);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void sb_bridge_status(IntPtr b, out SBStatus st);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int sb_list_inputs([Out] PCDevice[] o, int max);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int sb_list_outputs([Out] PCDevice[] o, int max);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void pc_set_device_change_callback(DeviceChangeCallback? cb, IntPtr ctx);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int sb_win_default_output(int role, byte* uid, int len);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int sb_win_set_default_output(int role, byte[] uid);

    public static string Str(byte* p, int max)
    {
        int n = 0;
        while (n < max && p[n] != 0) n++;
        return Encoding.UTF8.GetString(p, n);
    }

    public static byte[] Utf8z(string s) => Encoding.UTF8.GetBytes(s + "\0");

    public static IntPtr AllocUtf8(string? s) => s == null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(s);

    public record Device(string Uid, string Name, int Channels, bool IsHdmi, bool HasBitstream, bool IsDefault);

    private static Device[] Convert(PCDevice[] raw, int n)
    {
        var list = new Device[Math.Max(0, n)];
        for (int i = 0; i < list.Length; i++)
        {
            fixed (PCDevice* d = &raw[i])
                list[i] = new Device(Str(d->uid, 256), Str(d->name, 256), d->output_channels, d->is_hdmi != 0,
                                     d->has_compressed_formats != 0, d->is_default != 0);
        }
        return list;
    }

    public static Device[] Inputs() { var r = new PCDevice[64]; return Convert(r, sb_list_inputs(r, 64)); }
    public static Device[] Outputs() { var r = new PCDevice[64]; return Convert(r, sb_list_outputs(r, 64)); }

    /// role 0: what apps play to (console + multimedia); 1: communications.
    public static string? DefaultOutput(int role)
    {
        byte* buf = stackalloc byte[512];
        return sb_win_default_output(role, buf, 512) == 0 ? Str(buf, 512) : null;
    }

    public static bool SetDefaultOutput(int role, string uid) => sb_win_set_default_output(role, Utf8z(uid)) == 0;
}
