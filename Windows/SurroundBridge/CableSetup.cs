// One-time setup of the virtual audio device apps play into: VB-Audio Virtual Cable (the Windows
// counterpart of the Mac app's BlackHoleSetup). It's donationware whose licence doesn't allow
// bundling, so, like BlackHole on the Mac, the app downloads VB-Audio's official driver pack,
// checks that its setup program carries a valid Authenticode signature from VB-Audio, and runs
// it (it asks for administrator rights and has its own "Install Driver" button).
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
using System.Threading.Tasks;
using System.Windows.Threading;

namespace SurroundBridge;

public enum CablePhase { Idle, Downloading, InstallerOpen, NeedsRestart, Failed }

public sealed class CableSetup : INotifyPropertyChanged
{
    public static readonly CableSetup Shared = new();

    public const string PackUrl = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip";
    public const string Website = "https://vb-audio.com/Cable/";

    public event PropertyChangedEventHandler? PropertyChanged;
    private void Changed([CallerMemberName] string? n = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));

    private CablePhase phase = CablePhase.Idle;
    public CablePhase Phase { get => phase; private set { phase = value; Changed(); Changed(nameof(Message)); } }
    public string Error { get; private set; } = "";
    public string Message => phase switch
    {
        CablePhase.Idle => "One more thing: Surround Bridge needs VB-Audio Virtual Cable, a free virtual audio device that your apps play into. Its official installer will open; it takes a minute.",
        CablePhase.Downloading => "Downloading VB-Audio Virtual Cable…",
        CablePhase.InstallerOpen => "In VB-Audio's window, click \"Install Driver\" and allow it. Surround Bridge notices when it's done.",
        CablePhase.NeedsRestart => "VB-Audio Virtual Cable is installed, but Windows hasn't loaded it yet. Restart Windows to finish.",
        _ => $"{Error} You can also download VB-Audio Virtual Cable from its website.",
    };

    public async void Install()
    {
        if (phase is CablePhase.Downloading or CablePhase.InstallerOpen) return;
        var ui = Dispatcher.CurrentDispatcher;
        Phase = CablePhase.Downloading;
        try
        {
            var dir = Path.Combine(Path.GetTempPath(), "SurroundBridge", "vbcable");
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
            Directory.CreateDirectory(dir);
            var zip = Path.Combine(dir, "pack.zip");
            using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(2) })
            {
                http.DefaultRequestHeaders.UserAgent.ParseAdd("SurroundBridge/" + Installer.Version);
                var bytes = await http.GetByteArrayAsync(PackUrl);
                await File.WriteAllBytesAsync(zip, bytes);
            }
            var unpacked = Path.Combine(dir, "pack");
            ZipFile.ExtractToDirectory(zip, unpacked);
            var setup = Path.Combine(unpacked, "VBCABLE_Setup_x64.exe");
            if (!File.Exists(setup)) throw new Exception("The download doesn't contain VB-Audio's setup program.");
            if (!IsSignedByVbAudio(setup, out var signer))
                throw new Exception($"VB-Audio's setup program isn't signed as expected ({signer}), so it wasn't run.");

            Phase = CablePhase.InstallerOpen;
            var p = Process.Start(new ProcessStartInfo(setup) { UseShellExecute = true, Verb = "runas", WorkingDirectory = unpacked })
                    ?? throw new Exception("VB-Audio's setup didn't start.");
            await Task.Run(p.WaitForExit);
            // the endpoints usually appear within seconds; sometimes only after a restart
            for (int i = 0; i < 20 && BridgeModel.Shared.VirtualDeviceUid == null; i++)
            {
                await Task.Delay(500);
                BridgeModel.Shared.RefreshDevices();
            }
            Phase = BridgeModel.Shared.VirtualDeviceUid != null ? CablePhase.Idle : CablePhase.NeedsRestart;
            if (BridgeModel.Shared.VirtualDeviceUid != null && BridgeModel.Shared.Enabled)
            {
                BridgeModel.Shared.Enabled = false;   // start it the normal way, now that the device exists
                BridgeModel.Shared.Enabled = true;
            }
        }
        catch (Win32Exception e) when (e.NativeErrorCode == 1223)   // the user declined the admin prompt
        {
            Error = "The installer needs administrator permission to add the audio driver.";
            Phase = CablePhase.Failed;
        }
        catch (Exception e)
        {
            Error = e is HttpRequestException ? "The download failed (are you online?)." : e.Message;
            Phase = CablePhase.Failed;
        }
    }

    public static void OpenWebsite() => Process.Start(new ProcessStartInfo(Website) { UseShellExecute = true });

    // ---------------------------------------------------------------- Authenticode

    /// A valid, trusted signature (WinVerifyTrust) whose certificate names VB-Audio / its author.
    private static bool IsSignedByVbAudio(string path, out string signer)
    {
        signer = "no signature";
        if (!WinTrust.Verify(path)) { signer = "the signature is not valid"; return false; }
        try
        {
#pragma warning disable SYSLIB0057
            var cert = X509Certificate.CreateFromSignedFile(path);
#pragma warning restore SYSLIB0057
            signer = cert.Subject;
            return signer.Contains("VB-Audio", StringComparison.OrdinalIgnoreCase) || signer.Contains("Burel", StringComparison.OrdinalIgnoreCase);
        }
        catch { return false; }
    }

    private static class WinTrust
    {
        private static readonly Guid GenericVerifyV2 = new("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct FileInfo
        {
            public uint cbStruct;
            public string pcwszFilePath;
            public IntPtr hFile;
            public IntPtr pgKnownSubject;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct Data
        {
            public uint cbStruct;
            public IntPtr pPolicyCallbackData, pSIPClientData;
            public uint dwUIChoice, fdwRevocationChecks, dwUnionChoice;
            public IntPtr pFile;
            public uint dwStateAction;
            public IntPtr hWVTStateData;
            public IntPtr pwszURLReference;
            public uint dwProvFlags, dwUIContext;
            public IntPtr pSignatureSettings;
        }

        [DllImport("wintrust.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
        private static extern int WinVerifyTrust(IntPtr hwnd, [MarshalAs(UnmanagedType.LPStruct)] Guid action, ref Data data);

        public static bool Verify(string path)
        {
            var fi = new FileInfo { cbStruct = (uint)Marshal.SizeOf<FileInfo>(), pcwszFilePath = path };
            var pFile = Marshal.AllocHGlobal(Marshal.SizeOf<FileInfo>());
            try
            {
                Marshal.StructureToPtr(fi, pFile, false);
                var d = new Data
                {
                    cbStruct = (uint)Marshal.SizeOf<Data>(),
                    dwUIChoice = 2,                 // WTD_UI_NONE
                    fdwRevocationChecks = 0,        // WTD_REVOKE_NONE
                    dwUnionChoice = 1,              // WTD_CHOICE_FILE
                    pFile = pFile,
                    dwStateAction = 1,              // WTD_STATEACTION_VERIFY
                    dwProvFlags = 0x10,             // WTD_CACHE_ONLY_URL_RETRIEVAL
                };
                int rc = WinVerifyTrust(IntPtr.Zero, GenericVerifyV2, ref d);
                d.dwStateAction = 2;                // WTD_STATEACTION_CLOSE
                WinVerifyTrust(IntPtr.Zero, GenericVerifyV2, ref d);
                return rc == 0;
            }
            finally
            {
                Marshal.DestroyStructure<FileInfo>(pFile);
                Marshal.FreeHGlobal(pFile);
            }
        }
    }
}
