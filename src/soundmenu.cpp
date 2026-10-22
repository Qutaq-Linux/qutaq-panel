#include "soundmenu.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDebug>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

static const char *kMprisPrefix = "org.mpris.MediaPlayer2.";
static const char *kMprisPath = "/org/mpris/MediaPlayer2";
static const char *kMprisRoot = "org.mpris.MediaPlayer2";
static const char *kMprisPlayer = "org.mpris.MediaPlayer2.Player";

static QVariant setting(const QString &key, const QVariant &def)
{
    static QSettings s(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                           + QStringLiteral("/qutaq-panel/panel.conf"),
                       QSettings::IniFormat);
    s.sync();
    return s.value(QStringLiteral("sound/") + key, def);
}

// pactl с английским выводом (по-русски «Name:» стало бы «Имя:»)
static bool pactl(const QStringList &args, QString *out = nullptr, int timeoutMs = 1000)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    p.setProcessEnvironment(env);
    p.start(QStringLiteral("pactl"), args);
    if (!p.waitForFinished(timeoutMs) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        p.kill();
        return false;
    }
    if (out)
        *out = QString::fromUtf8(p.readAllStandardOutput());
    return true;
}

QIcon speakerIcon(const QColor &color, int size, int level, bool muted)
{
    QIcon icon;
    for (qreal dpr : {1.0, 2.0}) {
        QPixmap pm(QSize(size, size) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal u = size / 16.0;

        // Корпус динамика
        QPainterPath body;
        body.moveTo(1.5 * u, 5.5 * u);
        body.lineTo(4.5 * u, 5.5 * u);
        body.lineTo(8 * u, 2.5 * u);
        body.lineTo(8 * u, 13.5 * u);
        body.lineTo(4.5 * u, 10.5 * u);
        body.lineTo(1.5 * u, 10.5 * u);
        body.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(body);

        QPen pen(color, 1.4 * u, Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        if (muted) {
            p.drawLine(QPointF(10.5 * u, 5.5 * u), QPointF(14.5 * u, 10.5 * u));
            p.drawLine(QPointF(14.5 * u, 5.5 * u), QPointF(10.5 * u, 10.5 * u));
        } else {
            // «Волны»: неактивные — полупрозрачные, как в Unity
            for (int i = 1; i <= 3; ++i) {
                QColor c = color;
                if (i > level)
                    c.setAlphaF(0.3);
                p.setPen(QPen(c, 1.4 * u, Qt::SolidLine, Qt::RoundCap));
                const qreal r = (1.6 + 2.2 * i) * u;
                const QRectF rect(8 * u - r + 1.2 * u, 8 * u - r, 2 * r, 2 * r);
                p.drawArc(rect, -50 * 16, 100 * 16);
            }
        }
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

static int levelFor(int volume)
{
    if (volume <= 0) return 0;
    if (volume < 34) return 1;
    if (volume < 67) return 2;
    return 3;
}

// ---------------------------------------------------------------------------

SoundMenu::SoundMenu(QObject *parent)
    : QObject(parent)
    , m_menu(new QMenu)
{
    m_menu->setToolTipsVisible(true);
    connect(m_menu, &QMenu::aboutToShow, this, &SoundMenu::rebuild);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(40);
    connect(m_refreshTimer, &QTimer::timeout, this, &SoundMenu::refresh);

    // Перетаскивание ползунка даёт десятки значений в секунду — отправляем не чаще 25 раз.
    m_applyTimer = new QTimer(this);
    m_applyTimer->setSingleShot(true);
    m_applyTimer->setInterval(40);
    connect(m_applyTimer, &QTimer::timeout, this, [this] {
        if (m_pendingVolume >= 0)
            pactl({QStringLiteral("set-sink-volume"), QStringLiteral("@DEFAULT_SINK@"),
                   QStringLiteral("%1%").arg(m_pendingVolume)});
        m_pendingVolume = -1;
    });

    refresh();
    startSubscribe();
}

SoundMenu::~SoundMenu()
{
    if (m_subscribe) {
        m_subscribe->disconnect(this);
        m_subscribe->kill();
        m_subscribe->waitForFinished(500);
    }
    delete m_menu;
}

void SoundMenu::startSubscribe()
{
    if (!m_subscribe) {
        m_subscribe = new QProcess(this);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        m_subscribe->setProcessEnvironment(env);
        connect(m_subscribe, &QProcess::readyReadStandardOutput, this, [this] {
            while (m_subscribe->canReadLine()) {
                const QByteArray line = m_subscribe->readLine();
                // "Event 'change' on sink #47", "Event 'change' on server #-1" (сменилось устройство)
                if (line.contains(" sink ") || line.contains(" server "))
                    m_refreshTimer->start();
            }
        });
        // Звуковой сервер перезапустили (или его ещё нет) — переподключаемся.
        connect(m_subscribe, &QProcess::finished, this, [this] {
            QTimer::singleShot(3000, this, [this] {
                refresh();
                startSubscribe();
            });
        });
    }
    if (m_subscribe->state() == QProcess::NotRunning)
        m_subscribe->start(QStringLiteral("pactl"), {QStringLiteral("subscribe")});
}

void SoundMenu::refresh()
{
    QString vol, mute, sink;
    const bool ok = pactl({QStringLiteral("get-sink-volume"), QStringLiteral("@DEFAULT_SINK@")}, &vol)
                    && pactl({QStringLiteral("get-sink-mute"), QStringLiteral("@DEFAULT_SINK@")}, &mute);
    pactl({QStringLiteral("get-default-sink")}, &sink);

    const bool wasAvailable = m_available;
    const int oldVolume = m_volume;
    const bool oldMuted = m_muted;

    m_available = ok;
    if (ok) {
        static const QRegularExpression pctRe(QStringLiteral("(\\d+)%"));
        const auto m = pctRe.match(vol);
        m_volume = m.hasMatch() ? m.captured(1).toInt() : 0;
        m_muted = mute.contains(QLatin1String("yes"));
        m_defaultSink = sink.trimmed();
    }

    // Меню открыто — обновить его на месте
    if (m_slider && !m_slider->isSliderDown() && m_pendingVolume < 0) {
        const QSignalBlocker b(m_slider);
        m_slider->setValue(m_volume);
    }
    if (m_muteAction) {
        const QSignalBlocker b(m_muteAction);
        m_muteAction->setChecked(m_muted);
    }

    if (wasAvailable != m_available || oldVolume != m_volume || oldMuted != m_muted || m_menu->menuAction()->icon().isNull()) {
        updateIcon();
        Q_EMIT stateChanged();
    }
}

void SoundMenu::updateIcon()
{
    const QColor fg(0xe8, 0xe8, 0xec);
    QAction *a = m_menu->menuAction();
    if (!m_available) {
        a->setIcon(speakerIcon(QColor(0x7a, 0x7a, 0x82), 16, 0, true));
        a->setToolTip(QStringLiteral("Звуковой сервер недоступен"));
        return;
    }
    a->setIcon(speakerIcon(fg, 16, levelFor(m_volume), m_muted));
    a->setToolTip(m_muted ? QStringLiteral("Звук выключен") : QStringLiteral("Громкость: %1%").arg(m_volume));
}

void SoundMenu::setVolume(int percent)
{
    const int maxVolume = qBound(10, setting(QStringLiteral("maxVolume"), 100).toInt(), 150);
    percent = qBound(0, percent, maxVolume);
    m_pendingVolume = percent;
    if (!m_applyTimer->isActive())
        m_applyTimer->start();
    // Значок — сразу, не дожидаясь ответа сервера
    m_volume = percent;
    updateIcon();
}

void SoundMenu::changeVolume(int deltaPercent)
{
    if (!m_available)
        return;
    if (m_muted && deltaPercent > 0)
        toggleMute(); // как в Unity: прибавить громкость — значит включить звук
    setVolume(m_volume + deltaPercent);
}

void SoundMenu::toggleMute()
{
    pactl({QStringLiteral("set-sink-mute"), QStringLiteral("@DEFAULT_SINK@"), QStringLiteral("toggle")});
    m_refreshTimer->start();
}

QList<SoundMenu::Sink> SoundMenu::listSinks() const
{
    QList<Sink> out;
    QString text;
    if (!pactl({QStringLiteral("list"), QStringLiteral("sinks")}, &text))
        return out;
    Sink cur;
    for (const QString &raw : text.split('\n')) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("Sink #"))) {
            if (!cur.name.isEmpty())
                out << cur;
            cur = {};
        } else if (line.startsWith(QLatin1String("Name:"))) {
            cur.name = line.mid(5).trimmed();
        } else if (line.startsWith(QLatin1String("Description:"))) {
            cur.description = line.mid(12).trimmed();
        }
    }
    if (!cur.name.isEmpty())
        out << cur;
    return out;
}

// ---------- меню ----------

static QWidget *menuRow(QLayout *content, int leftIndent = 28)
{
    auto *w = new QWidget;
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(leftIndent, 4, 12, 4);
    l->addLayout(content);
    return w;
}

void SoundMenu::rebuild()
{
    refresh();
    m_menu->clear();
    m_muteAction = nullptr;

    if (!m_available) {
        m_menu->addAction(QStringLiteral("Звуковой сервер недоступен"))->setEnabled(false);
    } else {
        m_muteAction = m_menu->addAction(QStringLiteral("Выключить звук"));
        m_muteAction->setCheckable(true);
        m_muteAction->setChecked(m_muted);
        connect(m_muteAction, &QAction::triggered, this, [this] { toggleMute(); });

        // Ползунок громкости между значками тихо/громко
        const QColor fg = m_menu->palette().color(QPalette::WindowText);
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        auto *low = new QLabel;
        low->setPixmap(speakerIcon(fg, 16, 0, false).pixmap(16, 16));
        auto *high = new QLabel;
        high->setPixmap(speakerIcon(fg, 16, 3, false).pixmap(16, 16));
        auto *slider = new QSlider(Qt::Horizontal);
        slider->setObjectName(QStringLiteral("volume"));
        slider->setRange(0, qBound(10, setting(QStringLiteral("maxVolume"), 100).toInt(), 150));
        slider->setPageStep(10);
        slider->setMinimumWidth(180);
        slider->setValue(m_volume);
        slider->setFocusPolicy(Qt::StrongFocus);
        connect(slider, &QSlider::valueChanged, this, [this](int v) {
            if (m_muted && v > 0)
                toggleMute();
            setVolume(v);
        });
        row->addWidget(low);
        row->addWidget(slider, 1);
        row->addWidget(high);
        auto *wa = new QWidgetAction(m_menu);
        wa->setDefaultWidget(menuRow(row, 12));
        m_menu->addAction(wa);
        m_slider = slider;
    }

    addPlayers();

    m_menu->addSeparator();
    const QList<Sink> sinks = m_available ? listSinks() : QList<Sink>();
    if (sinks.size() > 1) {
        QMenu *out = m_menu->addMenu(QStringLiteral("Устройство вывода"));
        auto *group = new QActionGroup(out);
        for (const Sink &s : sinks) {
            QAction *a = out->addAction(s.description.isEmpty() ? s.name : s.description);
            a->setCheckable(true);
            a->setChecked(s.name == m_defaultSink);
            group->addAction(a);
            const QString name = s.name;
            connect(a, &QAction::triggered, this, [this, name] {
                pactl({QStringLiteral("set-default-sink"), name});
                m_refreshTimer->start();
            });
        }
    }
    m_menu->addAction(QStringLiteral("Параметры звука…"), this, &SoundMenu::openSettings);
}

void SoundMenu::openSettings()
{
    QString cmd = setting(QStringLiteral("settingsCommand"), QString()).toString();
    if (cmd.isEmpty()) {
        for (const char *app : {"pavucontrol", "pavucontrol-qt", "pwvucontrol", "xfce4-pulseaudio-plugin"}) {
            if (!QStandardPaths::findExecutable(QLatin1String(app)).isEmpty()) {
                cmd = QLatin1String(app);
                break;
            }
        }
    }
    if (cmd.isEmpty()) {
        qWarning() << "no sound settings program found (install pavucontrol)";
        return;
    }
    QStringList args = QProcess::splitCommand(cmd);
    const QString program = args.takeFirst();
    QProcess::startDetached(program, args);
}

// ---------- проигрыватели (MPRIS) ----------

static QVariantMap mprisProps(const QString &service, const QString &iface)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(service, kMprisPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                      QStringLiteral("GetAll"));
    msg << iface;
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 300);
    QVariantMap out;
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
        reply.arguments().first().value<QDBusArgument>() >> out;
    return out;
}

static QVariant unwrapVariant(QVariant v)
{
    while (v.userType() == qMetaTypeId<QDBusVariant>())
        v = v.value<QDBusVariant>().variant();
    return v;
}

static void mprisCall(const QString &service, const QString &iface, const QString &method)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(service, kMprisPath, iface, method);
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
}

// «<b>Название</b><br>Исполнитель — Альбом» из свойств org.mpris.MediaPlayer2.Player
static QString mprisTrackText(const QVariantMap &player)
{
    QVariantMap meta;
    const QVariant metaVar = unwrapVariant(player.value(QStringLiteral("Metadata")));
    if (metaVar.canConvert<QDBusArgument>())
        metaVar.value<QDBusArgument>() >> meta;
    else
        meta = metaVar.toMap();
    const QString title = unwrapVariant(meta.value(QStringLiteral("xesam:title"))).toString();
    if (title.isEmpty())
        return {};
    const QStringList artists = unwrapVariant(meta.value(QStringLiteral("xesam:artist"))).toStringList();
    const QString album = unwrapVariant(meta.value(QStringLiteral("xesam:album"))).toString();
    QStringList sub;
    if (!artists.isEmpty())
        sub << artists.join(QStringLiteral(", "));
    if (!album.isEmpty())
        sub << album;
    QString text = QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped());
    if (!sub.isEmpty())
        text += QStringLiteral("<br>") + sub.join(QStringLiteral(" — ")).toHtmlEscaped();
    return text;
}

void SoundMenu::addPlayers()
{
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (!bus)
        return;
    QStringList players;
    for (const QString &name : bus->registeredServiceNames().value())
        if (name.startsWith(QLatin1String(kMprisPrefix)))
            players << name;
    players.sort();

    for (const QString &service : std::as_const(players)) {
        const QVariantMap root = mprisProps(service, kMprisRoot);
        const QVariantMap player = mprisProps(service, kMprisPlayer);
        if (root.isEmpty() && player.isEmpty())
            continue;

        m_menu->addSeparator();

        // Заголовок: значок и имя программы; нажатие поднимает её окно
        QString identity = unwrapVariant(root.value(QStringLiteral("Identity"))).toString();
        if (identity.isEmpty())
            identity = service.mid(int(strlen(kMprisPrefix))).section('.', 0, 0);
        const QString desktop = unwrapVariant(root.value(QStringLiteral("DesktopEntry"))).toString();
        QIcon icon = QIcon::fromTheme(desktop);
        if (icon.isNull())
            icon = QIcon::fromTheme(QStringLiteral("multimedia-player"));
        QAction *head = m_menu->addAction(icon, identity);
        head->setIconVisibleInMenu(true);
        if (unwrapVariant(root.value(QStringLiteral("CanRaise"))).toBool())
            connect(head, &QAction::triggered, this, [service] { mprisCall(service, kMprisRoot, QStringLiteral("Raise")); });

        // Текущий трек
        QPointer<QLabel> trackLabel;
        const QString trackText = mprisTrackText(player);
        if (!trackText.isEmpty()) {
            auto *label = new QLabel(trackText);
            label->setObjectName(QStringLiteral("track"));
            label->setTextFormat(Qt::RichText);
            label->setMaximumWidth(260);
            label->setWordWrap(true);
            auto *row = new QHBoxLayout;
            row->addWidget(label);
            auto *wa = new QWidgetAction(m_menu);
            wa->setDefaultWidget(menuRow(row));
            m_menu->addAction(wa);
            trackLabel = label;
        }

        // Кнопки ⏮ ⏯ ⏭
        if (player.isEmpty())
            continue;
        const bool canControl = unwrapVariant(player.value(QStringLiteral("CanControl"), true)).toBool();
        auto *row = new QHBoxLayout;
        row->setSpacing(6);
        row->addStretch();
        QList<QToolButton *> buttons;
        auto button = [&](QStyle::StandardPixmap pm, const QString &tip, const QString &method, bool enabled) {
            auto *b = new QToolButton;
            b->setIcon(QApplication::style()->standardIcon(pm));
            b->setIconSize(QSize(20, 20));
            b->setAutoRaise(true);
            b->setToolTip(tip);
            b->setObjectName(method);
            b->setEnabled(canControl && enabled);
            connect(b, &QToolButton::clicked, this, [service, method] { mprisCall(service, kMprisPlayer, method); });
            buttons << b;
            row->addWidget(b);
            return b;
        };
        const bool playing = unwrapVariant(player.value(QStringLiteral("PlaybackStatus"))).toString() == QLatin1String("Playing");
        button(QStyle::SP_MediaSkipBackward, QStringLiteral("Предыдущий"), QStringLiteral("Previous"),
               unwrapVariant(player.value(QStringLiteral("CanGoPrevious"))).toBool());
        QToolButton *play = button(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay,
                                   playing ? QStringLiteral("Пауза") : QStringLiteral("Воспроизвести"),
                                   QStringLiteral("PlayPause"),
                                   unwrapVariant(player.value(QStringLiteral("CanPlay"))).toBool()
                                       || unwrapVariant(player.value(QStringLiteral("CanPause"))).toBool());
        button(QStyle::SP_MediaSkipForward, QStringLiteral("Следующий"), QStringLiteral("Next"),
               unwrapVariant(player.value(QStringLiteral("CanGoNext"))).toBool());
        row->addStretch();

        // После нажатия — обновить кнопку «пуск/пауза» и название трека, не закрывая меню
        QPointer<QToolButton> pp = play;
        for (QToolButton *b : std::as_const(buttons)) {
            connect(b, &QToolButton::clicked, this, [pp, trackLabel, service] {
                QTimer::singleShot(300, pp, [pp, trackLabel, service] {
                    if (!pp)
                        return;
                    const QVariantMap props = mprisProps(service, kMprisPlayer);
                    const bool on = unwrapVariant(props.value(QStringLiteral("PlaybackStatus"))).toString()
                                    == QLatin1String("Playing");
                    pp->setIcon(QApplication::style()->standardIcon(on ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
                    pp->setToolTip(on ? QStringLiteral("Пауза") : QStringLiteral("Воспроизвести"));
                    if (trackLabel)
                        trackLabel->setText(mprisTrackText(props));
                });
            });
        }

        auto *wa = new QWidgetAction(m_menu);
        wa->setDefaultWidget(menuRow(row));
        m_menu->addAction(wa);
    }
}
