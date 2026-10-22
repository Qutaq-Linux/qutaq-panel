#pragma once

#include <QSettings>
#include <QStandardPaths>
#include <QVariant>

// ~/.config/qutaq-panel/panel.conf; key — "группа/ключ", например "window/panelButtons".
// Файл перечитывается при каждом обращении, поэтому правки применяются без перезапуска.
inline QSettings &panelSettings()
{
    static QSettings s(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                           + QStringLiteral("/qutaq-panel/panel.conf"),
                       QSettings::IniFormat);
    return s;
}

inline QVariant panelSetting(const QString &key, const QVariant &def = QVariant())
{
    panelSettings().sync();
    return panelSettings().value(key, def);
}

// Запись — для переключателей прямо в меню панели (например, «Показывать процент»)
inline void panelSettingSet(const QString &key, const QVariant &value)
{
    panelSettings().setValue(key, value);
    panelSettings().sync();
}
