#include "batteryindicator.h"

#include "settings.h"

#include <QAction>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusObjectPath>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <cmath>

#include <algorithm>

static const char *kUPower = "org.freedesktop.UPower";
static const char *kUPowerPath = "/org/freedesktop/UPower";
static const char *kUPowerDevice = "org.freedesktop.UPower.Device";

enum { StateCharging = 1, StateDischarging = 2, StateEmpty = 3, StateFull = 4, StatePendingCharge = 5 };
enum { KindLinePower = 1, KindBattery = 2, KindUps = 3 };

static QDBusConnection sysBus()
{
    return QDBusConnection::systemBus();
}

static QString kindName(int kind)
{
    switch (kind) {
    case KindBattery: return QStringLiteral("Аккумулятор");
    case KindUps: return QStringLiteral("ИБП");
    case 5: return QStringLiteral("Мышь");
    case 6: return QStringLiteral("Клавиатура");
    case 7: return QStringLiteral("КПК");
    case 8: return QStringLiteral("Телефон");
    case 12: return QStringLiteral("Планшет");
    case 13: return QStringLiteral("Компьютер");
    case 14: return QStringLiteral("Геймпад");
    case 17: return QStringLiteral("Гарнитура");
    case 18: return QStringLiteral("Динамик");
    case 19: return QStringLiteral("Наушники");
    case 22: return QStringLiteral("Наушники");
    case 26: return QStringLiteral("Стилус");
    default: return QStringLiteral("Устройство");
    }
}

static QString duration(qint64 seconds)
{
    if (seconds <= 0)
        return {};
    const qint64 h = seconds / 3600, m = (seconds % 3600) / 60;
    if (h > 0)
        return m ? QStringLiteral("%1 ч %2 мин").arg(h).arg(m) : QStringLiteral("%1 ч").arg(h);
    return QStringLiteral("%1 мин").arg(qMax<qint64>(1, m));
}

QIcon batteryIcon(const QColor &color, int size, double percent, bool charging, bool full)
{
    QIcon icon;
    for (qreal dpr : {1.0, 2.0}) {
        QPixmap pm(QSize(size, size) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal u = size / 16.0;
        // Корпус и «плюс» (горизонтальная батарейка, как в Unity)
        const QRectF body(1 * u, 4 * u, 12.5 * u, 8 * u);
        p.setPen(QPen(color, 1.2 * u));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(body, 1.5 * u, 1.5 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(13.8 * u, 6.3 * u, 1.6 * u, 3.4 * u), 0.6 * u, 0.6 * u);
        // Заряд: красный при ≤10%
        const double frac = qBound(0.0, percent, 100.0) / 100.0;
        QColor fill = percent <= 10 && !charging ? QColor(0xe0, 0x4a, 0x3f) : color;
        const QRectF inner = body.adjusted(2 * u, 2 * u, -2 * u, -2 * u);
        if (frac > 0)
            p.fillRect(QRectF(inner.left(), inner.top(), inner.width() * qMax(0.08, frac), inner.height()), fill);
        if (charging && !full) {
            // Молния поверх, с тёмной обводкой, чтобы была видна на заполненной батарее
            QPainterPath bolt;
            bolt.moveTo(8.4 * u, 3 * u);
            bolt.lineTo(4.6 * u, 8.6 * u);
            bolt.lineTo(7.3 * u, 8.6 * u);
            bolt.lineTo(6.2 * u, 13 * u);
            bolt.lineTo(10 * u, 7.2 * u);
            bolt.lineTo(7.4 * u, 7.2 * u);
            bolt.closeSubpath();
            p.setPen(QPen(QColor(0x18, 0x19, 0x1e), 1.2 * u));
            p.setBrush(QColor(0xf5, 0xc2, 0x11));
            p.drawPath(bolt);
        }
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

// ---------------------------------------------------------------------------

BatteryIndicator::BatteryIndicator(QObject *parent)
    : QObject(parent)
    , m_menu(new QMenu)
    , m_label(new QAction(this))
{
    m_menu->setToolTipsVisible(true);
    connect(m_menu, &QMenu::aboutToShow, this, &BatteryIndicator::rebuildMenu);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(200);
    connect(m_debounce, &QTimer::timeout, this, &BatteryIndicator::refresh);

    // UPower шлёт сигналы при изменениях; без него — опрос раз в минуту
    m_timer = new QTimer(this);
    m_timer->setInterval(60 * 1000);
    connect(m_timer, &QTimer::timeout, this, &BatteryIndicator::refresh);
    m_timer->start();

    QDBusConnection b = sysBus();
    b.connect(kUPower, QString(), QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
              this, SLOT(onUPowerSignal(QDBusMessage)));
    b.connect(kUPower, kUPowerPath, kUPower, QStringLiteral("DeviceAdded"), this, SLOT(onUPowerSignal(QDBusMessage)));
    b.connect(kUPower, kUPowerPath, kUPower, QStringLiteral("DeviceRemoved"), this, SLOT(onUPowerSignal(QDBusMessage)));
    auto *watcher = new QDBusServiceWatcher(kUPower, b, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, m_debounce, qOverload<>(&QTimer::start));

    refresh();
}

BatteryIndicator::~BatteryIndicator()
{
    delete m_menu;
}

void BatteryIndicator::onUPowerSignal(const QDBusMessage &)
{
    m_debounce->start();
}

QList<BatteryIndicator::Device> BatteryIndicator::readUPower(bool *ok) const
{
    QList<Device> out;
    *ok = false;
    if (!sysBus().isConnected() || !sysBus().interface()
        || !sysBus().interface()->isServiceRegistered(kUPower).value())
        return out;
    QDBusMessage msg = QDBusMessage::createMethodCall(kUPower, kUPowerPath, kUPower, QStringLiteral("EnumerateDevices"));
    const QDBusMessage r = sysBus().call(msg, QDBus::Block, 1500);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
        return out;
    *ok = true;
    QList<QDBusObjectPath> paths;
    r.arguments().first().value<QDBusArgument>() >> paths;
    for (const QDBusObjectPath &path : paths) {
        QDBusMessage get = QDBusMessage::createMethodCall(kUPower, path.path(), QStringLiteral("org.freedesktop.DBus.Properties"),
                                                          QStringLiteral("GetAll"));
        get << QString::fromLatin1(kUPowerDevice);
        const QDBusMessage pr = sysBus().call(get, QDBus::Block, 1500);
        if (pr.type() != QDBusMessage::ReplyMessage || pr.arguments().isEmpty())
            continue;
        QVariantMap p;
        pr.arguments().first().value<QDBusArgument>() >> p;
        Device d;
        d.kind = p.value(QStringLiteral("Type")).toInt();
        if (d.kind == KindLinePower || !p.value(QStringLiteral("IsPresent"), true).toBool())
            continue; // сетевой адаптер и вынутые батареи не показываем
        d.percent = p.value(QStringLiteral("Percentage")).toDouble();
        d.state = p.value(QStringLiteral("State")).toInt();
        d.toEmpty = p.value(QStringLiteral("TimeToEmpty")).toLongLong();
        d.toFull = p.value(QStringLiteral("TimeToFull")).toLongLong();
        d.powerSupply = p.value(QStringLiteral("PowerSupply")).toBool();
        const QString model = p.value(QStringLiteral("Model")).toString().trimmed();
        d.name = d.powerSupply || model.isEmpty() ? kindName(d.kind) : kindName(d.kind) + QStringLiteral(" ") + model;
        out << d;
    }
    return out;
}

QList<BatteryIndicator::Device> BatteryIndicator::readSysfs() const
{
    QList<Device> out;
    const QString root = qEnvironmentVariable("GMP_POWER_SUPPLY_DIR", QStringLiteral("/sys/class/power_supply"));
    auto read = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
    };
    for (const QString &name : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString dir = root + '/' + name + '/';
        if (read(dir + QStringLiteral("type")) != QLatin1String("Battery"))
            continue;
        if (read(dir + QStringLiteral("present")) == QLatin1String("0"))
            continue;
        Device d;
        d.kind = KindBattery;
        // scope=Device — батарея периферии (мышь, геймпад), а не компьютера
        d.powerSupply = read(dir + QStringLiteral("scope")) != QLatin1String("Device");
        d.name = kindName(KindBattery);
        const QString cap = read(dir + QStringLiteral("capacity"));
        if (!cap.isEmpty()) {
            d.percent = cap.toDouble();
        } else { // некоторые ARM-контроллеры дают только energy_now/energy_full
            const double now = read(dir + QStringLiteral("energy_now")).toDouble() + read(dir + QStringLiteral("charge_now")).toDouble();
            const double full = read(dir + QStringLiteral("energy_full")).toDouble() + read(dir + QStringLiteral("charge_full")).toDouble();
            d.percent = full > 0 ? 100.0 * now / full : -1;
        }
        const QString st = read(dir + QStringLiteral("status"));
        d.state = st == QLatin1String("Charging") ? StateCharging
                : st == QLatin1String("Discharging") ? StateDischarging
                : st == QLatin1String("Full") ? StateFull
                : StatePendingCharge;
        // Оставшееся время: энергия (мкВт·ч) / мощность (мкВт) или заряд (мкА·ч) / ток (мкА)
        auto num = [&](const char *f) { return read(dir + QLatin1String(f)).toDouble(); };
        double now = num("energy_now"), full = num("energy_full"), rate = std::abs(num("power_now"));
        if (now <= 0 || rate <= 0) {
            now = num("charge_now"); full = num("charge_full"); rate = std::abs(num("current_now"));
        }
        if (now > 0 && rate > 0) {
            if (d.state == StateDischarging)
                d.toEmpty = qint64(3600.0 * now / rate);
            else if (d.state == StateCharging && full > now)
                d.toFull = qint64(3600.0 * (full - now) / rate);
        }
        if (!d.powerSupply) {
            const QString model = read(dir + QStringLiteral("model_name"));
            const QString lower = name.toLower() + ' ' + model.toLower();
            d.kind = lower.contains(QLatin1String("mouse")) ? 5 : lower.contains(QLatin1String("keyboard")) || lower.contains(QLatin1String("kbd")) ? 6 : 0;
            d.name = (d.kind ? kindName(d.kind) : QStringLiteral("Устройство")) + ' ' + model;
        }
        out << d;
    }
    return out;
}

void BatteryIndicator::refresh()
{
    bool ok = false;
    QList<Device> devs = readUPower(&ok);
    if (!ok)
        devs = readSysfs();
    // Сначала батареи компьютера, потом периферия
    std::stable_sort(devs.begin(), devs.end(), [](const Device &a, const Device &b) { return a.powerSupply > b.powerSupply; });
    m_devices = devs;

    const QString mode = panelSetting(QStringLiteral("battery/show"), QStringLiteral("auto")).toString();
    const bool shown = mode == QLatin1String("always") || (mode != QLatin1String("never") && !devs.isEmpty());

    // На панели — батарея компьютера (несколько — средний заряд), иначе первая периферия
    const QColor fg(0xe8, 0xe8, 0xec);
    QList<Device> mains;
    for (const Device &d : devs)
        if (d.powerSupply)
            mains << d;
    if (mains.isEmpty() && !devs.isEmpty())
        mains << devs.first();

    QString tip, label;
    if (mains.isEmpty()) {
        m_menu->menuAction()->setIcon(batteryIcon(QColor(0x7a, 0x7a, 0x82), 16, 0, false, false));
        tip = QStringLiteral("Батарей нет");
    } else {
        double sum = 0;
        bool charging = false, discharging = false, full = true;
        qint64 toEmpty = 0, toFull = 0;
        for (const Device &d : mains) {
            sum += qMax(0.0, d.percent);
            charging |= d.state == StateCharging;
            discharging |= d.state == StateDischarging;
            full &= d.state == StateFull;
            toEmpty += d.toEmpty;
            toFull = qMax(toFull, d.toFull);
        }
        const double pct = sum / mains.size();
        m_menu->menuAction()->setIcon(batteryIcon(fg, 16, pct, charging || full, full));
        const QString time = discharging ? duration(toEmpty) : charging ? duration(toFull) : QString();
        tip = QStringLiteral("%1%").arg(qRound(pct));
        if (full)
            tip += QStringLiteral(", заряжен");
        else if (charging)
            tip += QStringLiteral(", заряжается") + (time.isEmpty() ? QString() : QStringLiteral(" (%1 до полного)").arg(time));
        else if (discharging && !time.isEmpty())
            tip += QStringLiteral(", осталось %1").arg(time);

        if (panelSetting(QStringLiteral("battery/showPercentage"), false).toBool())
            label = QStringLiteral("%1%").arg(qRound(pct));
        if (panelSetting(QStringLiteral("battery/showTime"), false).toBool() && !time.isEmpty())
            label += (label.isEmpty() ? QString() : QStringLiteral(" ")) + QStringLiteral("(%1)").arg(time);

        // Предупреждения о низком заряде — один раз на каждый порог
        if (discharging && mains.first().powerSupply)
            notifyLow(Device{QString(), KindBattery, pct, StateDischarging, toEmpty});
        else
            m_lastWarned = 101;
    }
    m_menu->menuAction()->setToolTip(tip);
    m_label->setText(label);
    m_label->setVisible(shown && !label.isEmpty());
    m_label->setToolTip(tip);

    m_shown = shown;
    Q_EMIT changed();
}

void BatteryIndicator::notifyLow(const Device &main)
{
    for (int threshold : {5, 10}) {
        if (main.percent <= threshold && m_lastWarned > threshold) {
            m_lastWarned = threshold;
            QDBusMessage msg = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
                QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
            const QString time = duration(main.toEmpty);
            msg << QStringLiteral("qutaq-panel") << uint(0) << QStringLiteral("battery-caution")
                << QStringLiteral("Низкий заряд батареи")
                << QStringLiteral("Осталось %1%%2. Подключите зарядное устройство.")
                       .arg(qRound(main.percent))
                       .arg(time.isEmpty() ? QString() : QStringLiteral(" (около %1)").arg(time))
                << QStringList() << QVariantMap{{QStringLiteral("urgency"), QVariant::fromValue(uchar(threshold <= 5 ? 2 : 1))}}
                << int(-1);
            QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
            qInfo().noquote() << "battery low:" << qRound(main.percent) << "%";
            break;
        }
    }
}

void BatteryIndicator::rebuildMenu()
{
    refresh();
    m_menu->clear();
    const QColor fg = m_menu->palette().color(QPalette::WindowText);
    if (m_devices.isEmpty())
        m_menu->addAction(QStringLiteral("Батарей не найдено"))->setEnabled(false);
    for (const Device &d : std::as_const(m_devices)) {
        QString text = d.name;
        if (d.percent >= 0)
            text += QStringLiteral("   %1%").arg(qRound(d.percent));
        QString state;
        if (d.state == StateFull)
            state = QStringLiteral("заряжен");
        else if (d.state == StateCharging)
            state = d.toFull > 0 ? QStringLiteral("%1 до полного заряда").arg(duration(d.toFull)) : QStringLiteral("заряжается");
        else if (d.state == StateDischarging && d.toEmpty > 0)
            state = QStringLiteral("%1 до разряда").arg(duration(d.toEmpty));
        if (!state.isEmpty())
            text += QStringLiteral(" (%1)").arg(state);
        QAction *a = m_menu->addAction(batteryIcon(fg, 16, d.percent, d.state == StateCharging || d.state == StateFull,
                                                   d.state == StateFull),
                                       text);
        a->setIconVisibleInMenu(true);
        a->setEnabled(false); // как в Unity — строки только для чтения
    }
    m_menu->addSeparator();
    auto toggle = [this](const QString &text, const QString &key) {
        QAction *a = m_menu->addAction(text);
        a->setCheckable(true);
        a->setChecked(panelSetting(key, false).toBool());
        connect(a, &QAction::toggled, this, [this, key](bool on) {
            panelSettingSet(key, on);
            refresh();
        });
    };
    toggle(QStringLiteral("Показывать процент на панели"), QStringLiteral("battery/showPercentage"));
    toggle(QStringLiteral("Показывать оставшееся время"), QStringLiteral("battery/showTime"));
    m_menu->addSeparator();
    m_menu->addAction(QStringLiteral("Параметры электропитания…"), this, [] {
        QString cmd = panelSetting(QStringLiteral("battery/settingsCommand"), QString()).toString();
        if (cmd.isEmpty())
            for (const char *app : {"xfce4-power-manager-settings", "mate-power-preferences", "gnome-power-statistics"})
                if (!QStandardPaths::findExecutable(QLatin1String(app)).isEmpty()) {
                    cmd = QLatin1String(app);
                    break;
                }
        if (cmd.isEmpty())
            return;
        QStringList args = QProcess::splitCommand(cmd);
        const QString program = args.takeFirst();
        QProcess::startDetached(program, args);
    });
}
