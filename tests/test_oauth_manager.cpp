#include "auth/OAuthManager.h"
#include "auth/OAuthTokenStore.h"

#include "support/FakeGmailServer.h"

#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest>

#include <memory>

namespace {
class FakeOAuthTokenStore : public OAuthTokenStore
{
public:
    QHash<QString, OAuthToken> tokens;
    bool failLoad = false;
    bool failSave = false;
    bool failRemove = false;
    int saveCalls = 0;
    int removeCalls = 0;

    void load(const QString &accountId) override
    {
        const bool found = tokens.contains(accountId);
        const OAuthToken token = tokens.value(accountId);
        const QString error = failLoad ? QStringLiteral("fake keyring unavailable") : QString();
        QTimer::singleShot(0, this, [this, accountId, found, token, error] {
            emit loaded(accountId, found, token, error);
        });
    }

    void save(const QString &accountId, const OAuthToken &token) override
    {
        ++saveCalls;
        const QString error = failSave ? QStringLiteral("fake keyring write failed") : QString();
        QTimer::singleShot(0, this, [this, accountId, token, error] {
            if (error.isEmpty()) {
                tokens.insert(accountId, token);
            }
            emit saved(accountId, error);
        });
    }

    void remove(const QString &accountId) override
    {
        ++removeCalls;
        const QString error =
            failRemove ? QStringLiteral("fake keyring delete failed") : QString();
        QTimer::singleShot(0, this, [this, accountId, error] {
            if (error.isEmpty()) {
                tokens.remove(accountId);
            }
            emit removed(accountId, error);
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

OAuthClientConfig testClientConfig(const QUrl &baseUrl)
{
    OAuthClientConfig config = defaultOAuthClientConfig();
    config.clientId = QStringLiteral("manager-test.apps.googleusercontent.com");
    config.clientSecret = QStringLiteral("manager-test-secret");
    config.authUri = baseUrl.resolved(QUrl(QStringLiteral("/auth")));
    config.tokenUri = baseUrl.resolved(QUrl(QStringLiteral("/token")));
    config.revokeUri = baseUrl.resolved(QUrl(QStringLiteral("/revoke")));
    return config;
}

QUrlQuery formBody(const QByteArray &body)
{
    return QUrlQuery(QString::fromUtf8(body));
}

AccountConfig account(const QString &id, const QString &email)
{
    return AccountConfig{id, email, QString(), true};
}

QByteArray tokenResponse(const QByteArray &access, const QByteArray &refresh)
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

// Lets a test drive the loopback callback asynchronously after the manager has
// opened its listener. No browser or network outside loopback is involved.
void scheduleCallback(QNetworkAccessManager *network, const QUrl &authorizationUrl,
                      const QString &code, const QString &stateOverride = QString())
{
    const QUrlQuery authQuery(authorizationUrl);
    const QString redirect = authQuery.queryItemValue(QStringLiteral("redirect_uri"));
    const QString state = stateOverride.isEmpty()
        ? authQuery.queryItemValue(QStringLiteral("state"))
        : stateOverride;
    QUrl callback(redirect);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("code"), code);
    query.addQueryItem(QStringLiteral("state"), state);
    callback.setQuery(query);

    QTimer::singleShot(0, network, [network, callback] {
        QNetworkReply *reply = network->get(QNetworkRequest(callback));
        QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    });
}
} // namespace

class TestOAuthManager : public QObject
{
    Q_OBJECT

private slots:
    void accountIdIsStableAndSafe();
    void loadsCachedTokensAndReportsMissing();
    void addsAccountAfterProfileVerification();
    void duplicateEmailReusesExistingAccount();
    void reauthorizeRejectsDifferentAccount();
    void refreshPreservesRefreshAndInvalidGrantRequiresReauth();
    void removalRevokesAndDeletesToken();
    void removalIgnoresInFlightRefresh();
    void callbackStateMismatchDoesNotExchangeCode();
};

void TestOAuthManager::accountIdIsStableAndSafe()
{
    const QString id = OAuthManager::accountIdForEmail(QStringLiteral("User@Example.com"));
    QCOMPARE(id.size(), 64);
    QVERIFY(QRegularExpression(QStringLiteral("^[A-Za-z0-9._-]+$")).match(id).hasMatch());
    QCOMPARE(id, OAuthManager::accountIdForEmail(QStringLiteral(" user@example.com ")));
    QVERIFY(id != OAuthManager::accountIdForEmail(QStringLiteral("other@example.com")));
    QCOMPARE(OAuthManager::normalizedEmail(QStringLiteral(" User@Example.com ")),
             QStringLiteral("user@example.com"));
}

void TestOAuthManager::loadsCachedTokensAndReportsMissing()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    QNetworkAccessManager network;

    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    store->tokens.insert(QStringLiteral("account-one"),
                         storedToken(QStringLiteral("cached-access"),
                                     QStringLiteral("cached-refresh")));
    OAuthManager manager(std::move(storeOwner), &network);
    QVERIFY(manager.setOAuthClientConfig(testClientConfig(server.apiBaseUrl().resolved(
        QUrl(QStringLiteral("/"))))));
    manager.setGmailApiBaseUrl(server.apiBaseUrl());
    manager.setAccounts({account(QStringLiteral("account-one"),
                                 QStringLiteral("one@example.com")),
                         account(QStringLiteral("account-two"),
                                 QStringLiteral("two@example.com"))});

    QSignalSpy initial(&manager, &OAuthManager::initialTokenLoadFinished);
    QSignalSpy cache(&manager, &OAuthManager::tokenCacheChanged);
    QSignalSpy errors(&manager, &OAuthManager::tokenError);
    manager.loadTokens();

    QTRY_COMPARE_WITH_TIMEOUT(initial.count(), 1, 5000);
    QVERIFY(cache.count() >= 1);
    QCOMPARE(errors.count(), 0);

    QString token;
    QVERIFY(manager.accessToken(QStringLiteral("account-one"), &token));
    QCOMPARE(token, QStringLiteral("cached-access"));
    QVERIFY(!manager.accessToken(QStringLiteral("account-two"), &token));
    QVERIFY(token.isEmpty());
    QVERIFY(!manager.accessToken(QStringLiteral("missing"), &token));
}

void TestOAuthManager::addsAccountAfterProfileVerification()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    server.bodyHandler = [](const QUrl &url, const QByteArray &authorization,
                            const QByteArray &body) {
        const QUrlQuery form = formBody(body);
        if (url.path() == QStringLiteral("/token")
            && form.queryItemValue(QStringLiteral("grant_type"))
                == QStringLiteral("authorization_code")) {
            return FakeGmailServer::Response{200,
                                             tokenResponse(QByteArrayLiteral("access-new"),
                                                           QByteArrayLiteral("refresh-new"))};
        }
        if (url.path() == QStringLiteral("/gmail/v1/users/me/profile")
            && authorization == QByteArrayLiteral("Bearer access-new")) {
            return FakeGmailServer::Response{
                200, QByteArrayLiteral("{\"emailAddress\":\"User@Example.com\"}")};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager network;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    OAuthManager manager(std::move(storeOwner), &network);
    QVERIFY(manager.setOAuthClientConfig(testClientConfig(server.apiBaseUrl().resolved(
        QUrl(QStringLiteral("/"))))));
    manager.setGmailApiBaseUrl(server.apiBaseUrl());

    QUrl authorizationUrl;
    manager.setBrowserOpener([&](const QUrl &url) {
        authorizationUrl = url;
        scheduleCallback(&network, url, QStringLiteral("fake-code"));
        return true;
    });

    QSignalSpy started(&manager, &OAuthManager::authorizationStarted);
    QSignalSpy busy(&manager, &OAuthManager::authorizationBusyChanged);
    QSignalSpy finished(&manager, &OAuthManager::authorizationFinished);
    QSignalSpy failed(&manager, &OAuthManager::authorizationFailed);
    QVERIFY(manager.beginAddAccount());
    QVERIFY(manager.isAuthorizing());
    QCOMPARE(started.count(), 1);

    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QCOMPARE(failed.count(), 0);
    QVERIFY(!manager.isAuthorizing());
    QCOMPARE(busy.count(), 2);
    QCOMPARE(busy.first().at(0).toBool(), true);
    QCOMPARE(busy.last().at(0).toBool(), false);

    const QString expectedId =
        OAuthManager::accountIdForEmail(QStringLiteral("User@Example.com"));
    QCOMPARE(finished.first().at(0).toString(), expectedId);
    QCOMPARE(finished.first().at(1).toString(), QStringLiteral("User@Example.com"));
    QCOMPARE(finished.first().at(2).toBool(), true);
    QVERIFY(store->tokens.contains(expectedId));
    QCOMPARE(store->tokens.value(expectedId).accessToken, QStringLiteral("access-new"));
    QCOMPARE(store->tokens.value(expectedId).refreshToken, QStringLiteral("refresh-new"));

    QString cached;
    QVERIFY(manager.accessToken(expectedId, &cached));
    QCOMPARE(cached, QStringLiteral("access-new"));

    const QUrlQuery authQuery(authorizationUrl);
    QCOMPARE(authQuery.queryItemValue(QStringLiteral("client_id")),
             QStringLiteral("manager-test.apps.googleusercontent.com"));
    QVERIFY(!authQuery.hasQueryItem(QStringLiteral("client_secret")));
    QVERIFY(!authorizationUrl.toString().contains(QStringLiteral("manager-test-secret")));

    bool sawExchange = false;
    bool sawProfile = false;
    for (const FakeGmailServer::Request &request : server.requests) {
        if (request.url.path() == QStringLiteral("/token")) {
            sawExchange = true;
            const QUrlQuery form = formBody(request.body);
            QCOMPARE(form.queryItemValue(QStringLiteral("code")),
                     QStringLiteral("fake-code"));
            QCOMPARE(form.queryItemValue(QStringLiteral("code_verifier")).size(), 43);
            QCOMPARE(form.queryItemValue(QStringLiteral("redirect_uri")),
                     authQuery.queryItemValue(QStringLiteral("redirect_uri")));
        } else if (request.url.path() == QStringLiteral("/gmail/v1/users/me/profile")) {
            sawProfile = true;
        }
    }
    QVERIFY(sawExchange);
    QVERIFY(sawProfile);
}

void TestOAuthManager::duplicateEmailReusesExistingAccount()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    server.bodyHandler = [](const QUrl &url, const QByteArray &,
                            const QByteArray &body) {
        if (url.path() == QStringLiteral("/token")
            && formBody(body).queryItemValue(QStringLiteral("grant_type"))
                == QStringLiteral("authorization_code")) {
            return FakeGmailServer::Response{
                200, tokenResponse(QByteArrayLiteral("access-duplicate"),
                                   QByteArrayLiteral("refresh-duplicate"))};
        }
        if (url.path() == QStringLiteral("/gmail/v1/users/me/profile")) {
            return FakeGmailServer::Response{
                200, QByteArrayLiteral("{\"emailAddress\":\"USER@example.com\"}")};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager network;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    OAuthManager manager(std::move(storeOwner), &network);
    QVERIFY(manager.setOAuthClientConfig(testClientConfig(server.apiBaseUrl().resolved(
        QUrl(QStringLiteral("/"))))));
    manager.setGmailApiBaseUrl(server.apiBaseUrl());
    manager.setAccounts({account(QStringLiteral("existing-id"),
                                 QStringLiteral("user@example.com"))});
    manager.setBrowserOpener([&](const QUrl &url) {
        scheduleCallback(&network, url, QStringLiteral("duplicate-code"));
        return true;
    });

    QSignalSpy finished(&manager, &OAuthManager::authorizationFinished);
    QSignalSpy failed(&manager, &OAuthManager::authorizationFailed);
    QVERIFY(manager.beginAddAccount());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(manager.accounts().size(), 1);
    QCOMPARE(finished.first().at(0).toString(), QStringLiteral("existing-id"));
    QCOMPARE(finished.first().at(2).toBool(), false);
    QVERIFY(store->tokens.contains(QStringLiteral("existing-id")));
}

void TestOAuthManager::reauthorizeRejectsDifferentAccount()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    server.bodyHandler = [](const QUrl &url, const QByteArray &,
                            const QByteArray &body) {
        if (url.path() == QStringLiteral("/token")
            && formBody(body).queryItemValue(QStringLiteral("grant_type"))
                == QStringLiteral("authorization_code")) {
            return FakeGmailServer::Response{
                200, tokenResponse(QByteArrayLiteral("access-wrong"),
                                   QByteArrayLiteral("refresh-wrong"))};
        }
        if (url.path() == QStringLiteral("/gmail/v1/users/me/profile")) {
            return FakeGmailServer::Response{
                200, QByteArrayLiteral("{\"emailAddress\":\"other@example.com\"}")};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager network;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    OAuthManager manager(std::move(storeOwner), &network);
    QVERIFY(manager.setOAuthClientConfig(testClientConfig(server.apiBaseUrl().resolved(
        QUrl(QStringLiteral("/"))))));
    manager.setGmailApiBaseUrl(server.apiBaseUrl());
    manager.setAccounts({account(QStringLiteral("account-one"),
                                 QStringLiteral("user@example.com"))});
    manager.setBrowserOpener([&](const QUrl &url) {
        scheduleCallback(&network, url, QStringLiteral("wrong-code"));
        return true;
    });

    QSignalSpy finished(&manager, &OAuthManager::authorizationFinished);
    QSignalSpy failed(&manager, &OAuthManager::authorizationFailed);
    QVERIFY(manager.beginReauthorizeAccount(QStringLiteral("account-one")));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    QCOMPARE(finished.count(), 0);
    QVERIFY(!manager.isAuthorizing());
    QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("other@example.com")));
    QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("user@example.com")));
    QCOMPARE(store->saveCalls, 0);
    QVERIFY(store->tokens.isEmpty());
}

void TestOAuthManager::refreshPreservesRefreshAndInvalidGrantRequiresReauth()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    bool invalidGrant = false;
    server.bodyHandler = [&invalidGrant](const QUrl &url, const QByteArray &,
                                         const QByteArray &body) {
        if (url.path() == QStringLiteral("/token")
            && formBody(body).queryItemValue(QStringLiteral("grant_type"))
                == QStringLiteral("refresh_token")) {
            if (invalidGrant) {
                return FakeGmailServer::Response{
                    400, QByteArrayLiteral("{\"error\":\"invalid_grant\"}")};
            }
            return FakeGmailServer::Response{
                200, tokenResponse(QByteArrayLiteral("access-refreshed"), QByteArray())};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager network;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    const QString accountId = QStringLiteral("account-one");
    store->tokens.insert(accountId,
                         storedToken(QStringLiteral("access-old"),
                                     QStringLiteral("refresh-old")));
    OAuthManager manager(std::move(storeOwner), &network);
    QVERIFY(manager.setOAuthClientConfig(testClientConfig(server.apiBaseUrl().resolved(
        QUrl(QStringLiteral("/"))))));
    manager.setGmailApiBaseUrl(server.apiBaseUrl());
    manager.setAccounts({account(accountId, QStringLiteral("user@example.com"))});
    QSignalSpy initial(&manager, &OAuthManager::initialTokenLoadFinished);
    manager.loadTokens();
    QTRY_COMPARE_WITH_TIMEOUT(initial.count(), 1, 5000);

    QSignalSpy refreshed(&manager, &OAuthManager::tokenRefreshed);
    QSignalSpy refreshFailed(&manager, &OAuthManager::tokenRefreshFailed);
    QSignalSpy required(&manager, &OAuthManager::authorizationRequired);
    QVERIFY(manager.refreshAccessToken(accountId));
    QTRY_COMPARE_WITH_TIMEOUT(refreshed.count(), 1, 5000);

    QString token;
    QVERIFY(manager.accessToken(accountId, &token));
    QCOMPARE(token, QStringLiteral("access-refreshed"));
    QTRY_VERIFY_WITH_TIMEOUT(store->tokens.value(accountId).accessToken
                                 == QStringLiteral("access-refreshed"),
                             5000);
    QCOMPARE(store->tokens.value(accountId).refreshToken,
             QStringLiteral("refresh-old"));

    const QUrlQuery refreshForm = formBody(server.requests.first().body);
    QCOMPARE(refreshForm.queryItemValue(QStringLiteral("refresh_token")),
             QStringLiteral("refresh-old"));

    invalidGrant = true;
    const int requestsBefore = server.requests.size();
    QVERIFY(manager.refreshAccessToken(accountId));
    QTRY_COMPARE_WITH_TIMEOUT(required.count(), 1, 5000);
    QCOMPARE(refreshFailed.count(), 0);
    QCOMPARE(server.requests.size(), requestsBefore + 1);
    QVERIFY(!manager.accessToken(accountId, &token));
    QVERIFY(required.first().at(1).toString().contains(QStringLiteral("Reauthorize")));
    QTRY_VERIFY_WITH_TIMEOUT(!store->tokens.contains(accountId), 5000);
}

void TestOAuthManager::removalRevokesAndDeletesToken()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    bool sawRevoke = false;
    server.bodyHandler = [&sawRevoke](const QUrl &url, const QByteArray &,
                                      const QByteArray &body) {
        if (url.path() == QStringLiteral("/revoke")) {
            const QUrlQuery form = formBody(body);
            sawRevoke = form.queryItemValue(QStringLiteral("token"))
                    == QStringLiteral("refresh-old")
                && form.queryItemValue(QStringLiteral("token_type_hint"))
                    == QStringLiteral("refresh_token");
            return FakeGmailServer::Response{200, QByteArrayLiteral("{}")};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager network;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    const QString accountId = QStringLiteral("account-one");
    store->tokens.insert(accountId,
                         storedToken(QStringLiteral("access-old"),
                                     QStringLiteral("refresh-old")));
    OAuthManager manager(std::move(storeOwner), &network);
    QVERIFY(manager.setOAuthClientConfig(testClientConfig(server.apiBaseUrl().resolved(
        QUrl(QStringLiteral("/"))))));
    manager.setGmailApiBaseUrl(server.apiBaseUrl());
    manager.setAccounts({account(accountId, QStringLiteral("user@example.com"))});
    QSignalSpy initial(&manager, &OAuthManager::initialTokenLoadFinished);
    manager.loadTokens();
    QTRY_COMPARE_WITH_TIMEOUT(initial.count(), 1, 5000);

    QSignalSpy removed(&manager, &OAuthManager::tokenRemoved);
    manager.removeAccountToken(accountId);
    QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 5000);
    QVERIFY(sawRevoke);
    QVERIFY(removed.first().at(1).toString().isEmpty());
    QString token;
    QVERIFY(!manager.accessToken(accountId, &token));
    QVERIFY(!store->tokens.contains(accountId));
}

void TestOAuthManager::removalIgnoresInFlightRefresh()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    server.bodyHandler = [](const QUrl &url, const QByteArray &,
                            const QByteArray &body) {
        if (url.path() == QStringLiteral("/token")
            && formBody(body).queryItemValue(QStringLiteral("grant_type"))
                == QStringLiteral("refresh_token")) {
            return FakeGmailServer::Response{
                200, tokenResponse(QByteArrayLiteral("access-late"), QByteArray())};
        }
        if (url.path() == QStringLiteral("/revoke")) {
            return FakeGmailServer::Response{200, QByteArrayLiteral("{}")};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager network;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    auto *store = storeOwner.get();
    const QString accountId = QStringLiteral("account-one");
    store->tokens.insert(accountId,
                         storedToken(QStringLiteral("access-old"),
                                     QStringLiteral("refresh-old")));
    OAuthManager manager(std::move(storeOwner), &network);
    QVERIFY(manager.setOAuthClientConfig(testClientConfig(server.apiBaseUrl().resolved(
        QUrl(QStringLiteral("/"))))));
    manager.setGmailApiBaseUrl(server.apiBaseUrl());
    manager.setAccounts({account(accountId, QStringLiteral("user@example.com"))});
    QSignalSpy initial(&manager, &OAuthManager::initialTokenLoadFinished);
    manager.loadTokens();
    QTRY_COMPARE_WITH_TIMEOUT(initial.count(), 1, 5000);

    QSignalSpy refreshed(&manager, &OAuthManager::tokenRefreshed);
    QSignalSpy removed(&manager, &OAuthManager::tokenRemoved);
    QVERIFY(manager.refreshAccessToken(accountId));
    manager.removeAccountToken(accountId);

    QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 5000);
    QCOMPARE(refreshed.count(), 0);
    QVERIFY(removed.first().at(1).toString().isEmpty());
    QString token;
    QVERIFY(!manager.accessToken(accountId, &token));
    QVERIFY(!store->tokens.contains(accountId));
}

void TestOAuthManager::callbackStateMismatchDoesNotExchangeCode()
{
    FakeGmailServer server;
    QVERIFY(server.start());

    QNetworkAccessManager network;
    auto storeOwner = std::make_unique<FakeOAuthTokenStore>();
    OAuthManager manager(std::move(storeOwner), &network);
    QVERIFY(manager.setOAuthClientConfig(testClientConfig(server.apiBaseUrl().resolved(
        QUrl(QStringLiteral("/"))))));
    manager.setGmailApiBaseUrl(server.apiBaseUrl());
    manager.setBrowserOpener([&](const QUrl &url) {
        scheduleCallback(&network, url, QStringLiteral("unused-code"),
                         QStringLiteral("wrong-state"));
        return true;
    });

    QSignalSpy failed(&manager, &OAuthManager::authorizationFailed);
    QSignalSpy finished(&manager, &OAuthManager::authorizationFinished);
    QVERIFY(manager.beginAddAccount());
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    QCOMPARE(finished.count(), 0);
    QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("state")));
    QCOMPARE(server.requests.size(), 0);
    QVERIFY(!manager.isAuthorizing());
}

QTEST_GUILESS_MAIN(TestOAuthManager)
#include "test_oauth_manager.moc"
