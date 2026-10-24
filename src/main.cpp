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

// Подчёркнутые мнемоники в панели выглядят шумно — не рисуем их (Alt-навигация работает).
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

// GTK описывает действия без параметра пустой сигнатурой "g", и QtDBus на каждое такое
// действие пишет предупреждение. Это нормально, глушим только его.
static QtMessageHandler s_defaultHandler = nullptr;
static void messageFilter(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (type == QtWarningMsg && msg.startsWith(QLatin1String("QDBusSignature: invalid signature")))
        return;
    s_defaultHandler(type, ctx, msg);
}

// qutaq-panel --next-layout | --prev-layout — для горячей клавиши в rc.xml labwc:
// просит уже запущенную панель переключить раскладку.
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

// Язык интерфейса: из настроек ([panel] language=ru|en|de|…), иначе системный (LANG).
// Переводы встроены в программу (:/i18n); файлы .qm в ~/.local/share/qutaq-panel/translations
// (или /usr/share/…) имеют приоритет — так переводчик проверяет свой перевод без пересборки.
static void setupTranslations(QApplication &app)
{
    const QString lang = panelSetting(QStringLiteral("panel/language")).toString().trimmed();
    QLocale locale = QLocale::system();
    if (!lang.isEmpty() && lang != QLatin1String("auto")) {
        locale = QLocale(lang);
        QLocale::setDefault(locale);                 // дата в часах — на том же языке
        qputenv("LANGUAGE", lang.toUtf8());          // gettext: названия раскладок
    }

    auto *qt = new QTranslator(&app); // кнопки стандартных диалогов Qt
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
    // LayerShellQt::Shell::useLayerShell() не вызываем: с Qt >= 6.5 layer-shell включается
    // только для окон, у которых до показа вызван LayerShellQt::Window::get() (см. Panel).
    // Свой QMenuBar панель рисует сама и никуда не экспортирует.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qutaq-panel"));
    app.setQuitOnLastWindowClosed(false);
    app.setStyle(new PanelStyle);
    setupTranslations(app);

    setupIconTheme(); // до создания меню и значков

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

    // Команды для горячих клавиш: org.qutaq.Panel /Keyboard Next|Previous|Set
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Keyboard"), &keyboard, QDBusConnection::ExportAllSlots);
    QDBusConnection::sessionBus().registerService(QStringLiteral("org.qutaq.Panel"));

    // По панели на каждый монитор; все показывают одно и то же меню (QAction общие).
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
