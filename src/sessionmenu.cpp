#include "sessionmenu.h"

#include <QCoreApplication>
#include <QAction>
#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>
#include <QVBoxLayout>

#include <pwd.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

static const char *kLogind = "org.freedesktop.login1";
static const char *kLogindPath = "/org/freedesktop/login1";
static const char *kLogindManager = "org.freedesktop.login1.Manager";
static const char *kAccounts = "org.freedesktop.Accounts";
static const char *kDisplayManager = "org.freedesktop.DisplayManager";

static QSettings &settings()
{
    static QSettings s(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                           + QStringLiteral("/qutaq-panel/panel.conf"),
                       QSettings::IniFormat);
    return s;
}

static QVariant setting(const QString &key, const QVariant &def)
{
    settings().sync();
    return settings().value(QStringLiteral("session/") + key, def);
}

static QDBusConnection systemBus()
{
    return QDBusConnection::systemBus();
}

static bool serviceAvailable(const QString &name)
{
    QDBusConnection bus = systemBus();
    return bus.isConnected() && bus.interface() && bus.interface()->isServiceRegistered(name).value();
}

// logind: CanPowerOff etc. answer "yes", "no", "challenge" (will ask for a password) or "na"
static bool logindCan(const QString &what)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(kLogind, kLogindPath, kLogindManager,
                                                      QStringLiteral("Can") + what);
    const QDBusMessage reply = systemBus().call(msg, QDBus::Block, 500);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return false;
    const QString v = reply.arguments().first().toString();
    return v == QLatin1String("yes") || v == QLatin1String("challenge");
}

QIcon powerIcon(const QColor &color, int size)
{
    QIcon icon;
    for (qreal dpr : {1.0, 2.0}) {
        QPixmap pm(QSize(size, size) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(color, qMax(1.5, size / 9.0), Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        const qreal m = size * 0.18;
        const QRectF r(m, m, size - 2 * m, size - 2 * m);
        p.drawArc(r, (90 + 35) * 16, (360 - 70) * 16); // a circle with a gap at the top
        p.drawLine(QPointF(size / 2.0, size * 0.1), QPointF(size / 2.0, size * 0.48));
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

static QIcon personIcon()
{
    for (const char *name : {"avatar-default", "avatar-default-symbolic", "user-identity", "system-users"}) {
        const QIcon i = QIcon::fromTheme(QLatin1String(name));
        if (!i.isNull())
            return i;
    }
    // Draw it ourselves: head and shoulders
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x5a, 0x8d, 0xd6));
    p.drawEllipse(QRectF(0, 0, 32, 32));
    p.setBrush(Qt::white);
    p.drawEllipse(QRectF(11, 6, 10, 10));
    QPainterPath body;
    body.moveTo(6, 27);
    body.cubicTo(7, 18, 25, 18, 26, 27);
    body.closeSubpath();
    p.drawPath(body);
    return QIcon(pm);
}

// ---------------------------------------------------------------------------

SessionMenu::SessionMenu(QObject *parent)
    : QObject(parent)
    , m_menu(new QMenu)
{
    m_menu->menuAction()->setIcon(powerIcon(QColor(0xe8, 0xe8, 0xec), 16));
    m_menu->menuAction()->setToolTip(tr("Session"));
    m_menu->setToolTipsVisible(true);
    // The state (who is logged in, whether hibernation is available) changes, so build it on every opening.
    connect(m_menu, &QMenu::aboutToShow, this, &SessionMenu::rebuild);
    rebuild();
}

SessionMenu::~SessionMenu()
{
    delete m_menu;
}

void SessionMenu::rebuild()
{
    m_menu->clear();

    m_menu->addAction(tr("About This Computer"), this, &SessionMenu::showAbout);
    QAction *settingsAction = m_menu->addAction(tr("System Settings…"), this, &SessionMenu::openSettings);
    const QString cmd = setting(QStringLiteral("settingsCommand"), QString()).toString();
    if (cmd.isEmpty()) {
        bool found = false;
        for (const char *app : {"xfce4-settings-manager", "systemsettings", "gnome-control-center",
                                "lxqt-config", "mate-control-center", "cinnamon-settings"})
            found = found || !QStandardPaths::findExecutable(QLatin1String(app)).isEmpty();
        settingsAction->setEnabled(found);
    }

    if (setting(QStringLiteral("showUsers"), true).toBool()) {
        m_menu->addSeparator();
        addUsers();
    }

    m_menu->addSeparator();
    m_menu->addAction(tr("Log Out…"), this, [this] { confirm(Power::LogOut); });

    const bool logind = serviceAvailable(kLogind);
    if (logind && logindCan(QStringLiteral("Suspend")))
        m_menu->addAction(tr("Suspend"), this, [this] { callLogind(QStringLiteral("Suspend")); });
    if (logind && logindCan(QStringLiteral("Hibernate")))
        m_menu->addAction(tr("Hibernate"), this, [this] { callLogind(QStringLiteral("Hibernate")); });
    if (setting(QStringLiteral("showRestart"), false).toBool()) {
        QAction *a = m_menu->addAction(tr("Restart…"), this, [this] { confirm(Power::Restart); });
        a->setEnabled(logind && logindCan(QStringLiteral("Reboot")));
    }
    QAction *off = m_menu->addAction(tr("Shut Down…"), this, [this] { confirm(Power::Shutdown); });
    off->setEnabled(logind && (logindCan(QStringLiteral("PowerOff")) || logindCan(QStringLiteral("Reboot"))));
}

// ---------- users ----------

struct UserInfo {
    QString name;      // login
    QString realName;
    QString iconFile;
    uint uid = 0;
};

static QList<UserInfo> listUsers()
{
    QList<UserInfo> out;
    QDBusMessage msg = QDBusMessage::createMethodCall(kAccounts, QStringLiteral("/org/freedesktop/Accounts"),
                                                      kAccounts, QStringLiteral("ListCachedUsers"));
    const QDBusMessage reply = systemBus().call(msg, QDBus::Block, 500);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        QList<QDBusObjectPath> paths;
        reply.arguments().first().value<QDBusArgument>() >> paths;
        for (const QDBusObjectPath &path : paths) {
            QDBusMessage get = QDBusMessage::createMethodCall(kAccounts, path.path(),
                                                              QStringLiteral("org.freedesktop.DBus.Properties"),
                                                              QStringLiteral("GetAll"));
            get << QStringLiteral("org.freedesktop.Accounts.User");
            const QDBusMessage props = systemBus().call(get, QDBus::Block, 500);
            if (props.type() != QDBusMessage::ReplyMessage || props.arguments().isEmpty())
                continue;
            QVariantMap p;
            props.arguments().first().value<QDBusArgument>() >> p;
            if (p.value(QStringLiteral("SystemAccount")).toBool())
                continue;
            UserInfo u;
            u.name = p.value(QStringLiteral("UserName")).toString();
            u.realName = p.value(QStringLiteral("RealName")).toString();
            u.iconFile = p.value(QStringLiteral("IconFile")).toString();
            u.uid = p.value(QStringLiteral("Uid")).toUInt();
            if (!u.name.isEmpty())
                out << u;
        }
        return out;
    }

    // No AccountsService: regular users from /etc/passwd
    setpwent();
    while (passwd *pw = getpwent()) {
        const QString shell = QString::fromLocal8Bit(pw->pw_shell);
        if (pw->pw_uid < 1000 || pw->pw_uid >= 60000 || shell.endsWith(QLatin1String("nologin"))
            || shell.endsWith(QLatin1String("false")))
            continue;
        UserInfo u;
        u.name = QString::fromLocal8Bit(pw->pw_name);
        u.realName = QString::fromLocal8Bit(pw->pw_gecos).section(',', 0, 0);
        u.uid = pw->pw_uid;
        out << u;
    }
    endpwent();
    return out;
}

static QSet<uint> loggedInUids()
{
    QSet<uint> out;
    QDBusMessage msg = QDBusMessage::createMethodCall(kLogind, kLogindPath, kLogindManager, QStringLiteral("ListUsers"));
    const QDBusMessage reply = systemBus().call(msg, QDBus::Block, 500);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return out;
    const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        uint uid = 0;
        QString name;
        QDBusObjectPath path;
        arg.beginStructure();
        arg >> uid >> name >> path;
        arg.endStructure();
        out.insert(uid);
    }
    arg.endArray();
    return out;
}

static QString seatPath()
{
    return qEnvironmentVariable("XDG_SEAT_PATH", QStringLiteral("/org/freedesktop/DisplayManager/Seat0"));
}

void SessionMenu::addUsers()
{
    QList<UserInfo> users = listUsers();
    std::sort(users.begin(), users.end(), [](const UserInfo &a, const UserInfo &b) {
        const QString na = a.realName.isEmpty() ? a.name : a.realName;
        const QString nb = b.realName.isEmpty() ? b.name : b.realName;
        return na.localeAwareCompare(nb) < 0;
    });
    const QSet<uint> loggedIn = loggedInUids();
    const bool canSwitch = serviceAvailable(kDisplayManager);
    const uint me = getuid();

    for (const UserInfo &u : std::as_const(users)) {
        auto *a = m_menu->addAction(u.realName.isEmpty() ? u.name : u.realName);
        QIcon icon;
        if (!u.iconFile.isEmpty() && QFileInfo(u.iconFile).isReadable())
            icon = QIcon(u.iconFile);
        a->setIcon(icon.isNull() || icon.pixmap(16).isNull() ? personIcon() : icon);
        a->setIconVisibleInMenu(true);
        if (loggedIn.contains(u.uid) || u.uid == me) {
            QFont f = a->font();
            f.setBold(true);
            a->setFont(f);
        }
        if (u.uid == me) {
            a->setCheckable(true);
            a->setChecked(true);
            a->setToolTip(tr("Current session"));
            continue;
        }
        if (!canSwitch) {
            a->setEnabled(false);
            a->setToolTip(tr("Switching users works when you log in with LightDM"));
            continue;
        }
        const QString name = u.name;
        connect(a, &QAction::triggered, this, [this, name] { switchToUser(name); });
    }

    if (canSwitch) {
        QDBusInterface seat(kDisplayManager, seatPath(), QStringLiteral("org.freedesktop.DisplayManager.Seat"), systemBus());
        if (seat.property("HasGuestAccount").toBool()) {
            m_menu->addAction(personIcon(), tr("Guest Session"), this, [] {
                QDBusMessage msg = QDBusMessage::createMethodCall(kDisplayManager, seatPath(),
                                                                  QStringLiteral("org.freedesktop.DisplayManager.Seat"),
                                                                  QStringLiteral("SwitchToGuest"));
                msg << QString();
                systemBus().call(msg, QDBus::NoBlock);
            });
        }
    }
}

void SessionMenu::switchToUser(const QString &userName)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(kDisplayManager, seatPath(),
                                                      QStringLiteral("org.freedesktop.DisplayManager.Seat"),
                                                      QStringLiteral("SwitchToUser"));
    msg << userName << QString();
    const QDBusMessage reply = systemBus().call(msg, QDBus::Block, 2000);
    if (reply.type() == QDBusMessage::ErrorMessage)
        qWarning().noquote() << "SwitchToUser" << userName << reply.errorMessage();
    else
        qInfo().noquote() << "switching to user" << userName;
}

// ---------- power and logout ----------

void SessionMenu::callLogind(const QString &method)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(kLogind, kLogindPath, kLogindManager, method);
    msg << true; // interactive: let polkit ask for a password if needed
    const QDBusMessage reply = systemBus().call(msg, QDBus::Block, 5000);
    if (reply.type() == QDBusMessage::ErrorMessage)
        qWarning().noquote() << "logind" << method << reply.errorMessage();
    else
        qInfo().noquote() << "logind" << method;
}

void SessionMenu::doLogOut()
{
    qInfo() << "logging out";
    // labwc: SIGTERM to the compositor itself (`labwc --exit` does the same)
    const qint64 labwc = qEnvironmentVariable("LABWC_PID").toLongLong();
    if (labwc > 1) {
        ::kill(pid_t(labwc), SIGTERM);
        return;
    }
    if (qEnvironmentVariableIsSet("SWAYSOCK") && QProcess::startDetached(QStringLiteral("swaymsg"), {QStringLiteral("exit")}))
        return;
    if (qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE")
        && QProcess::startDetached(QStringLiteral("hyprctl"), {QStringLiteral("dispatch"), QStringLiteral("exit")}))
        return;
    // Otherwise close the whole session through logind
    const QString session = qEnvironmentVariable("XDG_SESSION_ID");
    if (!session.isEmpty()) {
        QDBusMessage msg = QDBusMessage::createMethodCall(kLogind, kLogindPath, kLogindManager,
                                                          QStringLiteral("TerminateSession"));
        msg << session;
        systemBus().call(msg, QDBus::NoBlock);
    }
}

void SessionMenu::confirm(Power what)
{
    const bool canReboot = logindCan(QStringLiteral("Reboot"));
    const bool canOff = logindCan(QStringLiteral("PowerOff"));

    if (!setting(QStringLiteral("confirm"), true).toBool()) {
        if (what == Power::LogOut) doLogOut();
        else if (what == Power::Restart) callLogind(QStringLiteral("Reboot"));
        else callLogind(canOff ? QStringLiteral("PowerOff") : QStringLiteral("Reboot"));
        return;
    }
    if (m_dialog)
        m_dialog->close();

    auto *dlg = new QDialog;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setObjectName(QStringLiteral("session-confirm"));
    m_dialog = dlg;

    QString title, text;
    switch (what) {
    case Power::LogOut:
        title = tr("Log Out", "dialog title");
        text = tr("Log out %1?").arg(QString::fromLocal8Bit(qgetenv("USER")));
        break;
    case Power::Restart:
        title = tr("Restart", "dialog title");
        text = tr("Restart the computer?");
        break;
    case Power::Shutdown:
        title = tr("Shut Down", "dialog title");
        text = tr("Shut down the computer?");
        break;
    }
    dlg->setWindowTitle(title);

    auto *icon = new QLabel;
    icon->setPixmap(powerIcon(dlg->palette().color(QPalette::WindowText), 48).pixmap(48, 48));
    auto *heading = new QLabel(text);
    QFont hf = heading->font();
    hf.setPointSizeF(hf.pointSizeF() * 1.25);
    hf.setBold(true);
    heading->setFont(hf);
    auto *details = new QLabel(tr("All open programs will be closed, and unsaved data will be lost."));
    details->setWordWrap(true);

    auto *texts = new QVBoxLayout;
    texts->addWidget(heading);
    texts->addWidget(details);
    texts->addStretch();
    auto *top = new QHBoxLayout;
    top->addWidget(icon, 0, Qt::AlignTop);
    top->addSpacing(12);
    top->addLayout(texts, 1);

    auto *buttons = new QDialogButtonBox;
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    QPushButton *main = nullptr;
    if (what == Power::LogOut) {
        main = buttons->addButton(tr("Log Out"), QDialogButtonBox::AcceptRole);
        connect(main, &QPushButton::clicked, this, [this] { doLogOut(); });
    } else {
        // As in Unity: the shutdown dialog also offers restart.
        if (canReboot) {
            QPushButton *r = buttons->addButton(tr("Restart"),
                                                what == Power::Restart ? QDialogButtonBox::AcceptRole
                                                                       : QDialogButtonBox::ActionRole);
            connect(r, &QPushButton::clicked, this, [this] { callLogind(QStringLiteral("Reboot")); });
            connect(r, &QPushButton::clicked, dlg, &QDialog::accept);
            if (what == Power::Restart)
                main = r;
        }
        if (what == Power::Shutdown && canOff) {
            main = buttons->addButton(tr("Shut Down"), QDialogButtonBox::AcceptRole);
            connect(main, &QPushButton::clicked, this, [this] { callLogind(QStringLiteral("PowerOff")); });
        }
    }
    connect(buttons, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);

    auto *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->addLayout(top);
    layout->addSpacing(12);
    layout->addWidget(buttons);
    dlg->setMinimumWidth(420);
    // Fixed size, so that tiling compositors (sway etc.) show the window floating.
    dlg->setFixedSize(dlg->sizeHint());
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    // The main button is the default (Enter). Set the focus after showing, otherwise Qt gives
    // it to the first button ("Cancel") and Enter would cancel.
    if (main) {
        main->setDefault(true);
        main->setFocus();
    }
}

// ---------- system settings and information ----------

void SessionMenu::openSettings()
{
    QString cmd = setting(QStringLiteral("settingsCommand"), QString()).toString();
    if (cmd.isEmpty()) {
        for (const char *app : {"xfce4-settings-manager", "systemsettings", "gnome-control-center",
                                "lxqt-config", "mate-control-center", "cinnamon-settings"}) {
            if (!QStandardPaths::findExecutable(QLatin1String(app)).isEmpty()) {
                cmd = QLatin1String(app);
                break;
            }
        }
    }
    if (cmd.isEmpty())
        return;
    QStringList args = QProcess::splitCommand(cmd);
    const QString program = args.takeFirst();
    QProcess::startDetached(program, args);
}

static QString osRelease(const QString &key)
{
    for (const char *path : {"/etc/os-release", "/usr/lib/os-release"}) {
        QFile f{QString::fromLatin1(path)};
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.startsWith(key + '=')) {
                QString v = line.mid(key.size() + 1);
                if (v.size() >= 2 && (v.startsWith('"') || v.startsWith('\'')))
                    v = v.mid(1, v.size() - 2);
                return v;
            }
        }
    }
    return {};
}

// /proc files report zero size, so read them whole instead of line by line until atEnd().
static QStringList procLines(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll()).split('\n');
}

static QString cpuModel()
{
    for (const QString &line : procLines(QStringLiteral("/proc/cpuinfo")))
        if (line.startsWith(QLatin1String("model name")))
            return line.section(':', 1).simplified();
    return QSysInfo::currentCpuArchitecture();
}

static QString memTotal()
{
    for (const QString &line : procLines(QStringLiteral("/proc/meminfo"))) {
        if (line.startsWith(QLatin1String("MemTotal:"))) {
            const double kib = line.section(':', 1).simplified().section(' ', 0, 0).toDouble();
            return QCoreApplication::translate("SessionMenu", "%1 GiB").arg(kib / 1024 / 1024, 0, 'f', 1);
        }
    }
    return {};
}

void SessionMenu::showAbout()
{
    const QString cmd = setting(QStringLiteral("aboutCommand"), QString()).toString();
    if (!cmd.isEmpty()) {
        QStringList args = QProcess::splitCommand(cmd);
        const QString program = args.takeFirst();
        QProcess::startDetached(program, args);
        return;
    }

    auto *dlg = new QDialog;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setObjectName(QStringLiteral("session-about"));
    dlg->setWindowTitle(tr("About This Computer"));

    auto *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(24, 20, 24, 16);

    QIcon logo = QIcon::fromTheme(osRelease(QStringLiteral("LOGO")));
    if (logo.isNull())
        logo = QIcon::fromTheme(QStringLiteral("distributor-logo"));
    if (!logo.isNull()) {
        auto *l = new QLabel;
        l->setPixmap(logo.pixmap(96, 96));
        l->setAlignment(Qt::AlignHCenter);
        layout->addWidget(l);
    }
    QString osName = osRelease(QStringLiteral("PRETTY_NAME"));
    if (osName.isEmpty())
        osName = QSysInfo::prettyProductName();
    auto *name = new QLabel(osName);
    QFont nf = name->font();
    nf.setPointSizeF(nf.pointSizeF() * 1.4);
    nf.setBold(true);
    name->setFont(nf);
    name->setAlignment(Qt::AlignHCenter);
    layout->addWidget(name);
    layout->addSpacing(12);

    const QStorageInfo root = QStorageInfo::root();
    QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    if (desktop.isEmpty())
        desktop = qEnvironmentVariable("XDG_SESSION_DESKTOP");
    const QString platform = QGuiApplication::platformName() == QLatin1String("wayland") ? QStringLiteral("Wayland")
                                                                                         : QGuiApplication::platformName();

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    auto row = [&](const QString &label, const QString &value) {
        if (value.isEmpty())
            return;
        auto *v = new QLabel(value);
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(QStringLiteral("<b>%1</b>").arg(label), v);
    };
    row(tr("Computer name"), QSysInfo::machineHostName());
    row(tr("Processor"), QStringLiteral("%1 × %2").arg(cpuModel()).arg(QThread::idealThreadCount()));
    row(tr("Memory"), memTotal());
    if (root.isValid())
        row(tr("Disk"), tr("%1 GB, %2 GB free")
                                        .arg(root.bytesTotal() / 1e9, 0, 'f', 0)
                                        .arg(root.bytesAvailable() / 1e9, 0, 'f', 0));
    row(tr("Kernel"), QSysInfo::kernelType() + ' ' + QSysInfo::kernelVersion());
    row(tr("Desktop"), desktop.isEmpty() ? platform : desktop + QStringLiteral(" (") + platform + ')');
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(tr("Close"));
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    layout->addSpacing(8);
    layout->addWidget(buttons);
    dlg->setMinimumWidth(440);
    dlg->setFixedSize(dlg->sizeHint());
    dlg->show();
}
