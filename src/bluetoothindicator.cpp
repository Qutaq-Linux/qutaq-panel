#include "bluetoothindicator.h"

#include "settings.h"

#include <QAction>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDebug>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <fcntl.h>
#include <linux/rfkill.h>
#include <unistd.h>

#include <algorithm>

static const char *kBluez = "org.bluez";
static const char *kAdapter = "org.bluez.Adapter1";
static const char *kDevice = "org.bluez.Device1";
static const char *kBattery = "org.bluez.Battery1";
static const char *kProps = "org.freedesktop.DBus.Properties";

static QDBusConnection sysBus()
{
    return QDBusConnection::systemBus();
}

static void showError(const QString &text)
{
    auto *box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Bluetooth"), text);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->show();
}

QIcon bluetoothIcon(const QColor &color, int size, bool on, bool connected)
{
    QIcon icon;
    for (qreal dpr : {1.0, 2.0}) {
        QPixmap pm(QSize(size, size) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal u = size / 16.0;
        QColor c = color;
        if (!on)
            c.setAlphaF(0.35);
        // Bluetooth rune
        QPainterPath rune;
        rune.moveTo(4 * u, 4.8 * u);
        rune.lineTo(11 * u, 10.8 * u);
        rune.lineTo(7.6 * u, 14 * u);
        rune.lineTo(7.6 * u, 2 * u);
        rune.lineTo(11 * u, 5.2 * u);
        rune.lineTo(4 * u, 11.2 * u);
        p.setPen(QPen(c, 1.4 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(rune);
        if (on && connected) {
            // Dots on both sides: devices are connected
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(QPointF(2.2 * u, 8 * u), 1.1 * u, 1.1 * u);
            p.drawEllipse(QPointF(13.8 * u, 8 * u), 1.1 * u, 1.1 * u);
        }
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

static QIcon deviceIcon(const QString &bluezIcon)
{
    // BlueZ uses freedesktop icon names: audio-headset, input-mouse, phone…
    for (const QString &name : {bluezIcon, bluezIcon + QStringLiteral("-symbolic"), QStringLiteral("bluetooth")}) {
        const QIcon i = QIcon::fromTheme(name);
        if (!name.isEmpty() && !i.isNull())
            return i;
    }
    return {};
}

// rfkill: lift the soft block on Bluetooth (like "rfkill unblock bluetooth").
// /dev/rfkill is accessible to the user of the active session (systemd uaccess rule).
static bool rfkillUnblock()
{
    const int fd = ::open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct rfkill_event ev = {};
    ev.type = RFKILL_TYPE_BLUETOOTH;
    ev.op = RFKILL_OP_CHANGE_ALL;
    ev.soft = 0;
    const bool ok = ::write(fd, &ev, sizeof(ev)) >= ssize_t(RFKILL_EVENT_SIZE_V1);
    ::close(fd);
    return ok;
}

// ---------------------------------------------------------------------------

BluetoothIndicator::BluetoothIndicator(QObject *parent)
    : QObject(parent)
    , m_menu(new QMenu)
{
    m_menu->setToolTipsVisible(true);
    connect(m_menu, &QMenu::aboutToShow, this, &BluetoothIndicator::rebuildMenu);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);
    connect(m_debounce, &QTimer::timeout, this, &BluetoothIndicator::refresh);

    QDBusConnection b = sysBus();
    b.connect(kBluez, QString(), kProps, QStringLiteral("PropertiesChanged"), this, SLOT(onBluezSignal(QDBusMessage)));
    b.connect(kBluez, QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("InterfacesAdded"),
              this, SLOT(onBluezSignal(QDBusMessage)));
    b.connect(kBluez, QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("InterfacesRemoved"),
              this, SLOT(onBluezSignal(QDBusMessage)));
    // The bluetooth service started/stopped, or an adapter was plugged in (USB dongle)
    auto *watcher = new QDBusServiceWatcher(kBluez, b, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, m_debounce, qOverload<>(&QTimer::start));

    refresh();
}

BluetoothIndicator::~BluetoothIndicator()
{
    delete m_menu;
}

void BluetoothIndicator::onBluezSignal(const QDBusMessage &)
{
    m_debounce->start();
}

BluetoothIndicator::Objects BluetoothIndicator::managedObjects() const
{
    Objects out;
    if (!sysBus().isConnected() || !sysBus().interface() || !sysBus().interface()->isServiceRegistered(kBluez).value())
        return out;
    QDBusMessage msg = QDBusMessage::createMethodCall(kBluez, QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                                                      QStringLiteral("GetManagedObjects"));
    const QDBusMessage r = sysBus().call(msg, QDBus::Block, 2000);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
        return out;
    // a{oa{sa{sv}}}
    const QDBusArgument arg = r.arguments().first().value<QDBusArgument>();
    arg.beginMap();
    while (!arg.atEnd()) {
        QDBusObjectPath path;
        arg.beginMapEntry();
        arg >> path;
        QHash<QString, QVariantMap> ifaces;
        arg.beginMap();
        while (!arg.atEnd()) {
            QString iface;
            QVariantMap props;
            arg.beginMapEntry();
            arg >> iface >> props;
            arg.endMapEntry();
            ifaces.insert(iface, props);
        }
        arg.endMap();
        arg.endMapEntry();
        out.insert(path.path(), ifaces);
    }
    arg.endMap();
    return out;
}

QString BluetoothIndicator::adapterPath(const Objects &objs) const
{
    QStringList adapters;
    for (auto it = objs.cbegin(); it != objs.cend(); ++it)
        if (it.value().contains(kAdapter))
            adapters << it.key();
    std::sort(adapters.begin(), adapters.end());
    for (const QString &a : adapters) // a powered one is preferred
        if (objs.value(a).value(kAdapter).value(QStringLiteral("Powered")).toBool())
            return a;
    return adapters.value(0);
}

void BluetoothIndicator::refresh()
{
    const Objects objs = managedObjects();
    const QString adapter = adapterPath(objs);
    const QString mode = panelSetting(QStringLiteral("bluetooth/show"), QStringLiteral("auto")).toString();
    m_shown = mode == QLatin1String("always") || (mode != QLatin1String("never") && !adapter.isEmpty());

    const QVariantMap a = objs.value(adapter).value(kAdapter);
    const bool on = a.value(QStringLiteral("Powered")).toBool();
    QStringList connected;
    for (auto it = objs.cbegin(); it != objs.cend(); ++it) {
        const QVariantMap d = it.value().value(kDevice);
        if (d.value(QStringLiteral("Connected")).toBool())
            connected << d.value(QStringLiteral("Alias")).toString();
    }
    const QColor fg(0xe8, 0xe8, 0xec);
    m_menu->menuAction()->setIcon(bluetoothIcon(adapter.isEmpty() ? QColor(0x7a, 0x7a, 0x82) : fg, 16, on, !connected.isEmpty()));
    QString tip;
    if (adapter.isEmpty())
        tip = tr("No Bluetooth adapter found");
    else if (!on)
        tip = tr("Bluetooth is off");
    else if (connected.isEmpty())
        tip = tr("Bluetooth is on");
    else
        tip = tr("Connected: %1").arg(connected.join(QStringLiteral(", ")));
    m_menu->menuAction()->setToolTip(tip);
    Q_EMIT changed();
}

void BluetoothIndicator::rebuildMenu()
{
    m_menu->clear();
    const Objects objs = managedObjects();
    const QString adapter = adapterPath(objs);
    if (adapter.isEmpty()) {
        m_menu->addAction(tr("No Bluetooth adapter found"))->setEnabled(false);
        m_menu->addSeparator();
        m_menu->addAction(tr("Bluetooth Settings…"), this, [this] {
            launch(QStringLiteral("bluetooth/settingsCommand"),
                   {QStringLiteral("blueman-manager"), QStringLiteral("blueberry"), QStringLiteral("gnome-control-center bluetooth")});
        });
        return;
    }
    const QVariantMap a = objs.value(adapter).value(kAdapter);
    const bool on = a.value(QStringLiteral("Powered")).toBool();

    QAction *power = m_menu->addAction(QStringLiteral("Bluetooth"));
    power->setCheckable(true);
    power->setChecked(on);
    if (a.value(QStringLiteral("PowerState")).toString() == QLatin1String("off-blocked"))
        power->setToolTip(tr("Blocked (rfkill): it will be unblocked when turned on"));
    connect(power, &QAction::triggered, this, [this](bool v) { setPowered(v); });

    QAction *visible = m_menu->addAction(tr("Visible"));
    visible->setCheckable(true);
    visible->setChecked(a.value(QStringLiteral("Discoverable")).toBool());
    visible->setEnabled(on);
    visible->setToolTip(tr("Other devices see this computer as “%1”").arg(a.value(QStringLiteral("Alias")).toString()));
    connect(visible, &QAction::triggered, this, [this](bool v) { setAdapterProperty(QStringLiteral("Discoverable"), v); });

    // Paired devices of this adapter
    struct Dev { QString path, name, icon; bool connected; int battery; };
    QList<Dev> devs;
    for (auto it = objs.cbegin(); it != objs.cend(); ++it) {
        const QVariantMap d = it.value().value(kDevice);
        if (d.isEmpty() || !d.value(QStringLiteral("Paired")).toBool()
            || d.value(QStringLiteral("Adapter")).value<QDBusObjectPath>().path() != adapter)
            continue;
        const QVariantMap bat = it.value().value(kBattery);
        devs << Dev{it.key(), d.value(QStringLiteral("Alias")).toString(), d.value(QStringLiteral("Icon")).toString(),
                    d.value(QStringLiteral("Connected")).toBool(),
                    bat.isEmpty() ? -1 : bat.value(QStringLiteral("Percentage")).toInt()};
    }
    std::sort(devs.begin(), devs.end(), [](const Dev &x, const Dev &y) {
        return x.connected != y.connected ? x.connected : x.name.localeAwareCompare(y.name) < 0;
    });
    if (on && !devs.isEmpty()) {
        m_menu->addSeparator();
        for (const Dev &d : std::as_const(devs)) {
            QMenu *sub = m_menu->addMenu(deviceIcon(d.icon), d.name + (d.connected ? QStringLiteral("  ●") : QString()));
            sub->menuAction()->setIconVisibleInMenu(true);
            QAction *conn = sub->addAction(tr("Connected"));
            conn->setCheckable(true);
            conn->setChecked(d.connected);
            const QString path = d.path;
            connect(conn, &QAction::triggered, this, [this, path](bool v) {
                deviceCall(path, v ? QStringLiteral("Connect") : QStringLiteral("Disconnect"));
            });
            if (d.battery >= 0)
                sub->addAction(tr("Battery: %1%").arg(d.battery))->setEnabled(false);
            sub->addSeparator();
            sub->addAction(tr("Forget Device"), this, [this, adapter, path, name = d.name] {
                QDBusMessage msg = QDBusMessage::createMethodCall(kBluez, adapter, kAdapter, QStringLiteral("RemoveDevice"));
                msg << QVariant::fromValue(QDBusObjectPath(path));
                const QDBusMessage r = sysBus().call(msg, QDBus::Block, 5000);
                if (r.type() == QDBusMessage::ErrorMessage)
                    showError(tr("Could not forget “%1”:\n%2").arg(name, r.errorMessage()));
                m_debounce->start();
            });
        }
    }

    m_menu->addSeparator();
    QAction *add = m_menu->addAction(tr("Add New Device…"), this, [this] {
        launch(QStringLiteral("bluetooth/wizardCommand"),
               {QStringLiteral("blueman-assistant"), QStringLiteral("blueman-manager"), QStringLiteral("blueberry"),
                QStringLiteral("bluedevil-wizard"), QStringLiteral("gnome-control-center bluetooth")});
    });
    add->setEnabled(on);
    m_menu->addAction(tr("Bluetooth Settings…"), this, [this] {
        launch(QStringLiteral("bluetooth/settingsCommand"),
               {QStringLiteral("blueman-manager"), QStringLiteral("blueberry"), QStringLiteral("gnome-control-center bluetooth"),
                QStringLiteral("systemsettings kcm_bluetooth")});
    });
}

void BluetoothIndicator::setAdapterProperty(const QString &name, bool value)
{
    const QString adapter = adapterPath(managedObjects());
    if (adapter.isEmpty())
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(kBluez, adapter, kProps, QStringLiteral("Set"));
    msg << QString::fromLatin1(kAdapter) << name << QVariant::fromValue(QDBusVariant(value));
    const QDBusMessage r = sysBus().call(msg, QDBus::Block, 5000);
    if (r.type() == QDBusMessage::ErrorMessage) {
        if (name == QLatin1String("Powered") && value && r.errorName() == QLatin1String("org.bluez.Error.Blocked")) {
            // The adapter is blocked by rfkill (e.g. airplane mode): unblock it and try again
            if (rfkillUnblock()) {
                qInfo() << "bluetooth: rfkill unblocked";
                QTimer::singleShot(700, this, [this] { setAdapterProperty(QStringLiteral("Powered"), true); });
                return;
            }
            showError(tr("Bluetooth is blocked (rfkill). Run “rfkill unblock bluetooth”."));
            return;
        }
        showError(tr("Could not change “%1”:\n%2").arg(name, r.errorMessage()));
    }
    m_debounce->start();
}

void BluetoothIndicator::setPowered(bool on)
{
    setAdapterProperty(QStringLiteral("Powered"), on);
}

void BluetoothIndicator::deviceCall(const QString &path, const QString &method)
{
    // Connecting can take up to half a minute, so don't block the panel
    QDBusMessage msg = QDBusMessage::createMethodCall(kBluez, path, kDevice, method);
    auto *w = new QDBusPendingCallWatcher(sysBus().asyncCall(msg, 30000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, method](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusMessage r = w->reply();
        if (r.type() == QDBusMessage::ErrorMessage)
            showError(QStringLiteral("%1: %2").arg(method == QLatin1String("Connect") ? tr("Could not connect")
                                                                                  : tr("Could not disconnect"),
                                                r.errorMessage()));
        m_debounce->start();
    });
}

void BluetoothIndicator::launch(const QString &settingKey, const QStringList &candidates)
{
    QString cmd = panelSetting(settingKey, QString()).toString();
    if (cmd.isEmpty()) {
        for (const QString &c : candidates)
            if (!QStandardPaths::findExecutable(c.section(' ', 0, 0)).isEmpty()) {
                cmd = c;
                break;
            }
    }
    if (cmd.isEmpty()) {
        showError(tr("No Bluetooth settings program found.\nInstall blueman."));
        return;
    }
    QStringList args = QProcess::splitCommand(cmd);
    const QString program = args.takeFirst();
    QProcess::startDetached(program, args);
}
