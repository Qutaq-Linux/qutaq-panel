#pragma once

#include <QDBusMessage>
#include <QObject>
#include <QString>

class QAction;
class QMenu;
class QTimer;

// Power indicator, like indicator-power in Ubuntu Unity:
//
//   Battery            85% (2 h 10 min remaining)
//   Mouse MX Master    40%            ← peripherals with a battery (Bluetooth etc.)
//   ─────────────
//   ☐ Show percentage in the panel
//   ☐ Show time remaining
//   ─────────────
//   Power Settings…
//
// Source: UPower (org.freedesktop.UPower), available on both x86-64 and ARM64.
// Without UPower: /sys/class/power_supply directly (*/type = Battery).
// Hidden when there are no batteries (a desktop PC without wireless peripherals).
// Notification at 10% and 5% (org.freedesktop.Notifications).
//
// Settings, section [battery]: show=auto|always|never, showPercentage, showTime,
// settingsCommand (empty: xfce4-power-manager-settings and the like).
class BatteryIndicator : public QObject
{
    Q_OBJECT
public:
    explicit BatteryIndicator(QObject *parent = nullptr);
    ~BatteryIndicator() override;

    QMenu *menu() const { return m_menu; }
    QAction *labelAction() const { return m_label; } // percentage/time next to the icon
    bool isShown() const { return m_shown; }

    struct Device {
        QString name;
        int kind = 0;          // UPower type: 2 laptop battery, 5 mouse, 6 keyboard…
        double percent = -1;
        int state = 0;         // 1 charging, 2 discharging, 3 empty, 4 full, 5/6 pending
        qint64 toEmpty = 0;    // seconds
        qint64 toFull = 0;
        bool powerSupply = false; // powers the computer (not a mouse)
    };

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    void onUPowerSignal(const QDBusMessage &msg);

private:
    void refresh();
    void rebuildMenu();
    QList<Device> readUPower(bool *ok) const;
    QList<Device> readSysfs() const;
    void notifyLow(const Device &main);

    QMenu *m_menu = nullptr;
    QAction *m_label = nullptr;
    QTimer *m_timer = nullptr;
    QTimer *m_debounce = nullptr;
    QList<Device> m_devices;
    bool m_shown = false;
    int m_lastWarned = 101;
};

class QIcon;
class QColor;
// Battery icon: percent 0–100, charging adds a bolt, empty an exclamation mark
QIcon batteryIcon(const QColor &color, int size, double percent, bool charging, bool full);
