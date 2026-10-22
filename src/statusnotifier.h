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

// Трей: значки программ по протоколу StatusNotifierItem (KDE/freedesktop).
// Его используют libappindicator / libayatana-appindicator (Unity), Qt (QSystemTrayIcon),
// Electron, Chromium, Telegram, Steam, nm-applet --indicator, blueman-applet и др.
// Старый X11-трей (XEmbed) на Wayland невозможен — для таких программ есть мосты
// snixembed или xembedsniproxy (см. README).

// org.kde.StatusNotifierWatcher — «справочник» значков. Панель становится им сама,
// если на шине его ещё нет (под labwc обычно нет).
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

    QStringList m_items; // «служба/путь»
    QSet<QString> m_hosts;
    QDBusServiceWatcher *m_serviceWatcher;
};

// Один значок
class TrayItem : public QObject
{
    Q_OBJECT
public:
    TrayItem(const QString &key, QObject *parent);
    ~TrayItem() override;

    QString key() const { return m_key; }        // «служба/путь», как в справочнике
    QString service() const { return m_service; }
    QString path() const { return m_path; }
    QString id() const { return m_id; }          // Id — для списка скрытых в настройках
    QString title() const { return m_title; }
    QAction *action() const { return m_action; } // значок в панели (общий для всех мониторов)
    QMenu *menu() const;                         // меню приложения или nullptr
    bool itemIsMenu() const { return m_itemIsMenu; }
    bool isReady() const { return m_ready; }
    bool isPassive() const { return m_status == QLatin1String("Passive"); }

    void activate(const QPoint &globalPos);          // левая кнопка
    void secondaryActivate(const QPoint &globalPos); // средняя
    void contextMenu(const QPoint &globalPos);       // правая, если у программы нет меню D-Bus
    void scroll(int delta, Qt::Orientation o);

Q_SIGNALS:
    void changed();
    void activateFailed(); // программа не умеет Activate — показать меню вместо этого

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

// Хост трея: держит список значков и сообщает панелям об изменениях.
// Настройки, секция [tray]: show=auto|never, hidden=Id1,Id2 (скрыть значки),
// showPassive=false (показывать и «пассивные» значки).
class SystemTray : public QObject
{
    Q_OBJECT
public:
    explicit SystemTray(QObject *parent = nullptr);

    QList<TrayItem *> visibleItems() const;
    TrayItem *itemFor(QAction *action) const;

Q_SIGNALS:
    void changed();
    void menuRequested(QAction *action); // открыть меню значка (Activate не поддерживается)

private Q_SLOTS:
    void onItemRegistered(const QString &key) { addItem(key); }
    void onItemUnregistered(const QString &key) { removeItem(key); }

private:
    void becomeWatcherOrClient();
    void registerHost();
    void addItem(const QString &key);
    void removeItem(const QString &key);
    void loadItemsFromWatcher();

    StatusNotifierWatcher *m_watcher = nullptr; // свой справочник, если удалось занять имя
    QString m_hostName;
    QList<TrayItem *> m_items;
    QTimer *m_changed;
};

// Если у Qt нет темы значков (под labwc обычно нет плагина платформы), взять ту же,
// что у GTK/Xfce — иначе значки трея и меню по именам не найдутся.
void setupIconTheme();
