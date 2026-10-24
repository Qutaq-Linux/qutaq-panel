#pragma once

#include <QDBusContext>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

class QAction;
class QDBusServiceWatcher;
class QMenu;
class QTimer;
class DBusMenuImporter;

// Tray: program icons via the StatusNotifierItem protocol (KDE/freedesktop).
// Used by libappindicator / libayatana-appindicator (Unity), Qt (QSystemTrayIcon),
// Electron, Chromium, Telegram, Steam, nm-applet --indicator, blueman-applet and others.
// The old X11 tray (XEmbed) is impossible on Wayland; for such programs there are the
// snixembed and xembedsniproxy bridges (see README).

// org.kde.StatusNotifierWatcher is the registry of icons. The panel becomes it itself
// if the bus doesn't have one yet (under labwc it usually doesn't).
class StatusNotifierWatcher : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierWatcher")
    Q_PROPERTY(QStringList RegisteredStatusNotifierItems READ registeredItems)
    Q_PROPERTY(bool IsStatusNotifierHostRegistered READ isHostRegistered)
    Q_PROPERTY(int ProtocolVersion READ protocolVersion)
public:
    explicit StatusNotifierWatcher(QObject *parent = nullptr);

    QStringList registeredItems() const { return m_items; }
    bool isHostRegistered() const { return !m_hosts.isEmpty(); }
    int protocolVersion() const { return 0; }

public Q_SLOTS:
    Q_SCRIPTABLE void RegisterStatusNotifierItem(const QString &serviceOrPath);
    Q_SCRIPTABLE void RegisterStatusNotifierHost(const QString &service);

Q_SIGNALS:
    Q_SCRIPTABLE void StatusNotifierItemRegistered(const QString &item);
    Q_SCRIPTABLE void StatusNotifierItemUnregistered(const QString &item);
    Q_SCRIPTABLE void StatusNotifierHostRegistered();
    Q_SCRIPTABLE void StatusNotifierHostUnregistered();

private:
    void onServiceGone(const QString &service);

    QStringList m_items; // "service/path"
    QSet<QString> m_hosts;
    QDBusServiceWatcher *m_serviceWatcher;
};

// A single icon
class TrayItem : public QObject
{
    Q_OBJECT
public:
    TrayItem(const QString &key, QObject *parent);
    ~TrayItem() override;

    QString key() const { return m_key; }        // "service/path", as in the watcher
    QString service() const { return m_service; }
    QString path() const { return m_path; }
    QString id() const { return m_id; }          // Id, for the hidden list in the settings
    QString title() const { return m_title; }
    QAction *action() const { return m_action; } // the icon in the panel (shared by all monitors)
    QMenu *menu() const;                         // the application's menu or nullptr
    bool itemIsMenu() const { return m_itemIsMenu; }
    bool isReady() const { return m_ready; }
    bool isPassive() const { return m_status == QLatin1String("Passive"); }

    void activate(const QPoint &globalPos);          // left button
    void secondaryActivate(const QPoint &globalPos); // middle button
    void contextMenu(const QPoint &globalPos);       // right button, if the program has no D-Bus menu
    void scroll(int delta, Qt::Orientation o);

Q_SIGNALS:
    void changed();
    void activateFailed(); // the program can't Activate: show the menu instead

private Q_SLOTS:
    void scheduleRefresh();

private:
    void refresh();
    void apply(const QVariantMap &props);
    void call(const QString &method, const QVariantList &args, bool reportFailure = false);

    QString m_key, m_service, m_path;
    QString m_iface = QStringLiteral("org.kde.StatusNotifierItem");
    bool m_triedOtherIface = false;
    QString m_id, m_title, m_status, m_menuPath;
    bool m_itemIsMenu = false;
    bool m_ready = false;
    QAction *m_action;
    QPointer<DBusMenuImporter> m_importer;
    QTimer *m_debounce;
};

// Tray host: keeps the list of icons and notifies panels of changes.
// Settings, section [tray]: show=auto|never, hidden=Id1,Id2 (hide icons),
// showPassive=false (also show "passive" icons).
class SystemTray : public QObject
{
    Q_OBJECT
public:
    explicit SystemTray(QObject *parent = nullptr);

    QList<TrayItem *> visibleItems() const;
    TrayItem *itemFor(QAction *action) const;

Q_SIGNALS:
    void changed();
    void menuRequested(QAction *action); // open the icon's menu (Activate unsupported)

private Q_SLOTS:
    void onItemRegistered(const QString &key) { addItem(key); }
    void onItemUnregistered(const QString &key) { removeItem(key); }

private:
    void becomeWatcherOrClient();
    void registerHost();
    void addItem(const QString &key);
    void removeItem(const QString &key);
    void loadItemsFromWatcher();

    StatusNotifierWatcher *m_watcher = nullptr; // our own watcher, if we got the name
    QString m_hostName;
    QList<TrayItem *> m_items;
    QTimer *m_changed;
};

// If Qt has no icon theme (under labwc there is usually no platform plugin), use the same
// one as GTK/Xfce, otherwise tray and menu icons won't be found by name.
void setupIconTheme();
