#pragma once

#include <QObject>
#include <QPointer>

class QAction;
class QDialog;
class QMenu;

// Session menu in the right corner of the panel, like the session indicator in Ubuntu Unity:
//
//   About This Computer
//   System Settings…
//   ─────────────
//   users (switching via LightDM)
//   ─────────────
//   Log Out…
//   Suspend
//   Hibernate            (if available)
//   Shut Down…           (the dialog also offers "Restart")
//
// Power goes through logind, users through AccountsService + logind, user switching through
// org.freedesktop.DisplayManager (LightDM). All over the D-Bus system bus.
//
// Settings: ~/.config/qutaq-panel/panel.conf, section [session]:
//   confirm=true            ask for confirmation before logging out and shutting down
//   showUsers=true          show the list of users
//   showRestart=false       a separate "Restart…" item (Unity has none: it is in the shutdown dialog)
//   settingsCommand=        command for "System Settings"; empty means find one
//   aboutCommand=           command for "About This Computer"; empty means the built-in window
class SessionMenu : public QObject
{
    Q_OBJECT
public:
    explicit SessionMenu(QObject *parent = nullptr);
    ~SessionMenu() override;

    QMenu *menu() const { return m_menu; }

private:
    enum class Power { LogOut, Shutdown, Restart };

    void rebuild();
    void addUsers();
    void showAbout();
    void openSettings();
    void confirm(Power what);
    void doLogOut();
    void callLogind(const QString &method);
    void switchToUser(const QString &userName);

    QMenu *m_menu = nullptr;
    QPointer<QDialog> m_dialog;
};

// A drawn power icon: independent of the icon theme and visible on the dark panel.
class QIcon;
QIcon powerIcon(const QColor &color, int size);
