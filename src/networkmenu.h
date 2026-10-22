#pragma once

#include <QDBusMessage>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

class QDialog;
class QMenu;
class QTimer;

// Индикатор сети слева от громкости — как nm-applet в панели Ubuntu Unity:
//
//   Проводная сеть               (заголовок)
//   ✓ Wired connection 1
//     Отключить
//   ─────────────
//   Беспроводная сеть            (заголовок)
//   ✓ HomeWiFi            📶🔒
//     CafeFree            📶
//     Другие сети ▸
//     Отключить
//   ─────────────
//   VPN-соединения ▸
//   ─────────────
//   ✓ Включить сеть
//   ✓ Включить Wi-Fi
//   ─────────────
//   Сведения о соединении
//   Изменить соединения…
//
// Всё — через NetworkManager по системной шине D-Bus. К новой защищённой сети Wi-Fi
// панель сама спрашивает пароль (WPA/WPA2/WPA3-Personal).
//
// Настройки: ~/.config/qutaq-panel/panel.conf, секция [network]:
//   editorCommand=   «Изменить соединения…»; пусто — nm-connection-editor
//   maxNetworks=6    сколько сетей Wi-Fi показывать сразу (остальные — в «Другие сети»)
class NetworkMenu : public QObject
{
    Q_OBJECT
public:
    explicit NetworkMenu(QObject *parent = nullptr);
    ~NetworkMenu() override;

    QMenu *menu() const { return m_menu; }

private Q_SLOTS:
    void onNmSignal(const QDBusMessage &msg);

private:
    void rebuild();
    void updateIcon();
    void addWiredSection(const QString &device);
    void addWifiSection(const QString &device);
    void addVpnMenu();
    void connectToAccessPoint(const QString &device, const QString &ap, const QString &ssid, bool secured, bool sae);
    void showInfo();
    void openEditor(const QStringList &extraArgs = {});

    QMenu *m_menu = nullptr;
    QTimer *m_updateTimer = nullptr;
    QPointer<QDialog> m_info;
};

// Значки индикатора. kind: 0 — нет сети, 1 — проводная, 2 — Wi-Fi.
// level — уровень Wi-Fi 0–4; dim — соединения нет или идёт подключение; vpn — замочек.
class QIcon;
class QColor;
QIcon networkIcon(const QColor &color, int size, int kind, int level, bool dim, bool vpn);
