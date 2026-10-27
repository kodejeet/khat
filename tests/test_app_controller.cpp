#include "app/AppController.h"
#include "auth/AccessTokenProvider.h"
#include "auth/OAuthManager.h"
#include "auth/OAuthTokenStore.h"
#include "ui/SettingsWindow.h"

#include "support/FakeGmailServer.h"

#include <QApplication>
#include <QDateTime>
#include <QHash>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QUrlQuery>
#include <QtTest>

#include <memory>

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

class FakeOAuthTokenStore final : public OAuthTokenStore
{
public:
    QHash<QString, OAuthToken> tokens;
    int removeCalls = 0;

    void load(const QString &accountId) override
    {
        const bool found = tokens.contains(accountId);
        const OAuthToken token = tokens.value(accountId);
        QTimer::singleShot(0, this, [this, accountId, found, token] {
            emit loaded(accountId, found, token, QString());
        });
    }

    void save(const QString &accountId, const OAuthToken &token) override
    {
        QTimer::singleShot(0, this, [this, accountId, token] {
            tokens.insert(accountId, token);
            emit saved(accountId, QString());
        });
    }

    void remove(const QString &accountId) override
    {
        ++removeCalls;
        QTimer::singleShot(0, this, [this, accountId] {
            tokens.remove(accountId);
            emit removed(accountId, QString());
        });
    }
};

OAuthToken storedToken(const QString &access, const QString &refresh)
{
    OAuthToken token;
    token.accessToken = access;
    token.refreshToken = refresh;
    token.tokenType = QStringLiteral("Bearer");
    token.scope = QStringLiteral("https://www.googleapis.com/auth/gmail.readonly");
    token.expiresAt = QDateTime::currentDateTimeUtc().addSecs(3600);
    return token;
}

OAuthClientConfig testOAuthClientConfig(const FakeGmailServer &server)
{
    OAuthClientConfig config = defaultOAuthClientConfig();
    config.clientId = QStringLiteral("controller-test.apps.googleusercontent.com");
    config.clientSecret = QStringLiteral("controller-test-secret");
    config.authUri = server.apiBaseUrl().resolved(QUrl(QStringLiteral("/auth")));
    config.tokenUri = server.apiBaseUrl().resolved(QUrl(QStringLiteral("/token")));
    config.revokeUri = server.apiBaseUrl().resolved(QUrl(QStringLiteral("/revoke")));
    return config;
}

QByteArray oauthTokenResponse(const QByteArray &access, const QByteArray &refresh)
{
    QByteArray json = QByteArrayLiteral("{\"access_token\":\"") + access
        + QByteArrayLiteral("\",\"token_type\":\"Bearer\",\"expires_in\":3600");
    if (!refresh.isEmpty()) {
        json += QByteArrayLiteral(",\"refresh_token\":\"") + refresh
            + QByteArrayLiteral("\"");
    }
    json += QByteArrayLiteral(",\"scope\":\"https://www.googleapis.com/auth/gmail.readonly\"}");
    return json;
}

QUrlQuery formBody(const QByteArray &body)
{
    return QUrlQuery(QString::fromUtf8(body));
}

void scheduleOAuthCallback(QNetworkAccessManager *network, const QUrl &authorizationUrl,
                           const QString &code)
{
    const QUrlQuery authQuery(authorizationUrl);
    QUrl callback(authQuery.queryItemValue(QStringLiteral("redirect_uri")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("code"), code);
    query.addQueryItem(QStringLiteral("state"),
                       authQuery.queryItemValue(QStringLiteral("state")));
    callback.setQuery(query);

    QTimer::singleShot(0, network, [network, callback] {
        QNetworkReply *reply = network->get(QNetworkRequest(callback));
        QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    });
}

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
    void oauthLoadsStoredTokenAndStartsPolling();
    void oauthAddAccountSavesConfigAndToken();
    void oauthUnauthorizedRefreshesToken();
    void oauthInvalidGrantRequiresReauthorization();
    void oauthAccountRemovalRevokesAndDeletesToken();
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

    QCOMPARE(server.syncRequestCount(), 1);
    QCOMPARE(server.syncRequests().first().authorization,
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
        QCOMPARE(server.syncRequestCount(), 1);

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
        QCOMPARE(server.syncRequestCount(), 2);

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
    QCOMPARE(server.syncRequestCount(), 1);
    QVERIFY(controller.scheduler()->nextCheckEpoch() > 0);

    controller.suspend();
    QVERIFY(controller.isSuspended());
    QVERIFY(!controller.scheduler()->isRunning());
    QCOMPARE(controller.scheduler()->nextCheckEpoch(), 0);

    controller.resume();
    QVERIFY(!controller.isSuspended());
    QVERIFY(controller.scheduler()->isRunning());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
    QCOMPARE(server.syncRequestCount(), 2);
    controller.shutdown();
}

void TestAppController::oauthLoadsStoredTokenAndStartsPolling()
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

    QNetworkAccessManager oauthNetwork;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    store->tokens.insert(QStringLiteral("acct-one"),
                         storedToken(QStringLiteral("stored-access"),
                                     QStringLiteral("stored-refresh")));
    auto manager =
        std::make_shared<OAuthManager>(std::move(storeOwner), &oauthNetwork);
    QVERIFY(manager->setOAuthClientConfig(testOAuthClientConfig(server)));

    AppController controller(controllerOptions(paths, server), {}, manager);
    QSignalSpy initial(manager.get(), &OAuthManager::initialTokenLoadFinished);
    QSignalSpy finished(controller.scheduler(), &SyncScheduler::checkFinished);
    QVERIFY2(controller.initialize(&error), qPrintable(error));
    QCOMPARE(server.requests.size(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(initial.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(controller.scheduler()->isRunning(), 5000);
    QVERIFY(!controller.isWaitingForAuthorization());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QCOMPARE(server.syncRequestCount(), 1);
    QCOMPARE(server.syncRequests().first().authorization,
             QByteArrayLiteral("Bearer stored-access"));
    QCOMPARE(store->tokens.value(QStringLiteral("acct-one")).accessToken,
             QStringLiteral("stored-access"));
    controller.shutdown();
}

void TestAppController::oauthAddAccountSavesConfigAndToken()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const AppPaths paths = testPaths(dir);

    FakeGmailServer server;
    QVERIFY(server.start());
    server.bodyHandler = [](const QUrl &url, const QByteArray &authorization,
                            const QByteArray &body) {
        if (url.path() == QStringLiteral("/token")) {
            const QUrlQuery form = formBody(body);
            if (form.queryItemValue(QStringLiteral("grant_type"))
                == QStringLiteral("authorization_code")) {
                return FakeGmailServer::Response{
                    200, oauthTokenResponse(QByteArrayLiteral("access-added"),
                                            QByteArrayLiteral("refresh-added"))};
            }
        }
        if (url.path() == QStringLiteral("/gmail/v1/users/me/profile")
            && authorization == QByteArrayLiteral("Bearer access-added")) {
            return FakeGmailServer::Response{
                200, QByteArrayLiteral("{\"emailAddress\":\"User@Example.com\"}")};
        }
        if (url.path().endsWith(QStringLiteral("/users/me/messages"))) {
            return FakeGmailServer::Response{200, emptyPage()};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager oauthNetwork;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    auto manager =
        std::make_shared<OAuthManager>(std::move(storeOwner), &oauthNetwork);
    QVERIFY(manager->setOAuthClientConfig(testOAuthClientConfig(server)));
    manager->setBrowserOpener([&oauthNetwork](const QUrl &url) {
        scheduleOAuthCallback(&oauthNetwork, url, QStringLiteral("add-code"));
        return true;
    });

    AppController controller(controllerOptions(paths, server), {}, manager);
    QString error;
    QSignalSpy initial(manager.get(), &OAuthManager::initialTokenLoadFinished);
    QSignalSpy authorized(manager.get(), &OAuthManager::authorizationFinished);
    QSignalSpy checked(controller.scheduler(), &SyncScheduler::checkFinished);
    QVERIFY2(controller.initialize(&error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(initial.count(), 1, 5000);
    QCOMPARE(controller.configStore()->config().accounts.size(), 0);

    QVERIFY(controller.showSettings());
    SettingsWindow *window = controller.settingsWindow();
    auto *add = window->findChild<QPushButton *>(QStringLiteral("addAccountButton"));
    auto *message = window->findChild<QLabel *>(QStringLiteral("accountMessage"));
    QVERIFY(add);
    QVERIFY(message);

    add->click();
    QVERIFY(!add->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(authorized.count(), 1, 5000);

    const QString expectedId =
        OAuthManager::accountIdForEmail(QStringLiteral("User@Example.com"));
    QTRY_COMPARE_WITH_TIMEOUT(controller.configStore()->config().accounts.size(), 1, 5000);
    QCOMPARE(controller.configStore()->config().accounts.first().id, expectedId);
    QCOMPARE(controller.configStore()->config().accounts.first().email,
             QStringLiteral("User@Example.com"));
    QCOMPARE(authorized.first().at(0).toString(), expectedId);
    QCOMPARE(authorized.first().at(2).toBool(), true);
    QVERIFY(add->isEnabled());
    QVERIFY(!message->isHidden());
    QVERIFY(message->text().contains(QStringLiteral("Added")));

    QTRY_VERIFY_WITH_TIMEOUT(store->tokens.contains(expectedId), 5000);
    QCOMPARE(store->tokens.value(expectedId).refreshToken,
             QStringLiteral("refresh-added"));
    QTRY_VERIFY_WITH_TIMEOUT(controller.scheduler()->isRunning(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 5000);

    ConfigStore reloaded(paths.configFile);
    QVERIFY2(reloaded.load(&error), qPrintable(error));
    QCOMPARE(reloaded.config().accounts.size(), 1);
    QCOMPARE(reloaded.config().accounts.first().id, expectedId);
    controller.shutdown();
}

void TestAppController::oauthUnauthorizedRefreshesToken()
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
    bool sawExpired = false;
    bool sawRefreshed = false;
    bool sawRefreshRequest = false;
    server.bodyHandler = [&](const QUrl &url, const QByteArray &authorization,
                             const QByteArray &body) {
        if (url.path() == QStringLiteral("/token")
            && formBody(body).queryItemValue(QStringLiteral("grant_type"))
                == QStringLiteral("refresh_token")) {
            sawRefreshRequest =
                formBody(body).queryItemValue(QStringLiteral("refresh_token"))
                == QStringLiteral("old-refresh");
            return FakeGmailServer::Response{
                200, oauthTokenResponse(QByteArrayLiteral("access-refreshed"), QByteArray())};
        }
        if (url.path().endsWith(QStringLiteral("/users/me/messages"))) {
            if (authorization == QByteArrayLiteral("Bearer old-access")) {
                sawExpired = true;
                return FakeGmailServer::Response{
                    401, QByteArrayLiteral("{\"error\":{\"errors\":[{\"reason\":"
                                           "\"authError\"}]}}")};
            }
            if (authorization == QByteArrayLiteral("Bearer access-refreshed")) {
                sawRefreshed = true;
                return FakeGmailServer::Response{200, emptyPage()};
            }
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager oauthNetwork;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    store->tokens.insert(QStringLiteral("acct-one"),
                         storedToken(QStringLiteral("old-access"),
                                     QStringLiteral("old-refresh")));
    auto manager =
        std::make_shared<OAuthManager>(std::move(storeOwner), &oauthNetwork);
    QVERIFY(manager->setOAuthClientConfig(testOAuthClientConfig(server)));

    AppController controller(controllerOptions(paths, server), {}, manager);
    QSignalSpy initial(manager.get(), &OAuthManager::initialTokenLoadFinished);
    QSignalSpy refreshed(manager.get(), &OAuthManager::tokenRefreshed);
    QSignalSpy required(manager.get(), &OAuthManager::authorizationRequired);
    QSignalSpy checked(controller.scheduler(), &SyncScheduler::checkFinished);
    QVERIFY2(controller.initialize(&error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(initial.count(), 1, 5000);

    QTRY_COMPARE_WITH_TIMEOUT(refreshed.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(sawExpired, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(sawRefreshRequest, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(sawRefreshed, 5000);
    QCOMPARE(required.count(), 0);
    QVERIFY(!controller.isWaitingForAuthorization());
    QTRY_COMPARE_WITH_TIMEOUT(
        store->tokens.value(QStringLiteral("acct-one")).accessToken,
        QStringLiteral("access-refreshed"), 5000);
    QCOMPARE(store->tokens.value(QStringLiteral("acct-one")).refreshToken,
             QStringLiteral("old-refresh"));
    QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 2, 5000);
    controller.shutdown();
}

void TestAppController::oauthInvalidGrantRequiresReauthorization()
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
    server.bodyHandler = [](const QUrl &url, const QByteArray &authorization,
                            const QByteArray &body) {
        if (url.path() == QStringLiteral("/token")
            && formBody(body).queryItemValue(QStringLiteral("grant_type"))
                == QStringLiteral("refresh_token")) {
            return FakeGmailServer::Response{400,
                                             QByteArrayLiteral("{\"error\":\"invalid_grant\"}")};
        }
        if (url.path().endsWith(QStringLiteral("/users/me/messages"))
            && authorization == QByteArrayLiteral("Bearer old-access")) {
            return FakeGmailServer::Response{401, QByteArrayLiteral("{}")};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager oauthNetwork;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    store->tokens.insert(QStringLiteral("acct-one"),
                         storedToken(QStringLiteral("old-access"),
                                     QStringLiteral("old-refresh")));
    auto manager =
        std::make_shared<OAuthManager>(std::move(storeOwner), &oauthNetwork);
    QVERIFY(manager->setOAuthClientConfig(testOAuthClientConfig(server)));

    AppController controller(controllerOptions(paths, server), {}, manager);
    QSignalSpy initial(manager.get(), &OAuthManager::initialTokenLoadFinished);
    QSignalSpy required(manager.get(), &OAuthManager::authorizationRequired);
    QVERIFY2(controller.initialize(&error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(initial.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(required.count(), 1, 5000);

    QVERIFY(!controller.scheduler()->isRunning());
    QVERIFY(controller.isWaitingForAuthorization());
    QTRY_VERIFY_WITH_TIMEOUT(!store->tokens.contains(QStringLiteral("acct-one")), 5000);
    QCOMPARE(controller.configStore()->config().accounts.size(), 1);

    AccountState state;
    QVERIFY2(controller.stateDatabase()->accountState(
                 QStringLiteral("acct-one"), &state, &error),
             qPrintable(error));
    QVERIFY(state.lastError.contains(QStringLiteral("Reauthorize")));

    QString token;
    QVERIFY(!manager->accessToken(QStringLiteral("acct-one"), &token));

    QVERIFY(controller.showSettings());
    auto *message = controller.settingsWindow()->findChild<QLabel *>(
        QStringLiteral("accountMessage"));
    QVERIFY(message);
    QVERIFY(!message->isHidden());
    QVERIFY(message->text().contains(QStringLiteral("Reauthorize")));
    controller.shutdown();
}

void TestAppController::oauthAccountRemovalRevokesAndDeletesToken()
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
    bool sawRevoke = false;
    server.bodyHandler = [&](const QUrl &url, const QByteArray &, const QByteArray &body) {
        if (url.path() == QStringLiteral("/revoke")) {
            sawRevoke =
                formBody(body).queryItemValue(QStringLiteral("token"))
                == QStringLiteral("stored-refresh");
            return FakeGmailServer::Response{200, QByteArrayLiteral("{}")};
        }
        if (url.path().endsWith(QStringLiteral("/users/me/messages"))) {
            return FakeGmailServer::Response{200, emptyPage()};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager oauthNetwork;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    store->tokens.insert(QStringLiteral("acct-one"),
                         storedToken(QStringLiteral("stored-access"),
                                     QStringLiteral("stored-refresh")));
    auto manager =
        std::make_shared<OAuthManager>(std::move(storeOwner), &oauthNetwork);
    QVERIFY(manager->setOAuthClientConfig(testOAuthClientConfig(server)));

    AppController controller(controllerOptions(paths, server), {}, manager);
    QSignalSpy initial(manager.get(), &OAuthManager::initialTokenLoadFinished);
    QSignalSpy removed(manager.get(), &OAuthManager::tokenRemoved);
    QSignalSpy checked(controller.scheduler(), &SyncScheduler::checkFinished);
    QVERIFY2(controller.initialize(&error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(initial.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(controller.scheduler()->isRunning(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 5000);

    QVERIFY(controller.showSettings());
    SettingsWindow *window = controller.settingsWindow();
    window->setRemovalConfirmation([](const QString &) { return true; });
    auto *tree = window->findChild<QTreeWidget *>(QStringLiteral("accountsTree"));
    auto *remove = window->findChild<QPushButton *>(QStringLiteral("removeAccountButton"));
    QVERIFY(tree);
    QVERIFY(remove);
    QCOMPARE(tree->topLevelItemCount(), 1);
    tree->setCurrentItem(tree->topLevelItem(0));
    remove->click();

    QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 5000);
    QVERIFY(sawRevoke);
    QTRY_VERIFY_WITH_TIMEOUT(!store->tokens.contains(QStringLiteral("acct-one")), 5000);
    QCOMPARE(store->removeCalls, 1);
    QCOMPARE(controller.configStore()->config().accounts.size(), 0);
    QVERIFY(!controller.scheduler()->isRunning());
    QString token;
    QVERIFY(!manager->accessToken(QStringLiteral("acct-one"), &token));
    controller.shutdown();
}

QTEST_MAIN(TestAppController)
#include "test_app_controller.moc"
