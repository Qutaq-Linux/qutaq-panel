#pragma once

#include <QDBusContext>
#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QStringList>

class QDBusServiceWatcher;

// A menu that an application has registered with us.
struct MenuEntry {
    enum Kind { DBusMenu, GMenu };
    Kind kind = DBusMenu;      // com.canonical.dbusmenu or org.gtk.Menus
    quint32 windowId = 0;      // almost always useless on Wayland; kept for the API
    QString service;           // unique bus name (":1.42")
    QString path;              // menu object path (com.canonical.dbusmenu or org.gtk.Menus)
    QString winPath;           // GtkApplication: <base>/window/<N>, the win.* actions of this window
    quint32 pid = 0;
    QStringList names;         // candidates for matching the window's app_id
    quint64 serial = 0;        // registration order (newer = larger)

    // The object that belongs to a single window: for GtkApplication the menu is shared, the window is winPath.
    QString key() const { return service + (winPath.isEmpty() ? path : winPath); }
};

// Implementation of com.canonical.AppMenu.Registrar.
// Qt applications (QDBusMenuBar) and GTK with appmenu-gtk-module register their menus
// here, provided the service is on the bus when the application starts.
class AppMenuRegistrar : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.canonical.AppMenu.Registrar")

public:
    explicit AppMenuRegistrar(QObject *parent = nullptr);
    bool start();

    QList<MenuEntry> entries() const { return m_entries.values(); }

public Q_SLOTS:
    // D-Bus API
    void RegisterWindow(uint windowId, const QDBusObjectPath &menuObjectPath);
    void UnregisterWindow(uint windowId);
    QString GetMenuForWindow(uint windowId, QDBusObjectPath &menuObjectPath);

Q_SIGNALS:
    // D-Bus signals
    void WindowRegistered(uint windowId, const QString &service, const QDBusObjectPath &menuObjectPath);
    void WindowUnregistered(uint windowId);

    // Internal signal for the rest of the panel
    void entriesChanged();

private:
    void removeService(const QString &service);
    static QString key(const QString &service, const QString &path) { return service + '|' + path; }

    QHash<QString, MenuEntry> m_entries;
    QDBusServiceWatcher *m_watcher = nullptr;
    quint64 m_serial = 0;
};

// Process names for matching against the Wayland app_id:
// comm, basename(exe), basename(argv0), *.desktop from the environment, FLATPAK_ID.
QStringList processNames(quint32 pid);
