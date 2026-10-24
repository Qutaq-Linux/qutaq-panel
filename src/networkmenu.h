#pragma once

#include <QDBusMessage>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

class QDialog;
class QMenu;
class QTimer;

// Network indicator left of the volume, like nm-applet in the Ubuntu Unity panel:
//
//   Wired Network                (header)
//   ✓ Wired connection 1
//     Disconnect
//   ─────────────
//   Wireless Network             (header)
//   ✓ HomeWiFi            📶🔒
//     CafeFree            📶
//     More Networks ▸
//     Disconnect
//   ─────────────
//   VPN Connections ▸
//   ─────────────
//   ✓ Enable Networking
//   ✓ Enable Wi-Fi
//   ─────────────
//   Connection Information
//   Edit Connections…
//
// Everything goes through NetworkManager over the D-Bus system bus. For a new secured Wi-Fi
// network the panel asks for the password itself (WPA/WPA2/WPA3-Personal).
//
// Settings: ~/.config/qutaq-panel/panel.conf, section [network]:
//   editorCommand=   "Edit Connections…"; empty means nm-connection-editor
//   maxNetworks=6    how many Wi-Fi networks to show at once (the rest go to "More Networks")
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

// Indicator icons. kind: 0 no network, 1 wired, 2 Wi-Fi.
// level: Wi-Fi level 0–4; dim: no connection or connecting; vpn: padlock.
class QIcon;
class QColor;
QIcon networkIcon(const QColor &color, int size, int kind, int level, bool dim, bool vpn);
