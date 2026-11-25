#pragma once

#include <QLocalSocket>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

// The panel's view of the running c1-tune: its state arrives as JSON lines on the app's
// socket, and every change the user makes goes back as a `set` request. The panel holds no
// camera logic of its own.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(bool restartPending READ restartPending NOTIFY changed)
    Q_PROPERTY(QString version READ version NOTIFY changed)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY changed)
    Q_PROPERTY(QVariantList zoomStops READ zoomStops NOTIFY changed)
    Q_PROPERTY(bool openAtLogin READ openAtLogin WRITE setOpenAtLogin NOTIFY openAtLoginChanged)
    Q_PROPERTY(bool preview READ preview WRITE setPreview NOTIFY previewChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    // The socket reports its disconnection while it is destroyed, by which time the members
    // its handlers touch are gone; stop listening first.
    ~Backend() override { m_socket.disconnect(this); }

    bool connected() const { return m_socket.state() == QLocalSocket::ConnectedState; }
    QString status() const { return m_status; }
    QString message() const { return m_message; }
    bool restartPending() const { return m_restartPending; }
    QString version() const { return m_version; }
    QVariantMap settings() const { return m_settings; }
    QVariantList zoomStops() const { return m_zoomStops; }
    bool openAtLogin() const;
    void setOpenAtLogin(bool on);
    bool preview() const;
    void setPreview(bool on);

    /// Changes one setting. A null value clears it back to the tuning's default.
    Q_INVOKABLE void set(const QString &key, const QVariant &value);
    Q_INVOKABLE void reset() { request("reset"); }
    Q_INVOKABLE void retry() { request("retry"); }
    /// Stops the app itself; the camera then returns to its factory webcam mode.
    Q_INVOKABLE void quitApp() { request("quit"); }
    /// The booted camera's /dev/videoN capture node, found by USB id; empty if there is none.
    Q_INVOKABLE QString cameraNode() const;
    /// Starts the app if it has stopped, then reconnects.
    Q_INVOKABLE void startApp();

signals:
    void changed();
    void openAtLoginChanged();
    void previewChanged();

private:
    void connectToApp();
    void request(const QByteArray &line);
    void read();

    QLocalSocket m_socket;
    QByteArray m_buffer;
    QString m_status = "notConnected";
    QString m_message;
    QString m_version;
    bool m_restartPending = false;
    QVariantMap m_settings;
    QVariantList m_zoomStops;
};
