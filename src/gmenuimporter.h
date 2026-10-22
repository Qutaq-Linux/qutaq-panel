#pragma once

#include <QDBusMessage>
#include <QHash>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QVariant>
#include <QVariantMap>

#include "menuimporter.h"

class QAction;
class QMenu;
class QTimer;

// Клиент GTK-меню: модель org.gtk.Menus + действия org.gtk.Actions.
// Так меню публикуют GtkApplication (Mousepad, gedit…) и appmenu-gtk-module
// (Thunar, xfce4-terminal и любые GtkMenuBar).
//
// Пути действий по префиксу:
//   unity.* — на том же объекте, что и меню (appmenu-gtk-module)
//   app.*   — <база>             (GtkApplication)
//   win.*   — <база>/window/<N>  (GtkApplicationWindow)
// где <база> — путь меню без хвоста /menus/menubar[/N].
class GMenuImporter : public MenuImporter
{
    Q_OBJECT
public:
    // winPath — объект действий win.* нужного окна GtkApplication; пусто — взять самое новое окно.
    GMenuImporter(const QString &service, const QString &menuPath, const QString &winPath,
                  QObject *parent = nullptr);
    ~GMenuImporter() override;

    QMenu *rootMenu() const override { return m_root; }
    void refresh() override;

private Q_SLOTS:
    void onMenusChanged(const QDBusMessage &msg);

private:
    using Key = QPair<uint, uint>; // (группа, меню)

    struct ActionInfo {
        bool enabled = true;
        bool hasState = false;
        QVariant state;
    };
    struct SubmenuInfo {
        Key key;
        QString submenuAction; // "unity.-File-0" и т.п. — включаем перед показом
    };

    bool subscribe(const QList<uint> &groups);
    void unsubscribe(const QList<uint> &groups);
    void describeActions();
    void setupActionPaths();
    QString actionPath(const QString &fullName, QString *shortName) const;

    void buildRoot();
    void populate(QMenu *menu, const Key &key);
    void clearMenu(QMenu *menu);
    void addItems(QMenu *menu, const Key &key, bool isRoot);
    QAction *createAction(QMenu *menu, const QVariantMap &item);
    QMenu *createSubmenu(QMenu *parent, const QVariantMap &item, const Key &key);
    void prepareSubmenu(QMenu *menu);
    void hideSubmenu(QMenu *menu);
    void activate(const QString &fullName, const QVariant &target);
    void setActionState(const QString &fullName, bool state, bool block);
    bool sectionHasNative(const Key &key) const;
    void collectUnitySections(const Key &key, QSet<Key> &out) const;

    QMenu *m_root = nullptr;
    QTimer *m_rebuildTimer = nullptr;
    QString m_winPath;

    QHash<Key, QList<QVariantMap>> m_model;
    QSet<uint> m_rootGroups;
    QHash<QString, QString> m_actionPaths;   // префикс → путь объекта
    QHash<QString, ActionInfo> m_actions;    // "win.save" → состояние

    QHash<QMenu *, SubmenuInfo> m_submenus;
    QHash<QMenu *, QSet<Key>> m_usedKeys;    // какие части модели отображает меню
    QHash<QMenu *, QList<QPointer<QObject>>> m_owned;
    QSet<Key> m_skipSections;                // дубликаты в корне (см. buildRoot)
};
