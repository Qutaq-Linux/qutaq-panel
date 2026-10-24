#include "labwctheme.h"

#include "settings.h"

#include <QBitmap>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QXmlStreamReader>

#include <cmath>

// Colors of the panel itself (see panel.cpp): buttons must be visible on it.
static const QColor kPanelBg(0x18, 0x19, 0x1e);
static const QColor kPanelFg(0xe8, 0xe8, 0xec);

QColor parseThemeColor(const QString &value, const QColor &fallback)
{
    QString v = value.trimmed().section(' ', 0, 0);
    if (v.isEmpty())
        return fallback;
    if (v.startsWith('#') && v.size() == 9) {
        // labwc: #rrggbbaa; Qt understands #aarrggbb, so reorder
        v = '#' + v.mid(7, 2) + v.mid(1, 6);
    }
    const QColor c = QColor::fromString(v);
    return c.isValid() ? c : fallback;
}

static QString configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/labwc");
}

static QString rcXmlPath()
{
    const QString user = configDir() + QStringLiteral("/rc.xml");
    if (QFileInfo::exists(user))
        return user;
    for (const QString &dir : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) {
        const QString p = dir + QStringLiteral("/labwc/rc.xml");
        if (QFileInfo::exists(p))
            return p;
    }
    return QStringLiteral("/etc/xdg/labwc/rc.xml");
}

// Theme directories in labwc's search order
static QStringList themeBases()
{
    QStringList out;
    const QString dataHome = qEnvironmentVariable("XDG_DATA_HOME", QDir::homePath() + QStringLiteral("/.local/share"));
    out << dataHome + QStringLiteral("/themes") << QDir::homePath() + QStringLiteral("/.themes");
    const QString dataDirs = qEnvironmentVariable("XDG_DATA_DIRS", QStringLiteral("/usr/local/share:/usr/share"));
    for (const QString &d : dataDirs.split(':', Qt::SkipEmptyParts))
        out << d + QStringLiteral("/themes");
    out << QStringLiteral("/usr/share/themes") << QStringLiteral("/opt/share/themes");
    out.removeDuplicates();
    return out;
}

// Relative luminance (WCAG), to tell whether a button is visible on the dark panel
static double luminance(const QColor &c)
{
    auto ch = [](double v) { v /= 255.0; return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * ch(c.red()) + 0.7152 * ch(c.green()) + 0.0722 * ch(c.blue());
}

static double contrast(const QColor &a, const QColor &b)
{
    const double la = luminance(a), lb = luminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

// Simple buttons when the theme has no images (labwc's built-in theme)
static QPixmap drawFallback(LabwcTheme::Button b, const QColor &color)
{
    const qreal dpr = 2.0;
    QPixmap pm(QSize(10, 10) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::SquareCap));
    switch (b) {
    case LabwcTheme::Close:
        p.drawLine(QPointF(1, 1), QPointF(9, 9));
        p.drawLine(QPointF(9, 1), QPointF(1, 9));
        break;
    case LabwcTheme::Iconify:
        p.drawLine(QPointF(1, 8.5), QPointF(9, 8.5));
        break;
    case LabwcTheme::Restore:
        p.drawRect(QRectF(1, 3.5, 5.5, 5.5));
        p.drawPolyline(QPolygonF({QPointF(3.5, 3.5), QPointF(3.5, 1), QPointF(9, 1), QPointF(9, 6.5), QPointF(6.5, 6.5)}));
        break;
    }
    return pm;
}

// ---------------------------------------------------------------------------

LabwcTheme::LabwcTheme(QObject *parent)
    : QObject(parent)
{
    m_watcher = new QFileSystemWatcher(this);
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(300); // editors save a file in several steps
    connect(m_reloadTimer, &QTimer::timeout, this, [this] {
        reload();
        Q_EMIT changed();
    });
    connect(m_watcher, &QFileSystemWatcher::fileChanged, m_reloadTimer, qOverload<>(&QTimer::start));
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, m_reloadTimer, qOverload<>(&QTimer::start));
    reload();
}

void LabwcTheme::reload()
{
    readRcXml();
    findTheme();

    m_rc.clear();
    if (!m_dir.isEmpty()) {
        for (const QString &sub : {QStringLiteral("labwc"), QStringLiteral("openbox-3")}) {
            const QString p = m_dir + '/' + sub + QStringLiteral("/themerc");
            if (QFileInfo::exists(p)) {
                readThemerc(p);
                break;
            }
        }
    }
    readThemerc(configDir() + QStringLiteral("/themerc-override"));

    const QString mode = panelSetting(QStringLiteral("window/buttonColor"), QStringLiteral("auto")).toString();
    m_hoverBg = mode == QLatin1String("theme")
                    ? parseThemeColor(m_rc.value(QStringLiteral("window.button.hover.bg.color")), QColor(128, 128, 128, 32))
                    : QColor(255, 255, 255, 40);

    struct Spec { Button b; QStringList names; QString kind; };
    const QList<Spec> specs = {
        {Close, {QStringLiteral("close")}, QStringLiteral("close")},
        {Iconify, {QStringLiteral("iconify")}, QStringLiteral("iconify")},
        {Restore, {QStringLiteral("max_toggled"), QStringLiteral("max")}, QStringLiteral("max")},
    };
    for (const Spec &s : specs) {
        const QColor color = buttonColor(s.kind);
        bool found = false;
        QPixmap normal = loadButton(s.names, false, color, &found);
        if (normal.isNull())
            normal = drawFallback(s.b, color);
        bool hoverFound = false;
        QPixmap hover = loadButton(s.names, true, color, &hoverFound);
        m_normal[s.b] = normal;
        m_hover[s.b] = hoverFound ? hover : normal;
        m_hoverFromTheme[s.b] = hoverFound;
    }

    qInfo().noquote() << "labwc theme:" << (m_name.isEmpty() ? QStringLiteral("(built-in)") : m_name)
                      << (m_dir.isEmpty() ? QStringLiteral("— not found, drawing own buttons") : m_dir)
                      << "| maximizedDecoration none:" << m_maxNoTitle;
    watch();
}

void LabwcTheme::readRcXml()
{
    m_name.clear();
    m_maxNoTitle = false;
    QFile f(rcXmlPath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    QXmlStreamReader xml(&f);
    QStringList stack;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            stack << xml.name().toString().toLower();
        } else if (xml.isEndElement()) {
            if (!stack.isEmpty())
                stack.removeLast();
        } else if (xml.isCharacters() && !xml.isWhitespace() && stack.size() >= 2
                   && stack.at(stack.size() - 2) == QLatin1String("theme")) {
            const QString v = xml.text().toString().trimmed();
            if (stack.last() == QLatin1String("name"))
                m_name = v;
            else if (stack.last() == QLatin1String("maximizeddecoration"))
                m_maxNoTitle = v.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0;
        }
    }
}

void LabwcTheme::findTheme()
{
    m_dir.clear();
    m_iconDirs.clear();
    if (m_name.isEmpty())
        return;
    const QStringList bases = themeBases();
    for (const QString &sub : {QStringLiteral("labwc"), QStringLiteral("openbox-3")})
        for (const QString &base : bases)
            if (QFileInfo::exists(base + '/' + m_name + '/' + sub + QStringLiteral("/themerc"))) {
                if (m_dir.isEmpty())
                    m_dir = base + '/' + m_name;
            }
    // labwc looks for images in the same directories: all labwc/ first, then all openbox-3/
    for (const QString &sub : {QStringLiteral("labwc"), QStringLiteral("openbox-3")})
        for (const QString &base : bases)
            if (QFileInfo(base + '/' + m_name + '/' + sub).isDir())
                m_iconDirs << base + '/' + m_name + '/' + sub;
}

void LabwcTheme::readThemerc(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith('!'))
            continue;
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        m_rc.insert(line.left(colon).trimmed(), line.mid(colon + 1).trimmed());
    }
}

QColor LabwcTheme::buttonColor(const QString &kind) const
{
    const QString specific = m_rc.value(QStringLiteral("window.active.button.%1.unpressed.image.color").arg(kind));
    const QString common = m_rc.value(QStringLiteral("window.active.button.unpressed.image.color"));
    const QColor themeColor = parseThemeColor(specific.isEmpty() ? common : specific, QColor(Qt::black));

    const QString mode = panelSetting(QStringLiteral("window/buttonColor"), QStringLiteral("auto")).toString();
    if (mode == QLatin1String("theme"))
        return themeColor;
    if (mode == QLatin1String("panel"))
        return kPanelFg;
    // auto: the theme color if it is visible on the dark panel (light themes have dark buttons, then use light ones)
    return contrast(themeColor, kPanelBg) >= 3.0 ? themeColor : kPanelFg;
}

QPixmap LabwcTheme::loadButton(const QStringList &names, bool hover, const QColor &color, bool *found) const
{
    *found = false;
    for (const QString &name : names) {
        const QString base = name + (hover ? QStringLiteral("_hover") : QString());
        // png and svg are color images used as is
        for (const char *ext : {"png", "svg"}) {
            for (const QString &dir : m_iconDirs) {
                const QString path = dir + '/' + base + QStringLiteral("-active.") + QLatin1String(ext);
                if (!QFileInfo::exists(path))
                    continue;
                QImageReader reader(path);
                QSize size = reader.size();
                if (!size.isValid() || size.height() > 20)
                    size = size.isValid() ? size.scaled(16, 16, Qt::KeepAspectRatio) : QSize(16, 16);
                reader.setScaledSize(size);
                const QImage img = reader.read();
                if (!img.isNull()) {
                    *found = true;
                    return QPixmap::fromImage(img);
                }
            }
        }
        // xbm is a one-color mask painted with the button color
        for (const QString &dir : m_iconDirs) {
            const QString path = dir + '/' + base + QStringLiteral(".xbm");
            if (!QFileInfo::exists(path))
                continue;
            const QImage img(path, "XBM");
            if (img.isNull())
                continue;
            QPixmap pm(img.size());
            pm.fill(color);
            pm.setMask(QBitmap::fromImage(img));
            *found = true;
            return pm;
        }
    }
    return {};
}

QPixmap LabwcTheme::button(Button b, bool hover) const
{
    return hover ? m_hover.value(b) : m_normal.value(b);
}

void LabwcTheme::watch()
{
    if (!m_watcher->files().isEmpty())
        m_watcher->removePaths(m_watcher->files());
    if (!m_watcher->directories().isEmpty())
        m_watcher->removePaths(m_watcher->directories());
    QStringList paths;
    for (const QString &p : {rcXmlPath(), configDir() + QStringLiteral("/themerc-override")})
        if (QFileInfo::exists(p))
            paths << p;
    if (QFileInfo(configDir()).isDir())
        paths << configDir(); // catches file creation and save-by-replace
    paths << m_iconDirs;
    if (!paths.isEmpty())
        m_watcher->addPaths(paths);
}
