#include "hud.h"

#include "menucontroller.h"
#include "toplevelmanager.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QMenu>
#include <QTimer>

namespace {
constexpr int kMaxDepth = 6;
constexpr int kMaxItems = 3000;
constexpr int kBudgetMs = 2500; // the whole walk; slow applications get a partial list

// "Save &As…" → "Save As…" ("&&" is a literal "&")
QString plainText(QString text)
{
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == '&') {
            if (i + 1 < text.size() && text.at(i + 1) == '&') {
                out += '&';
                ++i;
            }
            continue;
        }
        out += text.at(i);
    }
    // Some applications put the shortcut into the label after a tab
    const int tab = out.indexOf('\t');
    if (tab >= 0)
        out.truncate(tab);
    return out.trimmed();
}
} // namespace

Hud::Hud(MenuController *controller, QObject *parent)
    : QObject(parent)
    , m_controller(controller)
{
}

void Hud::collect(QMenu *menu, const QStringList &path, int depth)
{
    const QList<QAction *> actions = menu->actions();
    for (QAction *a : actions) {
        if (m_items.size() >= kMaxItems)
            return;
        if (!a || a->isSeparator() || !a->isVisible())
            continue;
        const QString text = plainText(a->text());
        if (text.isEmpty())
            continue;
        if (QMenu *sub = a->menu()) {
            if (!a->isEnabled() || depth >= kMaxDepth || QDateTime::currentMSecsSinceEpoch() > m_deadline)
                continue;
            // The importers fill a submenu when it is about to be shown (dbusmenu AboutToShow,
            // GTK Start + submenu-action): pretend to open it, then close it again.
            QPointer<QMenu> guard(sub);
            if (m_openSubmenus)
                Q_EMIT sub->aboutToShow();
            if (guard)
                collect(sub, path + QStringList{text}, depth + 1);
            if (guard && m_openSubmenus)
                Q_EMIT sub->aboutToHide();
            continue;
        }
        if (!a->isEnabled())
            continue;
        m_items.append({a, text, path.join(QStringLiteral(" > ")), a->shortcut().toString(QKeySequence::NativeText)});
    }
}

QVariantMap Hud::GetItems()
{
    m_items.clear();
    ++m_token;
    // While one of the panel's menus is open, the user is in it: don't touch its submenus.
    m_openSubmenus = !QApplication::activePopupWidget();
    m_deadline = QDateTime::currentMSecsSinceEpoch() + kBudgetMs;

    const QList<QAction *> top = m_controller->menuActions();
    for (QAction *a : top) {
        if (!a || !a->isVisible())
            continue;
        if (QMenu *sub = a->menu()) {
            QPointer<QMenu> guard(sub);
            if (m_openSubmenus)
                Q_EMIT sub->aboutToShow();
            if (guard)
                collect(sub, {plainText(a->text())}, 1);
            if (guard && m_openSubmenus)
                Q_EMIT sub->aboutToHide();
        } else if (a->isEnabled() && !plainText(a->text()).isEmpty()) {
            m_items.append({a, plainText(a->text()), QString(), a->shortcut().toString(QKeySequence::NativeText)});
        }
    }

    QStringList labels, paths, shortcuts;
    for (const Item &i : std::as_const(m_items)) {
        labels << i.label;
        paths << i.path;
        shortcuts << i.shortcut;
    }
    Toplevel *t = m_controller->activeWindow();
    qInfo().noquote() << "HUD:" << m_items.size() << "commands of" << m_controller->appName();
    return {
        {QStringLiteral("app"), m_controller->appName()},
        {QStringLiteral("appId"), t ? t->appId() : QString()},
        {QStringLiteral("token"), m_token},
        {QStringLiteral("labels"), labels},
        {QStringLiteral("paths"), paths},
        {QStringLiteral("shortcuts"), shortcuts},
    };
}

bool Hud::Activate(uint token, int index)
{
    if (token != m_token || index < 0 || index >= m_items.size())
        return false;
    QPointer<QAction> action = m_items.at(index).action;
    if (!action || !action->isEnabled())
        return false;
    qInfo().noquote() << "HUD: activate" << m_items.at(index).path << ">" << m_items.at(index).label;
    // After the D-Bus reply, so the HUD isn't waiting while the application opens a dialog
    QTimer::singleShot(0, action, [action] {
        if (action)
            action->trigger();
    });
    return true;
}
