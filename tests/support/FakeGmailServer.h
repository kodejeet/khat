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
        QByteArray body;
    };

    struct Response
    {
        int status = 200;
        QByteArray body;
        QByteArray contentType = QByteArrayLiteral("application/json");
    };

    using Handler = std::function<Response(const QUrl &url, const QByteArray &authorization)>;
    using BodyHandler = std::function<Response(const QUrl &url, const QByteArray &authorization,
                                               const QByteArray &body)>;

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

                            const QByteArray head = buffer->left(headerEnd);
                            const QList<QByteArray> lines = head.split('\n');
                            int contentLength = 0;
                            for (int i = 1; i < lines.size(); ++i) {
                                const QByteArray line = lines.at(i).trimmed();
                                const int colon = line.indexOf(':');
                                if (colon <= 0) {
                                    continue;
                                }
                                if (line.left(colon).trimmed().toLower()
                                    == QByteArrayLiteral("content-length")) {
                                    contentLength = line.mid(colon + 1).trimmed().toInt();
                                }
                            }
                            const int bodyStart = headerEnd + 4;
                            if (buffer->size() < bodyStart + contentLength) {
                                return;
                            }
                            *responded = true;

                            const QList<QByteArray> requestLine =
                                lines.value(0).trimmed().split(' ');
                            QUrl url = QUrl::fromEncoded(requestLine.value(1));
                            const QByteArray body =
                                buffer->mid(bodyStart, contentLength);

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
                            request.body = body;
                            requests.append(request);

                            const Response response =
                                bodyHandler
                                ? bodyHandler(url, authorization, body)
                                : handler ? handler(url, authorization) : Response{404, "{}"};
                            const QByteArray statusText = response.status == 200
                                ? QByteArrayLiteral("OK")
                                : response.status == 400 ? QByteArrayLiteral("Bad Request")
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
    BodyHandler bodyHandler;
    QList<Request> requests;

    // Requests aimed at Gmail data endpoints. After every successful check the
    // sync flow additionally performs one users/me/profile read to learn a
    // fresh historyId cursor; poll assertions normally exclude that read.
    QList<Request> syncRequests() const
    {
        QList<Request> result;
        for (const Request &request : requests) {
            if (!isProfileRead(request.url)) {
                result.append(request);
            }
        }
        return result;
    }

    int syncRequestCount() const
    {
        return syncRequests().size();
    }

private:
    static bool isProfileRead(const QUrl &url)
    {
        return url.path().endsWith(QLatin1String("/users/me/profile"));
    }
};
