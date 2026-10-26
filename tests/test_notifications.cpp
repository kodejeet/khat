#include "notify/NotificationBackend.h"
#include "notify/NotificationContent.h"
#include "notify/NotificationDispatcher.h"
#include "notify/NotificationService.h"
#include "state/StateDatabase.h"
#include "sync/SyncService.h"

#include "support/FakeGmailServer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

namespace {
class FakeNotificationBackend : public NotificationBackend
{
public:
    bool notify(const NotificationContent &content, QString *error) override
    {
        if (fail) {
            if (error) {
                *error = QStringLiteral("simulated notification failure");
            }
            return false;
        }
        sent.append(content);
        return true;
    }

    QList<NotificationContent> sent;
    bool fail = false;
};

MessageSummary makeMessage(const QString &id, const QString &sender,
                           const QString &subject, const QString &snippet)
{
    MessageSummary message;
    message.id = id;
    message.threadId = id + QStringLiteral("-thread");
    message.sender = sender;
    message.subject = subject;
    message.snippet = snippet;
    message.internalDate = 1700000000000;
    message.labelIds = {QStringLiteral("INBOX")};
    return message;
}

bool openState(StateDatabase &db, const QString &path)
{
    QString error;
    return db.open(&error);
}

void seedBaseline(StateDatabase &db, const QString &id, const QString &email)
{
    QString error;
    QVERIFY2(db.ensureAccount(id, email, &error), qPrintable(error));
    QVERIFY2(db.applyBaseline(id, {}, 1000, &error), qPrintable(error));
}

void record(StateDatabase &db, const QString &accountId, const MessageSummary &message)
{
    QString error;
    QVERIFY2(db.recordMessages(accountId, {message}, 2000, nullptr, &error),
             qPrintable(error));
}

QByteArray listBody(const QStringList &ids)
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
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray metadataBody(const QString &id)
{
    QJsonObject from;
    from.insert(QStringLiteral("name"), QStringLiteral("From"));
    from.insert(QStringLiteral("value"), QStringLiteral("Sender <sender@example.com>"));
    QJsonObject subject;
    subject.insert(QStringLiteral("name"), QStringLiteral("Subject"));
    subject.insert(QStringLiteral("value"), QStringLiteral("Subject for %1").arg(id));

    QJsonArray headers;
    headers.append(from);
    headers.append(subject);
    QJsonObject payload;
    payload.insert(QStringLiteral("headers"), headers);

    QJsonObject root;
    root.insert(QStringLiteral("id"), id);
    root.insert(QStringLiteral("threadId"), id + QStringLiteral("-thread"));
    root.insert(QStringLiteral("internalDate"), QStringLiteral("1700000000000"));
    root.insert(QStringLiteral("labelIds"),
                QJsonArray{QStringLiteral("INBOX")});
    root.insert(QStringLiteral("snippet"), QStringLiteral("Preview for %1").arg(id));
    root.insert(QStringLiteral("payload"), payload);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
} // namespace

class TestNotifications : public QObject
{
    Q_OBJECT

private slots:
    void contentFormatting();
    void senderDisplay();
    void serviceActivationAndUrlValidation();
    void dispatcherDeduplicatesAndHonorsConfig();
    void dispatcherWithSyncService();
};

void TestNotifications::contentFormatting()
{
    const MessageSummary message = makeMessage(
        QStringLiteral("abc-123_XYZ"), QStringLiteral("Alice Example <alice@example.com>"),
        QStringLiteral("Quarterly <update> & more"),
        QStringLiteral("  Line one\nLine two\twith   spaces  "));

    const NotificationContent content = buildNotificationContent(
        message, QStringLiteral("user+tag@example.com"), true, true);
    QCOMPARE(content.messageId, QStringLiteral("abc-123_XYZ"));
    QCOMPARE(content.summary, QStringLiteral("Alice Example"));
    QCOMPARE(content.body,
             QStringLiteral("Quarterly &lt;update&gt; &amp; more\n"
                            "Line one Line two with spaces\n"
                            "Account: user+tag@example.com"));
    QCOMPARE(content.url,
             QStringLiteral("https://mail.google.com/mail/?authuser=user%2Btag%40example.com"
                            "#inbox/abc-123_XYZ"));
    QCOMPARE(content.actionLabel, QStringLiteral("Open in Gmail"));
    QCOMPARE(content.urgency, 1);

    const NotificationContent noPreview = buildNotificationContent(
        message, QStringLiteral("user@example.com"), false, false);
    QCOMPARE(noPreview.body, QStringLiteral("Quarterly &lt;update&gt; &amp; more"));

    MessageSummary empty;
    empty.id = QStringLiteral("empty");
    const NotificationContent fallback =
        buildNotificationContent(empty, QStringLiteral("user@example.com"), true, false);
    QCOMPARE(fallback.summary, QStringLiteral("New Gmail message"));
    QCOMPARE(fallback.body, QStringLiteral("(No subject)"));

    MessageSummary longMessage;
    longMessage.id = QStringLiteral("long");
    longMessage.subject = QStringLiteral("S");
    longMessage.snippet = QString(300, QLatin1Char('a'));
    const NotificationContent truncated =
        buildNotificationContent(longMessage, QStringLiteral("user@example.com"), true, false);
    QCOMPARE(truncated.body,
             QStringLiteral("S\n") + QString(kNotificationPreviewLength, QLatin1Char('a'))
                 + QChar(0x2026));

    QCOMPARE(notificationTruncate(QStringLiteral("abcd"), 3),
             QStringLiteral("abc") + QChar(0x2026));
    const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
    QCOMPARE(notificationTruncate(emoji, 1), QString(QChar(0x2026)));
}

void TestNotifications::senderDisplay()
{
    QCOMPARE(notificationSenderDisplay(
                 QStringLiteral("Alice Example <alice@example.com>")),
             QStringLiteral("Alice Example"));
    QCOMPARE(notificationSenderDisplay(
                 QStringLiteral("\"Alice, Example\" <alice@example.com>")),
             QStringLiteral("Alice, Example"));
    QCOMPARE(notificationSenderDisplay(QStringLiteral("<alice@example.com>")),
             QStringLiteral("alice@example.com"));
    QCOMPARE(notificationSenderDisplay(QStringLiteral("alice@example.com")),
             QStringLiteral("alice@example.com"));
    QCOMPARE(notificationSenderDisplay(QStringLiteral("  Alice\n Example  ")),
             QStringLiteral("Alice Example"));
    QCOMPARE(notificationSenderDisplay(QString()), QString());
}

void TestNotifications::serviceActivationAndUrlValidation()
{
    FakeNotificationBackend backend;
    NotificationService service(&backend);

    QUrl opened;
    service.setUrlOpener([&opened](const QUrl &url) {
        opened = url;
        return true;
    });

    NotificationContent content;
    content.messageId = QStringLiteral("abc");
    content.summary = QStringLiteral("Subject");
    content.url = QStringLiteral(
        "https://mail.google.com/mail/?authuser=user%40example.com#inbox/abc");

    QVERIFY(service.notify(content));
    QVERIFY(!opened.isValid()); // Merely delivering must not open a browser.
    emit backend.activated(content.url);
    QCOMPARE(opened, QUrl(content.url));

    opened = QUrl();
    QSignalSpy openFailures(&service, &NotificationService::urlOpenFailed);
    emit backend.activated(QStringLiteral("https://example.com/phishing"));
    QCOMPARE(openFailures.count(), 1);
    QVERIFY(!opened.isValid());
    QCOMPARE(openFailures.first().at(0).toUrl(), QUrl(QStringLiteral("https://example.com/phishing")));

    content.url = QStringLiteral("https://example.com/not-gmail");
    QString error;
    QVERIFY(!service.notify(content, &error));
    QCOMPARE(error, QStringLiteral("Refusing a non-Gmail notification URL."));

    content.url = QStringLiteral("https://mail.google.com/mail/#inbox/abc");
    backend.fail = true;
    QVERIFY(!service.notify(content, &error));
    QCOMPARE(error, QStringLiteral("simulated notification failure"));
}

void TestNotifications::dispatcherDeduplicatesAndHonorsConfig()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QVERIFY2(openState(db, db.path()), "cannot open state database");
    seedBaseline(db, QStringLiteral("acct"), QStringLiteral("user@example.com"));

    FakeNotificationBackend backend;
    NotificationService service(&backend);
    QNetworkAccessManager network;
    SyncService sync(&db, &network);
    NotificationDispatcher dispatcher(&sync, &service, &db);

    NotificationConfig config = defaultAppConfig().notifications;
    config.enabled = true;
    config.showPreview = false;
    dispatcher.setConfig(config);

    QSignalSpy delivered(&dispatcher, &NotificationDispatcher::notificationDelivered);
    QSignalSpy failed(&dispatcher, &NotificationDispatcher::notificationFailed);

    const MessageSummary first = makeMessage(
        QStringLiteral("m1"), QStringLiteral("Alice <alice@example.com>"),
        QStringLiteral("First"), QStringLiteral("hidden preview"));
    record(db, QStringLiteral("acct"), first);

    dispatcher.processMessages(QStringLiteral("acct"), {first});
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(backend.sent.first().messageId, QStringLiteral("m1"));
    QVERIFY(!backend.sent.first().body.contains(QStringLiteral("hidden preview")));
    QCOMPARE(delivered.count(), 1);
    QCOMPARE(failed.count(), 0);

    bool notified = false;
    QVERIFY(db.messageWasNotified(QStringLiteral("acct"), QStringLiteral("m1"), &notified));
    QVERIFY(notified);

    dispatcher.processMessages(QStringLiteral("acct"), {first});
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(delivered.count(), 1);

    config.enabled = false;
    dispatcher.setConfig(config);
    const MessageSummary second = makeMessage(
        QStringLiteral("m2"), QStringLiteral("Bob <bob@example.com>"),
        QStringLiteral("Second"), QStringLiteral("second preview"));
    record(db, QStringLiteral("acct"), second);
    dispatcher.processMessages(QStringLiteral("acct"), {second});
    QCOMPARE(backend.sent.size(), 1);
    QVERIFY(db.messageWasNotified(QStringLiteral("acct"), QStringLiteral("m2"), &notified));
    QVERIFY(!notified);

    QString error;
    QVERIFY2(db.ensureAccount(QStringLiteral("acct2"), QStringLiteral("other@example.com"),
                              &error),
             qPrintable(error));
    config.enabled = true;
    config.showPreview = true;
    dispatcher.setConfig(config);
    const MessageSummary third = makeMessage(
        QStringLiteral("m3"), QStringLiteral("Carol <carol@example.com>"),
        QStringLiteral("Third"), QStringLiteral("visible preview"));
    record(db, QStringLiteral("acct"), third);
    dispatcher.processMessages(QStringLiteral("acct"), {third});
    QCOMPARE(backend.sent.size(), 2);
    QVERIFY(backend.sent.last().body.contains(QStringLiteral("visible preview")));
    QVERIFY(backend.sent.last().body.contains(QStringLiteral("Account: user@example.com")));

    backend.fail = true;
    const MessageSummary fourth = makeMessage(
        QStringLiteral("m4"), QStringLiteral("Dave <dave@example.com>"),
        QStringLiteral("Fourth"), QStringLiteral("failure preview"));
    record(db, QStringLiteral("acct"), fourth);
    dispatcher.processMessages(QStringLiteral("acct"), {fourth});
    QCOMPARE(backend.sent.size(), 2);
    QCOMPARE(failed.count(), 1);
    QVERIFY(db.messageWasNotified(QStringLiteral("acct"), QStringLiteral("m4"), &notified));
    QVERIFY(!notified);
}

void TestNotifications::dispatcherWithSyncService()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QVERIFY2(openState(db, db.path()), "cannot open state database");
    seedBaseline(db, QStringLiteral("acct"), QStringLiteral("user@example.com"));

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &url, const QByteArray &) {
        if (url.path().endsWith(QStringLiteral("/users/me/messages"))) {
            return FakeGmailServer::Response{200,
                                             listBody({QStringLiteral("new-1")})};
        }
        if (url.path().endsWith(QStringLiteral("/new-1"))) {
            return FakeGmailServer::Response{200, metadataBody(QStringLiteral("new-1"))};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager network;
    SyncService sync(&db, &network);
    sync.setApiBaseUrl(server.apiBaseUrl());

    FakeNotificationBackend backend;
    NotificationService service(&backend);
    NotificationDispatcher dispatcher(&sync, &service, &db);
    QSignalSpy delivered(&dispatcher, &NotificationDispatcher::notificationDelivered);

    SyncAccountRequest request;
    request.accountId = QStringLiteral("acct");
    request.email = QStringLiteral("user@example.com");
    request.accessToken = QStringLiteral("token");
    request.includeUpdates = true;

    QSignalSpy finished(&sync, &SyncService::syncFinished);
    QString error;
    QVERIFY2(sync.syncAccount(request, &error), qPrintable(error));
    QVERIFY2(finished.wait(5000), "sync timed out");
    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.first().at(0).value<SyncResult>().success);

    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(delivered.count(), 1);
    QCOMPARE(backend.sent.first().messageId, QStringLiteral("new-1"));
    QCOMPARE(backend.sent.first().summary, QStringLiteral("Sender"));
    QVERIFY(backend.sent.first().body.contains(QStringLiteral("Subject for new-1")));
    QVERIFY(backend.sent.first().body.contains(QStringLiteral("Preview for new-1")));
    QCOMPARE(backend.sent.first().url,
             QStringLiteral("https://mail.google.com/mail/?authuser=user%40example.com"
                            "#inbox/new-1"));

    bool notified = false;
    QVERIFY(db.messageWasNotified(QStringLiteral("acct"), QStringLiteral("new-1"), &notified));
    QVERIFY(notified);

    finished.clear();
    QVERIFY2(sync.syncAccount(request, &error), qPrintable(error));
    QVERIFY2(finished.wait(5000), "second sync timed out");
    QCOMPARE(finished.count(), 1);
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(delivered.count(), 1);
}

QTEST_GUILESS_MAIN(TestNotifications)
#include "test_notifications.moc"
