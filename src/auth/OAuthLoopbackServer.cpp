#include "auth/OAuthLoopbackServer.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

namespace {
constexpr int kMaxRequestBytes = 16 * 1024;
constexpr int kDefaultTimeoutMs = 5 * 60 * 1000;
constexpr auto kCallbackPath = "/callback";

const QByteArray kSuccessHtml =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>Gmail Notifier</title>"
    "</head><body><h1>Authorization received</h1>"
    "<p>You can close this window and return to Gmail Notifier.</p></body></html>";
const QByteArray kFailureHtml =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>Gmail Notifier</title>"
    "</head><body><h1>Authorization failed</h1>"
    "<p>Return to Gmail Notifier and try again.</p></body></html>";
} // namespace

OAuthLoopbackServer::OAuthLoopbackServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(kDefaultTimeoutMs);
    connect(&m_timeout, &QTimer::timeout, this, &OAuthLoopbackServer::handleTimeout);
    connect(m_server, &QTcpServer::newConnection, this,
            &OAuthLoopbackServer::handleNewConnection);
}

OAuthLoopbackServer::~OAuthLoopbackServer()
{
    stop();
}

bool OAuthLoopbackServer::start(QString *error)
{
    if (m_server->isListening()) {
        return true;
    }
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        if (error) {
            *error = QStringLiteral("Cannot start the local OAuth callback listener: %1")
                         .arg(m_server->errorString());
        }
        return false;
    }
    m_finished = false;
    m_timeout.start();
    return true;
}

void OAuthLoopbackServer::stop()
{
    m_timeout.stop();
    if (m_server->isListening()) {
        m_server->close();
    }
}

bool OAuthLoopbackServer::isListening() const
{
    return m_server->isListening();
}

QString OAuthLoopbackServer::redirectUri() const
{
    if (!m_server->isListening()) {
        return QString();
    }
    return QStringLiteral("http://127.0.0.1:%1%2")
        .arg(m_server->serverPort())
        .arg(QString::fromLatin1(kCallbackPath));
}

int OAuthLoopbackServer::port() const
{
    return m_server->serverPort();
}

void OAuthLoopbackServer::setTimeoutMilliseconds(int milliseconds)
{
    m_timeout.setInterval(qMax(1, milliseconds));
}

void OAuthLoopbackServer::handleNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        auto *buffer = new QByteArray;
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QObject::destroyed, socket, [buffer] { delete buffer; });
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
            if (m_finished) {
                return;
            }
            buffer->append(socket->readAll());
            if (buffer->size() > kMaxRequestBytes) {
                writeResponse(socket, 400, kFailureHtml);
                finishWithError(QStringLiteral("The local OAuth callback request was too large."));
                return;
            }

            const int headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }

            const QByteArray requestLine =
                buffer->left(headerEnd).split('\n').value(0).trimmed();
            const QList<QByteArray> parts = requestLine.split(' ');
            if (parts.size() < 2 || parts.at(0) != QByteArrayLiteral("GET")) {
                writeResponse(socket, 400, kFailureHtml);
                finishWithError(QStringLiteral("The OAuth callback used an invalid request."));
                return;
            }

            const QUrl url = QUrl::fromEncoded(parts.at(1));
            if (url.path() != QString::fromLatin1(kCallbackPath)) {
                writeResponse(socket, 404, kFailureHtml);
                finishWithError(QStringLiteral("The OAuth callback path was not recognized."));
                return;
            }

            const QUrlQuery query(url);
            if (query.hasQueryItem(QStringLiteral("error"))) {
                writeResponse(socket, 400, kFailureHtml);
                finishWithError(QStringLiteral("Google returned an authorization error: %1")
                                    .arg(query.queryItemValue(QStringLiteral("error")).left(80)));
                return;
            }

            const QString code = query.queryItemValue(QStringLiteral("code"),
                                                      QUrl::FullyDecoded);
            const QString state = query.queryItemValue(QStringLiteral("state"),
                                                       QUrl::FullyDecoded);
            if (code.isEmpty() || state.isEmpty()) {
                writeResponse(socket, 400, kFailureHtml);
                finishWithError(
                    QStringLiteral("The OAuth callback did not include a code and state."));
                return;
            }

            writeResponse(socket, 200, kSuccessHtml);
            m_finished = true;
            m_timeout.stop();
            m_server->close();
            emit callbackReceived(code, state);
        });
    }
}

void OAuthLoopbackServer::handleTimeout()
{
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_server->close();
    emit failed(QStringLiteral("The Google authorization request timed out. Try again."));
}

void OAuthLoopbackServer::finishWithError(const QString &message)
{
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_timeout.stop();
    m_server->close();
    emit failed(message);
}

void OAuthLoopbackServer::writeResponse(QTcpSocket *socket, int status,
                                        const QByteArray &body)
{
    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(status) + ' '
        + (status == 200 ? QByteArrayLiteral("OK") : QByteArrayLiteral("Bad Request"))
        + "\r\n";
    response += "Content-Type: text/html; charset=utf-8\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}
