#include "app/UnixSignalHandler.h"

#include <QSignalSpy>
#include <QtTest>

#include <csignal>

class TestUnixSignal : public QObject
{
    Q_OBJECT

private slots:
    void convertsSigtermToQtEvent();
    void canBeReinstalledAfterUninstall();
    void inactiveHandlerDoesNotDisturbActiveHandler();
};

void TestUnixSignal::convertsSigtermToQtEvent()
{
    UnixSignalHandler handler;
    QSignalSpy spy(&handler, &UnixSignalHandler::terminationRequested);
    QString error;
    QVERIFY2(handler.install(&error), qPrintable(error));

    QCOMPARE(::raise(SIGTERM), 0);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2000);
    QCOMPARE(spy.first().at(0).toInt(), int(SIGTERM));

    handler.uninstall();
}

void TestUnixSignal::canBeReinstalledAfterUninstall()
{
    UnixSignalHandler first;
    QString error;
    QVERIFY2(first.install(&error), qPrintable(error));
    first.uninstall();

    UnixSignalHandler second;
    QSignalSpy spy(&second, &UnixSignalHandler::terminationRequested);
    QVERIFY2(second.install(&error), qPrintable(error));
    QCOMPARE(::raise(SIGINT), 0);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2000);
    QCOMPARE(spy.first().at(0).toInt(), int(SIGINT));
    second.uninstall();
}

void TestUnixSignal::inactiveHandlerDoesNotDisturbActiveHandler()
{
    UnixSignalHandler active;
    QSignalSpy spy(&active, &UnixSignalHandler::terminationRequested);
    QString error;
    QVERIFY2(active.install(&error), qPrintable(error));

    {
        UnixSignalHandler inactive;
        QVERIFY(!inactive.install(&error));
        QVERIFY(error.contains(QStringLiteral("already installed")));
    }

    QCOMPARE(::raise(SIGTERM), 0);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2000);
    QCOMPARE(spy.first().at(0).toInt(), int(SIGTERM));
    active.uninstall();
}

QTEST_GUILESS_MAIN(TestUnixSignal)
#include "test_unix_signal.moc"
