#include "panel.h"
#include "statusnotifier.h"

#include "menucontroller.h"
#include "sessionmenu.h"
#include "soundmenu.h"
#include "networkmenu.h"
#include "keyboardindicator.h"
#include "batteryindicator.h"
#include "bluetoothindicator.h"
#include "labwctheme.h"
#include "settings.h"

#include <LayerShellQt/Window>

#include <QCoreApplication>
#include <QAbstractButton>
#include <QDateTime>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QToolTip>
#include <QHelpEvent>
#include <QWindow>

static const char *kStyle = R"(
QMenuBar {
    background: transparent;
    color: #e8e8ec;
    font-size: 10pt;
}
QMenuBar::item {
    padding: 4px 10px;
    border-radius: 5px;
    background: transparent;
}
QMenuBar::item:selected, QMenuBar::item:pressed {
    background: rgba(255, 255, 255, 30);
}
QMenuBar::item:disabled { color: #7a7a82; }
QLabel#clock {
    color: #e8e8ec;
    font-size: 10pt;
    padding: 0 8px 0 12px;
}
QMenuBar#session::item, QMenuBar#sound::item, QMenuBar#network::item,
QMenuBar#battery::item, QMenuBar#bluetooth::item, QMenuBar#tray::item {
    padding: 4px 8px;
}
QMenuBar#battery::item { padding: 4px 3px; }
/* Keyboard layout: KeyboardBar draws the frame and the letters; only the item width is set here.
   No vertical padding, otherwise with a large font the item doesn't fit
   vertically and QMenuBar hides it behind the "»" button. */
QMenuBar#keyboard::item {
    padding: 0px 10px;
    font-size: 8pt;
    font-weight: bold;
}
)";

// Keyboard layout indicator: "En" in a frame, as in Unity. QMenuBar opens the menu,
// but we paint it ourselves, so the item doesn't depend on style padding and always fits the panel.
class KeyboardBar : public QMenuBar
{
public:
    using QMenuBar::QMenuBar;

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (actions().isEmpty())
            return;
        QAction *a = actions().first();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QFont f = font();
        f.setPointSizeF(8);
        f.setBold(true);
        p.setFont(f);
        const QFontMetrics fm(f);
        const QRect item = actionGeometry(a);
        const int w = fm.horizontalAdvance(a->text()) + 8;
        const int h = qMin(fm.ascent() + fm.descent() + 2, height() - 6);
        const QRectF frame(item.center().x() - w / 2.0 + 0.5, (height() - h) / 2.0 + 0.5, w, h);
        const bool hot = activeAction() == a || underMouse();
        if (hot) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 40));
            p.drawRoundedRect(QRectF(item.adjusted(2, 2, -2, -2)), 5, 5);
        }
        const QColor fg = a->isEnabled() ? QColor(0xe8, 0xe8, 0xec) : QColor(0x7a, 0x7a, 0x82);
        p.setPen(QPen(fg, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(frame, 3, 3);
        p.drawText(frame, Qt::AlignCenter, a->text());
    }
    void enterEvent(QEnterEvent *e) override { QMenuBar::enterEvent(e); update(); }
    void leaveEvent(QEvent *e) override { QMenuBar::leaveEvent(e); update(); }
};

// A window button with an image from the labwc theme. Draws its own hover highlight
// if the theme has no separate *_hover image.
class WindowButton : public QAbstractButton
{
public:
    WindowButton(LabwcTheme *theme, LabwcTheme::Button kind, QWidget *parent)
        : QAbstractButton(parent), m_theme(theme), m_kind(kind)
    {
        setFixedSize(26, 24);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::NoFocus);
        static const char *const tips[] = {QT_TRANSLATE_NOOP("Panel", "Close"), QT_TRANSLATE_NOOP("Panel", "Minimize"),
                                           QT_TRANSLATE_NOOP("Panel", "Restore")};
        setToolTip(QCoreApplication::translate("Panel", tips[kind]));
    }
    LabwcTheme::Button kind() const { return m_kind; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool hover = underMouse();
        if ((hover && !m_theme->hasHoverImage(m_kind)) || isDown()) {
            QColor bg = m_theme->hoverBackground();
            if (isDown())
                bg.setAlpha(qMin(255, bg.alpha() * 2));
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 5, 5);
        }
        const QPixmap pm = m_theme->button(m_kind, hover);
        const QSize sz = pm.deviceIndependentSize().toSize();
        p.drawPixmap(QRect(QPoint((width() - sz.width()) / 2, (height() - sz.height()) / 2), sz), pm);
    }
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    LabwcTheme *m_theme;
    LabwcTheme::Button m_kind;
};

Panel::Panel(MenuController *controller, LabwcTheme *theme, const Indicators &ind, QScreen *screen)
    : m_controller(controller)
    , m_screen(screen)
    , m_appBar(new QMenuBar(this))
    , m_bar(new QMenuBar(this))
    , m_clock(new QLabel(this))
    , m_sessionBar(new QMenuBar(this))
    , m_soundBar(new QMenuBar(this))
    , m_netBar(new QMenuBar(this))
    , m_kbdBar(new KeyboardBar(this))
    , m_batBar(new QMenuBar(this))
    , m_btBar(new QMenuBar(this))
    , m_trayBar(new QMenuBar(this))
    , m_ind(ind)
    , m_sound(ind.sound)
    , m_theme(theme)
    , m_winButtons(new QWidget(this))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlag(Qt::FramelessWindowHint);
    setStyleSheet(kStyle);

    // Otherwise QMenuBar would try to export itself over D-Bus, i.e. to ourselves.
    m_bar->setNativeMenuBar(false);
    m_appBar->setNativeMenuBar(false);
    m_appBar->setStyleSheet(QStringLiteral("QMenuBar { font-weight: bold; }"));
    m_appBar->addAction(controller->windowMenu()->menuAction());
    m_sessionBar->setNativeMenuBar(false);
    m_sessionBar->setObjectName(QStringLiteral("session"));
    m_sessionBar->addAction(ind.session->menu()->menuAction());
    m_soundBar->setNativeMenuBar(false);
    m_soundBar->setObjectName(QStringLiteral("sound"));
    m_soundBar->addAction(ind.sound->menu()->menuAction());
    m_soundBar->installEventFilter(this); // wheel and middle button
    m_netBar->setNativeMenuBar(false);
    m_netBar->setObjectName(QStringLiteral("network"));
    m_netBar->addAction(ind.network->menu()->menuAction());
    m_kbdBar->setNativeMenuBar(false);
    m_kbdBar->setObjectName(QStringLiteral("keyboard"));
    m_kbdBar->addAction(ind.keyboard->menu()->menuAction());
    m_kbdBar->installEventFilter(this); // click: next layout; right-click: menu
    m_kbdBar->setFixedHeight(kHeight);  // highlight and clicks cover the full panel height
    m_batBar->setNativeMenuBar(false);
    m_batBar->setObjectName(QStringLiteral("battery"));
    m_batBar->addAction(ind.battery->menu()->menuAction());
    m_batBar->addAction(ind.battery->labelAction()); // "85%" next to the icon, if enabled
    m_batBar->installEventFilter(this);
    m_btBar->setNativeMenuBar(false);
    m_btBar->setObjectName(QStringLiteral("bluetooth"));
    m_btBar->addAction(ind.bluetooth->menu()->menuAction());
    m_trayBar->setNativeMenuBar(false);
    m_trayBar->setObjectName(QStringLiteral("tray"));
    m_trayBar->installEventFilter(this); // clicks on icons go to the program (Activate, menu, wheel)
    connect(ind.tray, &SystemTray::changed, this, &Panel::rebuildTray);
    connect(ind.tray, &SystemTray::menuRequested, this, [this](QAction *a) {
        // The program doesn't support Activate: open the menu where it was clicked
        if (a == m_trayClicked && m_trayBar->actions().contains(a))
            m_trayBar->setActiveAction(a);
    });
    for (QMenuBar *b : {m_btBar, m_netBar, m_sessionBar})
        b->installEventFilter(this); // tooltips
    for (QObject *o : {static_cast<QObject *>(ind.keyboard), static_cast<QObject *>(ind.battery),
                       static_cast<QObject *>(ind.bluetooth)})
        connect(o, SIGNAL(changed()), this, SLOT(updateIndicatorsSlot()));

    m_clock->setObjectName(QStringLiteral("clock"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_winButtons, 0, Qt::AlignVCenter);
    layout->addWidget(m_appBar, 0, Qt::AlignVCenter);
    layout->addWidget(m_bar, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    layout->addWidget(m_trayBar, 0, Qt::AlignVCenter);
    layout->addSpacing(4);
    layout->addWidget(m_btBar, 0, Qt::AlignVCenter);
    layout->addWidget(m_batBar, 0, Qt::AlignVCenter);
    layout->addWidget(m_kbdBar, 0, Qt::AlignVCenter);
    layout->addWidget(m_netBar, 0, Qt::AlignVCenter);
    layout->addWidget(m_soundBar, 0, Qt::AlignVCenter);
    layout->addWidget(m_clock, 0, Qt::AlignVCenter);
    layout->addWidget(m_sessionBar, 0, Qt::AlignVCenter);
    layout->addSpacing(4);

    connect(controller, &MenuController::changed, this, &Panel::rebuild);
    rebuild();
    updateIndicators();
    rebuildTray();

    auto *wl = new QHBoxLayout(m_winButtons);
    wl->setContentsMargins(0, 0, 6, 0);
    wl->setSpacing(0);
    buildWindowButtons();
    connect(theme, &LabwcTheme::changed, this, [this] {
        buildWindowButtons(); // the theme or the button order may have changed
        updateWindowButtons();
    });
    connect(controller, &MenuController::activeWindowChanged, this, &Panel::updateWindowButtons);
    updateWindowButtons();
    // Double-clicking an empty spot of the menu bar restores the window (like on the title bar)
    m_bar->installEventFilter(this);

    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Panel::updateClock);
    timer->start(1000);
    updateClock();

    setFixedHeight(kHeight);
    resize(screen->geometry().width(), kHeight);
}

void Panel::setupLayerShell()
{
    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        // For debugging under X11/offscreen: a plain frameless window at the top of the screen.
        setWindowFlag(Qt::WindowStaysOnTopHint);
        setGeometry(m_screen->geometry().x(), m_screen->geometry().y(), m_screen->geometry().width(), kHeight);
        return;
    }

    setScreen(m_screen);
    winId(); // creates the QWindow while it is not shown yet
    LayerShellQt::Window *ls = LayerShellQt::Window::get(windowHandle());
    if (!ls)
        return;
    ls->setScope(QStringLiteral("qutaq-panel"));
    ls->setLayer(LayerShellQt::Window::LayerTop);
    ls->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop
                                                 | LayerShellQt::Window::AnchorLeft
                                                 | LayerShellQt::Window::AnchorRight));
    ls->setExclusiveZone(kHeight); // windows won't go under the panel
    // OnDemand: the panel gets the keyboard only on click; needed for navigating menus with arrow keys.
    ls->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
#if LSQ_VERSION_MAJOR > 6 || (LSQ_VERSION_MAJOR == 6 && LSQ_VERSION_MINOR >= 6)
    ls->setWantsToBeOnActiveScreen(false);
    ls->setScreen(m_screen);
#endif // in 6.0–6.5 the output is taken from QWindow::screen(), set above
}

void Panel::rebuild()
{
    m_bar->clear(); // our QActions belong to the importer; clear() only detaches them
    m_bar->addActions(m_controller->menuActions());
}

void Panel::buildWindowButtons()
{
    QLayout *wl = m_winButtons->layout();
    while (QLayoutItem *item = wl->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    // Same order as in Unity: close, minimize, restore
    const QStringList order = panelSetting(QStringLiteral("window/buttonLayout"), QStringLiteral("close,iconify,max"))
                                  .toString().split(',', Qt::SkipEmptyParts);
    for (const QString &raw : order) {
        const QString name = raw.trimmed();
        LabwcTheme::Button kind;
        if (name == QLatin1String("close")) kind = LabwcTheme::Close;
        else if (name == QLatin1String("iconify")) kind = LabwcTheme::Iconify;
        else if (name == QLatin1String("max")) kind = LabwcTheme::Restore;
        else continue;
        auto *b = new WindowButton(m_theme, kind, m_winButtons);
        connect(b, &QAbstractButton::clicked, this, [this, kind] {
            switch (kind) {
            case LabwcTheme::Close: m_controller->closeActive(); break;
            case LabwcTheme::Iconify: m_controller->minimizeActive(); break;
            case LabwcTheme::Restore: m_controller->restoreActive(); break;
            }
        });
        wl->addWidget(b);
    }
}

void Panel::updateWindowButtons()
{
    // auto: only if labwc itself removes the title bar of maximized windows
    // (<maximizedDecoration>none</maximizedDecoration>), otherwise the buttons would be in two places.
    const QString mode = panelSetting(QStringLiteral("window/panelButtons"), QStringLiteral("auto")).toString();
    bool show = m_controller->activeMaximized();
    if (mode == QLatin1String("never"))
        show = false;
    else if (mode != QLatin1String("always"))
        show = show && m_theme->maximizedWithoutTitlebar();
    // isHidden(), not isVisible(): until the panel itself is shown, isVisible() is always false,
    // and the buttons could not be hidden in advance.
    if (show == m_winButtons->isHidden())
        m_winButtons->setVisible(show);
    for (QWidget *w : m_winButtons->findChildren<QWidget *>())
        w->update();
}

void Panel::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_winButtons->isVisible())
        m_controller->restoreActive();
    QWidget::mouseDoubleClickEvent(event);
}

void Panel::updateIndicators()
{
    // Show only what this device has (desktop PCs often have no battery or Bluetooth)
    m_kbdBar->setVisible(m_ind.keyboard->isShown());
    m_batBar->setVisible(m_ind.battery->isShown());
    m_btBar->setVisible(m_ind.bluetooth->isShown());
}

void Panel::rebuildTray()
{
    m_trayBar->clear(); // the QActions belong to the icons; clear() only detaches them
    for (TrayItem *i : m_ind.tray->visibleItems())
        m_trayBar->addAction(i->action());
    m_trayBar->setVisible(!m_trayBar->actions().isEmpty());
}

bool Panel::eventFilter(QObject *watched, QEvent *event)
{
    // Tooltips for the icons on the right (QMenuBar doesn't show them itself)
    if (event->type() == QEvent::ToolTip) {
        if (auto *bar = qobject_cast<QMenuBar *>(watched); bar && bar != m_bar && bar != m_appBar) {
            auto *he = static_cast<QHelpEvent *>(event);
            QAction *a = bar->actionAt(he->pos());
            const QString tip = a ? a->toolTip() : QString();
            if (!tip.isEmpty() && tip != a->text() && !(a->menu() && a->menu()->isVisible()))
                QToolTip::showText(he->globalPos(), tip, bar, bar->actionGeometry(a));
            else
                QToolTip::hideText();
            return true;
        }
    }
    if (watched == m_trayBar) {
        const QEvent::Type t = event->type();
        if (t == QEvent::MouseButtonPress || t == QEvent::MouseButtonRelease || t == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(event);
            QAction *a = m_trayBar->actionAt(me->position().toPoint());
            TrayItem *item = a ? m_ind.tray->itemFor(a) : nullptr;
            if (!item)
                return false;
            if (t != QEvent::MouseButtonPress)
                return true; // QMenuBar must not "press" the item by itself
            QToolTip::hideText();
            const QPoint gp = me->globalPosition().toPoint();
            m_trayClicked = a;
            switch (me->button()) {
            case Qt::LeftButton:
                if (item->itemIsMenu() && item->menu())
                    m_trayBar->setActiveAction(a);
                else
                    item->activate(gp); // if unsupported, the menu opens (menuRequested)
                break;
            case Qt::RightButton:
                if (item->menu())
                    m_trayBar->setActiveAction(a);
                else
                    item->contextMenu(gp);
                break;
            case Qt::MiddleButton:
                item->secondaryActivate(gp);
                break;
            default:
                break;
            }
            return true;
        }
        if (t == QEvent::Wheel) {
            auto *we = static_cast<QWheelEvent *>(event);
            if (TrayItem *item = m_ind.tray->itemFor(m_trayBar->actionAt(we->position().toPoint()))) {
                const QPoint d = we->angleDelta();
                if (d.y() != 0)
                    item->scroll(d.y(), Qt::Vertical);
                else if (d.x() != 0)
                    item->scroll(d.x(), Qt::Horizontal);
            }
            return true;
        }
    }
    if (watched == m_kbdBar && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            m_ind.keyboard->Next();
            return true;
        }
        if (me->button() == Qt::RightButton) {
            m_kbdBar->setActiveAction(m_ind.keyboard->menu()->menuAction());
            return true;
        }
    }
    if (watched == m_batBar && event->type() == QEvent::MouseButtonPress) {
        // Clicking the percentage opens the same menu as clicking the icon
        auto *me = static_cast<QMouseEvent *>(event);
        if (m_batBar->actionAt(me->position().toPoint()) == m_ind.battery->labelAction()) {
            m_batBar->setActiveAction(m_ind.battery->menu()->menuAction());
            return true;
        }
    }
    if (watched == m_bar && event->type() == QEvent::MouseButtonDblClick) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && !m_bar->actionAt(me->position().toPoint()) && m_winButtons->isVisible()) {
            m_controller->restoreActive();
            return true;
        }
    }
    if (watched == m_soundBar) {
        if (event->type() == QEvent::Wheel) {
            // As in Unity: the wheel over the icon changes the volume
            const int step = qMax(1, QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                                                   + QStringLiteral("/qutaq-panel/panel.conf"),
                                               QSettings::IniFormat)
                                         .value(QStringLiteral("sound/step"), 5)
                                         .toInt());
            const QPoint d = static_cast<QWheelEvent *>(event)->angleDelta();
            const int delta = d.y() != 0 ? d.y() : -d.x();
            if (delta != 0)
                m_sound->changeVolume(delta > 0 ? step : -step);
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress
            && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton) {
            m_sound->toggleMute(); // middle button: mute/unmute
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void Panel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), QColor(24, 25, 30, 235));
}

void Panel::updateClock()
{
    const QDateTime now = QDateTime::currentDateTime();
    m_clock->setText(QLocale().toString(now, QStringLiteral("ddd d MMM   HH:mm")));
}
