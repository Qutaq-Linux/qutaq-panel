#include "menucontroller.h"

#include "dbusmenuimporter.h"
#include "gmenuimporter.h"
#include "toplevelmanager.h"

#include <QAction>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>
#include <QFile>
#include <QLocale>
#include <QMap>
#include <QMenu>
#include <QStandardPaths>
#include <QTextStream>

#include <wayland-client.h>

QString desktopEntryName(const QString &appId)
{
    if (appId.isEmpty())
        return {};
    QString file = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, appId + ".desktop");
    if (file.isEmpty())
        file = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, appId.toLower() + ".desktop");
    QFile f(file);
    if (file.isEmpty() || !f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    const QString loc = QLocale().name();            // ru_RU
    const QString lang = loc.section('_', 0, 0);     // ru
    QString name, nameLang, nameLoc;
    bool inEntry = false;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (line.startsWith('[')) {
            if (inEntry)
                break;
            inEntry = line == QLatin1String("[Desktop Entry]");
            continue;
        }
        if (!inEntry)
            continue;
        const int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        const QString k = line.left(eq).trimmed();
        const QString v = line.mid(eq + 1).trimmed();
        if (k == QLatin1String("Name")) name = v;
        else if (k == QStringLiteral("Name[%1]").arg(lang)) nameLang = v;
        else if (k == QStringLiteral("Name[%1]").arg(loc)) nameLoc = v;
    }
    return !nameLoc.isEmpty() ? nameLoc : !nameLang.isEmpty() ? nameLang : name;
}

static QString prettyAppId(const QString &appId)
{
    QString s = appId.section('.', -1);   // org.kde.dolphin → dolphin
    if (!s.isEmpty())
        s[0] = s[0].toUpper();
    return s;
}

// How closely a process name resembles the window's app_id (0 = not at all).
static int matchScore(const QString &appIdRaw, const QStringList &names)
{
    const QString appId = appIdRaw.toLower();
    const QString shortId = appId.section('.', -1);
    int best = 0;
    for (const QString &raw : names) {
        const QString n = raw.toLower();
        const QString nShort = n.section('.', -1);
        if (n == appId) best = qMax(best, 3);                         // exact match / .desktop
        else if (n == shortId || nShort == shortId) best = qMax(best, 2);
        else if (!shortId.isEmpty()
                 && (n.startsWith(shortId + '-') || n.startsWith(shortId + '_')
                     || shortId.startsWith(n + '-')))                  // firefox ↔ firefox-bin
            best = qMax(best, 1);
    }
    return best;
}

MenuController::MenuController(QObject *parent)
    : QObject(parent)
    , m_registrar(new AppMenuRegistrar(this))
    , m_toplevels(new ToplevelManager)
    , m_windowMenu(new QMenu)
{
    m_toplevels->setParent(this);

    connect(m_toplevels, &ToplevelManager::activeToplevelChanged, this, &MenuController::reevaluate);
    connect(m_toplevels, &ToplevelManager::activeToplevelChanged, this, &MenuController::activeWindowChanged);
    connect(m_registrar, &AppMenuRegistrar::entriesChanged, this, &MenuController::reevaluate);
    // The protocol became (un)available, e.g. the compositor announced the global after us.
    connect(m_toplevels, &QWaylandClientExtension::activeChanged, this, &MenuController::reevaluate);
    // Items depend on the window state, so build them right before showing.
    connect(m_windowMenu, &QMenu::aboutToShow, this, &MenuController::rebuildWindowMenu);
    rebuildWindowMenu();

    // A window is often activated before the application has published its menu, so search again.
    m_rescanTimer = new QTimer(this);
    m_rescanTimer->setSingleShot(true);
    m_rescanTimer->setInterval(500);
    connect(m_rescanTimer, &QTimer::timeout, this, [this] {
        m_scanAppId.clear(); // reset the search cache
        reevaluate();
    });

    m_pairTimer = new QTimer(this);
    m_pairTimer->setSingleShot(true);
    m_pairTimer->setInterval(300);
    connect(m_pairTimer, &QTimer::timeout, this, &MenuController::pairPending);
    connect(m_toplevels, &ToplevelManager::toplevelAdded, this, &MenuController::onToplevelAdded);
    connect(m_toplevels, &ToplevelManager::toplevelRemoved, this, &MenuController::onToplevelRemoved);
}

// ---------------------------------------------------------------------------
// Pairing windows with menu objects.
//
// Wayland doesn't tell which bus object belongs to which window (GNOME and KDE have their own
// protocols for that, wlroots compositors don't). But window object numbers only grow:
// Qt — /MenuBar/<N>, appmenu-gtk-module — .../menubar/<N>, GtkApplication — .../window/<N>.
// So when a window appears, its object is the newest one not yet taken. Windows opened
// before the panel started are paired in order. Dialogs (windows with a parent) are skipped.

static int busOrder(const QString &service)
{
    return service.section('.', 1).toInt(); // ":1.42" → 42, names are handed out in increasing order
}

static bool entryLess(const MenuEntry &a, const MenuEntry &b)
{
    const int sa = busOrder(a.service), sb = busOrder(b.service);
    return sa != sb ? sa < sb : a.serial < b.serial;
}

void MenuController::onToplevelAdded(Toplevel *t)
{
    qInfo().noquote() << "window opened:" << t->title() << "| app_id =" << t->appId()
                      << (t->hasParent() ? "| dialog" : "") << (t->isInitial() ? "| existing" : "");
    if (t->hasParent())
        return;
    m_pending << t;
    m_pairAttempts = 0;
    m_scanAppId.clear();
    m_pairTimer->start();
}

void MenuController::onToplevelRemoved(Toplevel *t)
{
    m_pairs.remove(t);
    m_pending.removeAll(t);
    m_scanAppId.clear();
}

QSet<QString> MenuController::pairedKeys(Toplevel *except) const
{
    QSet<QString> keys;
    for (auto it = m_pairs.cbegin(); it != m_pairs.cend(); ++it)
        if (it.key() != except)
            keys.insert(it.value());
    return keys;
}

void MenuController::pairPending()
{
    m_pending.removeAll(nullptr);
    if (m_pending.isEmpty() || !hasToplevelTracking())
        return;
    ++m_pairAttempts;
    const bool lastAttempt = m_pairAttempts >= 10;

    QMap<QString, QList<Toplevel *>> byApp;
    for (const QPointer<Toplevel> &t : std::as_const(m_pending))
        byApp[t->appId()] << t;

    bool changedActive = false;
    for (auto it = byApp.cbegin(); it != byApp.cend(); ++it) {
        const QList<MenuEntry> found = scanBus(it.key(), true);

        // Pairs whose object disappeared (Qt recreates /MenuBar when a window is shown) go back to the queue.
        QSet<QString> alive;
        for (const MenuEntry &e : found)
            alive.insert(e.key());
        for (auto p = m_pairs.begin(); p != m_pairs.end();) {
            if (p.key()->appId() == it.key() && !alive.contains(p.value())) {
                if (!m_pending.contains(p.key()))
                    m_pending << p.key();
                p = m_pairs.erase(p);
            } else {
                ++p;
            }
        }

        const QSet<QString> taken = pairedKeys();
        QList<MenuEntry> free;
        QSet<QString> seen;
        for (const MenuEntry &e : found)
            if (!taken.contains(e.key()) && !seen.contains(e.key())) {
                seen.insert(e.key());
                free << e;
            }
        std::sort(free.begin(), free.end(), entryLess);

        QList<Toplevel *> fresh, existing;
        for (const QPointer<Toplevel> &t : std::as_const(m_pending))
            if (t && t->appId() == it.key())
                (t->isInitial() ? existing : fresh) << t;
        auto bySeq = [](Toplevel *a, Toplevel *b) { return a->sequence() < b->sequence(); };
        std::sort(fresh.begin(), fresh.end(), bySeq);
        // wlroots sends already open windows from newest to oldest, so reverse them.
        std::sort(existing.begin(), existing.end(), [](Toplevel *a, Toplevel *b) {
            return a->sequence() > b->sequence();
        });

        auto pair = [&](Toplevel *t, const MenuEntry &e) {
            m_pairs.insert(t, e.key());
            m_pending.removeAll(t);
            qInfo().noquote() << "window" << t->title() << "<->" << e.service
                              << (e.winPath.isEmpty() ? e.path : e.winPath);
            if (t == m_toplevels->active())
                changedActive = true;
        };

        // New windows get the newest objects. If there are fewer objects than windows yet, wait:
        // the application may not have published its menu yet.
        if (!fresh.isEmpty() && (free.size() >= fresh.size() || lastAttempt)) {
            const int k = qMin(fresh.size(), free.size());
            const QList<MenuEntry> newest = free.mid(free.size() - k);
            for (int i = 0; i < k; ++i)
                pair(fresh.at(i), newest.at(i));
            free = free.mid(0, free.size() - k);
        }
        // Already open windows get the remaining ones in order.
        for (int i = 0; i < existing.size() && i < free.size(); ++i)
            pair(existing.at(i), free.at(i));
    }

    if (lastAttempt)
        m_pending.clear(); // the rest fall back to the default choice (the newest menu)
    else if (!m_pending.isEmpty())
        m_pairTimer->start();

    if (changedActive) {
        m_candidateKeys.clear(); // make reevaluate() recreate the importer
        reevaluate();
    }
}

// Walks the object tree of one bus connection and collects menus of both kinds:
//  - com.canonical.dbusmenu: Qt publishes it as /MenuBar/<N>;
//  - org.gtk.Menus at .../menus/menubar[/N]: GtkApplication and appmenu-gtk-module.
QList<MenuEntry> MenuController::findMenusOf(const QString &service, bool *ok)
{
    if (ok)
        *ok = true;
    static const QRegularExpression ifaceRe(QStringLiteral("<interface name=\"([^\"]+)\""));
    static const QRegularExpression childRe(QStringLiteral("<node name=\"([^\"/][^\"]*)\""));
    static const QRegularExpression tailNumRe(QStringLiteral("/(\\d+)$"));
    // Large subtrees that are certainly empty for us
    static const QStringList skip = {QStringLiteral("/org/gtk/vfs"), QStringLiteral("/org/freedesktop"),
                                     QStringLiteral("/org/a11y"), QStringLiteral("/org/gtk/Profiler")};

    static const QRegularExpression appWindowRe(QStringLiteral("^(.+)/window/(\\d+)$"));

    QDBusConnection bus = QDBusConnection::sessionBus();
    QList<MenuEntry> out;
    QHash<QString, QList<int>> appWindows; // GtkApplication base → numbers of its windows
    QStringList queue{QStringLiteral("/")};
    int visited = 0;
    while (!queue.isEmpty() && visited < 200) {
        const QString path = queue.takeFirst();
        if (skip.contains(path))
            continue;
        ++visited;
        QDBusMessage msg = QDBusMessage::createMethodCall(service, path,
                                                          QStringLiteral("org.freedesktop.DBus.Introspectable"),
                                                          QStringLiteral("Introspect"));
        const QDBusMessage reply = bus.call(msg, QDBus::Block, 250);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            if (path == QLatin1String("/")) {
                if (ok)
                    *ok = false;
                // GDBus's second connection never answers: stop asking after two misses.
                // A connection that answered at least once was just busy (e.g. opening a window).
                if (reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.NoReply")
                    && !m_answeredServices.contains(service) && ++m_silentStrikes[service] >= 2)
                    m_deadServices.insert(service);
                return {};
            }
            continue;
        }
        if (path == QLatin1String("/"))
            m_answeredServices.insert(service);
        const QString xml = reply.arguments().first().toString();

        QStringList ifaces;
        for (auto m = ifaceRe.globalMatch(xml); m.hasNext();)
            ifaces << m.next().captured(1);
        const auto num = tailNumRe.match(path);
        const int n = num.hasMatch() ? num.captured(1).toInt() : 0;

        if (ifaces.contains(QLatin1String("com.canonical.dbusmenu"))) {
            MenuEntry e;
            e.service = service;
            e.path = path;
            e.serial = n; // a larger number means a newer window
            out << e;
        } else if (ifaces.contains(QLatin1String("org.gtk.Menus")) && path.contains(QLatin1String("/menus/menubar"))) {
            MenuEntry e;
            e.kind = MenuEntry::GMenu;
            e.service = service;
            e.path = path;
            // A GtkApplication's own menu is preferred over the module's per-window copies
            e.serial = path.endsWith(QLatin1String("/menus/menubar")) ? 1000 : 500 + n;
            out << e;
        } else if (ifaces.contains(QLatin1String("org.gtk.Actions"))) {
            const auto w = appWindowRe.match(path);
            if (w.hasMatch())
                appWindows[w.captured(1)] << w.captured(2).toInt();
        }

        const QString prefix = path == QLatin1String("/") ? QString() : path;
        for (auto m = childRe.globalMatch(xml); m.hasNext();)
            queue << prefix + '/' + m.next().captured(1);
    }

    // A GtkApplication has one menu for the whole application, but win.* actions belong to
    // each window (<base>/window/<N>). Split such a menu into per-window entries.
    QList<MenuEntry> expanded;
    for (const MenuEntry &e : std::as_const(out)) {
        const QString suffix = QStringLiteral("/menus/menubar");
        const QList<int> wins = e.kind == MenuEntry::GMenu && e.path.endsWith(suffix)
                                    ? appWindows.value(e.path.chopped(suffix.size()))
                                    : QList<int>();
        if (wins.isEmpty()) {
            expanded << e;
            continue;
        }
        for (int w : wins) {
            MenuEntry we = e;
            we.winPath = e.path.chopped(suffix.size()) + QStringLiteral("/window/%1").arg(w);
            we.serial = 1000 + w;
            expanded << we;
        }
    }
    return expanded;
}

// Qt >= 6.9 doesn't call RegisterWindow on Wayland (only on X11), and the GTK module
// cannot pass the menu address without a GNOME protocol. They still publish the menus,
// so find them among the processes of the active window.
QList<MenuEntry> MenuController::scanBus(const QString &appId, bool force)
{
    if (appId == m_scanAppId && !force)
        return m_scanResult;
    m_scanAppId = appId;
    const QList<MenuEntry> previous = m_scanResult;
    m_scanResult.clear();
    if (appId.isEmpty())
        return m_scanResult;

    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusConnectionInterface *iface = bus.interface();
    if (!iface)
        return m_scanResult;

    const QStringList names = iface->registeredServiceNames().value();
    QHash<QString, QStringList> alive;
    for (const QString &name : names) {
        if (!name.startsWith(':') || name == bus.baseService())
            continue;
        // Unique names are never reused, so cache the PID and process names.
        auto it = m_busNames.constFind(name);
        alive.insert(name, it != m_busNames.constEnd() ? it.value()
                                                       : processNames(iface->servicePid(name).value()));
    }
    m_busNames = alive;

    for (auto it = alive.cbegin(); it != alive.cend(); ++it) {
        if (matchScore(appId, it.value()) == 0 || m_deadServices.contains(it.key()))
            continue;
        bool ok = true;
        QList<MenuEntry> menus = findMenusOf(it.key(), &ok);
        if (!ok) // didn't answer in time: assume its menus haven't changed
            for (const MenuEntry &e : previous)
                if (e.service == it.key())
                    menus << e;
        for (MenuEntry e : menus) {
            e.names = it.value();
            m_scanResult.append(e);
            if (!m_loggedKeys.contains(e.key())) {
                m_loggedKeys.insert(e.key());
                qInfo().noquote() << "menu found on bus:" << e.service << e.path << e.winPath
                                  << (e.kind == MenuEntry::GMenu ? "(gtk)" : "(dbusmenu)")
                                  << "names" << e.names.join(',');
            }
        }
    }
    return m_scanResult;
}

bool MenuController::start()
{
    return m_registrar->start();
}

Toplevel *MenuController::activeWindow() const
{
    return m_toplevels->active();
}

bool MenuController::activeMaximized() const
{
    Toplevel *t = m_toplevels->active();
    return t && t->isMaximized() && !t->isFullscreen() && !t->isMinimized();
}

void MenuController::closeActive()
{
    if (Toplevel *t = m_toplevels->active())
        t->close();
}

void MenuController::minimizeActive()
{
    if (Toplevel *t = m_toplevels->active())
        t->set_minimized();
}

void MenuController::restoreActive()
{
    if (Toplevel *t = m_toplevels->active())
        t->unset_maximized();
}

bool MenuController::hasToplevelTracking() const
{
    return m_toplevels->isActive();
}

QList<QAction *> MenuController::menuActions() const
{
    return m_importer ? m_importer->rootMenu()->actions() : QList<QAction *>();
}

QList<MenuEntry> MenuController::candidatesFor(const QString &appId)
{
    struct Scored { int score; MenuEntry e; };
    QList<Scored> scored;
    QList<MenuEntry> all = m_registrar->entries();
    if (hasToplevelTracking()) {
        for (const MenuEntry &found : scanBus(appId)) {
            const bool known = std::any_of(all.cbegin(), all.cend(), [&](const MenuEntry &e) {
                return e.key() == found.key();
            });
            if (!known)
                all.append(found);
        }
    }
    // A dialog or a window not paired yet: show the menu of the last active window
    // of the same application rather than a random one.
    // Same if the window's menu is empty (appmenu-gtk-module creates such menus for dialogs too).
    Toplevel *owner = m_toplevels->active();
    const bool noOwnMenu = owner && (!m_pairs.contains(owner) || m_emptyKeys.contains(m_pairs.value(owner)));
    if (noOwnMenu && m_lastPaired && m_lastPaired != owner && m_lastPaired->appId() == appId)
        owner = m_lastPaired;
    const QString ownKey = owner ? m_pairs.value(owner) : QString();
    const QSet<QString> othersKeys = pairedKeys(owner);
    QSet<QString> seen;
    for (const MenuEntry &e : all) {
        if (seen.contains(e.key()))
            continue;
        seen.insert(e.key());
        // Without window tracking any menu fits; the most recent comes first.
        int score = hasToplevelTracking() ? matchScore(appId, e.names) : 1;
        if (score > 0 && e.key() == ownKey)
            score = 100;              // the menu of exactly this window
        else if (score > 0 && othersKeys.contains(e.key()))
            score = 0;                // the menu of another window of the same application
        if (score > 0)
            scored.append({score, e});
    }
    std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
        return a.score != b.score ? a.score > b.score : entryLess(b.e, a.e);
    });
    QList<MenuEntry> out;
    for (const Scored &s : scored)
        out.append(s.e);
    return out;
}

void MenuController::reevaluate()
{
    QString appId;
    Toplevel *t = m_toplevels->active();
    if (t)
        appId = t->appId();

    if (t && m_filledKeys.contains(m_pairs.value(t)))
        m_lastPaired = t;

    QList<MenuEntry> candidates;
    if (t || !hasToplevelTracking())
        candidates = candidatesFor(appId);

    QString name;
    if (t) {
        name = desktopEntryName(appId);
        if (name.isEmpty())
            name = prettyAppId(appId);
        if (name.isEmpty())
            name = t->title();
    } else if (!candidates.isEmpty() && !candidates.first().names.isEmpty()) {
        name = prettyAppId(candidates.first().names.first());
    }

    const bool appChanged = name != m_appName;
    m_appName = name;
    m_windowMenu->setTitle(m_appName.isEmpty() ? tr("Desktop") : m_appName);

    if (appChanged || appId != m_lastAppId) {
        QStringList c;
        for (const MenuEntry &e : candidates)
            c << QStringLiteral("%1%2[%3]").arg(e.service, e.path, e.names.join(','));
        qInfo().noquote() << "active window: app_id =" << appId << "| title =" << (t ? t->title() : QString())
                          << "| tracking =" << hasToplevelTracking()
                          << "| candidates:" << (c.isEmpty() ? QStringLiteral("none") : c.join(' '));
        m_lastAppId = appId;
        m_rescanAttempts = 0;
    }

    if (t && candidates.isEmpty() && m_rescanAttempts < 6) {
        ++m_rescanAttempts;
        m_rescanTimer->start();
    }

    // The candidate set hasn't changed: keep the current menu, otherwise it would flicker
    // (e.g. when the window title changes).
    QStringList keys;
    for (const MenuEntry &e : candidates)
        keys << e.key();
    if (m_importer && (keys == m_candidateKeys || (!keys.isEmpty() && m_importerKey == keys.first()))) {
        m_candidateKeys = keys;
        m_candidates = candidates;
        m_candidateIndex = keys.indexOf(m_importerKey);
        if (appChanged)
            Q_EMIT changed();
        return;
    }
    m_candidateKeys = keys;
    m_candidates = candidates;
    m_candidateIndex = -1;
    tryNextCandidate();
}

void MenuController::tryNextCandidate()
{
    if (m_importer)
        m_importer->deleteLater();   // deletes its QActions, so they disappear from the QMenuBar
    m_importer = nullptr;
    m_importerKey.clear();

    ++m_candidateIndex;
    if (m_candidateIndex >= m_candidates.size()) {
        Q_EMIT changed();
        return;
    }
    const MenuEntry e = m_candidates.at(m_candidateIndex);
    qInfo().noquote() << "using menu" << e.service << e.path << e.winPath;
    MenuImporter *imp = e.kind == MenuEntry::GMenu
                            ? static_cast<MenuImporter *>(new GMenuImporter(e.service, e.path, e.winPath, this))
                            : new DBusMenuImporter(e.service, e.path, this);
    m_importer = imp;
    m_importerKey = e.key();

    connect(imp, &MenuImporter::menuUpdated, this, [this, imp, e] {
        if (m_importer != imp)
            return;
        const auto acts = imp->rootMenu()->actions();
        const int n = std::count_if(acts.cbegin(), acts.cend(), [](QAction *a) { return a->isVisible(); });
        qInfo().noquote() << "menu" << e.service + (e.winPath.isEmpty() ? e.path : e.path + " " + e.winPath)
                          << "loaded," << n << "top-level items";
        // A Qt application may have several menus (hidden windows, old registrations):
        // skip an empty one and try the next.
        if (n > 0) {
            m_filledKeys.insert(e.key());
            m_emptyKeys.remove(e.key());
            if (Toplevel *owner = m_pairs.key(e.key()); owner && owner == m_toplevels->active())
                m_lastPaired = owner;
        }
        if (n == 0 && !m_emptyKeys.contains(e.key())) {
            m_emptyKeys.insert(e.key());
            // If this is the menu of a window that has none (a dialog), choose again with that in mind.
            if (m_pairs.key(e.key()) == m_toplevels->active()) {
                m_candidateKeys.clear();
                QTimer::singleShot(0, this, &MenuController::reevaluate);
                return;
            }
        }
        if (n == 0 && m_candidateIndex + 1 < m_candidates.size())
            tryNextCandidate();
        else
            Q_EMIT changed();
    });
    connect(imp, &MenuImporter::invalid, this, [this, imp, e] {
        if (m_importer != imp)
            return;
        qInfo().noquote() << "menu" << e.service + e.path << "unavailable, trying next";
        // If it was a menu paired with a window, re-check the pairing.
        for (auto p = m_pairs.begin(); p != m_pairs.end(); ++p) {
            if (p.value() == e.key()) {
                m_pending << p.key();
                m_pairs.erase(p);
                m_pairAttempts = 0;
                m_pairTimer->start();
                break;
            }
        }
        tryNextCandidate();
    });
    imp->refresh();
    Q_EMIT changed();
}

void MenuController::rebuildWindowMenu()
{
    m_windowMenu->clear();
    Toplevel *t = m_toplevels->active();
    if (!t) {
        auto *a = m_windowMenu->addAction(hasToplevelTracking()
                                              ? tr("No active window")
                                              : tr("The compositor does not support wlr-foreign-toplevel"));
        a->setEnabled(false);
        return;
    }
    QPointer<Toplevel> tp = t;
    m_windowMenu->addAction(QIcon::fromTheme("window-minimize"), tr("Minimize"), this,
                            [tp] { if (tp) tp->set_minimized(); });
    m_windowMenu->addAction(QIcon::fromTheme(t->isMaximized() ? "window-restore" : "window-maximize"),
                            t->isMaximized() ? tr("Restore") : tr("Maximize"), this,
                            [tp] {
                                if (!tp) return;
                                if (tp->isMaximized()) tp->unset_maximized();
                                else tp->set_maximized();
                            });
    // set/unset_fullscreen appeared in version 2 of the protocol
    if (wl_proxy_get_version(reinterpret_cast<wl_proxy *>(t->object())) >= 2) {
        m_windowMenu->addAction(QIcon::fromTheme("view-fullscreen"),
                                t->isFullscreen() ? tr("Exit Full Screen")
                                                  : tr("Full Screen"), this,
                                [tp] {
                                    if (!tp) return;
                                    if (tp->isFullscreen()) tp->unset_fullscreen();
                                    else tp->set_fullscreen(nullptr);
                                });
    }
    m_windowMenu->addSeparator();
    m_windowMenu->addAction(QIcon::fromTheme("window-close"), tr("Close Window"), this,
                            [tp] { if (tp) tp->close(); });
}
