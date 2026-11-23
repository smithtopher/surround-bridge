using System.Windows;
using System.Windows.Controls;

namespace SurroundBridge;

public partial class ToneSliders : UserControl
{
    public static readonly DependencyProperty CompactProperty =
        DependencyProperty.Register(nameof(Compact), typeof(bool), typeof(ToneSliders),
                                    new PropertyMetadata(false, (d, _) => ((ToneSliders)d).Layout()));

    /// Compact: the menu's layout ("Bass", "Treble"); otherwise Settings' ("Bass boost", ...).
    public bool Compact { get => (bool)GetValue(CompactProperty); set => SetValue(CompactProperty, value); }

    public ToneSliders()
    {
        InitializeComponent();
        Layout();
    }

    private void Layout()
    {
        BassTitle.Text = Compact ? "Bass" : "Bass boost";
        TrebleTitle.Text = Compact ? "Treble" : "Treble boost";
        LabelCol.Width = new GridLength(Compact ? 88 : 140);
        ValueCol.Width = new GridLength(Compact ? 74 : 84);
    }
}
