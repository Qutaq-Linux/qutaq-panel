#pragma once

#include <QObject>
#include <QStringList>

class QMenu;

// Индикатор раскладки клавиатуры («En», «Ru»), как в Ubuntu Unity.
//   щелчок            — следующая раскладка;
//   правый щелчок     — меню со списком раскладок.
//
// У labwc нет способа узнать или переключить раскладку извне (ни IPC, ни протокола
// Wayland), поэтому раскладку переключает сама панель:
//   1. пишет ~/.config/labwc/environment.d/zz-qutaq-panel.env с тем же списком
//      XKB_DEFAULT_LAYOUT, но нужной раскладкой первой;
//   2. посылает labwc сигнал HUP (= labwc --reconfigure) — он перечитывает окружение
//      и применяет раскладку.
// Свой файл environment панель не трогает. Сочетание клавиш для смены раскладки —
// через привязку labwc, которая вызывает «qutaq-panel --next-layout» (см. README):
// переключение по XKB-опции grp:… панель увидеть не может.
class KeyboardIndicator : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qutaq.Panel.Keyboard")
public:
    struct Layout {
        QString layout;      // us
        QString variant;     // пусто или dvorak
        QString shortName;   // En
        QString description; // English (US)
    };

    explicit KeyboardIndicator(QObject *parent = nullptr);

    QMenu *menu() const { return m_menu; }
    QString shortName() const;
    bool isShown() const;                 // раскладок больше одной (или keyboard/show=always)
    bool canSwitch() const;               // панель запущена из labwc
    QList<Layout> layouts() const { return m_layouts; }
    int current() const { return m_current; }

public Q_SLOTS:
    // Доступны и по D-Bus: org.qutaq.Panel /Keyboard
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
    QList<Layout> m_layouts; // в порядке из файла environment пользователя
    int m_current = 0;
};
