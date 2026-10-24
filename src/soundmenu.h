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

// Индикатор звука слева от часов — как в Ubuntu Unity:
//
//   ☐ Выключить звук
//   🔈 ━━━━━━●━━━━ 🔊
//   ─────────────
//   Проигрыватель (MPRIS): название, трек, ⏮ ⏯ ⏭
//   ─────────────
//   Устройство вывода ▸     (если устройств больше одного)
//   Параметры звука…
//
// На значке: колесо мыши — громкость, средняя кнопка — выключить/включить звук.
//
// Звук — через `pactl` (работает и с PipeWire, и с PulseAudio), изменения отслеживаются
// через `pactl subscribe`. Проигрыватели — через MPRIS (org.mpris.MediaPlayer2.*).
//
// Настройки: ~/.config/qutaq-panel/panel.conf, секция [sound]:
//   step=5             шаг громкости колесом мыши, %
//   maxVolume=100      верхний предел громкости, %
//   settingsCommand=   «Параметры звука…»; пусто — pavucontrol, pavucontrol-qt и т. п.
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

// Значок динамика: level 0–3 (число «волн»), muted — с крестиком.
class QIcon;
class QColor;
QIcon speakerIcon(const QColor &color, int size, int level, bool muted);
