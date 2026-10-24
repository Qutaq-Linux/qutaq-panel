#pragma once

#include <QDBusMessage>
#include <QHash>
#include <QObject>
#include <QVariantMap>

class QMenu;
class QTimer;

// Индикатор Bluetooth, как indicator-bluetooth в Ubuntu Unity:
//
//   ☑ Bluetooth                 ← включить/выключить адаптер (при rfkill-блокировке — снять её)
//   ☐ Видимость                 ← другие устройства видят компьютер
//   ─────────────
//   🎧 Наушники WH-1000XM4 ▸    ← сопряжённые устройства
//        ☑ Подключено
//        Заряд: 80%
//        Забыть устройство
//   ─────────────
//   Добавить новое устройство…
//   Параметры Bluetooth…
//
// Всё через BlueZ (org.bluez, системная шина). Если адаптера нет (у многих ПК его нет)
// или служба bluetooth не запущена — индикатор скрыт.
// Настройки, секция [bluetooth]: show=auto|always|never, settingsCommand, wizardCommand.
class BluetoothIndicator : public QObject
{
    Q_OBJECT
public:
    explicit BluetoothIndicator(QObject *parent = nullptr);
    ~BluetoothIndicator() override;

    QMenu *menu() const { return m_menu; }
    bool isShown() const { return m_shown; }

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    void onBluezSignal(const QDBusMessage &msg);

private:
    using Objects = QHash<QString, QHash<QString, QVariantMap>>; // путь → интерфейс → свойства

    Objects managedObjects() const;
    QString adapterPath(const Objects &objs) const;
    void refresh();
    void rebuildMenu();
    void setPowered(bool on);
    void setAdapterProperty(const QString &name, bool value);
    void deviceCall(const QString &path, const QString &method);
    void launch(const QString &settingKey, const QStringList &candidates);

    QMenu *m_menu = nullptr;
    QTimer *m_debounce = nullptr;
    bool m_shown = false;
};

class QIcon;
class QColor;
QIcon bluetoothIcon(const QColor &color, int size, bool on, bool connected);
