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

// Узел дерева из GetLayout: (ia{sv}av)
struct DBusMenuLayoutItem {
    int id = 0;
    QVariantMap properties;
    QList<DBusMenuLayoutItem> children;
};
Q_DECLARE_METATYPE(DBusMenuLayoutItem)

QDBusArgument &operator<<(QDBusArgument &arg, const DBusMenuLayoutItem &item);
const QDBusArgument &operator>>(const QDBusArgument &arg, DBusMenuLayoutItem &item);

// Клиент com.canonical.dbusmenu: превращает удалённое меню в дерево QMenu/QAction.
// rootMenu() содержит пункты верхнего уровня — панель кладёт их в QMenuBar.
class DBusMenuImporter : public MenuImporter
{
    Q_OBJECT
public:
    DBusMenuImporter(const QString &service, const QString &path, QObject *parent = nullptr);
    ~DBusMenuImporter() override;

    QMenu *rootMenu() const override { return m_root; }
    void refresh() override; // асинхронно перечитать всё дерево
    // Корень показывается как всплывающее меню (значок в трее), а не раскладывается
    // в строку меню — перед показом спросить приложение (AboutToShow), как для подменю.
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

    QHash<int, QPointer<QMenu>> m_menus;      // id → подменю (0 = корень)
    QHash<int, QPointer<QAction>> m_actions;  // id → пункт
    QHash<int, QVariantMap> m_props;          // последнее известное состояние свойств
    QHash<QMenu *, QList<QPointer<QObject>>> m_owned; // что удалить при перестройке меню
    QSet<int> m_dirty;                        // подменю, которые надо перечитать перед показом
};
