#include "backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

static QString socketPath() {
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR", "/tmp");
    return runtime + "/c1-tune.sock";
}

static QString autostartPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart/c1-tune.desktop";
}

// The app's binary sits beside the panel's, installed or in the build directory.
static QString appBinary() { return QCoreApplication::applicationDirPath() + "/c1-tune"; }

Backend::Backend(QObject *parent) : QObject(parent) {
    connect(&m_socket, &QLocalSocket::readyRead, this, &Backend::read);
    connect(&m_socket, &QLocalSocket::connected, this, [this] { m_socket.write("watch\n"); emit changed(); });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] { m_buffer.clear(); emit changed(); });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] { emit changed(); });
    connectToApp();
}

void Backend::connectToApp() {
    if (m_socket.state() == QLocalSocket::UnconnectedState)
        m_socket.connectToServer(socketPath());
}

void Backend::request(const QByteArray &line) {
    if (connected())
        m_socket.write(line + "\n");
}

void Backend::set(const QString &key, const QVariant &value) {
    QString text = value.isNull() ? "default" : value.typeId() == QMetaType::Bool ? (value.toBool() ? "true" : "false")
                                              : value.toString();
    if (text.contains('\t') || text.contains('\n'))
        return;
    request("set\t" + key.toUtf8() + "=" + text.toUtf8());
}

void Backend::startApp() {
    if (connected())
        return;
    QProcess::startDetached(appBinary(), {});
    QTimer::singleShot(300, this, &Backend::connectToApp);
    QTimer::singleShot(1000, this, &Backend::connectToApp);
}

void Backend::read() {
    m_buffer += m_socket.readAll();
    int newline;
    QByteArray last;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (line.startsWith('{'))
            last = line;   // only the newest state matters
    }
    if (last.isEmpty())
        return;
    const QJsonObject state = QJsonDocument::fromJson(last).object();
    m_status = state.value("status").toString();
    m_message = state.value("message").toString();
    m_restartPending = state.value("restartPending").toBool();
    m_version = state.value("version").toString();
    m_settings = state.value("settings").toObject().toVariantMap();
    m_zoomStops = state.value("zoomStops").toVariant().toList();
    emit changed();
}

// "Open at login" is an XDG autostart entry, which Omarchy's session starts like any other.
bool Backend::openAtLogin() const { return QFile::exists(autostartPath()); }

void Backend::setOpenAtLogin(bool on) {
    if (on == openAtLogin())
        return;
    if (on) {
        QDir().mkpath(QFileInfo(autostartPath()).absolutePath());
        QSaveFile file(autostartPath());
        if (file.open(QIODevice::WriteOnly)) {
            file.write("[Desktop Entry]\nType=Application\nName=C1 Tune\nComment=Keeps the Opal C1 webcam tuned\nExec="
                       + appBinary().toUtf8() + "\nIcon=c1-tune\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n");
            file.commit();
        }
    } else {
        QFile::remove(autostartPath());
    }
    emit openAtLoginChanged();
}

bool Backend::preview() const { return QSettings("c1-tune", "panel").value("preview", false).toBool(); }

void Backend::setPreview(bool on) {
    if (on == preview())
        return;
    QSettings("c1-tune", "panel").setValue("preview", on);
    emit previewChanged();
}

QString Backend::cameraNode() const {
    auto attribute = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.readAll()).trimmed() : QString();
    };
    // Each webcam has two nodes, pictures and metadata; pictures come first.
    QStringList names = QDir("/sys/class/video4linux").entryList({"video*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) { return a.mid(5).toInt() < b.mid(5).toInt(); });
    for (const QString &name : names) {
        const QString usb = "/sys/class/video4linux/" + name + "/device/..";
        if (attribute(usb + "/idVendor") == "03e7" && attribute(usb + "/idProduct") == "f63b")
            return "/dev/" + name;
    }
    return {};
}
