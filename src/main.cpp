#include "hud.h"
#include "menucontroller.h"
#include "panel.h"
#include "sessionmenu.h"
#include "soundmenu.h"
#include "networkmenu.h"
#include "keyboardindicator.h"
#include "batteryindicator.h"
#include "bluetoothindicator.h"
#include "labwctheme.h"
#include "statusnotifier.h"
#include "settings.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <cstring>
#include <QDebug>
#include <QHash>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
#include <QProxyStyle>
#include <QScreen>

// Underlined mnemonics look noisy in the panel, so don't draw them (Alt navigation still works).
class PanelStyle : public QProxyStyle
{
public:
    int styleHint(StyleHint hint, const QStyleOption *opt, const QWidget *w, QStyleHintReturn *ret) const override
    {
        if (hint == SH_UnderlineShortcut)
            return 0;
        return QProxyStyle::styleHint(hint, opt, w, ret);
    }
};

// GTK describes actions without a parameter with an empty signature "g", and QtDBus logs a warning
// for each such action. That is harmless; silence just this one.
static QtMessageHandler s_defaultHandler = nullptr;
static void messageFilter(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (type == QtWarningMsg && msg.startsWith(QLatin1String("QDBusSignature: invalid signature")))
        return;
    s_defaultHandler(type, ctx, msg);
}

// qutaq-panel --next-layout | --prev-layout, for a keybind in labwc's rc.xml:
// asks the running panel to switch the layout.
static int remoteCommand(int argc, char *argv[], const char *method)
{
    QCoreApplication app(argc, argv);
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.qutaq.Panel"), QStringLiteral("/Keyboard"),
                                                      QStringLiteral("org.qutaq.Panel.Keyboard"),
                                                      QString::fromLatin1(method));
    const QDBusMessage r = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    if (r.type() == QDBusMessage::ErrorMessage) {
        fprintf(stderr, "qutaq-panel: %s\n", qPrintable(r.errorMessage()));
        return 1;
    }
    return 0;
}

// UI language: from the settings ([panel] language=ru|en|de|…), otherwise the system one (LANG).
// Translations are built into the program (:/i18n); .qm files in ~/.local/share/qutaq-panel/translations
// (or /usr/share/…) take priority, so a translator can test a translation without rebuilding.
static void setupTranslations(QApplication &app)
{
    const QString lang = panelSetting(QStringLiteral("panel/language")).toString().trimmed();
    QLocale locale = QLocale::system();
    if (!lang.isEmpty() && lang != QLatin1String("auto")) {
        locale = QLocale(lang);
        QLocale::setDefault(locale);                 // the clock date uses the same language
        qputenv("LANGUAGE", lang.toUtf8());          // gettext: layout names
    }

    auto *qt = new QTranslator(&app); // buttons of Qt's standard dialogs
    if (qt->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                 QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(qt);
    else
        delete qt;

    QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                                 QStringLiteral("qutaq-panel/translations"),
                                                 QStandardPaths::LocateDirectory);
    dirs << QStringLiteral(":/i18n");
    for (const QString &dir : std::as_const(dirs)) {
        auto *own = new QTranslator(&app);
        if (own->load(locale, QStringLiteral("qutaq-panel"), QStringLiteral("_"), dir)) {
            app.installTranslator(own);
            qInfo().noquote() << "translation:" << own->filePath();
            return;
        }
        delete own;
    }
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--next-layout"))
            return remoteCommand(argc, argv, "Next");
        if (!strcmp(argv[i], "--prev-layout"))
            return remoteCommand(argc, argv, "Previous");
    }
    s_defaultHandler = qInstallMessageHandler(messageFilter);
    // Don't call LayerShellQt::Shell::useLayerShell(): with Qt >= 6.5 layer-shell is enabled
    // only for windows that called LayerShellQt::Window::get() before being shown (see Panel).
    // The panel draws its own QMenuBar and doesn't export it anywhere.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qutaq-panel"));
    app.setQuitOnLastWindowClosed(false);
    app.setStyle(new PanelStyle);
    setupTranslations(app);

    setupIconTheme(); // before creating menus and icons

    MenuController controller;
    if (!controller.start())
        return 1;
    SessionMenu session;
    SystemTray tray;
    BluetoothIndicator bluetooth;
    BatteryIndicator battery;
    KeyboardIndicator keyboard;
    NetworkMenu network;
    SoundMenu sound;
    LabwcTheme theme;

    // Commands for keybinds: org.qutaq.Panel /Keyboard Next|Previous|Set
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Keyboard"), &keyboard, QDBusConnection::ExportAllSlots);
    // The menus of the active application for the HUD (qutaq-dash --hud): org.qutaq.Panel /Hud
    Hud hud(&controller);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Hud"), &hud, QDBusConnection::ExportAllSlots);
    QDBusConnection::sessionBus().registerService(QStringLiteral("org.qutaq.Panel"));

    // One panel per monitor; all show the same menu (shared QActions).
    QHash<QScreen *, Panel *> panels;
    auto addPanel = [&](QScreen *screen) {
        auto *p = new Panel(&controller, &theme, {&bluetooth, &battery, &keyboard, &network, &sound, &session, &tray}, screen);
        p->setupLayerShell();
        p->show();
        panels.insert(screen, p);
    };
    for (QScreen *s : app.screens())
        addPanel(s);
    QObject::connect(&app, &QGuiApplication::screenAdded, &app, addPanel);
    QObject::connect(&app, &QGuiApplication::screenRemoved, &app, [&](QScreen *s) {
        if (Panel *p = panels.take(s))
            p->deleteLater();
    });

    return app.exec();
}
