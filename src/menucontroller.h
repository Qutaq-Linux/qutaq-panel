#pragma once

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QSet>

#include "appmenuregistrar.h"

class MenuImporter;
class QAction;
class QMenu;
class QTimer;
class Toplevel;
class ToplevelManager;

// Ties three sources together: the active window (Wayland), the menu registrar (D-Bus)
// and the menu importer of the chosen application. Panels only display the result.
class MenuController : public QObject
{
    Q_OBJECT
public:
    explicit MenuController(QObject *parent = nullptr);
    bool start();

    QString appName() const { return m_appName; }
    QMenu *windowMenu() const { return m_windowMenu; }   // "Minimize / Maximize / Close"
    QList<QAction *> menuActions() const;                 // top level of the application menu
    bool hasToplevelTracking() const;
    Toplevel *activeWindow() const;                       // the last active window (or nullptr)

    // For the window buttons in the panel (like a maximized window in Unity)
    bool activeMaximized() const;                         // maximized, not minimized, not full screen
    void closeActive();
    void minimizeActive();
    void restoreActive();

Q_SIGNALS:
    void changed();
    void activeWindowChanged(); // another window, or its state changed (maximized etc.)

private:
    void reevaluate();
    void tryNextCandidate();
    QList<MenuEntry> candidatesFor(const QString &appId);
    QList<MenuEntry> scanBus(const QString &appId, bool force = false);
    void onToplevelAdded(Toplevel *t);
    void onToplevelRemoved(Toplevel *t);
    void pairPending();
    QSet<QString> pairedKeys(Toplevel *except = nullptr) const;
    QList<MenuEntry> findMenusOf(const QString &service, bool *ok = nullptr);
    void rebuildWindowMenu();

    AppMenuRegistrar *m_registrar = nullptr;
    ToplevelManager *m_toplevels = nullptr;
    QPointer<MenuImporter> m_importer;
    QMenu *m_windowMenu = nullptr;
    QString m_appName;
    QString m_lastAppId;
    QList<MenuEntry> m_candidates;   // matching menus, best first
    int m_candidateIndex = -1;
    QStringList m_candidateKeys;
    QString m_importerKey;

    // Pairing windows with menu objects on the bus (see menucontroller.cpp)
    QHash<Toplevel *, QString> m_pairs;      // window → MenuEntry::key()
    QList<QPointer<Toplevel>> m_pending;     // waiting to be paired
    QPointer<Toplevel> m_lastPaired;         // the last active window with its own menu
    QSet<QString> m_emptyKeys;               // menu objects without a single item
    QSet<QString> m_filledKeys;              // menu objects that do have items
    QTimer *m_pairTimer = nullptr;
    int m_pairAttempts = 0;
    QSet<QString> m_loggedKeys;

    // Finding menus on the bus without registration (Qt on Wayland)
    QHash<QString, QStringList> m_busNames;  // unique name → process names
    QSet<QString> m_deadServices;            // don't answer Introspect
    QSet<QString> m_answeredServices;        // answered at least once
    QHash<QString, int> m_silentStrikes;
    QString m_scanAppId;
    QList<MenuEntry> m_scanResult;
    QTimer *m_rescanTimer = nullptr;
    int m_rescanAttempts = 0;
};

QString desktopEntryName(const QString &appId); // Name= from the .desktop file, if found
