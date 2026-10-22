#include "dbusmenuimporter.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDebug>
#include <QIcon>
#include <QKeySequence>
#include <QMenu>
#include <QPixmap>
#include <QTimer>

static const char *kIface = "com.canonical.dbusmenu";

// ---------- (ia{sv}av) ----------

QDBusArgument &operator<<(QDBusArgument &arg, const DBusMenuLayoutItem &item)
{
    arg.beginStructure();
    arg << item.id << item.properties;
    arg.beginArray(qMetaTypeId<QDBusVariant>());
    for (const DBusMenuLayoutItem &c : item.children)
        arg << QDBusVariant(QVariant::fromValue(c));
    arg.endArray();
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, DBusMenuLayoutItem &item)
{
    arg.beginStructure();
    arg >> item.id >> item.properties;
    arg.beginArray();
    item.children.clear();
    while (!arg.atEnd()) {
        QDBusVariant v;
        arg >> v;
        const QDBusArgument childArg = v.variant().value<QDBusArgument>();
        DBusMenuLayoutItem child;
        childArg >> child;
        item.children.append(child);
    }
    arg.endArray();
    arg.endStructure();
    return arg;
}

// ---------- helpers ----------

// dbusmenu: "_" — мнемоника, "__" — литерал. Qt: "&" — мнемоника, "&&" — литерал.
static QString convertLabel(const QString &in)
{
    QString out;
    out.reserve(in.size() + 2);
    for (int i = 0; i < in.size(); ++i) {
        const QChar c = in.at(i);
        if (c == '_') {
            if (i + 1 < in.size() && in.at(i + 1) == '_') {
                out += '_';
                ++i;
            } else {
                out += '&';
            }
        } else if (c == '&') {
            out += QLatin1String("&&");
        } else {
            out += c;
        }
    }
    return out;
}

static QKeySequence convertShortcut(const QVariant &v)
{
    QList<QStringList> seqs;
    if (v.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = v.value<QDBusArgument>();
        arg >> seqs;
    }
    if (seqs.isEmpty())
        return {};
    QStringList parts;
    for (const QString &t : seqs.first()) {
        if (t == QLatin1String("Control")) parts << QStringLiteral("Ctrl");
        else if (t == QLatin1String("Super")) parts << QStringLiteral("Meta");
        else parts << t;
    }
    return QKeySequence::fromString(parts.join('+'), QKeySequence::PortableText);
}

// ---------- importer ----------

DBusMenuImporter::DBusMenuImporter(const QString &service, const QString &path, QObject *parent)
    : MenuImporter(service, path, parent)
    , m_root(new QMenu)
{
    qDBusRegisterMetaType<DBusMenuLayoutItem>();
    m_menus.insert(0, m_root);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(50);
    connect(m_refreshTimer, &QTimer::timeout, this, &DBusMenuImporter::refresh);

    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(m_service, m_path, kIface, QStringLiteral("LayoutUpdated"),
                this, SLOT(onLayoutUpdated(uint,int)));
    bus.connect(m_service, m_path, kIface, QStringLiteral("ItemsPropertiesUpdated"),
                this, SLOT(onItemsPropertiesUpdated(QDBusMessage)));
}

DBusMenuImporter::~DBusMenuImporter()
{
    clearMenu(m_root);
    delete m_root;
}

QDBusMessage DBusMenuImporter::call(const QString &method) const
{
    return QDBusMessage::createMethodCall(m_service, m_path, kIface, method);
}

void DBusMenuImporter::refresh()
{
    QDBusMessage msg = call(QStringLiteral("GetLayout"));
    msg << 0 << -1 << QStringList();
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg, 2000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusMessage reply = w->reply();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            qWarning().noquote() << "GetLayout" << m_service << m_path << reply.errorMessage();
            Q_EMIT invalid();
            return;
        }
        // Не перестраиваем меню, пока пользователь в нём — иначе оно закроется под курсором.
        if (QApplication::activePopupWidget()) {
            m_refreshTimer->start(250);
            return;
        }
        if (reply.arguments().size() < 2)
            return;
        DBusMenuLayoutItem root;
        reply.arguments().at(1).value<QDBusArgument>() >> root;
        populate(m_root, root);
        m_dirty.clear();
        Q_EMIT menuUpdated();
    });
}

bool DBusMenuImporter::fetchSync(int parentId, DBusMenuLayoutItem &out, int timeoutMs)
{
    QDBusMessage msg = call(QStringLiteral("GetLayout"));
    msg << parentId << -1 << QStringList();
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, timeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2)
        return false;
    reply.arguments().at(1).value<QDBusArgument>() >> out;
    return true;
}

void DBusMenuImporter::clearMenu(QMenu *menu)
{
    const QList<QPointer<QObject>> owned = m_owned.take(menu);
    for (const QPointer<QObject> &obj : owned) {
        if (!obj)
            continue;
        if (auto *sub = qobject_cast<QMenu *>(obj.data()))
            clearMenu(sub);
        delete obj.data();
    }
    for (auto it = m_actions.begin(); it != m_actions.end();)
        it = it.value() ? std::next(it) : m_actions.erase(it);
    for (auto it = m_menus.begin(); it != m_menus.end();)
        it = it.value() ? std::next(it) : m_menus.erase(it);
    for (auto it = m_props.begin(); it != m_props.end();)
        it = m_actions.contains(it.key()) ? std::next(it) : m_props.erase(it);
}

void DBusMenuImporter::populate(QMenu *menu, const DBusMenuLayoutItem &item)
{
    clearMenu(menu);
    for (const DBusMenuLayoutItem &child : item.children)
        createAction(menu, child);
}

QAction *DBusMenuImporter::createAction(QMenu *into, const DBusMenuLayoutItem &child)
{
    const QVariantMap &p = child.properties;
    const int id = child.id;
    QAction *action = nullptr;

    if (p.value(QStringLiteral("type")).toString() == QLatin1String("separator")) {
        action = into->addSeparator();
        m_owned[into] << action;
    } else if (p.value(QStringLiteral("children-display")).toString() == QLatin1String("submenu")
               || !child.children.isEmpty()) {
        auto *sub = new QMenu;
        m_menus.insert(id, sub);
        populate(sub, child);
        action = sub->menuAction();
        into->addAction(action);
        m_owned[into] << sub;
        connect(sub, &QMenu::aboutToShow, this, [this, sub, id] { prepareSubmenu(sub, id); });
        connect(sub, &QMenu::aboutToHide, this, [this, id] { sendEvent(id, QStringLiteral("closed")); });
    } else {
        action = new QAction(into);
        into->addAction(action);
        m_owned[into] << action;
        connect(action, &QAction::triggered, this, [this, id] { sendEvent(id, QStringLiteral("clicked")); });

        const QString toggle = p.value(QStringLiteral("toggle-type")).toString();
        if (toggle == QLatin1String("radio")) {
            auto *group = into->findChild<QActionGroup *>(QStringLiteral("dbusmenu-radio"), Qt::FindDirectChildrenOnly);
            if (!group) {
                group = new QActionGroup(into);
                group->setObjectName(QStringLiteral("dbusmenu-radio"));
                group->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
            }
            action->setActionGroup(group);
        }
    }

    m_actions.insert(id, action);
    m_props.insert(id, p);
    applyProperties(action, p);
    return action;
}

void DBusMenuImporter::applyProperties(QAction *action, const QVariantMap &p)
{
    if (action->isSeparator()) {
        action->setVisible(p.value(QStringLiteral("visible"), true).toBool());
        return;
    }
    action->setText(convertLabel(p.value(QStringLiteral("label")).toString()));
    action->setEnabled(p.value(QStringLiteral("enabled"), true).toBool());
    action->setVisible(p.value(QStringLiteral("visible"), true).toBool());

    QIcon icon;
    const QByteArray iconData = p.value(QStringLiteral("icon-data")).toByteArray();
    if (!iconData.isEmpty()) {
        QPixmap pm;
        if (pm.loadFromData(iconData))
            icon = QIcon(pm);
    }
    const QString iconName = p.value(QStringLiteral("icon-name")).toString();
    if (icon.isNull() && !iconName.isEmpty())
        icon = QIcon::fromTheme(iconName);
    action->setIcon(icon);

    const QString toggle = p.value(QStringLiteral("toggle-type")).toString();
    const bool checkable = toggle == QLatin1String("checkmark") || toggle == QLatin1String("radio");
    action->setCheckable(checkable);
    if (checkable)
        action->setChecked(p.value(QStringLiteral("toggle-state")).toInt() == 1);

    // Ярлык только показываем: в меню он сработает, глобально — нет (это забота приложения).
    action->setShortcut(convertShortcut(p.value(QStringLiteral("shortcut"))));
    action->setShortcutContext(Qt::WidgetShortcut);
}

void DBusMenuImporter::prepareRootOnShow()
{
    connect(m_root, &QMenu::aboutToShow, this, [this] { prepareSubmenu(m_root, 0); });
}

void DBusMenuImporter::prepareSubmenu(QMenu *menu, int id)
{
    // Ленивые реализации (libdbusmenu, Electron, Chromium) наполняют подменю только после AboutToShow.
    QDBusMessage msg = call(QStringLiteral("AboutToShow"));
    msg << id;
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 200);
    const bool needUpdate = reply.type() == QDBusMessage::ReplyMessage
                            && !reply.arguments().isEmpty() && reply.arguments().first().toBool();

    if (needUpdate || m_dirty.contains(id) || menu->actions().isEmpty()) {
        DBusMenuLayoutItem item;
        if (fetchSync(id, item, 400))
            populate(menu, item);
        m_dirty.remove(id);
    }
    sendEvent(id, QStringLiteral("opened"));
}

void DBusMenuImporter::sendEvent(int id, const QString &event)
{
    QDBusMessage msg = call(QStringLiteral("Event"));
    msg << id << event << QVariant::fromValue(QDBusVariant(QVariant(0))) << uint(0);
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
}

void DBusMenuImporter::onLayoutUpdated(uint, int parentId)
{
    QMenu *menu = m_menus.value(parentId);
    if (parentId == 0 || !menu) {
        m_refreshTimer->start();
        return;
    }
    if (menu->isVisible()) {
        DBusMenuLayoutItem item;
        if (fetchSync(parentId, item, 300)) {
            populate(menu, item);
            return;
        }
    }
    m_dirty.insert(parentId); // перечитаем при следующем открытии
}

void DBusMenuImporter::onItemsPropertiesUpdated(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2)
        return;

    const QDBusArgument updated = msg.arguments().at(0).value<QDBusArgument>();
    updated.beginArray();
    while (!updated.atEnd()) {
        int id = 0;
        QVariantMap props;
        updated.beginStructure();
        updated >> id >> props;
        updated.endStructure();
        QVariantMap &stored = m_props[id];
        for (auto it = props.cbegin(); it != props.cend(); ++it)
            stored.insert(it.key(), it.value());
        if (QAction *a = m_actions.value(id))
            applyProperties(a, stored);
    }
    updated.endArray();

    const QDBusArgument removed = msg.arguments().at(1).value<QDBusArgument>();
    removed.beginArray();
    while (!removed.atEnd()) {
        int id = 0;
        QStringList keys;
        removed.beginStructure();
        removed >> id >> keys;
        removed.endStructure();
        QVariantMap &stored = m_props[id];
        for (const QString &k : keys)
            stored.remove(k);
        if (QAction *a = m_actions.value(id))
            applyProperties(a, stored);
    }
    removed.endArray();
}
