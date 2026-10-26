#include "state/StateDatabase.h"
#include "sync/SyncScheduler.h"
#include "sync/SyncService.h"

#include "support/FakeGmailServer.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class TestScheduler : public QObject
{
    Q_OBJECT

private slots:
    void intervalValidation();
    void successSchedulesIntervalAndManualRefresh();
    void failureUsesBackoffAndSuccessResets();
    void noAccountsCompletesWithoutNetwork();
    void multipleAccountsWaitForAll();
};

static void seedBaseline(StateDatabase &db, const QString &id, const QString &email)
{
    QString error;
    QVERIFY2(db.ensureAccount(id, email, &error), qPrintable(error));
    QVERIFY2(db.applyBaseline(id, {}, 1000, &error), qPrintable(error));
}

static SyncAccountRequest requestFor(const QString &id, const QString &email,
                                     const QString &token)
{
    SyncAccountRequest request;
    request.accountId = id;
    request.email = email;
    request.accessToken = token;
    return request;
}

void TestScheduler::intervalValidation()
{
    QNetworkAccessManager network;
    StateDatabase db(QStringLiteral(":memory:"));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    SyncService sync(&db, &network);
    SyncScheduler scheduler(&sync);

    QCOMPARE(scheduler.intervalMinutes(), 180);
    QVERIFY(!scheduler.setIntervalMinutes(4));
    QVERIFY(scheduler.setIntervalMinutes(5));
    QCOMPARE(scheduler.intervalMinutes(), 5);
    QVERIFY(scheduler.setIntervalMinutes(1440));
    QVERIFY(!scheduler.setIntervalMinutes(1441));
}

void TestScheduler::successSchedulesIntervalAndManualRefresh()
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
        return FakeGmailServer::Response{200, QByteArrayLiteral("{\"messages\":[]}")};
    };

    QNetworkAccessManager network;
    SyncService sync(&db, &network);
    sync.setApiBaseUrl(server.apiBaseUrl());
    SyncScheduler scheduler(&sync);
    scheduler.setAccounts(
        {requestFor(QStringLiteral("acct"), QStringLiteral("user@example.com"),
                    QStringLiteral("token"))});
    QVERIFY(scheduler.setIntervalMinutes(180));

    QSignalSpy started(&scheduler, &SyncScheduler::checkStarted);
    QSignalSpy finished(&scheduler, &SyncScheduler::checkFinished);

    QVERIFY(scheduler.start());
    QVERIFY(scheduler.isRunning());
    QVERIFY(scheduler.isCheckRunning());
    QVERIFY(!scheduler.start());
    QVERIFY(!scheduler.checkNow());

    QVERIFY2(finished.wait(5000), "first check timed out");
    QCOMPARE(started.count(), 1);
    QCOMPARE(scheduler.lastResults().size(), 1);
    QVERIFY(scheduler.lastResults().first().success);
    QVERIFY(scheduler.lastSuccessEpoch() > 0);
    QVERIFY(scheduler.lastError().isEmpty());
    QCOMPARE(scheduler.backoffFailureCount(), 0);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QVERIFY(qAbs(scheduler.nextCheckEpoch() - (now + 180 * 60)) <= 5);
    QVERIFY(!scheduler.isCheckRunning());

    QVERIFY(scheduler.checkNow());
    QVERIFY(scheduler.isCheckRunning());
    QVERIFY(finished.wait(5000));
    QCOMPARE(started.count(), 2);
    QVERIFY(scheduler.lastResults().first().success);

    scheduler.stop();
    QVERIFY(!scheduler.isRunning());
    QCOMPARE(scheduler.nextCheckEpoch(), 0);
}

void TestScheduler::failureUsesBackoffAndSuccessResets()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    seedBaseline(db, QStringLiteral("acct"), QStringLiteral("user@example.com"));

    FakeGmailServer server;
    QVERIFY(server.start());
    bool fail = true;
    server.handler = [&fail](const QUrl &, const QByteArray &) {
        if (fail) {
            return FakeGmailServer::Response{500, QByteArrayLiteral("{}")};
        }
        return FakeGmailServer::Response{200, QByteArrayLiteral("{\"messages\":[]}")};
    };

    QNetworkAccessManager network;
    SyncService sync(&db, &network);
    sync.setApiBaseUrl(server.apiBaseUrl());
    SyncScheduler scheduler(&sync);
    scheduler.setAccounts(
        {requestFor(QStringLiteral("acct"), QStringLiteral("user@example.com"),
                    QStringLiteral("token"))});
    QSignalSpy finished(&scheduler, &SyncScheduler::checkFinished);

    QVERIFY(scheduler.start());
    QVERIFY(finished.wait(5000));
    QCOMPARE(scheduler.lastResults().size(), 1);
    QVERIFY(!scheduler.lastResults().first().success);
    QCOMPARE(int(scheduler.lastResults().first().errorKind), int(GmailErrorKind::Server));
    QVERIFY(scheduler.lastError().contains(QStringLiteral("500")));
    QCOMPARE(scheduler.backoffFailureCount(), 1);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QVERIFY(qAbs(scheduler.nextCheckEpoch() - (now + 60)) <= 5);

    fail = false;
    QVERIFY(scheduler.checkNow());
    QVERIFY(finished.wait(5000));
    QVERIFY(scheduler.lastResults().first().success);
    QCOMPARE(scheduler.backoffFailureCount(), 0);
    QVERIFY(scheduler.lastError().isEmpty());
    const qint64 nowAfterSuccess = QDateTime::currentSecsSinceEpoch();
    QVERIFY(qAbs(scheduler.nextCheckEpoch() - (nowAfterSuccess + 180 * 60)) <= 5);

    scheduler.stop();
}

void TestScheduler::noAccountsCompletesWithoutNetwork()
{
    StateDatabase db(QStringLiteral(":memory:"));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    QNetworkAccessManager network;
    SyncService sync(&db, &network);
    SyncScheduler scheduler(&sync);

    QSignalSpy started(&scheduler, &SyncScheduler::checkStarted);
    QSignalSpy finished(&scheduler, &SyncScheduler::checkFinished);
    QVERIFY(scheduler.start());
    QCOMPARE(started.count(), 1);
    QCOMPARE(finished.count(), 1);
    QVERIFY(scheduler.lastResults().isEmpty());
    QVERIFY(scheduler.lastSuccessEpoch() > 0);
    QVERIFY(scheduler.nextCheckEpoch() > 0);
    scheduler.stop();
}

void TestScheduler::multipleAccountsWaitForAll()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    seedBaseline(db, QStringLiteral("one"), QStringLiteral("one@example.com"));
    seedBaseline(db, QStringLiteral("two"), QStringLiteral("two@example.com"));

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{200, QByteArrayLiteral("{\"messages\":[]}")};
    };

    QNetworkAccessManager network;
    SyncService sync(&db, &network);
    sync.setApiBaseUrl(server.apiBaseUrl());
    SyncScheduler scheduler(&sync);
    scheduler.setAccounts(
        {requestFor(QStringLiteral("one"), QStringLiteral("one@example.com"),
                    QStringLiteral("token-one")),
         requestFor(QStringLiteral("two"), QStringLiteral("two@example.com"),
                    QStringLiteral("token-two"))});

    QSignalSpy finished(&scheduler, &SyncScheduler::checkFinished);
    QVERIFY(scheduler.start());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QCOMPARE(scheduler.lastResults().size(), 2);
    QVERIFY(scheduler.lastResults().at(0).success);
    QVERIFY(scheduler.lastResults().at(1).success);
    QCOMPARE(sync.activeSyncs(), 0);
    scheduler.stop();
}

QTEST_GUILESS_MAIN(TestScheduler)
#include "test_scheduler.moc"
