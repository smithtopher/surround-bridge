using System.Windows;
using System.Windows.Controls;

namespace SurroundBridge;

public partial class UpmixSliders : UserControl
{
    public static readonly DependencyProperty CompactProperty =
        DependencyProperty.Register(nameof(Compact), typeof(bool), typeof(UpmixSliders),
                                    new PropertyMetadata(false, (d, _) => ((UpmixSliders)d).Layout()));

    /// Compact: the menu's four sliders, smaller labels, no Advanced.
    public bool Compact { get => (bool)GetValue(CompactProperty); set => SetValue(CompactProperty, value); }

    public UpmixSliders()
    {
        InitializeComponent();
        Layout();
    }

    private void Layout()
    {
        Advanced.Visibility = Compact ? Visibility.Collapsed : Visibility.Visible;
        LabelCol.Width = new GridLength(Compact ? 88 : 140);
        ValueCol.Width = new GridLength(Compact ? 74 : 84);
    }
}
