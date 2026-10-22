#include "statusnotifier.h"

#include "dbusmenuimporter.h"
#include "settings.h"

#include <QAction>
#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTimer>
#include <QtEndian>

#include <unistd.h>

static const char *kWatcherService = "org.kde.StatusNotifierWatcher";
static const char *kWatcherPath = "/StatusNotifierWatcher";
static const char *kWatcherIface = "org.kde.StatusNotifierWatcher";

static QDBusConnection bus() { return QDBusConnection::sessionBus(); }

// «:1.42/org/ayatana/NotificationItem/x» → служба и путь; без пути — /StatusNotifierItem
static void splitKey(const QString &key, QString *service, QString *path)
{
    const int slash = key.indexOf('/');
    if (slash < 0) {
        *service = key;
        *path = QStringLiteral("/StatusNotifierItem");
    } else {
        *service = key.left(slash);
        *path = key.mid(slash);
    }
}

// ---------------- тема значков ----------------

void setupIconTheme()
{
    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
    const QString current = QIcon::themeName();
    if (!current.isEmpty() && current != QLatin1String("hicolor"))
        return;

    QString name;
    // 1. GTK 3/4 — Manjaro Xfce пишет тему и туда
    const QString cfg = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    for (const char *f : {"/gtk-3.0/settings.ini", "/gtk-4.0/settings.ini"}) {
        QSettings s(cfg + QLatin1String(f), QSettings::IniFormat);
        name = s.value(QStringLiteral("Settings/gtk-icon-theme-name")).toString();
        if (!name.isEmpty())
            break;
    }
    // 2. Xfce (xsettings) и GNOME
    auto run = [](const QString &prog, const QStringList &args) {
        if (QStandardPaths::findExecutable(prog).isEmpty())
            return QString();
        QProcess p;
        p.start(prog, args);
        if (!p.waitForFinished(1500))
            return QString();
        QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        out.remove('\'');
        return out;
    };
    if (name.isEmpty())
        name = run(QStringLiteral("xfconf-query"), {QStringLiteral("-c"), QStringLiteral("xsettings"),
                                                    QStringLiteral("-p"), QStringLiteral("/Net/IconThemeName")});
    if (name.isEmpty())
        name = run(QStringLiteral("gsettings"), {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
                                                 QStringLiteral("icon-theme")});
    if (!name.isEmpty()) {
        QIcon::setThemeName(name);
        qInfo().noquote() << "icon theme:" << name;
    }
}

// ---------------- справочник ----------------

StatusNotifierWatcher::StatusNotifierWatcher(QObject *parent)
    : QObject(parent)
    , m_serviceWatcher(new QDBusServiceWatcher(this))
{
    m_serviceWatcher->setConnection(bus());
    m_serviceWatcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &StatusNotifierWatcher::onServiceGone);
}

void StatusNotifierWatcher::RegisterStatusNotifierItem(const QString &serviceOrPath)
{
    QString service, path;
    if (serviceOrPath.startsWith('/')) { // libappindicator передаёт путь, служба — отправитель
        service = message().service();
        path = serviceOrPath;
    } else {
        service = serviceOrPath;
        path = QStringLiteral("/StatusNotifierItem");
    }
    const QString key = service + path;
    if (m_items.contains(key))
        return;
    m_items << key;
    m_serviceWatcher->addWatchedService(service);
    // Значок с известным именем (org.kde.foo) умирает вместе с процессом — следим и за ним
    if (!service.startsWith(':') && calledFromDBus())
        m_serviceWatcher->addWatchedService(message().service());
    Q_EMIT StatusNotifierItemRegistered(key);
}

void StatusNotifierWatcher::RegisterStatusNotifierHost(const QString &service)
{
    if (m_hosts.contains(service))
        return;
    m_hosts.insert(service);
    m_serviceWatcher->addWatchedService(service);
    Q_EMIT StatusNotifierHostRegistered();
}

void StatusNotifierWatcher::onServiceGone(const QString &service)
{
    m_serviceWatcher->removeWatchedService(service);
    for (int i = m_items.size() - 1; i >= 0; --i) {
        const QString key = m_items.at(i);
        QString s, p;
        splitKey(key, &s, &p);
        if (s == service) {
            m_items.removeAt(i);
            Q_EMIT StatusNotifierItemUnregistered(key);
        }
    }
    if (m_hosts.remove(service))
        Q_EMIT StatusNotifierHostUnregistered();
}

// ---------------- значки ----------------

// a(iiay): ARGB32, порядок байтов сетевой
static QIcon iconFromPixmaps(const QVariant &v)
{
    QIcon icon;
    if (!v.canConvert<QDBusArgument>())
        return icon;
    const QDBusArgument arg = v.value<QDBusArgument>();
    if (arg.currentType() != QDBusArgument::ArrayType)
        return icon;
    arg.beginArray();
    while (!arg.atEnd()) {
        int w = 0, h = 0;
        QByteArray data;
        arg.beginStructure();
        arg >> w >> h >> data;
        arg.endStructure();
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || data.size() < w * h * 4)
            continue;
        QImage img(w, h, QImage::Format_ARGB32);
        const uchar *src = reinterpret_cast<const uchar *>(data.constData());
        for (int y = 0; y < h; ++y) {
            auto *line = reinterpret_cast<quint32 *>(img.scanLine(y));
            for (int x = 0; x < w; ++x)
                line[x] = qFromBigEndian<quint32>(src + 4 * (y * w + x));
        }
        icon.addPixmap(QPixmap::fromImage(img));
    }
    arg.endArray();
    return icon;
}

// Значок по имени: свой каталог программы (IconThemePath), тема, /usr/share/pixmaps
static QIcon iconFromName(const QString &name, const QString &themePath)
{
    if (name.isEmpty())
        return {};
    if (name.startsWith('/') && QFileInfo::exists(name))
        return QIcon(name);
    if (!themePath.isEmpty() && QDir(themePath).exists()) {
        QIcon icon;
        QDirIterator it(themePath, {name + QStringLiteral(".png"), name + QStringLiteral(".svg"),
                                    name + QStringLiteral(".svgz"), name + QStringLiteral(".xpm")},
                        QDir::Files, QDirIterator::Subdirectories);
        int n = 0;
        while (it.hasNext() && n++ < 16)
            icon.addFile(it.next());
        if (!icon.isNull())
            return icon;
    }
    QIcon icon = QIcon::fromTheme(name);
    if (!icon.isNull())
        return icon;
    for (const char *ext : {".png", ".svg", ".xpm"}) {
        const QString p = QStringLiteral("/usr/share/pixmaps/") + name + QLatin1String(ext);
        if (QFileInfo::exists(p))
            return QIcon(p);
    }
    return {};
}

// Значок-заглушка: первая буква названия в круге
static QIcon letterIcon(const QString &title)
{
    QIcon icon;
    for (qreal dpr : {1.0, 2.0}) {
        QPixmap pm(QSize(16, 16) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xe8, 0xe8, 0xec));
        p.drawEllipse(QRectF(1, 1, 14, 14));
        QFont f;
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(24, 25, 30));
        p.drawText(QRectF(0, 0, 16, 16), Qt::AlignCenter, title.left(1).toUpper());
        icon.addPixmap(pm);
    }
    return icon;
}

TrayItem::TrayItem(const QString &key, QObject *parent)
    : QObject(parent)
    , m_key(key)
    , m_action(new QAction(this))
    , m_debounce(new QTimer(this))
{
    splitKey(key, &m_service, &m_path);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(80);
    connect(m_debounce, &QTimer::timeout, this, &TrayItem::refresh);

    for (const char *sig : {"NewIcon", "NewAttentionIcon", "NewOverlayIcon", "NewToolTip", "NewTitle", "NewStatus", "NewMenu"})
        for (const char *iface : {"org.kde.StatusNotifierItem", "org.freedesktop.StatusNotifierItem"})
            bus().connect(m_service, m_path, QLatin1String(iface), QLatin1String(sig), this, SLOT(scheduleRefresh()));
    refresh();
}

TrayItem::~TrayItem()
{
    delete m_importer;
}

QMenu *TrayItem::menu() const
{
    return m_importer ? m_importer->rootMenu() : nullptr;
}

void TrayItem::scheduleRefresh()
{
    m_debounce->start();
}

void TrayItem::refresh()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, m_path, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                      QStringLiteral("GetAll"));
    msg << m_iface;
    auto *w = new QDBusPendingCallWatcher(bus().asyncCall(msg, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusMessage r = w->reply();
        QVariantMap props;
        if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty())
            r.arguments().first().value<QDBusArgument>() >> props;
        if (props.isEmpty() && !m_triedOtherIface) {
            // Некоторые программы используют интерфейс freedesktop вместо kde
            m_triedOtherIface = true;
            m_iface = m_iface.startsWith(QLatin1String("org.kde"))
                ? QStringLiteral("org.freedesktop.StatusNotifierItem") : QStringLiteral("org.kde.StatusNotifierItem");
            refresh();
            return;
        }
        if (props.isEmpty()) {
            qWarning().noquote() << "tray: no properties from" << m_key << r.errorMessage();
            return;
        }
        apply(props);
    });
}

void TrayItem::apply(const QVariantMap &p)
{
    m_id = p.value(QStringLiteral("Id")).toString();
    m_title = p.value(QStringLiteral("Title")).toString();
    m_status = p.value(QStringLiteral("Status")).toString();
    m_itemIsMenu = p.value(QStringLiteral("ItemIsMenu")).toBool();
    const QString themePath = p.value(QStringLiteral("IconThemePath")).toString();

    QIcon icon;
    if (m_status == QLatin1String("NeedsAttention")) {
        icon = iconFromName(p.value(QStringLiteral("AttentionIconName")).toString(), themePath);
        if (icon.isNull())
            icon = iconFromPixmaps(p.value(QStringLiteral("AttentionIconPixmap")));
    }
    if (icon.isNull())
        icon = iconFromName(p.value(QStringLiteral("IconName")).toString(), themePath);
    if (icon.isNull())
        icon = iconFromPixmaps(p.value(QStringLiteral("IconPixmap")));
    if (icon.isNull())
        icon = letterIcon(m_title.isEmpty() ? m_id : m_title);
    m_action->setIcon(icon);

    // Подсказка: (s a(iiay) s s) — значок, картинки, заголовок, текст (бывает HTML)
    QString tipTitle, tipText;
    const QVariant tt = p.value(QStringLiteral("ToolTip"));
    if (tt.canConvert<QDBusArgument>()) {
        const QDBusArgument a = tt.value<QDBusArgument>();
        if (a.currentType() == QDBusArgument::StructureType) {
            QString iconName;
            a.beginStructure();
            a >> iconName;
            a.beginArray();
            while (!a.atEnd()) {
                int w, h;
                QByteArray data;
                a.beginStructure();
                a >> w >> h >> data;
                a.endStructure();
            }
            a.endArray();
            a >> tipTitle >> tipText;
            a.endStructure();
        }
    }
    if (tipTitle.isEmpty())
        tipTitle = m_title.isEmpty() ? m_id : m_title;
    if (!tipText.isEmpty() && tipText != tipTitle) {
        QString text = tipText;
        if (!Qt::mightBeRichText(text))
            text = text.toHtmlEscaped().replace('\n', QLatin1String("<br>"));
        m_action->setToolTip(QStringLiteral("<b>%1</b><br>%2").arg(tipTitle.toHtmlEscaped(), text));
    } else {
        m_action->setToolTip(tipTitle);
    }
    m_action->setText(QString());
    m_action->setIconText(QString());

    const QString menuPath = p.value(QStringLiteral("Menu")).value<QDBusObjectPath>().path();
    if (menuPath != m_menuPath) {
        m_menuPath = menuPath;
        delete m_importer;
        m_action->setMenu(static_cast<QMenu *>(nullptr));
        if (!menuPath.isEmpty() && menuPath != QLatin1String("/")) {
            m_importer = new DBusMenuImporter(m_service, menuPath);
            m_importer->prepareRootOnShow();
            m_importer->refresh();
            // Меню показывает QMenuBar панели: пункт со значком + выпадающее меню
            m_action->setMenu(m_importer->rootMenu());
        }
    }
    if (!m_ready)
        qInfo().noquote() << "tray item:" << m_id << "|" << m_title << "|" << m_key;
    m_ready = true;
    Q_EMIT changed();
}

void TrayItem::call(const QString &method, const QVariantList &args, bool reportFailure)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, m_path, m_iface, method);
    msg.setArguments(args);
    if (!reportFailure) {
        bus().call(msg, QDBus::NoBlock);
        return;
    }
    auto *w = new QDBusPendingCallWatcher(bus().asyncCall(msg, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError()) {
            const QString err = w->error().name();
            // Нет такого метода — программа ждёт, что по щелчку откроется меню
            if (err == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod")
                || err == QLatin1String("org.freedesktop.DBus.Error.NotSupported"))
                Q_EMIT activateFailed();
        }
    });
}

void TrayItem::activate(const QPoint &pos)
{
    call(QStringLiteral("Activate"), {pos.x(), pos.y()}, true);
}

void TrayItem::secondaryActivate(const QPoint &pos)
{
    call(QStringLiteral("SecondaryActivate"), {pos.x(), pos.y()});
}

void TrayItem::contextMenu(const QPoint &pos)
{
    call(QStringLiteral("ContextMenu"), {pos.x(), pos.y()});
}

void TrayItem::scroll(int delta, Qt::Orientation o)
{
    call(QStringLiteral("Scroll"), {delta, o == Qt::Horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical")});
}

// ---------------- хост ----------------

SystemTray::SystemTray(QObject *parent)
    : QObject(parent)
    , m_changed(new QTimer(this))
{
    m_changed->setSingleShot(true);
    m_changed->setInterval(30);
    connect(m_changed, &QTimer::timeout, this, &SystemTray::changed);

    if (panelSetting(QStringLiteral("tray/show"), QStringLiteral("auto")).toString() == QLatin1String("never"))
        return;

    m_hostName = QStringLiteral("org.kde.StatusNotifierHost-%1").arg(getpid());
    bus().interface()->registerService(m_hostName, QDBusConnectionInterface::DontQueueService);

    // Чужой справочник исчез (например, закрыли другую панель) — занять его место
    auto *ownerWatch = new QDBusServiceWatcher(QLatin1String(kWatcherService), bus(),
                                               QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(ownerWatch, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &newOwner) {
                if (m_watcher)
                    return;
                if (newOwner.isEmpty())
                    QTimer::singleShot(200, this, &SystemTray::becomeWatcherOrClient);
                else
                    becomeWatcherOrClient();
            });
    becomeWatcherOrClient();
}

void SystemTray::becomeWatcherOrClient()
{
    if (m_watcher)
        return;
    QDBusConnectionInterface *iface = bus().interface();
    if (!iface->isServiceRegistered(QLatin1String(kWatcherService))) {
        auto *w = new StatusNotifierWatcher(this);
        if (bus().registerObject(QLatin1String(kWatcherPath), w,
                                 QDBusConnection::ExportScriptableContents | QDBusConnection::ExportAllProperties)
            && iface->registerService(QLatin1String(kWatcherService), QDBusConnectionInterface::DontQueueService,
                                      QDBusConnectionInterface::DontAllowReplacement).value()
                   == QDBusConnectionInterface::ServiceRegistered) {
            m_watcher = w;
            connect(w, &StatusNotifierWatcher::StatusNotifierItemRegistered, this, &SystemTray::addItem);
            connect(w, &StatusNotifierWatcher::StatusNotifierItemUnregistered, this, &SystemTray::removeItem);
            w->RegisterStatusNotifierHost(m_hostName);
            qInfo() << "tray: StatusNotifierWatcher started";
            return;
        }
        bus().unregisterObject(QLatin1String(kWatcherPath));
        delete w;
    }
    // Справочник уже есть (waybar, другая панель) — работаем только хостом
    static bool connected = false;
    if (!connected) {
        connected = true;
        bus().connect(QLatin1String(kWatcherService), QLatin1String(kWatcherPath), QLatin1String(kWatcherIface),
                      QStringLiteral("StatusNotifierItemRegistered"), this, SLOT(onItemRegistered(QString)));
        bus().connect(QLatin1String(kWatcherService), QLatin1String(kWatcherPath), QLatin1String(kWatcherIface),
                      QStringLiteral("StatusNotifierItemUnregistered"), this, SLOT(onItemUnregistered(QString)));
    }
    registerHost();
    loadItemsFromWatcher();
    qInfo() << "tray: using existing StatusNotifierWatcher";
}

void SystemTray::registerHost()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(kWatcherService), QLatin1String(kWatcherPath),
                                                      QLatin1String(kWatcherIface), QStringLiteral("RegisterStatusNotifierHost"));
    msg << m_hostName;
    bus().call(msg, QDBus::NoBlock);
}

void SystemTray::loadItemsFromWatcher()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(kWatcherService), QLatin1String(kWatcherPath),
                                                      QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    msg << QLatin1String(kWatcherIface) << QStringLiteral("RegisteredStatusNotifierItems");
    auto *w = new QDBusPendingCallWatcher(bus().asyncCall(msg, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusMessage r = w->reply();
        if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
            return;
        const QStringList keys = r.arguments().first().value<QDBusVariant>().variant().toStringList();
        for (const QString &k : keys)
            addItem(k);
    });
}

void SystemTray::addItem(const QString &key)
{
    for (TrayItem *i : std::as_const(m_items))
        if (i->key() == key)
            return;
    auto *item = new TrayItem(key, this);
    m_items << item;
    connect(item, &TrayItem::changed, m_changed, qOverload<>(&QTimer::start));
    connect(item, &TrayItem::activateFailed, this, [this, item] {
        if (item->menu())
            Q_EMIT menuRequested(item->action());
    });
}

void SystemTray::removeItem(const QString &key)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i)->key() == key) {
            m_items.takeAt(i)->deleteLater();
            m_changed->start();
            return;
        }
    }
}

QList<TrayItem *> SystemTray::visibleItems() const
{
    const QStringList hidden = panelSetting(QStringLiteral("tray/hidden"), QString()).toString().split(',', Qt::SkipEmptyParts);
    const bool showPassive = panelSetting(QStringLiteral("tray/showPassive"), false).toBool();
    QList<TrayItem *> out;
    for (TrayItem *i : m_items) {
        if (!i->isReady() || (i->isPassive() && !showPassive))
            continue;
        bool skip = false;
        for (const QString &h : hidden)
            if (h.trimmed().compare(i->id(), Qt::CaseInsensitive) == 0
                || h.trimmed().compare(i->title(), Qt::CaseInsensitive) == 0)
                skip = true;
        if (!skip)
            out << i;
    }
    return out;
}

TrayItem *SystemTray::itemFor(QAction *action) const
{
    for (TrayItem *i : m_items)
        if (i->action() == action)
            return i;
    return nullptr;
}
