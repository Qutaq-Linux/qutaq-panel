#pragma once

#include <QSettings>
#include <QStandardPaths>
#include <QVariant>

// ~/.config/qutaq-panel/panel.conf; key is "group/key", e.g. "window/panelButtons".
// The file is re-read on every access, so edits apply without a restart.
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

// Writing: for toggles right in the panel menus (e.g. "Show percentage")
inline void panelSettingSet(const QString &key, const QVariant &value)
{
    panelSettings().setValue(key, value);
    panelSettings().sync();
}
