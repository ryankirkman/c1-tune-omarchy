// c1-tune-panel: the control panel for C1 Tune. Opened and closed by `c1-tune --panel`.

#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include "backend.h"
#include "theme.h"

int main(int argc, char *argv[]) {
    // The compact Material variant: the panel is a column of twenty-odd small controls.
    qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", "Dense");

    QGuiApplication app(argc, argv);
    app.setApplicationName("c1-tune");
    app.setApplicationVersion(PANEL_VERSION);

    // Matches the window to c1-tune.desktop, so the compositor (Wayland app_id) and the
    // launcher pick up the installed icon.
    app.setDesktopFileName("c1-tune");
    app.setWindowIcon(QIcon::fromTheme("c1-tune"));

    // One panel at a time, and asking for it again closes it: that makes a single bar
    // button or keybinding a toggle. A second copy only has to connect for the first to quit.
    const QString toggle = qEnvironmentVariable("XDG_RUNTIME_DIR", "/tmp") + "/c1-tune-panel.sock";
    if (!app.arguments().contains("--no-toggle")) {
        QLocalSocket existing;
        existing.connectToServer(toggle);
        if (existing.waitForConnected(200))
            return 0;
    }
    QLocalServer server;
    QLocalServer::removeServer(toggle);
    server.listen(toggle);
    QObject::connect(&server, &QLocalServer::newConnection, &app, &QCoreApplication::quit);

    QQuickStyle::setStyle("Material");

    // C1TUNE_THEME_DIR stands in for ~/.local/state/omarchy/current when testing themes.
    Theme theme(qEnvironmentVariable("C1TUNE_THEME_DIR"));
    Backend backend;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("maxHeight", qEnvironmentVariableIntValue("C1TUNE_PANEL_MAX_HEIGHT"));
    // C1TUNE_QML_DIR loads the QML from a directory instead, for working on the layout.
    const QString qmlDirectory = qEnvironmentVariable("C1TUNE_QML_DIR");
    engine.load(qmlDirectory.isEmpty() ? QUrl("qrc:/Main.qml") : QUrl::fromLocalFile(qmlDirectory + "/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    // For the README's screenshot and for checking the layout without a desktop:
    // C1TUNE_PANEL_SCREENSHOT=<file> saves a picture of the panel and exits.
    const QString shot = qEnvironmentVariable("C1TUNE_PANEL_SCREENSHOT");
    if (!shot.isEmpty()) {
        QTimer::singleShot(1200, &app, [&] {
            if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()))
                window->grabWindow().save(shot);
            app.quit();
        });
    }

    return app.exec();
}
