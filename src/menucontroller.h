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

// Связывает три источника: активное окно (Wayland), регистратор меню (D-Bus)
// и импортёр меню выбранного приложения. Панели только отображают результат.
class MenuController : public QObject
{
    Q_OBJECT
public:
    explicit MenuController(QObject *parent = nullptr);
    bool start();

    QString appName() const { return m_appName; }
    QMenu *windowMenu() const { return m_windowMenu; }   // «Свернуть / Развернуть / Закрыть»
    QList<QAction *> menuActions() const;                 // верхний уровень меню приложения
    bool hasToplevelTracking() const;
    Toplevel *activeWindow() const;                       // последнее активное окно (или nullptr)

    // Для кнопок окна в панели (как у развёрнутого окна в Unity)
    bool activeMaximized() const;                         // развёрнуто, не свёрнуто, не во весь экран
    void closeActive();
    void minimizeActive();
    void restoreActive();

Q_SIGNALS:
    void changed();
    void activeWindowChanged(); // другое окно или изменилось его состояние (развёрнуто и т. п.)

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
    QList<MenuEntry> m_candidates;   // подходящие меню, лучшее первым
    int m_candidateIndex = -1;
    QStringList m_candidateKeys;
    QString m_importerKey;

    // Привязка окон к объектам меню на шине (см. menucontroller.cpp)
    QHash<Toplevel *, QString> m_pairs;      // окно → MenuEntry::key()
    QList<QPointer<Toplevel>> m_pending;     // ждут привязки
    QPointer<Toplevel> m_lastPaired;         // последнее активное окно со своим меню
    QSet<QString> m_emptyKeys;               // объекты меню без единого пункта
    QSet<QString> m_filledKeys;              // объекты меню, где пункты есть
    QTimer *m_pairTimer = nullptr;
    int m_pairAttempts = 0;
    QSet<QString> m_loggedKeys;

    // Поиск меню на шине без регистрации (Qt под Wayland)
    QHash<QString, QStringList> m_busNames;  // уникальное имя → имена процесса
    QSet<QString> m_deadServices;            // не отвечают на Introspect
    QSet<QString> m_answeredServices;        // хоть раз ответили
    QHash<QString, int> m_silentStrikes;
    QString m_scanAppId;
    QList<MenuEntry> m_scanResult;
    QTimer *m_rescanTimer = nullptr;
    int m_rescanAttempts = 0;
};

QString desktopEntryName(const QString &appId); // Name= из .desktop, если найдётся
