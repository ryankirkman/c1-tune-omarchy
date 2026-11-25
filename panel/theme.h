#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>

// Follows the Omarchy theme live: colors from the current theme's colors.toml, and
// Hyprland's window rounding as `radius`.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QString background READ background NOTIFY changed)
    Q_PROPERTY(QString raised READ raised NOTIFY changed)
    Q_PROPERTY(QString foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QString bright READ bright NOTIFY changed)
    Q_PROPERTY(QString dim READ dim NOTIFY changed)
    Q_PROPERTY(QString muted READ muted NOTIFY changed)
    Q_PROPERTY(QString red READ red NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr);

    QString color(const QString &name, const QString &fallback) const { return m_colors.value(name, fallback); }
    QString accent() const { return color("accent", "#FFD60A"); }
    QString accentForeground() const;
    QString background() const { return color("background", "#1a1b26"); }
    QString raised() const { return color("lighter_background", "#24283b"); }
    QString foreground() const { return color("foreground", "#a9b1d6"); }
    QString bright() const { return color("bright_foreground", foreground()); }
    QString dim() const { return color("dark_foreground", "#565f89"); }
    QString muted() const { return color("muted", "#414868"); }
    QString red() const { return color("red", "#f7768e"); }
    bool dark() const { return m_dark; }
    int radius() const { return m_radius; }

signals:
    void changed();

private:
    void reload();
    void refreshRounding();

    QString m_directory;
    QHash<QString, QString> m_colors;
    bool m_dark = true;
    int m_radius = 0;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QProcess m_rounding;
};
