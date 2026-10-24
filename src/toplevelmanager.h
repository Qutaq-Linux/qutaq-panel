#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QWaylandClientExtensionTemplate>

#include "qwayland-wlr-foreign-toplevel-management-unstable-v1.h"

// A window of another application (zwlr_foreign_toplevel_handle_v1).
class Toplevel : public QObject, public QtWayland::zwlr_foreign_toplevel_handle_v1
{
    Q_OBJECT
public:
    explicit Toplevel(::zwlr_foreign_toplevel_handle_v1 *handle);
    ~Toplevel() override;

    QString title() const { return m_title; }
    QString appId() const { return m_appId; }
    bool isActivated() const { return m_activated; }
    bool isMaximized() const { return m_maximized; }
    bool isFullscreen() const { return m_fullscreen; }
    bool isMinimized() const { return m_minimized; }
    bool hasParent() const { return m_hasParent; }   // a dialog or another child window
    quint64 sequence() const { return m_sequence; }  // order of appearance, increasing
    bool isInitial() const { return m_initial; }     // was open before the panel started
    bool isReady() const { return m_ready; }         // at least one done event arrived

Q_SIGNALS:
    void changed();   // done arrived: the state is consistent
    void closed();

protected:
    void zwlr_foreign_toplevel_handle_v1_title(const QString &title) override;
    void zwlr_foreign_toplevel_handle_v1_app_id(const QString &app_id) override;
    void zwlr_foreign_toplevel_handle_v1_state(wl_array *state) override;
    void zwlr_foreign_toplevel_handle_v1_done() override;
    void zwlr_foreign_toplevel_handle_v1_closed() override;
    void zwlr_foreign_toplevel_handle_v1_parent(struct ::zwlr_foreign_toplevel_handle_v1 *parent) override;

private:
    friend class ToplevelManager;
    QString m_title, m_appId;
    bool m_activated = false, m_maximized = false, m_fullscreen = false, m_minimized = false;
    bool m_hasParent = false, m_initial = false, m_ready = false;
    quint64 m_sequence = 0;
};

// Tracks windows via wlr-foreign-toplevel-management and reports the active one.
// Supported by sway, Hyprland, labwc, river, wayfire, niri and others (not KWin or Mutter).
class ToplevelManager : public QWaylandClientExtensionTemplate<ToplevelManager>,
                        public QtWayland::zwlr_foreign_toplevel_manager_v1
{
    Q_OBJECT
public:
    ToplevelManager();
    ~ToplevelManager() override;

    // The last active window. Not reset when the focus moves to the panel itself
    // (a layer-shell surface) or to the empty desktop, otherwise the menu would vanish on click.
    Toplevel *active() const { return m_active; }

    QList<Toplevel *> toplevels() const;

Q_SIGNALS:
    void activeToplevelChanged();
    void toplevelAdded(Toplevel *toplevel);    // after the first done: app_id and parent are known
    void toplevelRemoved(Toplevel *toplevel);  // before the object is deleted

protected:
    void zwlr_foreign_toplevel_manager_v1_toplevel(::zwlr_foreign_toplevel_handle_v1 *toplevel) override;
    void zwlr_foreign_toplevel_manager_v1_finished() override;

private:
    QPointer<Toplevel> m_active;
    QList<QPointer<Toplevel>> m_toplevels;
    quint64 m_nextSequence = 0;
    qint64 m_activatedAt = -1; // ms; windows arriving right after binding are already open
};
