#include "notify/FreedesktopNotificationBackend.h"
#include "notify/NotificationService.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QSignalSpy>
#include <QtTest>

namespace {
constexpr auto kService = "org.freedesktop.Notifications";
constexpr auto kPath = "/org/freedesktop/Notifications";

class FakeNotificationServer : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

public:
    struct Received
    {
        QString appName;
        uint replacesId = 0;
        QString appIcon;
        QString summary;
        QString body;
        QStringList actions;
        QVariantMap hints;
        int timeout = 0;
    };

    QList<Received> received;
    uint nextId = 1;

public slots:
    uint Notify(const QString &appName, uint replacesId, const QString &appIcon,
                const QString &summary, const QString &body, const QStringList &actions,
                const QVariantMap &hints, int timeout)
    {
        Received item;
        item.appName = appName;
        item.replacesId = replacesId;
        item.appIcon = appIcon;
        item.summary = summary;
        item.body = body;
        item.actions = actions;
        item.hints = hints;
        item.timeout = timeout;
        received.append(item);
        return nextId++;
    }

    void CloseNotification(uint) { }

signals:
    void ActionInvoked(uint id, const QString &action);
    void NotificationClosed(uint id, uint reason);
};

NotificationContent sampleContent()
{
    NotificationContent content;
    content.messageId = QStringLiteral("abc-123");
    content.url = QStringLiteral(
        "https://mail.google.com/mail/?authuser=user%40example.com#inbox/abc-123");
    content.summary = QStringLiteral("Sender");
    content.body = QStringLiteral("Subject &lt;update&gt;\nPreview");
    content.actionLabel = QStringLiteral("Open in Gmail");
    return content;
}
} // namespace

class TestFreedesktopNotification : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void missingServerFailsGracefully();
    void sendsNotificationAndOpensOnDefaultAction();
};

void TestFreedesktopNotification::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("KHAT_TEST_PRIVATE_BUS")) {
        QSKIP("Run through ctest under dbus-run-session (private session bus only).");
    }
    QVERIFY2(QDBusConnection::sessionBus().isConnected(), "private session bus unavailable");
}

void TestFreedesktopNotification::missingServerFailsGracefully()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.unregisterService(QString::fromLatin1(kService));
    QTRY_VERIFY(!bus.interface()->isServiceRegistered(QString::fromLatin1(kService)));

    FreedesktopNotificationBackend backend(bus, false);
    NotificationService service(&backend);
    QSignalSpy failures(&service, &NotificationService::notificationFailed);

    QString error;
    QVERIFY(service.notify(sampleContent(), &error));
    QTRY_COMPARE_WITH_TIMEOUT(failures.count(), 1, 8000);
    QVERIFY(failures.first().at(0).toString().contains(QStringLiteral("rejected")));

    NotificationContent invalid = sampleContent();
    invalid.summary.clear();
    QVERIFY(!backend.notify(invalid, &error));
    QCOMPARE(error, QStringLiteral("Notification summary is empty."));
}

void TestFreedesktopNotification::sendsNotificationAndOpensOnDefaultAction()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    FakeNotificationServer server;
    QVERIFY2(bus.registerService(QString::fromLatin1(kService)),
             qPrintable(bus.lastError().message()));
    QVERIFY2(bus.registerObject(QString::fromLatin1(kPath), &server,
                                QDBusConnection::ExportAllSlots
                                    | QDBusConnection::ExportAllSignals),
             qPrintable(bus.lastError().message()));

    FreedesktopNotificationBackend backend(bus, false);
    NotificationService service(&backend);
    QSignalSpy sent(&backend, &FreedesktopNotificationBackend::notificationSent);
    QSignalSpy failures(&service, &NotificationService::notificationFailed);

    QUrl opened;
    service.setUrlOpener([&opened](const QUrl &url) {
        opened = url;
        return true;
    });

    QString error;
    QVERIFY2(service.notify(sampleContent(), &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(server.received.size(), 1, 5000);

    const FakeNotificationServer::Received received = server.received.first();
    QCOMPARE(received.appName, QStringLiteral("khat"));
    QCOMPARE(received.replacesId, 0u);
    QCOMPARE(received.appIcon, QStringLiteral("khat"));
    QCOMPARE(received.summary, QStringLiteral("Sender"));
    QCOMPARE(received.body, QStringLiteral("Subject &lt;update&gt;\nPreview"));
    const QStringList expectedActions{QStringLiteral("default"),
                                      QStringLiteral("Open in Gmail")};
    QCOMPARE(received.actions, expectedActions);
    QCOMPARE(received.hints.value(QStringLiteral("urgency")).toInt(), 1);
    QCOMPARE(received.hints.value(QStringLiteral("category")).toString(),
             QStringLiteral("email"));
    QCOMPARE(received.hints.value(QStringLiteral("desktop-entry")).toString(),
             QStringLiteral("khat"));
    QCOMPARE(received.timeout, -1);

    QTRY_COMPARE_WITH_TIMEOUT(sent.count(), 1, 5000);
    const uint id = sent.first().at(0).toUInt();
    QVERIFY(!opened.isValid());

    emit server.ActionInvoked(id, QStringLiteral("default"));
    QTRY_VERIFY(opened == QUrl(sampleContent().url));
    QCOMPARE(failures.count(), 0);

    bus.unregisterObject(QString::fromLatin1(kPath));
    QVERIFY(bus.unregisterService(QString::fromLatin1(kService)));
}

QTEST_GUILESS_MAIN(TestFreedesktopNotification)
#include "test_freedesktop_notification.moc"
