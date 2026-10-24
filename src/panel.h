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

// Верхняя полоса на одном мониторе:
//   [✕ – ❐] [Приложение] [Файл Правка Вид …]   [трей] [ᛒ] [🔋] [En] [⇅] [🔊] [часы] [⏻]
// Кнопки окна слева видны, пока активное окно развёрнуто.
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
    QMenuBar *m_appBar;   // жирное имя приложения + действия с окном
    QMenuBar *m_bar;      // меню самого приложения
    QLabel *m_clock;
    QMenuBar *m_sessionBar; // значок питания справа
    QMenuBar *m_soundBar;   // значок громкости слева от часов
    QMenuBar *m_netBar;     // значок сети слева от громкости
    QMenuBar *m_kbdBar;     // раскладка («En») слева от сети
    QMenuBar *m_batBar;     // батарея слева от раскладки (скрыт, если батарей нет)
    QMenuBar *m_btBar;      // Bluetooth слева от батареи (скрыт, если адаптера нет)
    QMenuBar *m_trayBar;    // трей (значки программ) слева от Bluetooth
    QAction *m_trayClicked = nullptr; // значок, по которому щёлкнули на этом мониторе
    Indicators m_ind;
    void updateIndicators();
    SoundMenu *m_sound;
    LabwcTheme *m_theme;
    QWidget *m_winButtons;  // закрыть/свернуть/восстановить развёрнутого окна (как в Unity)
};
