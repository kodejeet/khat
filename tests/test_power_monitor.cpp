#include "app/PowerMonitor.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QSignalSpy>
#include <QtTest>

namespace {
constexpr auto kLogin1Service = "org.freedesktop.login1";
constexpr auto kLogin1Path = "/org/freedesktop/login1";

class FakeLogin1 : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.login1.Manager")

signals:
    void PrepareForSleep(bool start);
};
} // namespace

class TestPowerMonitor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void forwardsSleepSignals();
    void missingBusFailsGracefully();
};

void TestPowerMonitor::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("KHAT_TEST_PRIVATE_BUS")) {
        QSKIP("Run through ctest under dbus-run-session (private session bus only).");
    }
    QVERIFY2(QDBusConnection::sessionBus().isConnected(), "private session bus unavailable");
}

void TestPowerMonitor::forwardsSleepSignals()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    FakeLogin1 login1;
    QVERIFY2(bus.registerService(QString::fromLatin1(kLogin1Service)),
             qPrintable(bus.lastError().message()));
    QVERIFY2(bus.registerObject(QString::fromLatin1(kLogin1Path), &login1,
                                QDBusConnection::ExportAllSignals),
             qPrintable(bus.lastError().message()));

    PowerMonitor monitor(bus);
    QString error;
    QVERIFY2(monitor.subscribe(&error), qPrintable(error));
    QVERIFY(monitor.isSubscribed());

    QSignalSpy suspended(&monitor, &PowerMonitor::aboutToSuspend);
    QSignalSpy resumed(&monitor, &PowerMonitor::resumed);

    emit login1.PrepareForSleep(true);
    QTRY_COMPARE_WITH_TIMEOUT(suspended.count(), 1, 5000);
    QCOMPARE(resumed.count(), 0);

    emit login1.PrepareForSleep(false);
    QTRY_COMPARE_WITH_TIMEOUT(resumed.count(), 1, 5000);
    QCOMPARE(suspended.count(), 1);

    bus.unregisterObject(QString::fromLatin1(kLogin1Path));
    QVERIFY(bus.unregisterService(QString::fromLatin1(kLogin1Service)));
}

void TestPowerMonitor::missingBusFailsGracefully()
{
    QDBusConnection missing(QStringLiteral("khat-test-unconnected"));
    PowerMonitor monitor(missing);
    QString error;
    QVERIFY(!monitor.subscribe(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!monitor.isSubscribed());
}

QTEST_GUILESS_MAIN(TestPowerMonitor)
#include "test_power_monitor.moc"
