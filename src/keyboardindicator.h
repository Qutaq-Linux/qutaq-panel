#pragma once

#include <QObject>
#include <QStringList>

class QMenu;

// Keyboard layout indicator ("En", "Ru"), as in Ubuntu Unity.
//   click             next layout;
//   right-click       menu with the list of layouts.
//
// labwc offers no way to query or switch the layout from outside (no IPC, no Wayland
// protocol), so the panel switches it itself:
//   1. writes ~/.config/labwc/environment.d/zz-qutaq-panel.env with the same
//      XKB_DEFAULT_LAYOUT list, but with the wanted layout first;
//   2. sends labwc SIGHUP (= labwc --reconfigure); it re-reads the environment
//      and applies the layout.
// The user's own environment file is left untouched. The keyboard shortcut for switching
// is a labwc keybind that runs "qutaq-panel --next-layout" (see README):
// the panel cannot see switching done by the XKB option grp:….
class KeyboardIndicator : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qutaq.Panel.Keyboard")
public:
    struct Layout {
        QString layout;      // us
        QString variant;     // empty or dvorak
        QString shortName;   // En
        QString description; // English (US)
    };

    explicit KeyboardIndicator(QObject *parent = nullptr);

    QMenu *menu() const { return m_menu; }
    QString shortName() const;
    bool isShown() const;                 // more than one layout (or keyboard/show=always)
    bool canSwitch() const;               // the panel was started from labwc
    QList<Layout> layouts() const { return m_layouts; }
    int current() const { return m_current; }

public Q_SLOTS:
    // Also available over D-Bus: org.qutaq.Panel /Keyboard
    void Next();
    void Previous();
    void Set(int index);

Q_SIGNALS:
    void changed();

private:
    void load();
    void rebuildMenu();
    void apply();

    QMenu *m_menu = nullptr;
    QList<Layout> m_layouts; // in the order of the user's environment file
    int m_current = 0;
};
