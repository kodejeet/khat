#include "connectivity/ConnectivityMonitor.h"
#include "state/StateDatabase.h"
#include "sync/SyncScheduler.h"
#include "sync/SyncService.h"

#include "support/FakeGmailServer.h"

#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class TestConnectivity : public QObject
{
    Q_OBJECT

private slots:
    void simulatedTransitions();
    void backendInitialization();
    void offlineDefersChecksAndReconnectSyncs();
};

static void seedBaseline(StateDatabase &db, const QString &id, const QString &email)
{
    QString error;
    QVERIFY2(db.ensureAccount(id, email, &error), qPrintable(error));
    QVERIFY2(db.applyBaseline(id, {}, 1000, &error), qPrintable(error));
}

void TestConnectivity::simulatedTransitions()
{
    ConnectivityMonitor monitor;
    QCOMPARE(monitor.state(), ConnectivityMonitor::State::Unknown);
    QVERIFY(monitor.isOnline());

    QSignalSpy stateSpy(&monitor, &ConnectivityMonitor::stateChanged);
    QSignalSpy onlineSpy(&monitor, &ConnectivityMonitor::onlineChanged);

    monitor.setSimulatedState(ConnectivityMonitor::State::Offline);
    QCOMPARE(monitor.state(), ConnectivityMonitor::State::Offline);
    QVERIFY(!monitor.isOnline());
    QCOMPARE(monitor.stateText(), QStringLiteral("Offline"));
    QCOMPARE(onlineSpy.count(), 1);
    QCOMPARE(onlineSpy.takeFirst().at(0).toBool(), false);

    monitor.setSimulatedState(ConnectivityMonitor::State::Online);
    QCOMPARE(monitor.state(), ConnectivityMonitor::State::Online);
    QVERIFY(monitor.isOnline());
    QCOMPARE(onlineSpy.count(), 1);
    QCOMPARE(onlineSpy.takeFirst().at(0).toBool(), true);
    QVERIFY(stateSpy.count() >= 2);

    monitor.setSimulatedState(ConnectivityMonitor::State::Online);
    QCOMPARE(onlineSpy.count(), 0);

    monitor.clearSimulatedState();
    QCOMPARE(monitor.state(), ConnectivityMonitor::State::Unknown);
    QVERIFY(monitor.isOnline());
}

void TestConnectivity::backendInitialization()
{
    ConnectivityMonitor monitor;
    const bool available = monitor.initialize();
    if (!available) {
        QSKIP("QNetworkInformation backend is not available on this system");
    }
    QVERIFY(monitor.backendAvailable());
    QVERIFY(monitor.state() == ConnectivityMonitor::State::Online
            || monitor.state() == ConnectivityMonitor::State::Offline
            || monitor.state() == ConnectivityMonitor::State::Unknown);
}

void TestConnectivity::offlineDefersChecksAndReconnectSyncs()
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

    ConnectivityMonitor monitor;
    monitor.setSimulatedState(ConnectivityMonitor::State::Offline);

    SyncScheduler scheduler(&sync);
    scheduler.setConnectivityMonitor(&monitor);
    scheduler.setAccounts(
        {SyncAccountRequest{QStringLiteral("acct"), QStringLiteral("user@example.com"),
                            QStringLiteral("token"), true}});

    QSignalSpy deferred(&scheduler, &SyncScheduler::checkDeferred);
    QSignalSpy finished(&scheduler, &SyncScheduler::checkFinished);

    QVERIFY(scheduler.start());
    QCOMPARE(deferred.count(), 1);
    QVERIFY(scheduler.isCheckDeferredForOffline());
    QVERIFY(!scheduler.isCheckRunning());
    QCOMPARE(scheduler.nextCheckEpoch(), 0);
    QCOMPARE(server.requests.size(), 0);

    QVERIFY(!scheduler.checkNow());
    QCOMPARE(server.requests.size(), 0);
    QCOMPARE(deferred.count(), 2);

    monitor.setSimulatedState(ConnectivityMonitor::State::Online);
    QVERIFY2(finished.wait(5000), "reconnect check timed out");
    QCOMPARE(server.syncRequestCount(), 1);
    QVERIFY(scheduler.lastResults().first().success);
    QVERIFY(!scheduler.isCheckDeferredForOffline());
    QVERIFY(scheduler.nextCheckEpoch() > 0);

    monitor.setSimulatedState(ConnectivityMonitor::State::Offline);
    QVERIFY(scheduler.isCheckDeferredForOffline());
    QCOMPARE(scheduler.nextCheckEpoch(), 0);
    QVERIFY(!scheduler.checkNow());
    QVERIFY(!scheduler.checkNow());
    QCOMPARE(server.syncRequestCount(), 1);

    monitor.setSimulatedState(ConnectivityMonitor::State::Online);
    QVERIFY2(finished.wait(5000), "second reconnect check timed out");
    QCOMPARE(server.syncRequestCount(), 2);
    QCOMPARE(finished.count(), 2);

    scheduler.stop();
}

QTEST_GUILESS_MAIN(TestConnectivity)
#include "test_connectivity.moc"
