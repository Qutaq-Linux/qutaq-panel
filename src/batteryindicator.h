#pragma once

#include <QDBusMessage>
#include <QObject>
#include <QString>

class QAction;
class QMenu;
class QTimer;

// Индикатор питания, как indicator-power в Ubuntu Unity:
//
//   Аккумулятор        85% (2 ч 10 мин до разряда)
//   Мышь MX Master     40%            ← периферия с батареей (Bluetooth и др.)
//   ─────────────
//   ☐ Показывать процент на панели
//   ☐ Показывать оставшееся время
//   ─────────────
//   Параметры электропитания…
//
// Источник — UPower (org.freedesktop.UPower), он есть и на x86-64, и на ARM64.
// Без UPower — напрямую /sys/class/power_supply (*/type = Battery).
// Индикатор скрыт, если батарей нет (обычный ПК без беспроводной периферии).
// При разряде до 10% и 5% — уведомление (org.freedesktop.Notifications).
//
// Настройки, секция [battery]: show=auto|always|never, showPercentage, showTime,
// settingsCommand (пусто — xfce4-power-manager-settings и т. п.).
class BatteryIndicator : public QObject
{
    Q_OBJECT
public:
    explicit BatteryIndicator(QObject *parent = nullptr);
    ~BatteryIndicator() override;

    QMenu *menu() const { return m_menu; }
    QAction *labelAction() const { return m_label; } // процент/время рядом со значком
    bool isShown() const { return m_shown; }

    struct Device {
        QString name;
        int kind = 0;          // тип UPower: 2 — батарея ноутбука, 5 — мышь, 6 — клавиатура…
        double percent = -1;
        int state = 0;         // 1 заряжается, 2 разряжается, 3 пуст, 4 заряжен, 5/6 ожидание
        qint64 toEmpty = 0;    // секунды
        qint64 toFull = 0;
        bool powerSupply = false; // питает компьютер (а не мышь)
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
// Значок батареи: percent 0–100, charging — молния, empty — восклицательный знак
QIcon batteryIcon(const QColor &color, int size, double percent, bool charging, bool full);
