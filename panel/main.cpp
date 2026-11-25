// c1-tune-panel: the control panel for C1 Tune. Opened and closed by `c1-tune --panel`.

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <LayerShellQt/Window>

#include "backend.h"
#include "theme.h"

namespace {

QJsonDocument hyprctl(const QString &what) {
    const QString tool = QStandardPaths::findExecutable("hyprctl");
    if (tool.isEmpty() || qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
        return {};
    QProcess process;
    process.start(tool, {"-j", what});
    if (!process.waitForFinished(500))
        return {};
    return QJsonDocument::fromJson(process.readAllStandardOutput());
}

// Where the panel goes. Opened from the bar it drops from the bar, centred on the pointer
// (which is on the icon that was just clicked); opened any other way it sits in the middle
// of the screen. `edge` is the screen edge it hangs off, or empty for the middle.
struct Placement {
    QString screen;
    QString edge;
    int offset = 0;     // from the left (top or bottom edge) or the top (left or right edge)
};

Placement placePanel(int panelWidth) {
    Placement placement;
    const QJsonObject cursor = hyprctl("cursorpos").object();
    const QJsonArray monitors = hyprctl("monitors").array();
    for (const auto &value : monitors) {
        const QJsonObject m = value.toObject();
        const double scale = m.value("scale").toDouble(1);
        const int x = cursor.value("x").toInt() - m.value("x").toInt(), y = cursor.value("y").toInt() - m.value("y").toInt();
        const int width = int(m.value("width").toInt() / scale), height = int(m.value("height").toInt() / scale);
        if (cursor.isEmpty() || x < 0 || y < 0 || x >= width || y >= height)
            continue;
        placement.screen = m.value("name").toString();
        // What the bar (or a dock) keeps for itself on each side: left, top, right, bottom.
        const QJsonArray reserved = m.value("reserved").toArray();
        const int left = reserved.at(0).toInt(), top = reserved.at(1).toInt(), right = reserved.at(2).toInt(), bottom = reserved.at(3).toInt();
        if (top > 0 && y < top) placement.edge = "top";
        else if (bottom > 0 && y >= height - bottom) placement.edge = "bottom";
        else if (left > 0 && x < left) placement.edge = "left";
        else if (right > 0 && x >= width - right) placement.edge = "right";
        const bool horizontal = placement.edge == "top" || placement.edge == "bottom";
        placement.offset = horizontal ? qBound(8, x - panelWidth / 2, qMax(8, width - panelWidth - 8)) : qMax(8, y - 60);
        break;
    }
    return placement;
}

}

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
    // A popover, like the bar's own: a layer surface that closes when you click elsewhere.
    // (C1TUNE_PANEL_WINDOW=1 makes it an ordinary window instead; the screenshot mode and
    // other desktops need that.)
    const bool popover = app.platformName().startsWith("wayland") && qEnvironmentVariableIsEmpty("C1TUNE_PANEL_WINDOW")
        && qEnvironmentVariableIsEmpty("C1TUNE_PANEL_SCREENSHOT");
    engine.rootContext()->setContextProperty("popover", popover);
    engine.load(qmlDirectory.isEmpty() ? QUrl("qrc:/Main.qml") : QUrl::fromLocalFile(qmlDirectory + "/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return 1;
    if (popover) {
        const Placement placement = placePanel(window->width());
        for (QScreen *screen : app.screens())
            if (screen->name() == placement.screen)
                window->setScreen(screen);
        auto *layer = LayerShellQt::Window::get(window);
        layer->setScope("c1-tune");
        layer->setLayer(LayerShellQt::Window::LayerTop);
        layer->setExclusiveZone(0);
        // On demand: it takes the keyboard while it is being used and gives it up when
        // something else is clicked, which is also how it knows to close.
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        const int gap = 6;
        using W = LayerShellQt::Window;
        if (placement.edge == "top") { layer->setAnchors(W::Anchors(W::AnchorTop | W::AnchorLeft)); layer->setMargins({placement.offset, gap, 0, 0}); }
        else if (placement.edge == "bottom") { layer->setAnchors(W::Anchors(W::AnchorBottom | W::AnchorLeft)); layer->setMargins({placement.offset, 0, 0, gap}); }
        else if (placement.edge == "left") { layer->setAnchors(W::Anchors(W::AnchorLeft | W::AnchorTop)); layer->setMargins({gap, placement.offset, 0, 0}); }
        else if (placement.edge == "right") { layer->setAnchors(W::Anchors(W::AnchorRight | W::AnchorTop)); layer->setMargins({0, placement.offset, gap, 0}); }
        else layer->setAnchors(W::AnchorNone);
    }
    window->show();
    window->requestActivate();

    // For the README's screenshot and for checking the layout without a desktop:
    // C1TUNE_PANEL_SCREENSHOT=<file> saves a picture of the panel and exits.
    const QString shot = qEnvironmentVariable("C1TUNE_PANEL_SCREENSHOT");
    if (!shot.isEmpty()) {
        QTimer::singleShot(1200, &app, [&] {
            window->grabWindow().save(shot);
            app.quit();
        });
    }

    return app.exec();
}
