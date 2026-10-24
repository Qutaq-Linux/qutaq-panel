#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QStringList>

class QFileSystemWatcher;
class QTimer;

// Reads the labwc settings the panel needs:
//   - the theme name and <maximizedDecoration> from ~/.config/labwc/rc.xml;
//   - window button images and colors from the theme, looked up the same way labwc does
//     (<theme>/labwc/, then <theme>/openbox-3/; png, svg, xbm formats; *_hover on hover);
//   - overrides from ~/.config/labwc/themerc-override.
// Watches these files and reports changes (for example, after the theme is changed in rc.xml).
class LabwcTheme : public QObject
{
    Q_OBJECT
public:
    enum Button { Close, Iconify, Restore };

    explicit LabwcTheme(QObject *parent = nullptr);

    QString themeName() const { return m_name; }
    QString themeDir() const { return m_dir; }          // the theme directory found (empty: built-in theme)
    bool maximizedWithoutTitlebar() const { return m_maxNoTitle; } // <maximizedDecoration>none

    // Button image for the panel. hover: the hover variant (if the theme has none, the normal
    // image is returned and hasHoverImage() is false, so the panel draws the highlight itself).
    QPixmap button(Button b, bool hover) const;
    bool hasHoverImage(Button b) const { return m_hoverFromTheme.value(b); }
    QColor hoverBackground() const { return m_hoverBg; }

Q_SIGNALS:
    void changed();

private:
    void reload();
    void readRcXml();
    void findTheme();
    void readThemerc(const QString &path);
    QPixmap loadButton(const QStringList &names, bool hover, const QColor &color, bool *found) const;
    QColor buttonColor(const QString &kind) const;
    void watch();

    QString m_name;
    QString m_dir;
    QStringList m_iconDirs;      // <theme>/labwc, <theme>/openbox-3: where to look for images
    bool m_maxNoTitle = false;
    QHash<QString, QString> m_rc; // themerc keys
    QColor m_hoverBg;

    QHash<int, QPixmap> m_normal, m_hover;
    QHash<int, bool> m_hoverFromTheme;

    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_reloadTimer = nullptr;
};

// Color from themerc: "#rrggbb", "#rrggbbaa" (alpha last, as in labwc), "#rgb" or a color name.
QColor parseThemeColor(const QString &value, const QColor &fallback = QColor());
