#include "gmail/GmailClient.h"
#include "gmail/GmailQuery.h"

#include "support/FakeGmailServer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QUrlQuery>
#include <QtTest>

class TestGmail : public QObject
{
    Q_OBJECT

private slots:
    void queryBuilderFiltersAndUpdates();
    void listPageParsing();
    void messageMetadataParsing();
    void errorClassification_data();
    void errorClassification();
    void malformedJsonRejected();
    void networkFailureClassified();
    void noTokenDoesNotRequest();
    void gmailMessageUrlEncoding();
};

static QByteArray listBody(const QList<QPair<QString, QString>> &messages,
                           const QString &nextPageToken = QString())
{
    QJsonArray array;
    for (const auto &pair : messages) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), pair.first);
        object.insert(QStringLiteral("threadId"), pair.second);
        array.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("messages"), array);
    root.insert(QStringLiteral("resultSizeEstimate"), array.size());
    if (!nextPageToken.isEmpty()) {
        root.insert(QStringLiteral("nextPageToken"), nextPageToken);
    }
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static QByteArray metadataBody(const QString &id, const QString &from,
                               const QString &subject, const QString &snippet)
{
    QJsonArray headers;
    QJsonObject fromHeader;
    fromHeader.insert(QStringLiteral("name"), QStringLiteral("From"));
    fromHeader.insert(QStringLiteral("value"), from);
    headers.append(fromHeader);
    QJsonObject subjectHeader;
    subjectHeader.insert(QStringLiteral("name"), QStringLiteral("Subject"));
    subjectHeader.insert(QStringLiteral("value"), subject);
    headers.append(subjectHeader);

    QJsonObject payload;
    payload.insert(QStringLiteral("headers"), headers);

    QJsonObject root;
    root.insert(QStringLiteral("id"), id);
    root.insert(QStringLiteral("threadId"), id + QStringLiteral("-thread"));
    root.insert(QStringLiteral("internalDate"), QStringLiteral("1700000000000"));
    root.insert(QStringLiteral("labelIds"), QJsonArray{QStringLiteral("INBOX")});
    root.insert(QStringLiteral("snippet"), snippet);
    root.insert(QStringLiteral("payload"), payload);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void TestGmail::queryBuilderFiltersAndUpdates()
{
    const QString base = QStringLiteral(
        "in:inbox -category:promotions -category:social -category:forums");
    QCOMPARE(buildGmailQuery(true), base);
    QCOMPARE(buildGmailQuery(false), base + QStringLiteral(" -category:updates"));
    QCOMPARE(buildGmailQuery(false, QDate(2026, 1, 2)),
             base + QStringLiteral(" -category:updates after:2026/01/02"));
}

void TestGmail::listPageParsing()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{
            200, listBody({{QStringLiteral("m1"), QStringLiteral("t1")},
                           {QStringLiteral("m2"), QStringLiteral("t2")}},
                          QStringLiteral("page-2"))};
    };

    QNetworkAccessManager network;
    GmailClient client(&network);
    client.setAccessToken(QStringLiteral("test-token"));
    client.setApiBaseUrl(server.apiBaseUrl());

    QSignalSpy pageSpy(&client, &GmailClient::listPageReady);
    QSignalSpy errorSpy(&client, &GmailClient::requestFailed);
    client.listMessages(buildGmailQuery(false), 100);

    QVERIFY2(pageSpy.wait(5000), "timed out waiting for list page");
    QCOMPARE(errorSpy.count(), 0);
    const GmailListPage page = pageSpy.takeFirst().at(0).value<GmailListPage>();
    QCOMPARE(page.messages.size(), 2);
    QCOMPARE(page.messages.at(0).id, QStringLiteral("m1"));
    QCOMPARE(page.messages.at(1).threadId, QStringLiteral("t2"));
    QCOMPARE(page.nextPageToken, QStringLiteral("page-2"));
    QCOMPARE(page.resultSizeEstimate, 2);

    QCOMPARE(server.requests.size(), 1);
    const QUrlQuery sent(server.requests.first().url);
    QCOMPARE(server.requests.first().authorization, QByteArrayLiteral("Bearer test-token"));
    QCOMPARE(sent.queryItemValue(QStringLiteral("q")), buildGmailQuery(false));
    QCOMPARE(sent.queryItemValue(QStringLiteral("maxResults")), QStringLiteral("100"));
    QCOMPARE(sent.queryItemValue(QStringLiteral("format")), QString());
}

void TestGmail::messageMetadataParsing()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{
            200, metadataBody(QStringLiteral("m1"),
                              QStringLiteral("Alice <alice@example.com>"),
                              QStringLiteral("Hello"), QStringLiteral("Short preview"))};
    };

    QNetworkAccessManager network;
    GmailClient client(&network);
    client.setAccessToken(QStringLiteral("test-token"));
    client.setApiBaseUrl(server.apiBaseUrl());

    QSignalSpy messageSpy(&client, &GmailClient::messageReady);
    client.fetchMessage(QStringLiteral("m1"));
    QVERIFY2(messageSpy.wait(5000), "timed out waiting for metadata");

    const MessageSummary message = messageSpy.takeFirst().at(0).value<MessageSummary>();
    QCOMPARE(message.id, QStringLiteral("m1"));
    QCOMPARE(message.threadId, QStringLiteral("m1-thread"));
    QCOMPARE(message.internalDate, 1700000000000LL);
    QCOMPARE(message.sender, QStringLiteral("Alice <alice@example.com>"));
    QCOMPARE(message.subject, QStringLiteral("Hello"));
    QCOMPARE(message.snippet, QStringLiteral("Short preview"));
    QCOMPARE(message.labelIds, QStringList{QStringLiteral("INBOX")});

    QCOMPARE(server.requests.size(), 1);
    const QUrlQuery sent(server.requests.first().url);
    QCOMPARE(sent.queryItemValue(QStringLiteral("format")), QStringLiteral("metadata"));
    QCOMPARE(server.requests.first().url.path(),
             QStringLiteral("/gmail/v1/users/me/messages/m1"));
}

void TestGmail::errorClassification_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("expectedKind");

    QTest::newRow("401") << 401 << QByteArrayLiteral("{}")
                         << int(GmailErrorKind::Unauthorized);
    QTest::newRow("403-rate")
        << 403
        << QByteArrayLiteral(
               "{\"error\":{\"code\":403,\"errors\":[{\"reason\":"
               "\"userRateLimitExceeded\"}]}}")
        << int(GmailErrorKind::RateLimited);
    QTest::newRow("403-forbidden")
        << 403
        << QByteArrayLiteral(
               "{\"error\":{\"code\":403,\"errors\":[{\"reason\":"
               "\"insufficientPermissions\"}]}}")
        << int(GmailErrorKind::Forbidden);
    QTest::newRow("404") << 404 << QByteArrayLiteral("{}")
                         << int(GmailErrorKind::NotFound);
    QTest::newRow("429") << 429 << QByteArrayLiteral("{}")
                         << int(GmailErrorKind::RateLimited);
    QTest::newRow("500") << 500 << QByteArrayLiteral("{}")
                         << int(GmailErrorKind::Server);
    QTest::newRow("503") << 503 << QByteArrayLiteral("{}")
                         << int(GmailErrorKind::Server);
}

void TestGmail::errorClassification()
{
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QFETCH(int, expectedKind);

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [status, body](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{status, body};
    };

    QNetworkAccessManager network;
    GmailClient client(&network);
    client.setAccessToken(QStringLiteral("test-token"));
    client.setApiBaseUrl(server.apiBaseUrl());

    QSignalSpy errorSpy(&client, &GmailClient::requestFailed);
    client.listMessages(QStringLiteral("in:inbox"));
    QVERIFY2(errorSpy.wait(5000), "timed out waiting for error");
    const GmailError error = errorSpy.takeFirst().at(0).value<GmailError>();
    QCOMPARE(int(error.kind), expectedKind);
    QCOMPARE(error.httpStatus, status);
    QVERIFY(!error.message.isEmpty());
}

void TestGmail::malformedJsonRejected()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{200, QByteArrayLiteral("{not json")};
    };

    QNetworkAccessManager network;
    GmailClient client(&network);
    client.setAccessToken(QStringLiteral("test-token"));
    client.setApiBaseUrl(server.apiBaseUrl());

    QSignalSpy errorSpy(&client, &GmailClient::requestFailed);
    client.listMessages(QStringLiteral("in:inbox"));
    QVERIFY(errorSpy.wait(5000));
    const GmailError error = errorSpy.takeFirst().at(0).value<GmailError>();
    QCOMPARE(error.kind, GmailErrorKind::InvalidResponse);
}

void TestGmail::networkFailureClassified()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    const QUrl base = server.apiBaseUrl();
    server.close();

    QNetworkAccessManager network;
    GmailClient client(&network);
    client.setAccessToken(QStringLiteral("test-token"));
    client.setApiBaseUrl(base);

    QSignalSpy errorSpy(&client, &GmailClient::requestFailed);
    client.listMessages(QStringLiteral("in:inbox"));
    QVERIFY(errorSpy.wait(5000));
    const GmailError error = errorSpy.takeFirst().at(0).value<GmailError>();
    QCOMPARE(error.kind, GmailErrorKind::Network);
    QVERIFY(error.isRetryable());
    QCOMPARE(error.httpStatus, 0);
}

void TestGmail::noTokenDoesNotRequest()
{
    FakeGmailServer server;
    QVERIFY(server.start());

    QNetworkAccessManager network;
    GmailClient client(&network);
    client.setApiBaseUrl(server.apiBaseUrl());
    QSignalSpy errorSpy(&client, &GmailClient::requestFailed);
    client.listMessages(QStringLiteral("in:inbox"));
    QCOMPARE(errorSpy.count(), 1);
    const GmailError error = errorSpy.takeFirst().at(0).value<GmailError>();
    QCOMPARE(error.kind, GmailErrorKind::NoToken);
    QCOMPARE(server.requests.size(), 0);
}

void TestGmail::gmailMessageUrlEncoding()
{
    QCOMPARE(gmailMessageUrl(QStringLiteral("abc-123_XYZ")),
             QStringLiteral("https://mail.google.com/mail/u/0/#inbox/abc-123_XYZ"));
}

QTEST_GUILESS_MAIN(TestGmail)
#include "test_gmail.moc"
