#pragma once

#include <QDBusMessage>
#include <QHash>
#include <QObject>
#include <QVariantMap>

class QMenu;
class QTimer;

// Bluetooth indicator, like indicator-bluetooth in Ubuntu Unity:
//
//   ☑ Bluetooth                 ← turn the adapter on/off (lifting an rfkill block)
//   ☐ Visible                   ← other devices can see the computer
//   ─────────────
//   🎧 WH-1000XM4 Headphones ▸  ← paired devices
//        ☑ Connected
//        Battery: 80%
//        Forget Device
//   ─────────────
//   Add New Device…
//   Bluetooth Settings…
//
// Everything goes through BlueZ (org.bluez, system bus). With no adapter (many PCs have none)
// or with the bluetooth service stopped, the indicator is hidden.
// Settings, section [bluetooth]: show=auto|always|never, settingsCommand, wizardCommand.
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
    using Objects = QHash<QString, QHash<QString, QVariantMap>>; // path → interface → properties

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
