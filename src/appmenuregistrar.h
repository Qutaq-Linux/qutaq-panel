#pragma once

#include <QDBusContext>
#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QStringList>

class QDBusServiceWatcher;

// Запись о меню, которое приложение зарегистрировало у нас.
struct MenuEntry {
    enum Kind { DBusMenu, GMenu };
    Kind kind = DBusMenu;      // com.canonical.dbusmenu или org.gtk.Menus
    quint32 windowId = 0;      // на Wayland почти всегда бесполезен, храним для API
    QString service;           // уникальное имя на шине (":1.42")
    QString path;              // путь объекта меню (com.canonical.dbusmenu или org.gtk.Menus)
    QString winPath;           // GtkApplication: <база>/window/<N> — действия win.* этого окна
    quint32 pid = 0;
    QStringList names;         // кандидаты для сопоставления с app_id окна
    quint64 serial = 0;        // порядок регистрации (новее = больше)

    // Объект, который принадлежит одному окну: для GtkApplication меню общее, а окно — winPath.
    QString key() const { return service + (winPath.isEmpty() ? path : winPath); }
};

// Реализация com.canonical.AppMenu.Registrar.
// Qt-приложения (QDBusMenuBar) и GTK с appmenu-gtk-module отдают свои меню
// именно сюда — если сервис есть на шине к моменту запуска приложения.
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
    // D-Bus сигналы
    void WindowRegistered(uint windowId, const QString &service, const QDBusObjectPath &menuObjectPath);
    void WindowUnregistered(uint windowId);

    // Внутренний сигнал для остальной панели
    void entriesChanged();

private:
    void removeService(const QString &service);
    static QString key(const QString &service, const QString &path) { return service + '|' + path; }

    QHash<QString, MenuEntry> m_entries;
    QDBusServiceWatcher *m_watcher = nullptr;
    quint64 m_serial = 0;
};

// Имена процесса для сопоставления с Wayland app_id:
// comm, basename(exe), basename(argv0), *.desktop из окружения, FLATPAK_ID.
QStringList processNames(quint32 pid);
