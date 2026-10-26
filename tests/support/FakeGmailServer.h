#pragma once

#include <QHostAddress>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <memory>

// Minimal HTTP/1.1 test server. It returns fake Gmail JSON over loopback; no
// live Google endpoint is contacted.
class FakeGmailServer : public QTcpServer
{
public:
    struct Request
    {
        QUrl url;
        QByteArray authorization;
        QByteArray method;
    };

    struct Response
    {
        int status = 200;
        QByteArray body;
        QByteArray contentType = QByteArrayLiteral("application/json");
    };

    using Handler = std::function<Response(const QUrl &url, const QByteArray &authorization)>;

    explicit FakeGmailServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket *socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                auto responded = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, socket,
                        [this, socket, buffer, responded] {
                            if (*responded) {
                                return;
                            }
                            buffer->append(socket->readAll());
                            const int headerEnd = buffer->indexOf("\r\n\r\n");
                            if (headerEnd < 0) {
                                return;
                            }
                            *responded = true;

                            const QByteArray head = buffer->left(headerEnd);
                            const QList<QByteArray> lines = head.split('\n');
                            const QList<QByteArray> requestLine =
                                lines.value(0).trimmed().split(' ');
                            QUrl url = QUrl::fromEncoded(requestLine.value(1));

                            QByteArray authorization;
                            for (int i = 1; i < lines.size(); ++i) {
                                const QByteArray line = lines.at(i).trimmed();
                                const int colon = line.indexOf(':');
                                if (colon <= 0) {
                                    continue;
                                }
                                const QByteArray name = line.left(colon).trimmed().toLower();
                                if (name == "authorization") {
                                    authorization = line.mid(colon + 1).trimmed();
                                }
                            }

                            Request request;
                            request.url = url;
                            request.authorization = authorization;
                            request.method = requestLine.value(0);
                            requests.append(request);

                            const Response response =
                                handler ? handler(url, authorization) : Response{404, "{}"};
                            const QByteArray statusText = response.status == 200
                                ? QByteArrayLiteral("OK")
                                : response.status == 401 ? QByteArrayLiteral("Unauthorized")
                                : response.status == 403 ? QByteArrayLiteral("Forbidden")
                                : response.status == 404 ? QByteArrayLiteral("Not Found")
                                : response.status == 429 ? QByteArrayLiteral("Too Many Requests")
                                : response.status == 500 ? QByteArrayLiteral("Internal Server Error")
                                : response.status == 503
                                ? QByteArrayLiteral("Service Unavailable")
                                : QByteArrayLiteral("Error");

                            QByteArray out;
                            out += "HTTP/1.1 " + QByteArray::number(response.status) + ' '
                                + statusText + "\r\n";
                            out += "Content-Type: " + response.contentType + "\r\n";
                            out += "Content-Length: " + QByteArray::number(response.body.size())
                                + "\r\n";
                            out += "Connection: close\r\n\r\n";
                            out += response.body;
                            socket->write(out);
                            socket->disconnectFromHost();
                        });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool start()
    {
        return listen(QHostAddress::LocalHost, 0);
    }

    QUrl apiBaseUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/gmail/v1").arg(serverPort()));
    }

    Handler handler;
    QList<Request> requests;
};
