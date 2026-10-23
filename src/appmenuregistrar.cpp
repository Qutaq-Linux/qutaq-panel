#include "appmenuregistrar.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QFile>
#include <QFileInfo>

static const char *kService = "com.canonical.AppMenu.Registrar";
static const char *kPath = "/com/canonical/AppMenu/Registrar";

static QString stripDesktop(QString s)
{
    s = QFileInfo(s).fileName();
    if (s.endsWith(QLatin1String(".desktop")))
        s.chop(8);
    return s;
}

QStringList processNames(quint32 pid)
{
    QStringList out;
    if (!pid)
        return out;
    const QString proc = QStringLiteral("/proc/%1/").arg(pid);

    QFile comm(proc + "comm");
    if (comm.open(QIODevice::ReadOnly))
        out << QString::fromLocal8Bit(comm.readAll()).trimmed();

    const QString exe = QFileInfo(proc + "exe").symLinkTarget();
    if (!exe.isEmpty())
        out << QFileInfo(exe).fileName();

    QFile cmdline(proc + "cmdline");
    if (cmdline.open(QIODevice::ReadOnly)) {
        const QByteArray argv0 = cmdline.readAll().split('\0').value(0);
        if (!argv0.isEmpty())
            out << QFileInfo(QString::fromLocal8Bit(argv0)).fileName();
    }

    QFile environ(proc + "environ");
    if (environ.open(QIODevice::ReadOnly)) {
        for (const QByteArray &kv : environ.readAll().split('\0')) {
            const int eq = kv.indexOf('=');
            if (eq <= 0)
                continue;
            const QByteArray k = kv.left(eq);
            const QString v = QString::fromLocal8Bit(kv.mid(eq + 1));
            if (k == "GIO_LAUNCHED_DESKTOP_FILE" || k == "BAMF_DESKTOP_FILE_HINT")
                out << stripDesktop(v);
            else if (k == "FLATPAK_ID" || k == "SNAP_NAME")
                out << v;
        }
    }

    out.removeAll(QString());
    out.removeDuplicates();
    return out;
}

AppMenuRegistrar::AppMenuRegistrar(QObject *parent)
    : QObject(parent)
{
}

bool AppMenuRegistrar::start()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qWarning() << "No connection to the D-Bus session bus";
        return false;
    }

    m_watcher = new QDBusServiceWatcher(this);
    m_watcher->setConnection(bus);
    m_watcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered,
            this, &AppMenuRegistrar::removeService);

    if (!bus.registerObject(kPath, this, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        qWarning() << "Failed to export" << kPath;
        return false;
    }
    if (!bus.registerService(kService)) {
        qWarning() << kService << "is already owned by another process (another panel?)."
                   << "Close it: there can be only one registrar.";
        return false;
    }
    return true;
}

void AppMenuRegistrar::RegisterWindow(uint windowId, const QDBusObjectPath &menuObjectPath)
{
    if (!calledFromDBus())
        return;
    const QString service = message().service();
    const QString path = menuObjectPath.path();

    MenuEntry e;
    e.windowId = windowId;
    e.service = service;
    e.path = path;
    e.pid = connection().interface()->servicePid(service).value();
    e.names = processNames(e.pid);
    e.serial = ++m_serial;

    // Одно окно — одна запись: если тот же процесс перерегистрирует тот же windowId,
    // старую запись выбрасываем.
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->service == service && it->windowId == windowId && windowId != 0)
            it = m_entries.erase(it);
        else
            ++it;
    }
    m_entries.insert(key(service, path), e);
    m_watcher->addWatchedService(service);

    qInfo().noquote() << "menu registered:" << service << path << "pid" << e.pid
                      << "names" << e.names.join(',');

    Q_EMIT WindowRegistered(windowId, service, menuObjectPath);
    Q_EMIT entriesChanged();
}

void AppMenuRegistrar::UnregisterWindow(uint windowId)
{
    const QString service = calledFromDBus() ? message().service() : QString();
    bool changed = false;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->windowId == windowId && (service.isEmpty() || it->service == service)) {
            it = m_entries.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        Q_EMIT WindowUnregistered(windowId);
        Q_EMIT entriesChanged();
    }
}

QString AppMenuRegistrar::GetMenuForWindow(uint windowId, QDBusObjectPath &menuObjectPath)
{
    const MenuEntry *best = nullptr;
    for (const MenuEntry &e : m_entries)
        if (e.windowId == windowId && (!best || e.serial > best->serial))
            best = &e;
    if (!best) {
        menuObjectPath = QDBusObjectPath("/");
        return QString();
    }
    menuObjectPath = QDBusObjectPath(best->path);
    return best->service;
}

void AppMenuRegistrar::removeService(const QString &service)
{
    bool changed = false;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->service == service) {
            Q_EMIT WindowUnregistered(it->windowId);
            it = m_entries.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    m_watcher->removeWatchedService(service);
    if (changed)
        Q_EMIT entriesChanged();
}
