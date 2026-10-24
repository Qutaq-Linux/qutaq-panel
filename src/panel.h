#pragma once

#include <QWidget>

class MenuController;
class SessionMenu;
class SoundMenu;
class NetworkMenu;
class KeyboardIndicator;
class BatteryIndicator;
class BluetoothIndicator;
class SystemTray;
class QAction;
class LabwcTheme;
class WindowButton;
class QLabel;
class QMenuBar;
class QScreen;

// The top bar on one monitor:
//   [✕ – ❐] [Application] [File Edit View …]   [tray] [ᛒ] [🔋] [En] [⇅] [🔊] [clock] [⏻]
// The window buttons on the left are visible while the active window is maximized.
class Panel : public QWidget
{
    Q_OBJECT
public:
    static constexpr int kHeight = 28;

    struct Indicators {
        BluetoothIndicator *bluetooth;
        BatteryIndicator *battery;
        KeyboardIndicator *keyboard;
        NetworkMenu *network;
        SoundMenu *sound;
        SessionMenu *session;
        SystemTray *tray;
    };

    Panel(MenuController *controller, LabwcTheme *theme, const Indicators &ind, QScreen *screen);
    void setupLayerShell();

private Q_SLOTS:
    void updateIndicatorsSlot() { updateIndicators(); }
    void rebuildTray();

private:
    void paintEvent(QPaintEvent *) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void rebuild();
    void updateClock();
    void updateWindowButtons();
    void buildWindowButtons();

    MenuController *m_controller;
    QScreen *m_screen;
    QMenuBar *m_appBar;   // bold application name + window actions
    QMenuBar *m_bar;      // the application's own menu
    QLabel *m_clock;
    QMenuBar *m_sessionBar; // power icon on the right
    QMenuBar *m_soundBar;   // volume icon left of the clock
    QMenuBar *m_netBar;     // network icon left of the volume
    QMenuBar *m_kbdBar;     // keyboard layout ("En") left of the network
    QMenuBar *m_batBar;     // battery left of the layout (hidden if there are no batteries)
    QMenuBar *m_btBar;      // Bluetooth left of the battery (hidden if there is no adapter)
    QMenuBar *m_trayBar;    // tray (program icons) left of Bluetooth
    QAction *m_trayClicked = nullptr; // the icon clicked on this monitor
    Indicators m_ind;
    void updateIndicators();
    SoundMenu *m_sound;
    LabwcTheme *m_theme;
    QWidget *m_winButtons;  // close/minimize/restore of the maximized window (as in Unity)
};
