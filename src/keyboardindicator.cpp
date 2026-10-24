#include "keyboardindicator.h"

#include "settings.h"

#include <QAction>
#include <QActionGroup>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QHash>
#include <QMenu>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QXmlStreamReader>

#include <libintl.h>
#include <signal.h>
#include <sys/types.h>

static const char *kOverrideName = "zz-qutaq-panel.env";

static QString labwcConfigDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/labwc");
}

static QString overridePath()
{
    return labwcConfigDir() + QStringLiteral("/environment.d/") + QLatin1String(kOverrideName);
}

// KEY=value from a labwc environment file (no quotes: labwc doesn't parse them either)
static QHash<QString, QString> readEnvFile(const QString &path)
{
    QHash<QString, QString> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const int eq = line.indexOf('=');
        if (eq > 0)
            out.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return out;
}

// Same order as labwc: environment, then environment.d/*.env alphabetically
static QHash<QString, QString> userEnvironment(bool withOverride)
{
    QHash<QString, QString> env = readEnvFile(labwcConfigDir() + QStringLiteral("/environment"));
    QDir dir(labwcConfigDir() + QStringLiteral("/environment.d"));
    const QStringList files = dir.entryList({QStringLiteral("*.env")}, QDir::Files, QDir::Name);
    for (const QString &f : files) {
        if (!withOverride && f == QLatin1String(kOverrideName))
            continue;
        const auto more = readEnvFile(dir.filePath(f));
        for (auto it = more.cbegin(); it != more.cend(); ++it)
            env.insert(it.key(), it.value());
    }
    return env;
}

// Short and full layout names from the xkeyboard-config database
struct XkbNames {
    QString shortName, description;
};

static QHash<QString, XkbNames> xkbDatabase()
{
    static QHash<QString, XkbNames> db; // "us" and "us(dvorak)"
    if (!db.isEmpty())
        return db;
    QFile f(QStringLiteral("/usr/share/X11/xkb/rules/evdev.xml"));
    if (!f.open(QIODevice::ReadOnly))
        return db;
    QXmlStreamReader xml(&f);
    QString layout, variant, element;
    XkbNames current;
    bool inVariant = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            element = xml.name().toString();
            if (element == QLatin1String("layout")) {
                layout.clear();
                inVariant = false;
            } else if (element == QLatin1String("variant")) {
                inVariant = true;
                variant.clear();
            } else if (element == QLatin1String("configItem")) {
                current = {};
            }
        } else if (xml.isCharacters() && !xml.isWhitespace()) {
            const QString t = xml.text().toString().trimmed();
            if (element == QLatin1String("name"))
                (inVariant ? variant : layout) = t;
            else if (element == QLatin1String("shortDescription"))
                current.shortName = t;
            else if (element == QLatin1String("description"))
                current.description = t;
        } else if (xml.isEndElement()) {
            const auto name = xml.name();
            if (name == QLatin1String("configItem") && !layout.isEmpty()) {
                const QString key = inVariant ? layout + '(' + variant + ')' : layout;
                if (!db.contains(key))
                    db.insert(key, current);
            } else if (name == QLatin1String("variant")) {
                inVariant = false;
            }
            element.clear();
        }
    }
    return db;
}

// The name translated by xkeyboard-config ("Russian" → "Русская"), if available
static QString translate(const QString &description)
{
    static bool bound = false;
    if (!bound) {
        bind_textdomain_codeset("xkeyboard-config", "UTF-8");
        bound = true;
    }
    return QString::fromUtf8(dgettext("xkeyboard-config", description.toUtf8().constData()));
}

// ---------------------------------------------------------------------------

KeyboardIndicator::KeyboardIndicator(QObject *parent)
    : QObject(parent)
    , m_menu(new QMenu)
{
    load();
    rebuildMenu();

    // Layouts changed in the environment file: re-read
    auto *watcher = new QFileSystemWatcher(this);
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(300);
    auto rewatch = [watcher] {
        QStringList paths;
        for (const QString &p : {labwcConfigDir(), labwcConfigDir() + QStringLiteral("/environment"),
                                 labwcConfigDir() + QStringLiteral("/environment.d")})
            if (QFileInfo::exists(p))
                paths << p;
        if (!watcher->files().isEmpty())
            watcher->removePaths(watcher->files());
        if (!watcher->directories().isEmpty())
            watcher->removePaths(watcher->directories());
        if (!paths.isEmpty())
            watcher->addPaths(paths);
    };
    rewatch();
    connect(watcher, &QFileSystemWatcher::fileChanged, timer, qOverload<>(&QTimer::start));
    connect(watcher, &QFileSystemWatcher::directoryChanged, timer, qOverload<>(&QTimer::start));
    connect(timer, &QTimer::timeout, this, [this, rewatch] {
        const QString before = shortName() + QString::number(m_layouts.size());
        load();
        rebuildMenu();
        rewatch();
        if (before != shortName() + QString::number(m_layouts.size()))
            Q_EMIT changed();
    });
}

void KeyboardIndicator::load()
{
    const QHash<QString, QString> base = userEnvironment(false);
    QString layouts = base.value(QStringLiteral("XKB_DEFAULT_LAYOUT"));
    QString variants = base.value(QStringLiteral("XKB_DEFAULT_VARIANT"));
    if (layouts.isEmpty()) { // the layout is set in the session environment, not in files
        layouts = qEnvironmentVariable("XKB_DEFAULT_LAYOUT");
        variants = qEnvironmentVariable("XKB_DEFAULT_VARIANT");
    }
    if (layouts.isEmpty())
        layouts = QStringLiteral("us");

    const QStringList ls = layouts.split(',');
    const QStringList vs = variants.split(',');
    const QHash<QString, XkbNames> db = xkbDatabase();
    m_layouts.clear();
    for (int i = 0; i < ls.size(); ++i) {
        Layout l;
        l.layout = ls.at(i).trimmed();
        l.variant = vs.value(i).trimmed();
        if (l.layout.isEmpty())
            continue;
        const XkbNames base = db.value(l.layout);
        const XkbNames var = l.variant.isEmpty() ? XkbNames() : db.value(l.layout + '(' + l.variant + ')');
        QString shortName = var.shortName.isEmpty() ? base.shortName : var.shortName;
        if (shortName.isEmpty())
            shortName = l.layout.left(2);
        shortName[0] = shortName[0].toUpper(); // en → En, as in Unity
        l.shortName = shortName;
        const QString desc = var.description.isEmpty() ? base.description : var.description;
        l.description = desc.isEmpty() ? l.layout : translate(desc);
        m_layouts << l;
    }

    // The current one is the first in our file if it covers the same set of layouts; otherwise the first one
    m_current = 0;
    const QHash<QString, QString> mine = readEnvFile(overridePath());
    const QString first = mine.value(QStringLiteral("XKB_DEFAULT_LAYOUT")).section(',', 0, 0);
    const QString firstVar = mine.value(QStringLiteral("XKB_DEFAULT_VARIANT")).section(',', 0, 0);
    QStringList mineSet = mine.value(QStringLiteral("XKB_DEFAULT_LAYOUT")).split(',');
    QStringList baseSet = ls;
    mineSet.sort();
    baseSet.sort();
    if (!first.isEmpty() && mineSet == baseSet) {
        for (int i = 0; i < m_layouts.size(); ++i)
            if (m_layouts.at(i).layout == first && m_layouts.at(i).variant == firstVar)
                m_current = i;
    } else if (QFileInfo::exists(overridePath())) {
        QFile::remove(overridePath()); // the layout list changed, so our file is stale
    }
}

QString KeyboardIndicator::shortName() const
{
    return m_layouts.value(m_current).shortName;
}

bool KeyboardIndicator::isShown() const
{
    const QString mode = panelSetting(QStringLiteral("keyboard/show"), QStringLiteral("auto")).toString();
    if (mode == QLatin1String("never"))
        return false;
    if (mode == QLatin1String("always"))
        return !m_layouts.isEmpty();
    return m_layouts.size() > 1;
}

// labwc reads environment.d since version 0.7.2 (May 2024)
static bool labwcReadsEnvDir()
{
    static int cached = -1;
    if (cached < 0) {
        cached = 1; // could not find out: assume a recent version
        QProcess p;
        p.start(QStringLiteral("labwc"), {QStringLiteral("--version")});
        if (p.waitForFinished(2000)) {
            // "labwc 0.7.1" or "labwc 0.9.2 (+xwayland +nls …)"
            const QString out = QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError());
            const QStringList v = out.section(' ', 1, 1).split('.');
            if (v.size() >= 3) {
                const int ver = v[0].toInt() * 10000 + v[1].toInt() * 100 + v[2].toInt();
                cached = ver >= 702;
            }
        }
    }
    return cached == 1;
}

bool KeyboardIndicator::canSwitch() const
{
    return qEnvironmentVariableIntValue("LABWC_PID") > 1 && labwcReadsEnvDir();
}

void KeyboardIndicator::rebuildMenu()
{
    m_menu->clear();
    auto *group = new QActionGroup(m_menu);
    for (int i = 0; i < m_layouts.size(); ++i) {
        const Layout &l = m_layouts.at(i);
        QAction *a = m_menu->addAction(QStringLiteral("%1   %2").arg(l.shortName, l.description));
        a->setCheckable(true);
        a->setChecked(i == m_current);
        a->setEnabled(canSwitch());
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] { Set(i); });
    }
    m_menu->addSeparator();
    m_menu->addAction(tr("Configure Layouts…"), this, [] {
        // Layouts are set in the labwc environment: open it in a text editor
        const QString path = labwcConfigDir() + QStringLiteral("/environment");
        if (!QFileInfo::exists(path)) {
            QDir().mkpath(labwcConfigDir());
            // A user file hides the system one, so start from a copy of it
            QByteArray content("# Keyboard layouts, for example:\n# XKB_DEFAULT_LAYOUT=us,ru\n");
            for (const QString &d : qEnvironmentVariable("XDG_CONFIG_DIRS", QStringLiteral("/etc/xdg")).split(':', Qt::SkipEmptyParts)) {
                QFile sys(d + QStringLiteral("/labwc/environment"));
                if (sys.open(QIODevice::ReadOnly)) {
                    content = sys.readAll();
                    break;
                }
            }
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text))
                f.write(content);
        }
        QProcess::startDetached(QStringLiteral("xdg-open"), {path});
    });
    m_menu->menuAction()->setText(shortName());
    m_menu->menuAction()->setToolTip(canSwitch()
        ? tr("%1. Click: next layout; right-click: list").arg(m_layouts.value(m_current).description)
        : qEnvironmentVariableIntValue("LABWC_PID") > 1
            ? tr("Switching layouts requires labwc 0.7.2 or newer")
            : tr("Layout switching works only in labwc"));
}

void KeyboardIndicator::Next()
{
    if (!m_layouts.isEmpty())
        Set((m_current + 1) % m_layouts.size());
}

void KeyboardIndicator::Previous()
{
    if (!m_layouts.isEmpty())
        Set((m_current + m_layouts.size() - 1) % m_layouts.size());
}

void KeyboardIndicator::Set(int index)
{
    if (index < 0 || index >= m_layouts.size() || !canSwitch())
        return;
    m_current = index;
    apply();
    rebuildMenu();
    Q_EMIT changed();
}

void KeyboardIndicator::apply()
{
    // The same list, with the current layout first. The others stay in the keymap
    // so that shortcuts like Ctrl+C keep working in a Cyrillic layout too.
    QStringList layouts, variants;
    for (int k = 0; k < m_layouts.size(); ++k) {
        const Layout &l = m_layouts.at((m_current + k) % m_layouts.size());
        layouts << l.layout;
        variants << l.variant;
    }
    QDir().mkpath(labwcConfigDir() + QStringLiteral("/environment.d"));
    QSaveFile f(overridePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << "# Created by qutaq-panel: the current layout comes first.\n"
              "# Delete this file to restore the order from ~/.config/labwc/environment.\n";
        // labwc reads only the first environment file it finds (with its environment.d):
        // if the user has none, our file would hide the system /etc/xdg/labwc/environment,
        // so copy its lines here.
        if (!QFileInfo::exists(labwcConfigDir() + QStringLiteral("/environment"))) {
            const QString dirs = qEnvironmentVariable("XDG_CONFIG_DIRS", QStringLiteral("/etc/xdg"));
            for (const QString &d : dirs.split(':', Qt::SkipEmptyParts)) {
                QFile sys(d + QStringLiteral("/labwc/environment"));
                if (!sys.open(QIODevice::ReadOnly | QIODevice::Text))
                    continue;
                ts << "# from " << sys.fileName() << '\n' << QString::fromUtf8(sys.readAll()) << '\n';
                break;
            }
        }
        ts << "XKB_DEFAULT_LAYOUT=" << layouts.join(',') << '\n';
        if (std::any_of(variants.cbegin(), variants.cend(), [](const QString &v) { return !v.isEmpty(); }))
            ts << "XKB_DEFAULT_VARIANT=" << variants.join(',') << '\n';
        ts.flush();
        f.commit();
    }
    const qint64 pid = qEnvironmentVariable("LABWC_PID").toLongLong();
    if (pid > 1)
        ::kill(pid_t(pid), SIGHUP); // = labwc --reconfigure
    qInfo().noquote() << "keyboard layout:" << m_layouts.at(m_current).layout << m_layouts.at(m_current).variant;
}
