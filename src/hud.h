#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

class MenuController;
class QAction;
class QMenu;

// The menus of the active application for a HUD (qutaq-dash --hud): a flat list of commands
// such as "Save As…" with their path "File ▸ Save As…", and a way to run one of them.
//
// D-Bus: org.qutaq.Panel /Hud, interface org.qutaq.Panel.Hud
//   GetItems() → a{sv}: "app" (name), "appId", "token" (u), "labels", "paths" (as: the parent
//                menus joined with " > "), "shortcuts" (as)
//   Activate(u token, i index) → b: runs labels[index] of that GetItems reply
class Hud : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qutaq.Panel.Hud")
public:
    explicit Hud(MenuController *controller, QObject *parent = nullptr);

public Q_SLOTS:
    QVariantMap GetItems();
    bool Activate(uint token, int index);

private:
    struct Item {
        QPointer<QAction> action;
        QString label, path, shortcut;
    };
    void collect(QMenu *menu, const QStringList &path, int depth);

    MenuController *m_controller;
    QList<Item> m_items;
    uint m_token = 0;
    qint64 m_deadline = 0;
    bool m_openSubmenus = true;
};
