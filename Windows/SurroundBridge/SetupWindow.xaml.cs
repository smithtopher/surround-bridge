using System;
using System.Threading.Tasks;
using System.Windows;

namespace SurroundBridge;

public partial class SetupWindow : Window
{
    public enum Mode { Install, Uninstall }

    private readonly Mode mode;
    private bool done;

    public SetupWindow(Mode mode)
    {
        InitializeComponent();
        this.mode = mode;
        Icon = App.IconImage(32);
        Logo.Source = App.IconImage(128);
        Theme.TitleBar(this);
        if (mode == Mode.Install)
        {
            var installed = Installer.InstalledVersion;
            SubHeading.Text = $"Version {Installer.Version}";
            if (installed == null) MainButton.Content = "Install";
            else if (installed == Installer.Version) { MainButton.Content = "Reinstall"; SubHeading.Text += " · already installed"; }
            else { MainButton.Content = "Update"; SubHeading.Text += $" · updates {installed}"; }
            StartAtLogin.IsChecked = installed == null || Installer.LaunchAtLogin;
            DesktopShortcut.IsChecked = Installer.HasDesktopShortcut;
            WhereText.Text = "Installs for your account only, in " + Installer.InstallDir + ". No administrator rights needed.";
        }
        else
        {
            Title = "Uninstall Surround Bridge";
            Heading.Text = "Uninstall Surround Bridge";
            SubHeading.Text = $"Version {Installer.InstalledVersion ?? Installer.Version}";
            InstallPanel.Visibility = Visibility.Collapsed;
            UninstallPanel.Visibility = Visibility.Visible;
            MainButton.Content = "Uninstall";
        }
    }

    private void Cancel_Click(object sender, RoutedEventArgs e) => Close();

    private async void Main_Click(object sender, RoutedEventArgs e)
    {
        if (done) { Close(); return; }
        MainButton.IsEnabled = CancelButton.IsEnabled = false;
        InstallPanel.IsEnabled = UninstallPanel.IsEnabled = false;
        StatusText.Visibility = Visibility.Visible;
        try
        {
            if (mode == Mode.Install)
            {
                StatusText.Text = "Installing…";
                bool desktop = DesktopShortcut.IsChecked == true, login = StartAtLogin.IsChecked == true;
                await Task.Run(() => Installer.Install(desktop, login));
                StatusText.Text = "Surround Bridge is installed. It's starting now: look for its icon in the notification area of the taskbar (under ^ if it's hidden).";
                Installer.LaunchInstalled();
            }
            else
            {
                StatusText.Text = "Removing…";
                bool settings = RemoveSettings.IsChecked == true;
                await Task.Run(() => Installer.Uninstall(settings));
                StatusText.Text = "Surround Bridge was removed.";
            }
            done = true;
            MainButton.Content = mode == Mode.Install ? "Done" : "Close";
            MainButton.IsEnabled = true;
            CancelButton.Visibility = Visibility.Collapsed;
        }
        catch (Exception ex)
        {
            StatusText.Text = (mode == Mode.Install ? "Installing failed: " : "Uninstalling failed: ") + ex.Message;
            MainButton.IsEnabled = CancelButton.IsEnabled = true;
            InstallPanel.IsEnabled = UninstallPanel.IsEnabled = true;
        }
    }
}
