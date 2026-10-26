#include "app/SingleInstance.h"

#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>

class TestSingleInstance : public QObject
{
    Q_OBJECT

private slots:
    void forwardsCommandsAndSecuresRuntimeDirectory();
    void retriesWhilePrimaryStarts();
    void rejectsSecondOwnerAndReleasesOnDestruction();
    void reportsUnavailableCommandHandler();
};

void TestSingleInstance::forwardsCommandsAndSecuresRuntimeDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString runtimeDir = dir.filePath(QStringLiteral("runtime"));

    SingleInstance primary(runtimeDir);
    QString error;
    QVERIFY2(primary.acquire(&error), qPrintable(error));
    QVERIFY(primary.isPrimary());
    QVERIFY(QFileInfo::exists(primary.socketPath()));

    const QFile::Permissions permissions = QFileInfo(runtimeDir).permissions();
    QVERIFY(permissions.testFlag(QFile::ReadOwner));
    QVERIFY(permissions.testFlag(QFile::WriteOwner));
    QVERIFY(permissions.testFlag(QFile::ExeOwner));
    QVERIFY(!permissions.testFlag(QFile::ReadGroup));
    QVERIFY(!permissions.testFlag(QFile::WriteGroup));
    QVERIFY(!permissions.testFlag(QFile::ReadOther));
    QVERIFY(!permissions.testFlag(QFile::WriteOther));

    QStringList commands;
    primary.setCommandHandler([&commands](const QString &command, QString *message) {
        commands.append(command);
        if (command == QStringLiteral("show-settings")) {
            *message = QStringLiteral("Settings opened.");
            return true;
        }
        if (command == QStringLiteral("check-now")) {
            *message = QStringLiteral("Gmail check started.");
            return true;
        }
        *message = QStringLiteral("Unknown command.");
        return false;
    });

    SingleInstance requester(runtimeDir);
    QString response;
    QVERIFY2(requester.sendCommand(QStringLiteral("show-settings"), &response, &error),
             qPrintable(error));
    QCOMPARE(response, QStringLiteral("Settings opened."));
    QCOMPARE(commands, QStringList{QStringLiteral("show-settings")});

    QVERIFY2(requester.sendCommand(QStringLiteral("check-now"), &response, &error),
             qPrintable(error));
    QCOMPARE(response, QStringLiteral("Gmail check started."));
    QCOMPARE(commands.size(), 2);

    QVERIFY(!requester.sendCommand(QStringLiteral("not-a-command"), &response, &error));
    QCOMPARE(error, QStringLiteral("Unknown command."));
    QCOMPARE(commands.size(), 3);
}

void TestSingleInstance::retriesWhilePrimaryStarts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString runtimeDir = dir.filePath(QStringLiteral("runtime"));

    SingleInstance requester(runtimeDir);
    std::unique_ptr<SingleInstance> primary;
    QString primaryError;
    bool primaryReady = false;
    QTimer::singleShot(150, this, [&] {
        primary = std::make_unique<SingleInstance>(runtimeDir);
        primaryReady = primary->acquire(&primaryError);
        if (primaryReady) {
            primary->setCommandHandler([](const QString &, QString *message) {
                *message = QStringLiteral("pong");
                return true;
            });
        }
    });

    QString response;
    QString error;
    const bool sent = requester.sendCommand(QStringLiteral("ping"), &response, &error);
    QVERIFY2(primaryReady, qPrintable(primaryError));
    QVERIFY2(sent, qPrintable(error));
    QVERIFY(primary);
    QCOMPARE(response, QStringLiteral("pong"));
}

void TestSingleInstance::rejectsSecondOwnerAndReleasesOnDestruction()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString runtimeDir = dir.filePath(QStringLiteral("runtime"));

    QString socketPath;
    {
        SingleInstance primary(runtimeDir);
        QString error;
        QVERIFY2(primary.acquire(&error), qPrintable(error));
        socketPath = primary.socketPath();
        QVERIFY(!socketPath.isEmpty());

        SingleInstance second(runtimeDir);
        QString secondError;
        QVERIFY(!second.acquire(&secondError));
        QVERIFY(!second.isPrimary());
        QVERIFY(!secondError.isEmpty());

        primary.setCommandHandler(
            [](const QString &, QString *message) {
                *message = QStringLiteral("pong");
                return true;
            });
        QString response;
        QVERIFY2(second.sendCommand(QStringLiteral("ping"), &response, &error),
                 qPrintable(error));
        QCOMPARE(response, QStringLiteral("pong"));
    }

    QVERIFY(!QFileInfo::exists(socketPath));

    SingleInstance replacement(runtimeDir);
    QString error;
    QVERIFY2(replacement.acquire(&error), qPrintable(error));
    QVERIFY(replacement.isPrimary());
}

void TestSingleInstance::reportsUnavailableCommandHandler()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString runtimeDir = dir.filePath(QStringLiteral("runtime"));

    SingleInstance primary(runtimeDir);
    QString error;
    QVERIFY2(primary.acquire(&error), qPrintable(error));

    SingleInstance requester(runtimeDir);
    QString response;
    QVERIFY(!requester.sendCommand(QStringLiteral("ping"), &response, &error));
    QVERIFY(error.contains(QStringLiteral("still starting")));
}

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "test_single_instance.moc"
