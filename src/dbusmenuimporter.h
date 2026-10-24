#pragma once

#include <QDBusArgument>
#include <QDBusMessage>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVariantMap>

#include "menuimporter.h"

class QAction;
class QMenu;
class QTimer;

// A tree node from GetLayout: (ia{sv}av)
struct DBusMenuLayoutItem {
    int id = 0;
    QVariantMap properties;
    QList<DBusMenuLayoutItem> children;
};
Q_DECLARE_METATYPE(DBusMenuLayoutItem)

QDBusArgument &operator<<(QDBusArgument &arg, const DBusMenuLayoutItem &item);
const QDBusArgument &operator>>(const QDBusArgument &arg, DBusMenuLayoutItem &item);

// com.canonical.dbusmenu client: turns a remote menu into a QMenu/QAction tree.
// rootMenu() holds the top-level items; the panel puts them into a QMenuBar.
class DBusMenuImporter : public MenuImporter
{
    Q_OBJECT
public:
    DBusMenuImporter(const QString &service, const QString &path, QObject *parent = nullptr);
    ~DBusMenuImporter() override;

    QMenu *rootMenu() const override { return m_root; }
    void refresh() override; // re-read the whole tree asynchronously
    // The root is shown as a popup menu (a tray icon) rather than laid out in a menu bar,
    // so ask the application (AboutToShow) before showing it, as for submenus.
    void prepareRootOnShow();

private Q_SLOTS:
    void onLayoutUpdated(uint revision, int parentId);
    void onItemsPropertiesUpdated(const QDBusMessage &msg);

private:
    QDBusMessage call(const QString &method) const;
    void populate(QMenu *menu, const DBusMenuLayoutItem &item);
    void clearMenu(QMenu *menu);
    QAction *createAction(QMenu *into, const DBusMenuLayoutItem &child);
    void applyProperties(QAction *action, const QVariantMap &props);
    void sendEvent(int id, const QString &event);
    void prepareSubmenu(QMenu *menu, int id);
    bool fetchSync(int parentId, DBusMenuLayoutItem &out, int timeoutMs);

    QMenu *m_root = nullptr;
    QTimer *m_refreshTimer = nullptr;

    QHash<int, QPointer<QMenu>> m_menus;      // id → submenu (0 = root)
    QHash<int, QPointer<QAction>> m_actions;  // id → item
    QHash<int, QVariantMap> m_props;          // last known property state
    QHash<QMenu *, QList<QPointer<QObject>>> m_owned; // what to delete when a menu is rebuilt
    QSet<int> m_dirty;                        // submenus to re-read before showing
};
