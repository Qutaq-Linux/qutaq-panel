#include "gmenuimporter.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusVariant>
#include <QDebug>
#include <QKeySequence>
#include <QMenu>
#include <QRegularExpression>
#include <QTimer>

static const char *kMenus = "org.gtk.Menus";
static const char *kActions = "org.gtk.Actions";

// ---------- helpers ----------

static QVariant unwrap(QVariant v)
{
    while (v.userType() == qMetaTypeId<QDBusVariant>())
        v = v.value<QDBusVariant>().variant();
    return v;
}

static bool toKey(const QVariant &v, QPair<uint, uint> &key)
{
    if (!v.canConvert<QDBusArgument>())
        return false;
    const QDBusArgument arg = v.value<QDBusArgument>();
    arg.beginStructure();
    arg >> key.first >> key.second;
    arg.endStructure();
    return true;
}

// GTK: "_" — мнемоника, "__" — литерал. Qt: "&" — мнемоника, "&&" — литерал.
static QString convertLabel(const QString &raw)
{
    const QString in = raw.trimmed();
    QString out;
    for (int i = 0; i < in.size(); ++i) {
        const QChar c = in.at(i);
        if (c == '_') {
            if (i + 1 < in.size() && in.at(i + 1) == '_') { out += '_'; ++i; }
            else out += '&';
        } else if (c == '&') {
            out += QLatin1String("&&");
        } else {
            out += c;
        }
    }
    return out;
}

// "<Primary><Shift>s" → Ctrl+Shift+S
static QKeySequence convertAccel(const QString &accel)
{
    if (accel.isEmpty())
        return {};
    static const QRegularExpression modRe(QStringLiteral("<([^>]+)>"));
    QStringList parts;
    auto it = modRe.globalMatch(accel);
    int end = 0;
    while (it.hasNext()) {
        const auto m = it.next();
        const QString mod = m.captured(1).toLower();
        if (mod == QLatin1String("primary") || mod == QLatin1String("control") || mod == QLatin1String("ctrl"))
            parts << QStringLiteral("Ctrl");
        else if (mod == QLatin1String("shift")) parts << QStringLiteral("Shift");
        else if (mod == QLatin1String("alt") || mod == QLatin1String("mod1")) parts << QStringLiteral("Alt");
        else if (mod == QLatin1String("super") || mod == QLatin1String("meta")) parts << QStringLiteral("Meta");
        end = m.capturedEnd();
    }
    QString key = accel.mid(end);
    static const QHash<QString, QString> names = {
        {"plus", "+"}, {"minus", "-"}, {"equal", "="}, {"comma", ","}, {"period", "."},
        {"slash", "/"}, {"backslash", "\\"}, {"question", "?"}, {"space", "Space"},
        {"Page_Up", "PgUp"}, {"Page_Down", "PgDown"}, {"Delete", "Del"}, {"Insert", "Ins"},
        {"Escape", "Esc"}, {"BackSpace", "Backspace"}, {"Return", "Return"}, {"Tab", "Tab"},
        {"KP_Add", "+"}, {"KP_Subtract", "-"}, {"KP_0", "0"}, {"bracketleft", "["}, {"bracketright", "]"},
    };
    key = names.value(key, key.size() == 1 ? key.toUpper() : key);
    parts << key;
    return QKeySequence::fromString(parts.join('+'), QKeySequence::PortableText);
}

// ---------- importer ----------

GMenuImporter::GMenuImporter(const QString &service, const QString &menuPath, const QString &winPath,
                             QObject *parent)
    : MenuImporter(service, menuPath, parent)
    , m_root(new QMenu)
    , m_winPath(winPath)
{
    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(50);
    connect(m_rebuildTimer, &QTimer::timeout, this, [this] {
        if (QApplication::activePopupWidget()) { // пользователь в меню — подождём
            m_rebuildTimer->start(250);
            return;
        }
        buildRoot();
        Q_EMIT menuUpdated();
    });

    QDBusConnection::sessionBus().connect(m_service, m_path, kMenus, QStringLiteral("Changed"),
                                          this, SLOT(onMenusChanged(QDBusMessage)));
    setupActionPaths();
}

GMenuImporter::~GMenuImporter()
{
    unsubscribe(m_rootGroups.values());
    clearMenu(m_root);
    delete m_root;
}

void GMenuImporter::setupActionPaths()
{
    m_actionPaths.insert(QStringLiteral("unity"), m_path);

    static const QRegularExpression tailRe(QStringLiteral("/menus/menubar(/\\d+)?$"));
    const auto m = tailRe.match(m_path);
    if (!m.hasMatch())
        return;
    const QString base = m_path.left(m.capturedStart());
    if (base.isEmpty())
        return;
    m_actionPaths.insert(QStringLiteral("app"), base);
    if (!m_winPath.isEmpty()) {
        m_actionPaths.insert(QStringLiteral("win"), m_winPath);
        return;
    }

    // Окна GtkApplication: <база>/window/<N>. Какое из них активно, по шине не узнать —
    // берём самое новое (наибольший N).
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, base + QStringLiteral("/window"),
                                                      QStringLiteral("org.freedesktop.DBus.Introspectable"),
                                                      QStringLiteral("Introspect"));
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 300);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        static const QRegularExpression nodeRe(QStringLiteral("<node name=\"(\\d+)\""));
        int best = -1;
        auto it = nodeRe.globalMatch(reply.arguments().first().toString());
        while (it.hasNext())
            best = qMax(best, it.next().captured(1).toInt());
        if (best >= 0)
            m_actionPaths.insert(QStringLiteral("win"), base + QStringLiteral("/window/%1").arg(best));
    }
}

QString GMenuImporter::actionPath(const QString &fullName, QString *shortName) const
{
    const int dot = fullName.indexOf('.');
    if (dot <= 0)
        return {};
    if (shortName)
        *shortName = fullName.mid(dot + 1);
    return m_actionPaths.value(fullName.left(dot));
}

bool GMenuImporter::subscribe(const QList<uint> &groups)
{
    if (groups.isEmpty())
        return true;
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, m_path, kMenus, QStringLiteral("Start"));
    QList<uint> g = groups;
    msg << QVariant::fromValue(g);
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 500);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        qWarning().noquote() << "org.gtk.Menus.Start" << m_service << m_path << reply.errorMessage();
        return false;
    }
    // Ответ полностью описывает запрошенные группы — старое содержимое выбрасываем.
    for (auto it = m_model.begin(); it != m_model.end();)
        it = groups.contains(it.key().first) ? m_model.erase(it) : std::next(it);

    const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        uint group = 0, menu = 0;
        QList<QVariantMap> items;
        arg.beginStructure();
        arg >> group >> menu;
        arg.beginArray();
        while (!arg.atEnd()) {
            QVariantMap item;
            arg >> item;
            items << item;
        }
        arg.endArray();
        arg.endStructure();
        m_model.insert({group, menu}, items);
    }
    arg.endArray();
    return true;
}

void GMenuImporter::unsubscribe(const QList<uint> &groups)
{
    if (groups.isEmpty())
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, m_path, kMenus, QStringLiteral("End"));
    QList<uint> g = groups;
    msg << QVariant::fromValue(g);
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
}

void GMenuImporter::describeActions()
{
    m_actions.clear();
    for (auto it = m_actionPaths.cbegin(); it != m_actionPaths.cend(); ++it) {
        QDBusMessage msg = QDBusMessage::createMethodCall(m_service, it.value(), kActions,
                                                          QStringLiteral("DescribeAll"));
        const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 300);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            continue;
        const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
        arg.beginMap();
        while (!arg.atEnd()) {
            QString name;
            bool enabled = true;
            QVariantList state;
            arg.beginMapEntry();
            arg >> name;
            arg.beginStructure();
            arg >> enabled;
            // Тип параметра ("g") у действий без параметра пустой; QDBusSignature на нём
            // ругается в лог, поэтому читаем как вариант и не используем.
            (void)arg.asVariant();
            arg >> state;
            arg.endStructure();
            arg.endMapEntry();

            ActionInfo info;
            info.enabled = enabled;
            info.hasState = !state.isEmpty();
            if (info.hasState)
                info.state = unwrap(state.first());
            m_actions.insert(it.key() + '.' + name, info);
        }
        arg.endMap();
    }
}

void GMenuImporter::refresh()
{
    if (!m_rootGroups.isEmpty())
        unsubscribe(m_rootGroups.values());
    m_rootGroups.clear();
    m_model.clear();

    if (!subscribe({0})) {
        QMetaObject::invokeMethod(this, &GMenuImporter::invalid, Qt::QueuedConnection);
        return;
    }
    m_rootGroups.insert(0);
    describeActions();
    buildRoot();
    QMetaObject::invokeMethod(this, &GMenuImporter::menuUpdated, Qt::QueuedConnection);
}

static bool isUnityItem(const QVariantMap &item)
{
    return item.value(QStringLiteral("action")).toString().startsWith(QLatin1String("unity."))
        || item.value(QStringLiteral("submenu-action")).toString().startsWith(QLatin1String("unity."));
}

// Есть ли в секции хоть один пункт не от appmenu-gtk-module
bool GMenuImporter::sectionHasNative(const Key &key) const
{
    for (const QVariantMap &item : m_model.value(key)) {
        Key sub;
        if (toKey(item.value(QStringLiteral(":section")), sub)) {
            if (sectionHasNative(sub))
                return true;
        } else if (!isUnityItem(item)) {
            return true;
        }
    }
    return false;
}

// Секции, целиком состоящие из пунктов модуля (на любой глубине корня)
void GMenuImporter::collectUnitySections(const Key &key, QSet<Key> &out) const
{
    for (const QVariantMap &item : m_model.value(key)) {
        Key sub;
        if (!toKey(item.value(QStringLiteral(":section")), sub))
            continue;
        if (!m_model.value(sub).isEmpty() && !sectionHasNative(sub))
            out.insert(sub);
        else
            collectUnitySections(sub, out);
    }
}

void GMenuImporter::buildRoot()
{
    // Если у GtkApplication своё меню, appmenu-gtk-module вкладывает в его модель ещё и
    // копии строк меню всех окон (действия unity.*) — в панели было бы по набору
    // «Файл, Правка…» на каждое окно. Оставляем родное меню приложения: в копиях модуля
    // искажены состояния пунктов, нет горячих клавиш, и они не различают окна.
    m_skipSections.clear();

    // Секции корня могут лежать в других группах — подгружаем их заранее.
    for (int round = 0; round < 4; ++round) {
        QList<uint> missing;
        for (auto it = m_model.cbegin(); it != m_model.cend(); ++it) {
            if (!m_rootGroups.contains(it.key().first))
                continue;
            for (const QVariantMap &item : it.value()) {
                Key sub;
                if (toKey(item.value(QStringLiteral(":section")), sub) && !m_rootGroups.contains(sub.first)
                    && !missing.contains(sub.first))
                    missing << sub.first;
            }
        }
        if (missing.isEmpty() || !subscribe(missing))
            break;
        for (uint g : missing)
            m_rootGroups.insert(g);
        describeActions();
    }

    if (sectionHasNative({0, 0}))
        collectUnitySections({0, 0}, m_skipSections);

    populate(m_root, {0, 0});
}

void GMenuImporter::clearMenu(QMenu *menu)
{
    const QList<QPointer<QObject>> owned = m_owned.take(menu);
    for (const QPointer<QObject> &obj : owned) {
        if (!obj)
            continue;
        if (auto *sub = qobject_cast<QMenu *>(obj.data())) {
            clearMenu(sub);
            m_submenus.remove(sub);
            m_usedKeys.remove(sub);
        }
        delete obj.data();
    }
    m_usedKeys.remove(menu);
}

void GMenuImporter::populate(QMenu *menu, const Key &key)
{
    clearMenu(menu);
    addItems(menu, key, menu == m_root);
    // Убрать висящий в конце разделитель
    const auto acts = menu->actions();
    if (!acts.isEmpty() && acts.last()->isSeparator())
        acts.last()->setVisible(false);
}

void GMenuImporter::addItems(QMenu *menu, const Key &key, bool isRoot)
{
    m_usedKeys[menu].insert(key);
    if (!m_model.contains(key) && !m_rootGroups.contains(key.first))
        subscribe({key.first});

    for (const QVariantMap &item : m_model.value(key)) {
        Key sub;
        if (toKey(item.value(QStringLiteral(":section")), sub)) {
            if (m_skipSections.contains(sub))
                continue;
            // GTK рисует разделитель между секциями
            const auto acts = menu->actions();
            if (!isRoot && !acts.isEmpty() && !acts.last()->isSeparator()) {
                QAction *sep = menu->addSeparator();
                m_owned[menu] << sep;
            }
            addItems(menu, sub, isRoot);
            continue;
        }
        if (toKey(item.value(QStringLiteral(":submenu")), sub)) {
            createSubmenu(menu, item, sub);
            continue;
        }
        createAction(menu, item);
    }
}

QMenu *GMenuImporter::createSubmenu(QMenu *parent, const QVariantMap &item, const Key &key)
{
    auto *sub = new QMenu;
    sub->setTitle(convertLabel(item.value(QStringLiteral("label")).toString()));
    sub->menuAction()->setVisible(!sub->title().isEmpty()); // у Thunar бывает пустой служебный пункт
    parent->addAction(sub->menuAction());
    m_owned[parent] << sub;
    m_submenus.insert(sub, {key, item.value(QStringLiteral("submenu-action")).toString()});
    connect(sub, &QMenu::aboutToShow, this, [this, sub] { prepareSubmenu(sub); });
    connect(sub, &QMenu::aboutToHide, this, [this, sub] { hideSubmenu(sub); });
    return sub;
}

QAction *GMenuImporter::createAction(QMenu *menu, const QVariantMap &item)
{
    auto *a = new QAction(convertLabel(item.value(QStringLiteral("label")).toString()), menu);
    menu->addAction(a);
    m_owned[menu] << a;

    const QString name = item.value(QStringLiteral("action")).toString();
    const QVariant target = unwrap(item.value(QStringLiteral("target")));
    const QString hiddenWhen = item.value(QStringLiteral("hidden-when")).toString();

    const auto it = m_actions.constFind(name);
    const bool exists = it != m_actions.constEnd();
    const ActionInfo info = exists ? it.value() : ActionInfo{};
    a->setEnabled(exists && info.enabled);
    if (a->text().isEmpty()
        || (hiddenWhen == QLatin1String("action-missing") && !exists)
        || (hiddenWhen == QLatin1String("action-disabled") && !(exists && info.enabled)))
        a->setVisible(false);

    if (exists && info.hasState) {
        if (target.isValid()) {
            // Радио: действие со строковым (или иным) состоянием, пункт отмечен, если state == target
            auto *group = menu->findChild<QActionGroup *>(QStringLiteral("gmenu-radio"), Qt::FindDirectChildrenOnly);
            if (!group) {
                group = new QActionGroup(menu);
                group->setObjectName(QStringLiteral("gmenu-radio"));
                group->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
            }
            a->setCheckable(true);
            a->setActionGroup(group);
            a->setChecked(info.state == target);
        } else if (info.state.userType() == QMetaType::Bool) {
            a->setCheckable(true);
            a->setChecked(info.state.toBool());
        }
    }

    a->setShortcut(convertAccel(item.value(QStringLiteral("accel")).toString()));
    a->setShortcutContext(Qt::WidgetShortcut);

    connect(a, &QAction::triggered, this, [this, name, target] { activate(name, target); });
    return a;
}

void GMenuImporter::activate(const QString &fullName, const QVariant &target)
{
    QString shortName;
    const QString path = actionPath(fullName, &shortName);
    if (path.isEmpty())
        return;
    QVariantList params;
    if (target.isValid())
        params << QVariant::fromValue(QDBusVariant(target));
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, path, kActions, QStringLiteral("Activate"));
    msg << shortName << QVariant::fromValue(params) << QVariantMap();
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
}

void GMenuImporter::setActionState(const QString &fullName, bool state, bool block)
{
    QString shortName;
    const QString path = actionPath(fullName, &shortName);
    if (path.isEmpty())
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, path, kActions, QStringLiteral("SetState"));
    msg << shortName << QVariant::fromValue(QDBusVariant(state)) << QVariantMap();
    QDBusConnection::sessionBus().call(msg, block ? QDBus::Block : QDBus::NoBlock, 300);
}

void GMenuImporter::prepareSubmenu(QMenu *menu)
{
    const SubmenuInfo info = m_submenus.value(menu);
    // appmenu-gtk-module наполняет подменю только после включения submenu-action.
    if (!info.submenuAction.isEmpty())
        setActionState(info.submenuAction, true, true);
    subscribe({info.key.first});
    describeActions();
    populate(menu, info.key);
}

void GMenuImporter::hideSubmenu(QMenu *menu)
{
    const SubmenuInfo info = m_submenus.value(menu);
    if (!info.submenuAction.isEmpty())
        setActionState(info.submenuAction, false, false);
    if (!m_rootGroups.contains(info.key.first))
        unsubscribe({info.key.first});
}

void GMenuImporter::onMenusChanged(const QDBusMessage &msg)
{
    if (msg.arguments().isEmpty())
        return;
    QSet<Key> changed;
    const QDBusArgument arg = msg.arguments().first().value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        uint group = 0, menu = 0, position = 0, removed = 0;
        QList<QVariantMap> added;
        arg.beginStructure();
        arg >> group >> menu >> position >> removed;
        arg.beginArray();
        while (!arg.atEnd()) {
            QVariantMap item;
            arg >> item;
            added << item;
        }
        arg.endArray();
        arg.endStructure();

        QList<QVariantMap> &items = m_model[{group, menu}];
        for (uint i = 0; i < removed && int(position) < items.size(); ++i)
            items.removeAt(position);
        for (int i = 0; i < added.size(); ++i)
            items.insert(qMin<int>(position + i, items.size()), added.at(i));
        changed.insert({group, menu});
    }
    arg.endArray();

    // Корень перестраиваем с задержкой; открытые подменю — сразу.
    for (const Key &k : changed) {
        if (m_usedKeys.value(m_root).contains(k) || k == Key{0, 0})
            m_rebuildTimer->start();
    }
    // populate() удаляет вложенные подменю, поэтому сначала собираем список.
    QList<QPointer<QMenu>> toRepopulate;
    for (auto it = m_submenus.cbegin(); it != m_submenus.cend(); ++it) {
        if (!it.key()->isVisible())
            continue;
        const QSet<Key> used = m_usedKeys.value(it.key());
        for (const Key &k : changed) {
            if (used.contains(k)) {
                toRepopulate << it.key();
                break;
            }
        }
    }
    for (const QPointer<QMenu> &menu : toRepopulate)
        if (menu && m_submenus.contains(menu))
            populate(menu, m_submenus.value(menu).key);
}
