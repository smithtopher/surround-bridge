using System.ComponentModel;
using System.Diagnostics;
using System.Windows;

namespace SurroundBridge;

public partial class SettingsWindow : Window
{
    private readonly BridgeModel model;

    public SettingsWindow(BridgeModel model)
    {
        InitializeComponent();
        this.model = model;
        DataContext = model;
        Icon = App.IconImage(32);
        AboutIcon.Source = App.IconImage(96);
        VersionText.Text = "Version " + Installer.Version + (Installer.IsDevBuild ? " (development build)" : "");
        if (!Installer.RunningInstalled)
        {
            UninstallButton.IsEnabled = false;
            UninstallNote.Text = "This copy isn't installed (it runs from where it is), so there's nothing to uninstall.";
        }
        Theme.TitleBar(this);
        model.PropertyChanged += OnModel;
        CableSetup.Shared.PropertyChanged += OnModel;
        Closed += (_, _) => { model.PropertyChanged -= OnModel; CableSetup.Shared.PropertyChanged -= OnModel; };
        Refresh();
    }

    private void OnModel(object? sender, PropertyChangedEventArgs e) => Dispatcher.BeginInvoke(Refresh);

    private void Refresh()
    {
        NoOutputs.Visibility = model.HasOutputs ? Visibility.Collapsed : Visibility.Visible;
        var cable = CableSetup.Shared;
        bool installed = model.VirtualDeviceUid != null;
        CableState.Text = installed ? "Installed" : cable.Phase switch
        {
            CablePhase.Downloading => "Downloading…",
            CablePhase.InstallerOpen => "VB-Audio's installer is open: click \"Install Driver\".",
            CablePhase.NeedsRestart => "Installed; restart Windows to finish.",
            CablePhase.Failed => cable.Error,
            _ => "Not installed: Surround Bridge needs it.",
        };
        CableButton.Content = installed ? "Website" : cable.Phase == CablePhase.Failed ? "Website" : "Install…";
        CableButton.IsEnabled = installed || cable.Phase is CablePhase.Idle or CablePhase.Failed;
        CableButton.Style = installed || cable.Phase == CablePhase.Failed ? (Style)FindResource(typeof(System.Windows.Controls.Button))
                                                                         : (Style)FindResource("AccentButton");
    }

    private void Cable_Click(object sender, RoutedEventArgs e)
    {
        if (model.VirtualDeviceUid != null || CableSetup.Shared.Phase == CablePhase.Failed) CableSetup.OpenWebsite();
        else CableSetup.Shared.Install();
    }

    private void CableSite_Click(object sender, RoutedEventArgs e) => CableSetup.OpenWebsite();

    private void Uninstall_Click(object sender, RoutedEventArgs e)
    {
        // the uninstaller runs as its own process: it asks this one to quit (restoring the sound output)
        Process.Start(new ProcessStartInfo(Installer.InstalledExe, "--uninstall") { UseShellExecute = false });
    }
}
