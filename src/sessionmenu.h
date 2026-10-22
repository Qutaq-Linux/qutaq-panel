#pragma once

#include <QObject>
#include <QPointer>

class QAction;
class QDialog;
class QMenu;

// Меню сессии в правом углу панели — как индикатор сессии в Ubuntu Unity:
//
//   Об этом компьютере
//   Параметры системы…
//   ─────────────
//   пользователи (смена через LightDM)
//   ─────────────
//   Завершить сеанс…
//   Ждущий режим
//   Гибернация           (если доступна)
//   Выключить…           (в диалоге — ещё и «Перезагрузить»)
//
// Питание — через logind, пользователи — AccountsService + logind, смена пользователя —
// org.freedesktop.DisplayManager (LightDM). Всё по системной шине D-Bus.
//
// Настройки: ~/.config/qutaq-panel/panel.conf, секция [session]:
//   confirm=true            спрашивать подтверждение перед выходом и выключением
//   showUsers=true          показывать список пользователей
//   showRestart=false       отдельный пункт «Перезагрузить…» (в Unity его нет — он в диалоге выключения)
//   settingsCommand=        команда «Параметров системы»; пусто — найти самому
//   aboutCommand=           команда «Об этом компьютере»; пусто — встроенное окно
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

// Нарисованный значок питания — не зависит от темы значков и виден на тёмной панели.
class QIcon;
QIcon powerIcon(const QColor &color, int size);
