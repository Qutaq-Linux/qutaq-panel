#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QWaylandClientExtensionTemplate>

#include "qwayland-wlr-foreign-toplevel-management-unstable-v1.h"

// Одно окно другого приложения (zwlr_foreign_toplevel_handle_v1).
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
    bool hasParent() const { return m_hasParent; }   // диалог или другое подчинённое окно
    quint64 sequence() const { return m_sequence; }  // порядок появления, растёт
    bool isInitial() const { return m_initial; }     // было открыто ещё до запуска панели
    bool isReady() const { return m_ready; }         // пришёл хотя бы один done

Q_SIGNALS:
    void changed();   // пришёл done — состояние консистентно
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

// Следит за окнами через wlr-foreign-toplevel-management и сообщает об активном.
// Поддерживают: sway, Hyprland, labwc, river, wayfire, niri и др. (KWin и Mutter — нет).
class ToplevelManager : public QWaylandClientExtensionTemplate<ToplevelManager>,
                        public QtWayland::zwlr_foreign_toplevel_manager_v1
{
    Q_OBJECT
public:
    ToplevelManager();
    ~ToplevelManager() override;

    // Последнее активное окно. Не сбрасывается, когда фокус уходит в саму панель
    // (layer-shell поверхность) или на пустой рабочий стол — иначе меню исчезало бы при клике.
    Toplevel *active() const { return m_active; }

    QList<Toplevel *> toplevels() const;

Q_SIGNALS:
    void activeToplevelChanged();
    void toplevelAdded(Toplevel *toplevel);    // после первого done: app_id и родитель известны
    void toplevelRemoved(Toplevel *toplevel);  // перед удалением объекта

protected:
    void zwlr_foreign_toplevel_manager_v1_toplevel(::zwlr_foreign_toplevel_handle_v1 *toplevel) override;
    void zwlr_foreign_toplevel_manager_v1_finished() override;

private:
    QPointer<Toplevel> m_active;
    QList<QPointer<Toplevel>> m_toplevels;
    quint64 m_nextSequence = 0;
    qint64 m_activatedAt = -1; // мс; окна, пришедшие сразу после привязки, — уже открытые
};
