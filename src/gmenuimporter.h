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

// GTK menu client: an org.gtk.Menus model plus org.gtk.Actions actions.
// This is how GtkApplication (Mousepad, gedit…) and appmenu-gtk-module
// (Thunar, xfce4-terminal and any GtkMenuBar) publish their menus.
//
// Action paths by prefix:
//   unity.* on the same object as the menu (appmenu-gtk-module)
//   app.*   <base>             (GtkApplication)
//   win.*   <base>/window/<N>  (GtkApplicationWindow)
// where <base> is the menu path without the trailing /menus/menubar[/N].
class GMenuImporter : public MenuImporter
{
    Q_OBJECT
public:
    // winPath: the object with the win.* actions of the right GtkApplication window; empty means the newest window.
    GMenuImporter(const QString &service, const QString &menuPath, const QString &winPath,
                  QObject *parent = nullptr);
    ~GMenuImporter() override;

    QMenu *rootMenu() const override { return m_root; }
    void refresh() override;

private Q_SLOTS:
    void onMenusChanged(const QDBusMessage &msg);

private:
    using Key = QPair<uint, uint>; // (group, menu)

    struct ActionInfo {
        bool enabled = true;
        bool hasState = false;
        QVariant state;
    };
    struct SubmenuInfo {
        Key key;
        QString submenuAction; // "unity.-File-0" etc., enabled before showing
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
    QHash<QString, QString> m_actionPaths;   // prefix → object path
    QHash<QString, ActionInfo> m_actions;    // "win.save" → state

    QHash<QMenu *, SubmenuInfo> m_submenus;
    QHash<QMenu *, QSet<Key>> m_usedKeys;    // which parts of the model a menu shows
    QHash<QMenu *, QList<QPointer<QObject>>> m_owned;
    QSet<Key> m_skipSections;                // duplicates in the root (see buildRoot)
};
