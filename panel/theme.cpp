// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

Theme::Theme(const QString &directory, QObject *parent)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal overrides can
    // both set, so ask hyprctl rather than reading its config: once now, and again whenever
    // the theme changes. Without Hyprland the corners stay sharp, like Omarchy's default.
    connect(&m_rounding, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const auto value = QJsonDocument::fromJson(m_rounding.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    reload();
}

void Theme::refreshRounding() {
    if (qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") || m_rounding.state() != QProcess::NotRunning)
        return;
    const QString hyprctl = QStandardPaths::findExecutable("hyprctl");
    if (!hyprctl.isEmpty())
        m_rounding.start(hyprctl, {"-j", "getoption", "decoration:rounding"});
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QString Theme::accentForeground() const {
    const QColor c(accent());
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? "black" : "white";
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and re-arm after
    // every change. Watching the nearest existing ancestor means installing Omarchy later
    // works without restarting.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    QHash<QString, QString> colors;
    bool dark = true;
    QFile file(m_directory + "/theme/colors.toml");
    if (file.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(file.readAll());
        static const QRegularExpression color(R"re(^\s*([a-z_]+)\s*=\s*["'](#[0-9a-fA-F]{6})["'])re",
                                              QRegularExpression::MultilineOption);
        for (auto it = color.globalMatch(text); it.hasNext();) {
            const auto match = it.next();
            colors.insert(match.captured(1), match.captured(2));
        }
        static const QRegularExpression mode(R"re(^\s*mode\s*=\s*["']light["'])re", QRegularExpression::MultilineOption);
        dark = !mode.match(text).hasMatch();
    }
    if (colors != m_colors || dark != m_dark) {
        m_colors = colors;
        m_dark = dark;
        emit changed();
    }
}
