#include "gmail/GmailQuery.h"
#include "state/StateDatabase.h"
#include "sync/SyncService.h"

#include "support/FakeGmailServer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>

#include <algorithm>

class TestSync : public QObject
{
    Q_OBJECT

private slots:
    void baselineThenNewMessageAndDedupe();
    void categoryFilteringAndUpdatesToggle();
    void paginationAndMultipleAccounts();
    void listErrorUpdatesStatus();
    void metadataNotFoundIsSkipped();
    void overlappingSyncRejected();
};

static QByteArray listBody(const QStringList &ids, const QString &nextPageToken = QString())
{
    QJsonArray messages;
    for (const QString &id : ids) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), id);
        object.insert(QStringLiteral("threadId"), id + QStringLiteral("-thread"));
        messages.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("messages"), messages);
    if (!nextPageToken.isEmpty()) {
        root.insert(QStringLiteral("nextPageToken"), nextPageToken);
    }
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static QByteArray metadataBody(const QString &id, const QStringList &labels)
{
    QJsonArray labelArray;
    for (const QString &label : labels) {
        labelArray.append(label);
    }
    QJsonArray headers;
    QJsonObject from;
    from.insert(QStringLiteral("name"), QStringLiteral("From"));
    from.insert(QStringLiteral("value"), QStringLiteral("Sender <sender@example.com>"));
    headers.append(from);
    QJsonObject subject;
    subject.insert(QStringLiteral("name"), QStringLiteral("Subject"));
    subject.insert(QStringLiteral("value"), QStringLiteral("Subject for %1").arg(id));
    headers.append(subject);
    QJsonObject payload;
    payload.insert(QStringLiteral("headers"), headers);

    QJsonObject root;
    root.insert(QStringLiteral("id"), id);
    root.insert(QStringLiteral("threadId"), id + QStringLiteral("-thread"));
    root.insert(QStringLiteral("internalDate"), QStringLiteral("1700000000000"));
    root.insert(QStringLiteral("labelIds"), labelArray);
    root.insert(QStringLiteral("snippet"), QStringLiteral("Preview for %1").arg(id));
    root.insert(QStringLiteral("payload"), payload);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static QString encodedToken(const QByteArray &authorization)
{
    return QString::fromLatin1(authorization).remove(QStringLiteral("Bearer ")).trimmed();
}

static bool openState(StateDatabase &db, const QString &path, QString *error)
{
    return db.open(error);
}

static void seedBaseline(StateDatabase &db, const QString &id, const QString &email)
{
    QString error;
    QVERIFY2(db.ensureAccount(id, email, &error), qPrintable(error));
    QVERIFY2(db.applyBaseline(id, {}, 1000, &error), qPrintable(error));
}

void TestSync::baselineThenNewMessageAndDedupe()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(openState(db, db.path(), &error), qPrintable(error));

    FakeGmailServer server;
    QVERIFY(server.start());
    int listCalls = 0;
    int metadataCalls = 0;
    server.handler = [&](const QUrl &url, const QByteArray &) {
        if (url.path().endsWith(QStringLiteral("/users/me/messages"))) {
            ++listCalls;
            if (listCalls == 1) {
                return FakeGmailServer::Response{
                    200, listBody({QStringLiteral("old-1"), QStringLiteral("old-2")})};
            }
            return FakeGmailServer::Response{
                200,
                listBody({QStringLiteral("old-1"), QStringLiteral("old-2"),
                          QStringLiteral("new-1")})};
        }
        if (url.path().endsWith(QStringLiteral("/new-1"))) {
            ++metadataCalls;
            return FakeGmailServer::Response{
                200, metadataBody(QStringLiteral("new-1"), {QStringLiteral("INBOX")})};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager network;
    SyncService service(&db, &network);
    service.setApiBaseUrl(server.apiBaseUrl());

    QSignalSpy finished(&service, &SyncService::syncFinished);
    QSignalSpy newMessages(&service, &SyncService::newMessages);

    SyncAccountRequest request;
    request.accountId = QStringLiteral("acct");
    request.email = QStringLiteral("user@example.com");
    request.accessToken = QStringLiteral("token");
    request.includeUpdates = true;

    QVERIFY2(service.syncAccount(request, &error), qPrintable(error));
    QVERIFY(service.isSyncing(QStringLiteral("acct")));
    QVERIFY2(finished.wait(5000), "baseline sync timed out");
    SyncResult result = finished.takeFirst().at(0).value<SyncResult>();
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(result.baseline);
    QCOMPARE(result.newMessageCount, 0);
    QCOMPARE(result.pagesFetched, 1);
    QCOMPARE(newMessages.count(), 0);
    QCOMPARE(listCalls, 1);
    QCOMPARE(metadataCalls, 0);
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 2);

    AccountState state;
    QVERIFY(db.accountState(QStringLiteral("acct"), &state));
    QVERIFY(state.baselineDone);

    QVERIFY2(service.syncAccount(request, &error), qPrintable(error));
    QVERIFY(finished.wait(5000));
    result = finished.takeFirst().at(0).value<SyncResult>();
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(!result.baseline);
    QCOMPARE(result.newMessageCount, 1);
    QCOMPARE(result.pagesFetched, 1);
    QCOMPARE(listCalls, 2);
    QCOMPARE(metadataCalls, 1);
    QCOMPARE(newMessages.count(), 1);
    const QList<MessageSummary> notified = newMessages.takeFirst().at(1).value<QList<MessageSummary>>();
    QCOMPARE(notified.size(), 1);
    QCOMPARE(notified.first().id, QStringLiteral("new-1"));
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 3);

    QVERIFY2(service.syncAccount(request, &error), qPrintable(error));
    QVERIFY(finished.wait(5000));
    result = finished.takeFirst().at(0).value<SyncResult>();
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.newMessageCount, 0);
    QCOMPARE(newMessages.count(), 0);
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 3);

    QCOMPARE(server.requests.size(), 4);
    const QUrlQuery firstQuery(server.requests.at(0).url);
    QCOMPARE(firstQuery.queryItemValue(QStringLiteral("q")),
             buildGmailQuery(true));
    const QUrlQuery secondQuery(server.requests.at(1).url);
    QVERIFY(secondQuery.queryItemValue(QStringLiteral("q"))
                .contains(QStringLiteral(" after:")));
}

void TestSync::categoryFilteringAndUpdatesToggle()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    seedBaseline(db, QStringLiteral("with-updates"), QStringLiteral("a@example.com"));
    seedBaseline(db, QStringLiteral("without-updates"), QStringLiteral("b@example.com"));

    const QStringList ids = {
        QStringLiteral("promo"),   QStringLiteral("social"), QStringLiteral("forum"),
        QStringLiteral("spam"),    QStringLiteral("trash"),  QStringLiteral("sent"),
        QStringLiteral("draft"),   QStringLiteral("update"), QStringLiteral("inbox"),
    };
    QHash<QString, QStringList> labels;
    labels.insert(QStringLiteral("promo"),
                  {QStringLiteral("INBOX"), QStringLiteral("CATEGORY_PROMOTIONS")});
    labels.insert(QStringLiteral("social"),
                  {QStringLiteral("INBOX"), QStringLiteral("CATEGORY_SOCIAL")});
    labels.insert(QStringLiteral("forum"),
                  {QStringLiteral("INBOX"), QStringLiteral("CATEGORY_FORUMS")});
    labels.insert(QStringLiteral("spam"), {QStringLiteral("SPAM")});
    labels.insert(QStringLiteral("trash"), {QStringLiteral("TRASH")});
    labels.insert(QStringLiteral("sent"), {QStringLiteral("SENT")});
    labels.insert(QStringLiteral("draft"), {QStringLiteral("DRAFT")});
    labels.insert(QStringLiteral("update"),
                  {QStringLiteral("INBOX"), QStringLiteral("CATEGORY_UPDATES")});
    labels.insert(QStringLiteral("inbox"), {QStringLiteral("INBOX")});

    FakeGmailServer server;
    QVERIFY(server.start());
    QStringList listQueries;
    server.handler = [&](const QUrl &url, const QByteArray &) {
        if (url.path().endsWith(QStringLiteral("/users/me/messages"))) {
            listQueries.append(QUrlQuery(url).queryItemValue(QStringLiteral("q")));
            return FakeGmailServer::Response{200, listBody(ids)};
        }
        const QString id = url.path().section(QLatin1Char('/'), -1);
        return FakeGmailServer::Response{200, metadataBody(id, labels.value(id))};
    };

    QNetworkAccessManager network;
    SyncService service(&db, &network);
    service.setApiBaseUrl(server.apiBaseUrl());

    QSignalSpy finished(&service, &SyncService::syncFinished);
    QSignalSpy newMessages(&service, &SyncService::newMessages);

    SyncAccountRequest request;
    request.accountId = QStringLiteral("without-updates");
    request.email = QStringLiteral("b@example.com");
    request.accessToken = QStringLiteral("token-b");
    request.includeUpdates = false;
    QVERIFY(service.syncAccount(request, &error));
    QVERIFY(finished.wait(5000));
    SyncResult result = finished.takeFirst().at(0).value<SyncResult>();
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.newMessageCount, 1);
    QCOMPARE(newMessages.count(), 1);
    QList<MessageSummary> notified = newMessages.takeFirst().at(1).value<QList<MessageSummary>>();
    QCOMPARE(notified.size(), 1);
    QCOMPARE(notified.first().id, QStringLiteral("inbox"));
    QVERIFY(listQueries.first().contains(QStringLiteral("-category:updates")));

    request.accountId = QStringLiteral("with-updates");
    request.email = QStringLiteral("a@example.com");
    request.accessToken = QStringLiteral("token-a");
    request.includeUpdates = true;
    QVERIFY(service.syncAccount(request, &error));
    QVERIFY(finished.wait(5000));
    result = finished.takeFirst().at(0).value<SyncResult>();
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.newMessageCount, 2);
    QCOMPARE(newMessages.count(), 1);
    notified = newMessages.takeFirst().at(1).value<QList<MessageSummary>>();
    QCOMPARE(notified.size(), 2);
    QCOMPARE(notified.at(0).id, QStringLiteral("update"));
    QCOMPARE(notified.at(1).id, QStringLiteral("inbox"));
    QVERIFY(!listQueries.at(1).contains(QStringLiteral("-category:updates")));

    for (const QString &query : listQueries) {
        QVERIFY(query.contains(QStringLiteral("in:inbox")));
        QVERIFY(query.contains(QStringLiteral("-category:promotions")));
        QVERIFY(query.contains(QStringLiteral("-category:social")));
        QVERIFY(query.contains(QStringLiteral("-category:forums")));
    }
}

void TestSync::paginationAndMultipleAccounts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    seedBaseline(db, QStringLiteral("account-a"), QStringLiteral("a@example.com"));
    seedBaseline(db, QStringLiteral("account-b"), QStringLiteral("b@example.com"));

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [&](const QUrl &url, const QByteArray &authorization) {
        const QString token = encodedToken(authorization);
        if (url.path().endsWith(QStringLiteral("/users/me/messages"))) {
            const QString pageToken =
                QUrlQuery(url).queryItemValue(QStringLiteral("pageToken"));
            if (token == QStringLiteral("token-a")) {
                if (pageToken == QStringLiteral("a-page-2")) {
                    return FakeGmailServer::Response{
                        200, listBody({QStringLiteral("a-3")})};
                }
                return FakeGmailServer::Response{
                    200,
                    listBody({QStringLiteral("a-1"), QStringLiteral("a-2")},
                             QStringLiteral("a-page-2"))};
            }
            return FakeGmailServer::Response{200, listBody({QStringLiteral("b-1")})};
        }
        const QString id = url.path().section(QLatin1Char('/'), -1);
        return FakeGmailServer::Response{
            200, metadataBody(id, {QStringLiteral("INBOX")})};
    };

    QNetworkAccessManager network;
    SyncService service(&db, &network);
    service.setApiBaseUrl(server.apiBaseUrl());

    QSignalSpy finished(&service, &SyncService::syncFinished);
    QSignalSpy newMessages(&service, &SyncService::newMessages);

    SyncAccountRequest a;
    a.accountId = QStringLiteral("account-a");
    a.email = QStringLiteral("a@example.com");
    a.accessToken = QStringLiteral("token-a");
    a.includeUpdates = true;

    SyncAccountRequest b;
    b.accountId = QStringLiteral("account-b");
    b.email = QStringLiteral("b@example.com");
    b.accessToken = QStringLiteral("token-b");
    b.includeUpdates = true;

    QVERIFY(service.syncAccount(a, &error));
    QVERIFY(service.syncAccount(b, &error));
    QVERIFY(service.isSyncing(QStringLiteral("account-a")));
    QVERIFY(service.isSyncing(QStringLiteral("account-b")));
    QCOMPARE(service.activeSyncs(), 2);

    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
    QCOMPARE(service.activeSyncs(), 0);

    QHash<QString, int> counts;
    for (const QList<QVariant> &args : finished) {
        const SyncResult result = args.at(0).value<SyncResult>();
        QVERIFY2(result.success, qPrintable(result.error));
        counts.insert(result.accountId, result.newMessageCount);
    }
    QCOMPARE(counts.value(QStringLiteral("account-a")), 3);
    QCOMPARE(counts.value(QStringLiteral("account-b")), 1);
    QCOMPARE(newMessages.count(), 2);
    QCOMPARE(db.countSeenMessages(QStringLiteral("account-a")), 3);
    QCOMPARE(db.countSeenMessages(QStringLiteral("account-b")), 1);

    const auto pageRequests = std::count_if(
        server.requests.cbegin(), server.requests.cend(), [](const FakeGmailServer::Request &r) {
            return QUrlQuery(r.url).queryItemValue(QStringLiteral("pageToken"))
                == QStringLiteral("a-page-2");
        });
    QCOMPARE(pageRequests, 1);

    finished.clear();
    newMessages.clear();
    QVERIFY(service.syncAccount(a, &error));
    QVERIFY(service.syncAccount(b, &error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
    for (const QList<QVariant> &args : finished) {
        const SyncResult result = args.at(0).value<SyncResult>();
        QVERIFY(result.success);
        QCOMPARE(result.newMessageCount, 0);
    }
    QCOMPARE(newMessages.count(), 0);
    QCOMPARE(db.countSeenMessages(QStringLiteral("account-a")), 3);
    QCOMPARE(db.countSeenMessages(QStringLiteral("account-b")), 1);
}

void TestSync::listErrorUpdatesStatus()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    seedBaseline(db, QStringLiteral("acct"), QStringLiteral("user@example.com"));

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{
            401, QByteArrayLiteral("{\"error\":{\"code\":401}}")};
    };

    QNetworkAccessManager network;
    SyncService service(&db, &network);
    service.setApiBaseUrl(server.apiBaseUrl());
    QSignalSpy finished(&service, &SyncService::syncFinished);

    SyncAccountRequest request;
    request.accountId = QStringLiteral("acct");
    request.email = QStringLiteral("user@example.com");
    request.accessToken = QStringLiteral("expired-token");
    QVERIFY(service.syncAccount(request, &error));
    QVERIFY(finished.wait(5000));
    const SyncResult result = finished.takeFirst().at(0).value<SyncResult>();
    QVERIFY(!result.success);
    QCOMPARE(result.errorKind, GmailErrorKind::Unauthorized);
    QVERIFY(result.error.contains(QStringLiteral("401")));

    AccountState state;
    QVERIFY(db.accountState(QStringLiteral("acct"), &state));
    QVERIFY(state.lastError.contains(QStringLiteral("401")));
    QCOMPARE(state.lastSyncEpoch, 1000);
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 0);
}

void TestSync::metadataNotFoundIsSkipped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    seedBaseline(db, QStringLiteral("acct"), QStringLiteral("user@example.com"));

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &url, const QByteArray &) {
        if (url.path().endsWith(QStringLiteral("/users/me/messages"))) {
            return FakeGmailServer::Response{200, listBody({QStringLiteral("gone")})};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager network;
    SyncService service(&db, &network);
    service.setApiBaseUrl(server.apiBaseUrl());
    QSignalSpy finished(&service, &SyncService::syncFinished);
    QSignalSpy newMessages(&service, &SyncService::newMessages);

    SyncAccountRequest request;
    request.accountId = QStringLiteral("acct");
    request.email = QStringLiteral("user@example.com");
    request.accessToken = QStringLiteral("token");
    QVERIFY(service.syncAccount(request, &error));
    QVERIFY(finished.wait(5000));
    const SyncResult result = finished.takeFirst().at(0).value<SyncResult>();
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.newMessageCount, 0);
    QCOMPARE(newMessages.count(), 0);
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 0);
}

void TestSync::overlappingSyncRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    seedBaseline(db, QStringLiteral("acct"), QStringLiteral("user@example.com"));

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{200, listBody({})};
    };

    QNetworkAccessManager network;
    SyncService service(&db, &network);
    service.setApiBaseUrl(server.apiBaseUrl());

    SyncAccountRequest request;
    request.accountId = QStringLiteral("acct");
    request.email = QStringLiteral("user@example.com");
    request.accessToken = QStringLiteral("token");
    QVERIFY(service.syncAccount(request, &error));
    QVERIFY(!service.syncAccount(request, &error));
    QVERIFY(error.contains(QStringLiteral("already running")));
}

QTEST_GUILESS_MAIN(TestSync)
#include "test_sync.moc"
