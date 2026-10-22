#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QStringList>

class QFileSystemWatcher;
class QTimer;

// Читает настройки labwc, нужные панели:
//   - имя темы и <maximizedDecoration> из ~/.config/labwc/rc.xml;
//   - картинки кнопок окна и их цвета из темы — так же, как их ищет сам labwc
//     (<тема>/labwc/, затем <тема>/openbox-3/; форматы png, svg, xbm; *_hover — при наведении);
//   - переопределения из ~/.config/labwc/themerc-override.
// Следит за этими файлами и сообщает об изменениях (например, после смены темы в rc.xml).
class LabwcTheme : public QObject
{
    Q_OBJECT
public:
    enum Button { Close, Iconify, Restore };

    explicit LabwcTheme(QObject *parent = nullptr);

    QString themeName() const { return m_name; }
    QString themeDir() const { return m_dir; }          // найденный каталог темы (пусто — встроенная)
    bool maximizedWithoutTitlebar() const { return m_maxNoTitle; } // <maximizedDecoration>none

    // Картинка кнопки для панели. hover — вариант при наведении (если в теме его нет,
    // возвращается обычная, а hasHoverImage() = false — тогда панель рисует подложку сама).
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
    QStringList m_iconDirs;      // <тема>/labwc, <тема>/openbox-3 — где искать картинки
    bool m_maxNoTitle = false;
    QHash<QString, QString> m_rc; // ключи themerc
    QColor m_hoverBg;

    QHash<int, QPixmap> m_normal, m_hover;
    QHash<int, bool> m_hoverFromTheme;

    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_reloadTimer = nullptr;
};

// Цвет из themerc: "#rrggbb", "#rrggbbaa" (альфа в конце, как в labwc), "#rgb" или имя цвета.
QColor parseThemeColor(const QString &value, const QColor &fallback = QColor());
