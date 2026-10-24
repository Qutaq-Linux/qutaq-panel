#include "networkmenu.h"

#include "settings.h"

#include <QCoreApplication>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using NMSettings = QMap<QString, QVariantMap>; // a{sa{sv}}
Q_DECLARE_METATYPE(NMSettings)

static const char *kNM = "org.freedesktop.NetworkManager";
static const char *kNMPath = "/org/freedesktop/NetworkManager";
static const char *kDevice = "org.freedesktop.NetworkManager.Device";
static const char *kWireless = "org.freedesktop.NetworkManager.Device.Wireless";
static const char *kAP = "org.freedesktop.NetworkManager.AccessPoint";
static const char *kActive = "org.freedesktop.NetworkManager.Connection.Active";
static const char *kSettingsPath = "/org/freedesktop/NetworkManager/Settings";
static const char *kSettings = "org.freedesktop.NetworkManager.Settings";
static const char *kConnection = "org.freedesktop.NetworkManager.Settings.Connection";
static const char *kProps = "org.freedesktop.DBus.Properties";

enum { DeviceEthernet = 1, DeviceWifi = 2 };
enum { DevUnavailable = 20, DevDisconnected = 30, DevPrepare = 40, DevActivated = 100 };
enum { NMConnecting = 40, NMConnectedLocal = 50 };
enum { ActiveActivated = 2 };

// ---------- D-Bus ----------

static QDBusConnection bus()
{
    return QDBusConnection::systemBus();
}

static bool nmAvailable()
{
    return bus().isConnected() && bus().interface() && bus().interface()->isServiceRegistered(kNM).value();
}

static QDBusMessage nmCall(const QString &path, const QString &iface, const QString &method,
                           const QVariantList &args = {}, int timeoutMs = 3000)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(kNM, path, iface, method);
    msg.setArguments(args);
    return bus().call(msg, QDBus::Block, timeoutMs);
}

static QVariant unwrap(QVariant v)
{
    while (v.userType() == qMetaTypeId<QDBusVariant>())
        v = v.value<QDBusVariant>().variant();
    return v;
}

static QVariantMap getAll(const QString &path, const QString &iface)
{
    const QDBusMessage r = nmCall(path, kProps, QStringLiteral("GetAll"), {iface}, 1500);
    QVariantMap out;
    if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty())
        r.arguments().first().value<QDBusArgument>() >> out;
    return out;
}

static QVariant getProp(const QString &path, const QString &iface, const QString &name)
{
    const QDBusMessage r = nmCall(path, kProps, QStringLiteral("Get"), {iface, name}, 1500);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
        return {};
    return unwrap(r.arguments().first());
}

static QStringList toPaths(const QVariant &v)
{
    QStringList out;
    const QVariant u = unwrap(v);
    if (u.canConvert<QDBusArgument>()) {
        QList<QDBusObjectPath> list;
        u.value<QDBusArgument>() >> list;
        for (const QDBusObjectPath &p : list)
            out << p.path();
    } else if (u.canConvert<QList<QDBusObjectPath>>()) {
        for (const QDBusObjectPath &p : u.value<QList<QDBusObjectPath>>())
            out << p.path();
    }
    return out;
}

static QString toPath(const QVariant &v)
{
    const QVariant u = unwrap(v);
    if (u.canConvert<QDBusObjectPath>())
        return u.value<QDBusObjectPath>().path();
    return u.toString();
}

static QList<QVariantMap> toMapList(const QVariant &v)
{
    QList<QVariantMap> out;
    const QVariant u = unwrap(v);
    if (u.canConvert<QDBusArgument>())
        u.value<QDBusArgument>() >> out;
    return out;
}

static bool reportError(const QDBusMessage &r, const QString &what)
{
    if (r.type() != QDBusMessage::ErrorMessage)
        return false;
    qWarning().noquote() << "NetworkManager:" << what << r.errorMessage();
    auto *box = new QMessageBox(QMessageBox::Warning, QCoreApplication::translate("NetworkMenu", "Network"),
                                what + QStringLiteral(":\n") + r.errorMessage());
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->show();
    return true;
}

// ---------- saved connections ----------

struct SavedConnection {
    QString path;
    QString id;
    QString type;   // 802-3-ethernet, 802-11-wireless, vpn, wireguard…
    QByteArray ssid;
};

static QList<SavedConnection> savedConnections()
{
    QList<SavedConnection> out;
    const QDBusMessage r = nmCall(kSettingsPath, kSettings, QStringLiteral("ListConnections"));
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
        return out;
    for (const QString &path : toPaths(r.arguments().first())) {
        const QDBusMessage s = nmCall(path, kConnection, QStringLiteral("GetSettings"), {}, 1500);
        if (s.type() != QDBusMessage::ReplyMessage || s.arguments().isEmpty())
            continue;
        NMSettings settings;
        const QDBusArgument arg = s.arguments().first().value<QDBusArgument>();
        arg.beginMap();
        while (!arg.atEnd()) {
            QString key;
            QVariantMap value;
            arg.beginMapEntry();
            arg >> key >> value;
            arg.endMapEntry();
            settings.insert(key, value);
        }
        arg.endMap();
        SavedConnection c;
        c.path = path;
        c.id = settings.value(QStringLiteral("connection")).value(QStringLiteral("id")).toString();
        c.type = settings.value(QStringLiteral("connection")).value(QStringLiteral("type")).toString();
        c.ssid = unwrap(settings.value(QStringLiteral("802-11-wireless")).value(QStringLiteral("ssid"))).toByteArray();
        out << c;
    }
    return out;
}

struct ActiveConnection {
    QString path;
    QString connection; // path of the saved connection
    QString id;
    QString type;
    bool vpn = false;
    uint state = 0;
    QStringList devices;
};

static QList<ActiveConnection> activeConnections()
{
    QList<ActiveConnection> out;
    for (const QString &path : toPaths(getProp(kNMPath, kNM, QStringLiteral("ActiveConnections")))) {
        const QVariantMap p = getAll(path, kActive);
        ActiveConnection a;
        a.path = path;
        a.connection = toPath(p.value(QStringLiteral("Connection")));
        a.id = unwrap(p.value(QStringLiteral("Id"))).toString();
        a.type = unwrap(p.value(QStringLiteral("Type"))).toString();
        a.vpn = unwrap(p.value(QStringLiteral("Vpn"))).toBool() || a.type == QLatin1String("wireguard");
        a.state = unwrap(p.value(QStringLiteral("State"))).toUInt();
        a.devices = toPaths(p.value(QStringLiteral("Devices")));
        out << a;
    }
    return out;
}

static int wifiLevel(int strength)
{
    if (strength <= 0) return 0;
    if (strength < 30) return 1;
    if (strength < 55) return 2;
    if (strength < 80) return 3;
    return 4;
}

// ---------- icons ----------

QIcon networkIcon(const QColor &color, int size, int kind, int level, bool dim, bool vpn)
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
        if (dim)
            c.setAlphaF(0.35);

        if (kind == 2) {
            // Wi-Fi fan: a dot and four arcs, inactive ones pale
            const QPointF o(8 * u, 13.5 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(o, 1.3 * u, 1.3 * u);
            for (int i = 1; i <= 4; ++i) {
                QColor ci = c;
                if (!dim && i > level)
                    ci.setAlphaF(0.3);
                p.setPen(QPen(ci, 1.5 * u, Qt::SolidLine, Qt::RoundCap));
                p.setBrush(Qt::NoBrush);
                const qreal r = 2.9 * u * i;
                p.drawArc(QRectF(o.x() - r, o.y() - r, 2 * r, 2 * r), 50 * 16, 80 * 16);
            }
        } else {
            // Two arrows, up and down, like the wired network icon in Unity
            p.setPen(QPen(c, 1.5 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawLine(QPointF(5 * u, 13.5 * u), QPointF(5 * u, 2.5 * u));
            p.drawPolyline(QPolygonF({QPointF(2.2 * u, 5.3 * u), QPointF(5 * u, 2.5 * u), QPointF(7.8 * u, 5.3 * u)}));
            p.drawLine(QPointF(11 * u, 2.5 * u), QPointF(11 * u, 13.5 * u));
            p.drawPolyline(QPolygonF({QPointF(8.2 * u, 10.7 * u), QPointF(11 * u, 13.5 * u), QPointF(13.8 * u, 10.7 * u)}));
            if (kind == 0) {
                // No network: a slash
                QColor cs = color;
                p.setPen(QPen(cs, 1.5 * u, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(2 * u, 14 * u), QPointF(14 * u, 2 * u));
            }
        }
        if (vpn) {
            // A padlock in the bottom right corner (VPN or a secured network)
            const QRectF body(10.5 * u, 10.5 * u, 5 * u, 4.5 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawRoundedRect(body, 0.8 * u, 0.8 * u);
            p.setPen(QPen(color, 1.1 * u));
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(11.4 * u, 7.6 * u, 3.2 * u, 5.4 * u), 0, 180 * 16);
        }
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

// ---------------------------------------------------------------------------

NetworkMenu::NetworkMenu(QObject *parent)
    : QObject(parent)
    , m_menu(new QMenu)
{
    qDBusRegisterMetaType<NMSettings>();
    m_menu->setToolTipsVisible(true);
    connect(m_menu, &QMenu::aboutToShow, this, &NetworkMenu::rebuild);

    m_updateTimer = new QTimer(this);
    m_updateTimer->setSingleShot(true);
    m_updateTimer->setInterval(300); // NetworkManager sends bursts of signals
    connect(m_updateTimer, &QTimer::timeout, this, &NetworkMenu::updateIcon);

    // Any change of properties or state of NetworkManager and its devices
    QDBusConnection b = bus();
    b.connect(kNM, QString(), kProps, QStringLiteral("PropertiesChanged"), this, SLOT(onNmSignal(QDBusMessage)));
    b.connect(kNM, QString(), kDevice, QStringLiteral("StateChanged"), this, SLOT(onNmSignal(QDBusMessage)));
    b.connect(kNM, kNMPath, kNM, QStringLiteral("StateChanged"), this, SLOT(onNmSignal(QDBusMessage)));
    auto *watcher = new QDBusServiceWatcher(kNM, b, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, m_updateTimer, qOverload<>(&QTimer::start));

    updateIcon();
}

NetworkMenu::~NetworkMenu()
{
    delete m_menu;
}

void NetworkMenu::onNmSignal(const QDBusMessage &)
{
    m_updateTimer->start();
}

void NetworkMenu::updateIcon()
{
    const QColor fg(0xe8, 0xe8, 0xec);
    QAction *a = m_menu->menuAction();
    if (!nmAvailable()) {
        a->setIcon(networkIcon(QColor(0x7a, 0x7a, 0x82), 16, 0, 0, true, false));
        a->setToolTip(tr("NetworkManager is not available"));
        return;
    }
    if (!getProp(kNMPath, kNM, QStringLiteral("NetworkingEnabled")).toBool()) {
        a->setIcon(networkIcon(fg, 16, 0, 0, true, false));
        a->setToolTip(tr("Networking is off"));
        return;
    }

    const QList<ActiveConnection> active = activeConnections();
    const bool vpn = std::any_of(active.cbegin(), active.cend(), [](const ActiveConnection &c) {
        return c.vpn && c.state == ActiveActivated;
    });
    const QString primary = toPath(getProp(kNMPath, kNM, QStringLiteral("PrimaryConnection")));
    const uint state = getProp(kNMPath, kNM, QStringLiteral("State")).toUInt();

    const ActiveConnection *main = nullptr;
    for (const ActiveConnection &c : active)
        if (c.path == primary)
            main = &c;
    if (!main) // no primary one (still connecting): take any non-VPN
        for (const ActiveConnection &c : active)
            if (!c.vpn)
                main = &c;

    QString tip;
    if (main) {
        const bool connecting = main->state != ActiveActivated;
        if (main->type == QLatin1String("802-11-wireless")) {
            int strength = 0;
            if (!main->devices.isEmpty()) {
                const QString ap = toPath(getProp(main->devices.first(), kWireless, QStringLiteral("ActiveAccessPoint")));
                if (!ap.isEmpty() && ap != QLatin1String("/"))
                    strength = getProp(ap, kAP, QStringLiteral("Strength")).toInt();
            }
            a->setIcon(networkIcon(fg, 16, 2, wifiLevel(strength), connecting, vpn));
            tip = connecting ? tr("Connecting to Wi-Fi “%1”…").arg(main->id)
                             : tr("Wi-Fi “%1”, signal %2%").arg(main->id).arg(strength);
        } else {
            a->setIcon(networkIcon(fg, 16, 1, 0, connecting, vpn));
            tip = connecting ? tr("Connecting “%1”…").arg(main->id)
                             : tr("Connection “%1”").arg(main->id);
        }
        if (!connecting && state < NMConnectedLocal)
            tip += tr(" (no network access)");
    } else {
        a->setIcon(networkIcon(fg, 16, 0, 0, state != NMConnecting, false));
        tip = state == NMConnecting ? tr("Connecting…") : tr("Not connected");
    }
    if (vpn)
        tip += QStringLiteral(" + VPN");
    a->setToolTip(tip);
}

// ---------- menu ----------

static QAction *addHeader(QMenu *menu, const QString &text)
{
    QAction *h = menu->addAction(text);
    h->setEnabled(false);
    return h;
}

void NetworkMenu::rebuild()
{
    m_menu->clear();
    if (!nmAvailable()) {
        addHeader(m_menu, tr("NetworkManager is not available"));
        m_menu->addSeparator();
        m_menu->addAction(tr("Edit Connections…"), this, [this] { openEditor(); });
        return;
    }
    const bool networking = getProp(kNMPath, kNM, QStringLiteral("NetworkingEnabled")).toBool();

    QStringList wired, wifi;
    const QDBusMessage r = nmCall(kNMPath, kNM, QStringLiteral("GetDevices"));
    if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty()) {
        for (const QString &dev : toPaths(r.arguments().first())) {
            const QVariantMap p = getAll(dev, kDevice);
            if (unwrap(p.value(QStringLiteral("Managed"))).toBool() == false)
                continue;
            const uint type = unwrap(p.value(QStringLiteral("DeviceType"))).toUInt();
            if (type == DeviceEthernet)
                wired << dev;
            else if (type == DeviceWifi)
                wifi << dev;
        }
    }

    if (networking) {
        for (const QString &dev : std::as_const(wired)) {
            addWiredSection(dev);
            m_menu->addSeparator();
        }
        for (const QString &dev : std::as_const(wifi)) {
            addWifiSection(dev);
            m_menu->addSeparator();
        }
        addVpnMenu();
        m_menu->addSeparator();
    }

    QAction *enable = m_menu->addAction(tr("Enable Networking"));
    enable->setCheckable(true);
    enable->setChecked(networking);
    connect(enable, &QAction::triggered, this, [](bool on) {
        reportError(nmCall(kNMPath, kNM, QStringLiteral("Enable"), {on}), tr("Could not change the network state"));
    });
    if (!wifi.isEmpty()) {
        QAction *w = m_menu->addAction(tr("Enable Wi-Fi"));
        w->setCheckable(true);
        w->setChecked(getProp(kNMPath, kNM, QStringLiteral("WirelessEnabled")).toBool());
        w->setEnabled(networking && getProp(kNMPath, kNM, QStringLiteral("WirelessHardwareEnabled")).toBool());
        connect(w, &QAction::triggered, this, [](bool on) {
            reportError(nmCall(kNMPath, kProps, QStringLiteral("Set"),
                               {QString::fromLatin1(kNM), QStringLiteral("WirelessEnabled"),
                                QVariant::fromValue(QDBusVariant(on))}),
                        tr("Could not turn Wi-Fi on or off"));
        });
    }
    m_menu->addSeparator();

    const QList<ActiveConnection> active = activeConnections();
    QAction *info = m_menu->addAction(tr("Connection Information"), this, &NetworkMenu::showInfo);
    info->setEnabled(std::any_of(active.cbegin(), active.cend(), [](const ActiveConnection &c) {
        return !c.vpn && c.state == ActiveActivated;
    }));
    m_menu->addAction(tr("Edit Connections…"), this, [this] { openEditor(); });
}

void NetworkMenu::addWiredSection(const QString &dev)
{
    const QVariantMap p = getAll(dev, kDevice);
    const QString iface = unwrap(p.value(QStringLiteral("Interface"))).toString();
    const uint state = unwrap(p.value(QStringLiteral("State"))).toUInt();
    const QString activePath = toPath(p.value(QStringLiteral("ActiveConnection")));
    const QString activeConn = activePath.isEmpty() || activePath == QLatin1String("/")
                                   ? QString()
                                   : toPath(getProp(activePath, kActive, QStringLiteral("Connection")));

    addHeader(m_menu, tr("Wired Network (%1)").arg(iface));
    if (state == DevUnavailable) {
        addHeader(m_menu, tr("Cable unplugged"));
        return;
    }
    const QList<SavedConnection> saved = savedConnections();
    const QStringList available = toPaths(p.value(QStringLiteral("AvailableConnections")));
    auto *group = new QActionGroup(m_menu);
    for (const SavedConnection &c : saved) {
        if (!available.contains(c.path))
            continue;
        QAction *a = m_menu->addAction(c.id);
        a->setCheckable(true);
        a->setChecked(c.path == activeConn);
        group->addAction(a);
        const QString conn = c.path;
        connect(a, &QAction::triggered, this, [dev, conn] {
            reportError(nmCall(kNMPath, kNM, QStringLiteral("ActivateConnection"),
                               {QVariant::fromValue(QDBusObjectPath(conn)), QVariant::fromValue(QDBusObjectPath(dev)),
                                QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")))}),
                        tr("Could not connect"));
        });
    }
    if (available.isEmpty() && state == DevDisconnected) {
        // No profile: NetworkManager will create one itself
        m_menu->addAction(tr("Connect"), this, [dev] {
            reportError(nmCall(kNMPath, kNM, QStringLiteral("ActivateConnection"),
                               {QVariant::fromValue(QDBusObjectPath(QStringLiteral("/"))), QVariant::fromValue(QDBusObjectPath(dev)),
                                QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")))}),
                        tr("Could not connect"));
        });
    }
    if (state >= DevPrepare && state <= DevActivated) {
        m_menu->addAction(tr("Disconnect"), this, [dev] {
            reportError(nmCall(dev, kDevice, QStringLiteral("Disconnect")), tr("Could not disconnect"));
        });
    }
}

void NetworkMenu::addWifiSection(const QString &dev)
{
    const QVariantMap p = getAll(dev, kDevice);
    const QString iface = unwrap(p.value(QStringLiteral("Interface"))).toString();
    const uint state = unwrap(p.value(QStringLiteral("State"))).toUInt();

    addHeader(m_menu, tr("Wireless Network (%1)").arg(iface));
    if (!getProp(kNMPath, kNM, QStringLiteral("WirelessEnabled")).toBool()) {
        addHeader(m_menu, tr("Wi-Fi is off"));
        return;
    }

    // Refresh the network list for the next time the menu opens
    QDBusMessage scan = QDBusMessage::createMethodCall(kNM, dev, kWireless, QStringLiteral("RequestScan"));
    scan << QVariantMap();
    bus().call(scan, QDBus::NoBlock);

    struct Net {
        QString ap;
        QString ssid;
        int strength = 0;
        bool secured = false;
        bool sae = false;
        bool saved = false;
        bool active = false;
    };
    const QString activeAp = toPath(getProp(dev, kWireless, QStringLiteral("ActiveAccessPoint")));
    QByteArray activeSsid;
    if (!activeAp.isEmpty() && activeAp != QLatin1String("/") && state == DevActivated)
        activeSsid = getProp(activeAp, kAP, QStringLiteral("Ssid")).toByteArray();
    const QList<SavedConnection> saved = savedConnections();

    // One row per network (SSID), from the strongest access point
    QHash<QByteArray, Net> bySsid;
    for (const QString &ap : toPaths(getProp(dev, kWireless, QStringLiteral("AccessPoints")))) {
        const QVariantMap a = getAll(ap, kAP);
        const QByteArray ssid = unwrap(a.value(QStringLiteral("Ssid"))).toByteArray();
        if (ssid.isEmpty())
            continue; // hidden network
        Net n;
        n.ap = ap;
        n.ssid = QString::fromUtf8(ssid);
        n.strength = unwrap(a.value(QStringLiteral("Strength"))).toInt();
        const uint wpa = unwrap(a.value(QStringLiteral("WpaFlags"))).toUInt();
        const uint rsn = unwrap(a.value(QStringLiteral("RsnFlags"))).toUInt();
        n.secured = wpa || rsn || (unwrap(a.value(QStringLiteral("Flags"))).toUInt() & 0x1);
        n.sae = (rsn & 0x400) && !(rsn & 0x100); // WPA3-Personal only
        n.active = !activeSsid.isEmpty() && ssid == activeSsid;
        n.saved = std::any_of(saved.cbegin(), saved.cend(), [&](const SavedConnection &c) { return c.ssid == ssid; });
        auto it = bySsid.find(ssid);
        if (it == bySsid.end() || it->strength < n.strength || (n.active && !it->active))
            bySsid.insert(ssid, n);
    }
    QList<Net> nets = bySsid.values();
    std::sort(nets.begin(), nets.end(), [](const Net &a, const Net &b) {
        if (a.active != b.active) return a.active;
        if (a.saved != b.saved) return a.saved;
        return a.strength > b.strength;
    });

    const int shown = qMax(1, panelSetting(QStringLiteral("network/maxNetworks"), 6).toInt());
    const QColor fg = m_menu->palette().color(QPalette::WindowText);
    QMenu *more = nullptr;
    auto *group = new QActionGroup(m_menu);
    for (int i = 0; i < nets.size(); ++i) {
        const Net &n = nets.at(i);
        QMenu *target = m_menu;
        if (i >= shown) {
            if (!more)
                more = m_menu->addMenu(tr("More Networks"));
            target = more;
        }
        QAction *a = target->addAction(networkIcon(fg, 16, 2, wifiLevel(n.strength), false, n.secured), n.ssid);
        a->setIconVisibleInMenu(true);
        a->setCheckable(true);
        a->setChecked(n.active);
        a->setToolTip(tr("Signal %1%%2").arg(n.strength).arg(n.secured ? tr(", secured") : QString()));
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, dev, n] {
            if (!n.active)
                connectToAccessPoint(dev, n.ap, n.ssid, n.secured, n.sae);
        });
    }
    if (nets.isEmpty())
        addHeader(m_menu, tr("No networks found"));

    m_menu->addAction(tr("Connect to Hidden Network…"), this, [this, dev] {
        bool ok = false;
        const QString ssid = QInputDialog::getText(nullptr, tr("Hidden Wi-Fi Network"),
                                                   tr("Network name (SSID):"), QLineEdit::Normal, QString(), &ok);
        if (!ok || ssid.isEmpty())
            return;
        const QString pass = QInputDialog::getText(nullptr, tr("Hidden Wi-Fi Network"),
                                                   tr("Password (leave empty for an open network):"), QLineEdit::Password,
                                                   QString(), &ok);
        if (!ok)
            return;
        NMSettings s;
        s[QStringLiteral("connection")] = {{QStringLiteral("id"), ssid}, {QStringLiteral("type"), QStringLiteral("802-11-wireless")}};
        s[QStringLiteral("802-11-wireless")] = {{QStringLiteral("ssid"), ssid.toUtf8()}, {QStringLiteral("hidden"), true}};
        if (!pass.isEmpty())
            s[QStringLiteral("802-11-wireless-security")] = {{QStringLiteral("key-mgmt"), QStringLiteral("wpa-psk")},
                                                              {QStringLiteral("psk"), pass}};
        reportError(nmCall(kNMPath, kNM, QStringLiteral("AddAndActivateConnection"),
                           {QVariant::fromValue(s), QVariant::fromValue(QDBusObjectPath(dev)),
                            QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")))}),
                    tr("Could not connect to “%1”").arg(ssid));
    });

    if (state >= DevPrepare && state <= DevActivated) {
        m_menu->addAction(tr("Disconnect"), this, [dev] {
            reportError(nmCall(dev, kDevice, QStringLiteral("Disconnect")), tr("Could not disconnect"));
        });
    }
}

void NetworkMenu::connectToAccessPoint(const QString &dev, const QString &ap, const QString &ssid, bool secured, bool sae)
{
    // There is a saved connection: just activate it
    for (const SavedConnection &c : savedConnections()) {
        if (c.type == QLatin1String("802-11-wireless") && c.ssid == ssid.toUtf8()) {
            reportError(nmCall(kNMPath, kNM, QStringLiteral("ActivateConnection"),
                               {QVariant::fromValue(QDBusObjectPath(c.path)), QVariant::fromValue(QDBusObjectPath(dev)),
                                QVariant::fromValue(QDBusObjectPath(ap))}),
                        tr("Could not connect to “%1”").arg(ssid));
            return;
        }
    }
    // A new network: NetworkManager takes the rest from the access point
    NMSettings s;
    if (secured) {
        bool ok = false;
        const QString pass = QInputDialog::getText(nullptr, tr("Wi-Fi Connection"),
                                                   tr("Password for “%1”:").arg(ssid), QLineEdit::Password,
                                                   QString(), &ok);
        if (!ok)
            return;
        s[QStringLiteral("802-11-wireless-security")] = {
            {QStringLiteral("key-mgmt"), sae ? QStringLiteral("sae") : QStringLiteral("wpa-psk")},
            {QStringLiteral("psk"), pass}};
    }
    reportError(nmCall(kNMPath, kNM, QStringLiteral("AddAndActivateConnection"),
                       {QVariant::fromValue(s), QVariant::fromValue(QDBusObjectPath(dev)),
                        QVariant::fromValue(QDBusObjectPath(ap))}),
                tr("Could not connect to “%1”").arg(ssid));
}

void NetworkMenu::addVpnMenu()
{
    QMenu *vpn = m_menu->addMenu(tr("VPN Connections"));
    const QList<ActiveConnection> active = activeConnections();
    bool any = false;
    for (const SavedConnection &c : savedConnections()) {
        if (c.type != QLatin1String("vpn") && c.type != QLatin1String("wireguard"))
            continue;
        any = true;
        QString activePath;
        for (const ActiveConnection &a : active)
            if (a.connection == c.path)
                activePath = a.path;
        QAction *a = vpn->addAction(c.id);
        a->setCheckable(true);
        a->setChecked(!activePath.isEmpty());
        const QString conn = c.path;
        connect(a, &QAction::triggered, this, [conn, activePath, id = c.id] {
            if (!activePath.isEmpty())
                reportError(nmCall(kNMPath, kNM, QStringLiteral("DeactivateConnection"),
                                   {QVariant::fromValue(QDBusObjectPath(activePath))}),
                            tr("Could not disconnect “%1”").arg(id));
            else
                reportError(nmCall(kNMPath, kNM, QStringLiteral("ActivateConnection"),
                                   {QVariant::fromValue(QDBusObjectPath(conn)),
                                    QVariant::fromValue(QDBusObjectPath(QStringLiteral("/"))),
                                    QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")))}),
                            tr("Could not connect “%1”").arg(id));
        });
    }
    if (!any)
        addHeader(vpn, tr("No VPN connections"));
    vpn->addSeparator();
    vpn->addAction(tr("Configure VPN…"), this, [this] { openEditor(); });
}

void NetworkMenu::openEditor(const QStringList &extraArgs)
{
    QString cmd = panelSetting(QStringLiteral("network/editorCommand"), QString()).toString();
    if (cmd.isEmpty())
        cmd = QStringLiteral("nm-connection-editor");
    QStringList args = QProcess::splitCommand(cmd) + extraArgs;
    const QString program = args.takeFirst();
    if (QStandardPaths::findExecutable(program).isEmpty() && !QFileInfo::exists(program)) {
        auto *box = new QMessageBox(QMessageBox::Information, tr("Network"),
                                    tr("Program “%1” not found.\nInstall the network-manager-applet package.")
                                        .arg(program));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->show();
        return;
    }
    QProcess::startDetached(program, args);
}

// ---------- connection information ----------

static QString prefixToMask(int prefix)
{
    const quint32 m = prefix <= 0 ? 0 : (prefix >= 32 ? 0xffffffffu : ~((1u << (32 - prefix)) - 1));
    return QStringLiteral("%1.%2.%3.%4").arg(m >> 24).arg((m >> 16) & 0xff).arg((m >> 8) & 0xff).arg(m & 0xff);
}

void NetworkMenu::showInfo()
{
    if (m_info) {
        m_info->raise();
        m_info->activateWindow();
        return;
    }
    auto *dlg = new QDialog;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setObjectName(QStringLiteral("network-info"));
    dlg->setWindowTitle(tr("Connection Information"));
    m_info = dlg;
    auto *layout = new QVBoxLayout(dlg);

    for (const ActiveConnection &c : activeConnections()) {
        if (c.vpn || c.state != ActiveActivated || c.devices.isEmpty())
            continue;
        const QString dev = c.devices.first();
        const QVariantMap d = getAll(dev, kDevice);
        const uint type = unwrap(d.value(QStringLiteral("DeviceType"))).toUInt();

        auto *box = new QGroupBox(c.id);
        auto *form = new QFormLayout(box);
        form->setLabelAlignment(Qt::AlignRight);
        auto row = [&](const QString &label, const QString &value) {
            if (value.isEmpty())
                return;
            auto *v = new QLabel(value);
            v->setTextInteractionFlags(Qt::TextSelectableByMouse);
            form->addRow(QStringLiteral("<b>%1</b>").arg(label), v);
        };
        row(tr("Interface"), unwrap(d.value(QStringLiteral("Interface"))).toString()
                                            + (type == DeviceWifi ? QStringLiteral(" (Wi-Fi)") : type == DeviceEthernet ? QStringLiteral(" (Ethernet)") : QString()));
        row(tr("MAC address"), unwrap(d.value(QStringLiteral("HwAddress"))).toString());
        if (type == DeviceEthernet) {
            const uint speed = getProp(dev, QStringLiteral("org.freedesktop.NetworkManager.Device.Wired"), QStringLiteral("Speed")).toUInt();
            if (speed)
                row(tr("Speed"), tr("%1 Mbit/s").arg(speed));
        } else if (type == DeviceWifi) {
            const uint rate = getProp(dev, kWireless, QStringLiteral("Bitrate")).toUInt();
            if (rate)
                row(tr("Speed"), tr("%1 Mbit/s").arg(rate / 1000));
        }

        const QString ip4 = toPath(d.value(QStringLiteral("Ip4Config")));
        if (!ip4.isEmpty() && ip4 != QLatin1String("/")) {
            const QVariantMap cfg = getAll(ip4, QStringLiteral("org.freedesktop.NetworkManager.IP4Config"));
            const QList<QVariantMap> addrs = toMapList(cfg.value(QStringLiteral("AddressData")));
            if (!addrs.isEmpty()) {
                row(tr("IPv4 address"), addrs.first().value(QStringLiteral("address")).toString());
                row(tr("Subnet mask"), prefixToMask(addrs.first().value(QStringLiteral("prefix")).toInt()));
            }
            row(tr("Gateway"), unwrap(cfg.value(QStringLiteral("Gateway"))).toString());
            QStringList dns;
            for (const QVariantMap &n : toMapList(cfg.value(QStringLiteral("NameserverData"))))
                dns << n.value(QStringLiteral("address")).toString();
            row(QStringLiteral("DNS"), dns.join(QStringLiteral(", ")));
        }
        const QString ip6 = toPath(d.value(QStringLiteral("Ip6Config")));
        if (!ip6.isEmpty() && ip6 != QLatin1String("/")) {
            const QVariantMap cfg = getAll(ip6, QStringLiteral("org.freedesktop.NetworkManager.IP6Config"));
            for (const QVariantMap &a : toMapList(cfg.value(QStringLiteral("AddressData")))) {
                const QString addr = a.value(QStringLiteral("address")).toString();
                if (!addr.startsWith(QLatin1String("fe80"))) {
                    row(tr("IPv6 address"), addr + '/' + a.value(QStringLiteral("prefix")).toString());
                    break;
                }
            }
        }
        layout->addWidget(box);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(tr("Close"));
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    layout->addWidget(buttons);
    dlg->setMinimumWidth(380);
    dlg->setFixedSize(dlg->sizeHint());
    dlg->show();
}
