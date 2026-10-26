#include "app/AppController.h"
#include "auth/AccessTokenProvider.h"
#include "ui/SettingsWindow.h"

#include "support/FakeGmailServer.h"

#include <QApplication>
#include <QHash>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

namespace {
class FakeTokenProvider final : public AccessTokenProvider
{
public:
    bool accessToken(const QString &accountId, QString *token) const override
    {
        const auto it = tokens.constFind(accountId);
        if (it == tokens.cend()) {
            if (token) {
                token->clear();
            }
            return false;
        }
        if (token) {
            *token = it.value();
        }
        return true;
    }

    QHash<QString, QString> tokens;
};

AppPaths testPaths(const QTemporaryDir &dir)
{
    AppPaths paths;
    paths.configDir = dir.filePath(QStringLiteral("config"));
    paths.configFile = paths.configDir + QStringLiteral("/config.json");
    paths.oauthClientFile = paths.configDir + QStringLiteral("/oauth-client.json");
    paths.stateFile = dir.filePath(QStringLiteral("state.db"));
    paths.runtimeDir = dir.filePath(QStringLiteral("runtime"));
    return paths;
}

bool saveConfig(const AppPaths &paths, const AppConfig &config, QString *error)
{
    ConfigStore store(paths.configFile);
    if (!store.load(error)) {
        return false;
    }
    return store.save(config, error);
}

AccountConfig account(const QString &id, const QString &email, bool enabled = true)
{
    return AccountConfig{id, email, QString(), enabled};
}

QByteArray emptyPage()
{
    return QByteArrayLiteral("{\"messages\":[]}");
}

AppControllerOptions controllerOptions(const AppPaths &paths, const FakeGmailServer &server)
{
    AppControllerOptions options;
    options.paths = paths;
    options.gmailApiBaseUrl = server.apiBaseUrl();
    options.subscribeToSleep = false;
    return options;
}
} // namespace

class TestAppController : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void initializesIdleWithoutTokensAndStartsAfterTokenAppears();
    void startsAuthorizedAccountAndPreservesBaselineAcrossRestart();
    void disabledAccountDoesNotPoll();
    void settingsWindowIsLazyAndStaysReopenable();
    void suspendAndResumeRestartsPolling();
};

void TestAppController::initTestCase()
{
    QApplication::setQuitOnLastWindowClosed(false);
}

void TestAppController::initializesIdleWithoutTokensAndStartsAfterTokenAppears()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppPaths paths = testPaths(dir);

    AppConfig config = defaultAppConfig();
    config.accounts = {account(QStringLiteral("acct-one"), QStringLiteral("one@example.com"))};
    QString error;
    QVERIFY2(saveConfig(paths, config, &error), qPrintable(error));

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{200, emptyPage()};
    };

    auto tokens = std::make_shared<FakeTokenProvider>();
    AppController controller(controllerOptions(paths, server), tokens);
    QSignalSpy finished(controller.scheduler(), &SyncScheduler::checkFinished);
    QVERIFY2(controller.initialize(&error), qPrintable(error));

    QVERIFY(!controller.scheduler()->isRunning());
    QVERIFY(controller.isWaitingForAuthorization());
    QCOMPARE(controller.accountsAwaitingAuthorization(), 1);
    QCOMPARE(server.requests.size(), 0);

    AccountState state;
    QVERIFY2(controller.stateDatabase()->accountState(QStringLiteral("acct-one"), &state,
                                                       &error),
             qPrintable(error));
    QVERIFY(!state.baselineDone);

    tokens->tokens.insert(QStringLiteral("acct-one"), QStringLiteral("test-token"));
    controller.reloadAccounts();
    QVERIFY(controller.scheduler()->isRunning());
    QVERIFY(!controller.isWaitingForAuthorization());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);

    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().authorization,
             QByteArrayLiteral("Bearer test-token"));
    QVERIFY2(controller.stateDatabase()->accountState(QStringLiteral("acct-one"), &state,
                                                       &error),
             qPrintable(error));
    QVERIFY(state.baselineDone);
    controller.shutdown();
}

void TestAppController::startsAuthorizedAccountAndPreservesBaselineAcrossRestart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppPaths paths = testPaths(dir);

    AppConfig config = defaultAppConfig();
    config.accounts = {account(QStringLiteral("acct-one"), QStringLiteral("one@example.com"))};
    QString error;
    QVERIFY2(saveConfig(paths, config, &error), qPrintable(error));

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{200, emptyPage()};
    };

    auto tokens = std::make_shared<FakeTokenProvider>();
    tokens->tokens.insert(QStringLiteral("acct-one"), QStringLiteral("test-token"));

    {
        AppController controller(controllerOptions(paths, server), tokens);
        QSignalSpy finished(controller.scheduler(), &SyncScheduler::checkFinished);
        QVERIFY2(controller.initialize(&error), qPrintable(error));
        QVERIFY(controller.scheduler()->isRunning());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(server.requests.size(), 1);

        AccountState state;
        QVERIFY2(controller.stateDatabase()->accountState(
                     QStringLiteral("acct-one"), &state, &error),
                 qPrintable(error));
        QVERIFY(state.baselineDone);
        QVERIFY(state.lastSyncEpoch > 0);

        QVERIFY(controller.showSettings());
        QVERIFY(controller.settingsWindow());
        controller.settingsWindow()->close();
        QVERIFY(!controller.settingsWindow()->isVisible());
        QVERIFY(controller.scheduler()->isRunning());
        QVERIFY(controller.scheduler()->nextCheckEpoch() > 0);
        controller.shutdown();
    }

    {
        AppController controller(controllerOptions(paths, server), tokens);
        QSignalSpy finished(controller.scheduler(), &SyncScheduler::checkFinished);
        QVERIFY2(controller.initialize(&error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(server.requests.size(), 2);

        AccountState state;
        QVERIFY2(controller.stateDatabase()->accountState(
                     QStringLiteral("acct-one"), &state, &error),
                 qPrintable(error));
        QVERIFY(state.baselineDone);
        QCOMPARE(controller.stateDatabase()->countSeenMessages(
                     QStringLiteral("acct-one"), &error),
                 0);
        controller.shutdown();
    }
}

void TestAppController::disabledAccountDoesNotPoll()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppPaths paths = testPaths(dir);

    AppConfig config = defaultAppConfig();
    config.accounts = {
        account(QStringLiteral("acct-one"), QStringLiteral("one@example.com"), false)};
    QString error;
    QVERIFY2(saveConfig(paths, config, &error), qPrintable(error));

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{200, emptyPage()};
    };

    auto tokens = std::make_shared<FakeTokenProvider>();
    tokens->tokens.insert(QStringLiteral("acct-one"), QStringLiteral("test-token"));
    AppController controller(controllerOptions(paths, server), tokens);
    QVERIFY2(controller.initialize(&error), qPrintable(error));

    QVERIFY(!controller.scheduler()->isRunning());
    QVERIFY(!controller.isWaitingForAuthorization());
    QCOMPARE(server.requests.size(), 0);
    QCOMPARE(controller.stateDatabase()->accountStates().size(), 1);
    controller.shutdown();
}

void TestAppController::settingsWindowIsLazyAndStaysReopenable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppPaths paths = testPaths(dir);

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{200, emptyPage()};
    };

    auto tokens = std::make_shared<FakeTokenProvider>();
    AppController controller(controllerOptions(paths, server), tokens);
    QString error;
    QVERIFY2(controller.initialize(&error), qPrintable(error));
    QVERIFY(controller.settingsWindow() == nullptr);

    QVERIFY(controller.showSettings());
    SettingsWindow *window = controller.settingsWindow();
    QVERIFY(window);
    QVERIFY(window->isVisible());

    auto *interval = window->findChild<QSpinBox *>(QStringLiteral("pollIntervalSpin"));
    auto *add = window->findChild<QPushButton *>(QStringLiteral("addAccountButton"));
    QVERIFY(interval);
    QVERIFY(add);
    QSignalSpy addSpy(&controller, &AppController::addAccountRequested);
    add->click();
    QCOMPARE(addSpy.count(), 1);
    interval->setValue(60);
    QCOMPARE(controller.scheduler()->intervalMinutes(), 60);
    QCOMPARE(controller.configStore()->config().notifications.pollIntervalMinutes, 60);

    window->close();
    QVERIFY(!window->isVisible());
    QVERIFY(controller.showSettings());
    QCOMPARE(controller.settingsWindow(), window);
    QVERIFY(window->isVisible());

    controller.shutdown();
    QVERIFY(!window->isVisible());
}

void TestAppController::suspendAndResumeRestartsPolling()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppPaths paths = testPaths(dir);

    AppConfig config = defaultAppConfig();
    config.accounts = {account(QStringLiteral("acct-one"), QStringLiteral("one@example.com"))};
    QString error;
    QVERIFY2(saveConfig(paths, config, &error), qPrintable(error));

    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{200, emptyPage()};
    };

    auto tokens = std::make_shared<FakeTokenProvider>();
    tokens->tokens.insert(QStringLiteral("acct-one"), QStringLiteral("test-token"));
    AppController controller(controllerOptions(paths, server), tokens);
    QSignalSpy finished(controller.scheduler(), &SyncScheduler::checkFinished);
    QVERIFY2(controller.initialize(&error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(controller.scheduler()->nextCheckEpoch() > 0);

    controller.suspend();
    QVERIFY(controller.isSuspended());
    QVERIFY(!controller.scheduler()->isRunning());
    QCOMPARE(controller.scheduler()->nextCheckEpoch(), 0);

    controller.resume();
    QVERIFY(!controller.isSuspended());
    QVERIFY(controller.scheduler()->isRunning());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
    QCOMPARE(server.requests.size(), 2);
    controller.shutdown();
}

QTEST_MAIN(TestAppController)
#include "test_app_controller.moc"
