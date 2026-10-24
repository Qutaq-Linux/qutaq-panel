#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QAction;
class QMenu;
class QProcess;
class QSlider;
class QTimer;

// Sound indicator left of the clock, as in Ubuntu Unity:
//
//   ☐ Mute
//   🔈 ━━━━━━●━━━━ 🔊
//   ─────────────
//   Player (MPRIS): name, track, ⏮ ⏯ ⏭
//   ─────────────
//   Output Device ▸         (if there is more than one device)
//   Sound Settings…
//
// On the icon: the mouse wheel changes the volume, the middle button mutes/unmutes.
//
// Sound goes through `pactl` (works with both PipeWire and PulseAudio); changes are tracked
// with `pactl subscribe`. Players go through MPRIS (org.mpris.MediaPlayer2.*).
//
// Settings: ~/.config/qutaq-panel/panel.conf, section [sound]:
//   step=5             volume step for the mouse wheel, %
//   maxVolume=100      upper volume limit, %
//   settingsCommand=   "Sound Settings…"; empty means pavucontrol, pavucontrol-qt and the like
class SoundMenu : public QObject
{
    Q_OBJECT
public:
    explicit SoundMenu(QObject *parent = nullptr);
    ~SoundMenu() override;

    QMenu *menu() const { return m_menu; }

    int volume() const { return m_volume; }
    bool isMuted() const { return m_muted; }
    bool isAvailable() const { return m_available; }

    void changeVolume(int deltaPercent);
    void setVolume(int percent);
    void toggleMute();

Q_SIGNALS:
    void stateChanged();

private:
    struct Sink {
        QString name;
        QString description;
    };

    void startSubscribe();
    void refresh();
    void updateIcon();
    void rebuild();
    void addPlayers();
    QList<Sink> listSinks() const;
    void openSettings();

    QMenu *m_menu = nullptr;
    QAction *m_muteAction = nullptr;
    QPointer<QSlider> m_slider;
    QProcess *m_subscribe = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QTimer *m_applyTimer = nullptr;
    int m_pendingVolume = -1;

    bool m_available = false;
    int m_volume = 0;
    bool m_muted = false;
    QString m_defaultSink;
};

// Speaker icon: level 0–3 (number of "waves"), muted adds a cross.
class QIcon;
class QColor;
QIcon speakerIcon(const QColor &color, int size, int level, bool muted);
