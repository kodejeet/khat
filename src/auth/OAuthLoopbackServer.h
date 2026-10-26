#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

class QTcpServer;
class QTcpSocket;

// One-shot loopback listener for the desktop OAuth redirect. It binds only
// 127.0.0.1 on an ephemeral port, accepts one GET /callback request, and never
// logs the query string (which contains a short-lived authorization code).
class OAuthLoopbackServer : public QObject
{
    Q_OBJECT

public:
    explicit OAuthLoopbackServer(QObject *parent = nullptr);
    ~OAuthLoopbackServer() override;

    bool start(QString *error = nullptr);
    void stop();
    bool isListening() const;

    QString redirectUri() const;
    int port() const;

    void setTimeoutMilliseconds(int milliseconds);
    int timeoutMilliseconds() const { return m_timeout.interval(); }

signals:
    void callbackReceived(const QString &code, const QString &state);
    void failed(const QString &message);

private slots:
    void handleNewConnection();
    void handleTimeout();

private:
    void finishWithError(const QString &message);
    void writeResponse(QTcpSocket *socket, int status, const QByteArray &body);

    QTcpServer *m_server;
    QTimer m_timeout;
    bool m_finished = false;
};
