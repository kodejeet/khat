#include "auth/OAuthLoopbackServer.h"
#include "auth/OAuthPkce.h"
#include "auth/OAuthTokenClient.h"
#include "gmail/GmailClient.h"

#include "support/FakeGmailServer.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QUrlQuery>
#include <QtTest>

namespace {
OAuthClientConfig testClientConfig(const QUrl &baseUrl)
{
    OAuthClientConfig config = defaultOAuthClientConfig();
    config.clientId = QStringLiteral("test-client.apps.googleusercontent.com");
    config.clientSecret = QStringLiteral("test-secret");
    config.authUri = baseUrl.resolved(QUrl(QStringLiteral("/auth")));
    config.tokenUri = baseUrl.resolved(QUrl(QStringLiteral("/token")));
    config.revokeUri = baseUrl.resolved(QUrl(QStringLiteral("/revoke")));
    return config;
}

QUrlQuery formBody(const QByteArray &body)
{
    return QUrlQuery(QString::fromUtf8(body));
}
} // namespace

class TestOAuth : public QObject
{
    Q_OBJECT

private slots:
    void pkceMatchesRfc7636Vector();
    void authorizationUrlHasRequiredParameters();
    void loopbackServerReceivesCallback();
    void loopbackServerRejectsWrongPath();
    void tokenClientExchangeRefreshRevoke();
    void tokenClientMapsInvalidGrant();
    void gmailProfileFetchesEmail();
};

void TestOAuth::pkceMatchesRfc7636Vector()
{
    // RFC 7636 appendix B.
    QCOMPARE(OAuthPkce::codeChallenge(
                 QStringLiteral("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")),
             QStringLiteral("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));

    const OAuthPkce pkce = OAuthPkce::generate();
    QCOMPARE(pkce.verifier.size(), 43);
    QCOMPARE(pkce.state.size(), 43);
    QCOMPARE(pkce.challenge, OAuthPkce::codeChallenge(pkce.verifier));
    QVERIFY(pkce.challenge != pkce.verifier);
    QVERIFY(OAuthPkce::generate().verifier != pkce.verifier);
}

void TestOAuth::authorizationUrlHasRequiredParameters()
{
    OAuthClientConfig config = defaultOAuthClientConfig();
    config.clientId = QStringLiteral("desktop-client.apps.googleusercontent.com");
    config.clientSecret = QStringLiteral("do-not-leak-this");

    const OAuthPkce pkce = OAuthPkce::generate();
    const QUrl url = buildOAuthAuthorizationUrl(
        config, QStringLiteral("http://127.0.0.1:54321/callback"), pkce);
    const QUrlQuery query(url);

    QCOMPARE(url.host(), QStringLiteral("accounts.google.com"));
    QCOMPARE(query.queryItemValue(QStringLiteral("response_type")), QStringLiteral("code"));
    QCOMPARE(query.queryItemValue(QStringLiteral("client_id")), config.clientId);
    QCOMPARE(query.queryItemValue(QStringLiteral("redirect_uri")),
             QStringLiteral("http://127.0.0.1:54321/callback"));
    QCOMPARE(query.queryItemValue(QStringLiteral("scope")), config.scope);
    QCOMPARE(query.queryItemValue(QStringLiteral("code_challenge")), pkce.challenge);
    QCOMPARE(query.queryItemValue(QStringLiteral("code_challenge_method")),
             QStringLiteral("S256"));
    QCOMPARE(query.queryItemValue(QStringLiteral("state")), pkce.state);
    QCOMPARE(query.queryItemValue(QStringLiteral("access_type")), QStringLiteral("offline"));
    QCOMPARE(query.queryItemValue(QStringLiteral("prompt")), QStringLiteral("consent"));
    QVERIFY(!url.toString().contains(config.clientSecret));
    QVERIFY(!query.hasQueryItem(QStringLiteral("client_secret")));
}

void TestOAuth::loopbackServerReceivesCallback()
{
    OAuthLoopbackServer server;
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));
    QVERIFY(server.isListening());
    QVERIFY(server.port() > 0);
    QVERIFY(server.redirectUri().startsWith(
        QStringLiteral("http://127.0.0.1:%1/callback").arg(server.port())));

    QSignalSpy received(&server, &OAuthLoopbackServer::callbackReceived);
    QSignalSpy failed(&server, &OAuthLoopbackServer::failed);
    QNetworkAccessManager network;
    QNetworkReply *reply = network.get(QNetworkRequest(
        QUrl(server.redirectUri() + QStringLiteral("?code=test-code&state=test-state"))));
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 5000);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QVERIFY(reply->readAll().contains("khat"));
    reply->deleteLater();

    QTRY_COMPARE_WITH_TIMEOUT(received.count(), 1, 5000);
    QCOMPARE(received.first().at(0).toString(), QStringLiteral("test-code"));
    QCOMPARE(received.first().at(1).toString(), QStringLiteral("test-state"));
    QCOMPARE(failed.count(), 0);
    QVERIFY(!server.isListening());
}

void TestOAuth::loopbackServerRejectsWrongPath()
{
    OAuthLoopbackServer server;
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));
    QSignalSpy received(&server, &OAuthLoopbackServer::callbackReceived);
    QSignalSpy failed(&server, &OAuthLoopbackServer::failed);

    QNetworkAccessManager network;
    QNetworkReply *reply = network.get(
        QNetworkRequest(QUrl(server.redirectUri().replace(
            QStringLiteral("/callback"), QStringLiteral("/other")))));
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 5000);
    reply->deleteLater();

    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    QCOMPARE(received.count(), 0);
    QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("path")));
    QVERIFY(!server.isListening());
}

void TestOAuth::tokenClientExchangeRefreshRevoke()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    server.bodyHandler = [](const QUrl &url, const QByteArray &, const QByteArray &body) {
        const QUrlQuery form = formBody(body);
        if (url.path() == QStringLiteral("/token")) {
            if (form.queryItemValue(QStringLiteral("grant_type"))
                == QStringLiteral("authorization_code")) {
                return FakeGmailServer::Response{
                    200,
                    QByteArrayLiteral("{\"access_token\":\"access-one\","
                                      "\"refresh_token\":\"refresh-one\","
                                      "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                                      "\"scope\":\"https://www.googleapis.com/auth/"
                                      "gmail.readonly\"}")};
            }
            if (form.queryItemValue(QStringLiteral("grant_type"))
                == QStringLiteral("refresh_token")) {
                return FakeGmailServer::Response{
                    200,
                    QByteArrayLiteral("{\"access_token\":\"access-two\","
                                      "\"token_type\":\"Bearer\",\"expires_in\":3600}")};
            }
        }
        if (url.path() == QStringLiteral("/revoke")) {
            return FakeGmailServer::Response{200, QByteArrayLiteral("{}")};
        }
        return FakeGmailServer::Response{404, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager network;
    OAuthTokenClient client(&network);
    client.setClientConfig(testClientConfig(server.apiBaseUrl().resolved(QUrl("/"))));
    QSignalSpy received(&client, &OAuthTokenClient::tokenReceived);
    QSignalSpy revoked(&client, &OAuthTokenClient::tokenRevoked);
    QSignalSpy failed(&client, &OAuthTokenClient::requestFailed);

    client.exchangeAuthorizationCode(QStringLiteral("auth"), QStringLiteral("code-123"),
                                      QStringLiteral("verifier-456"),
                                      QStringLiteral("http://127.0.0.1:1234/callback"));
    QTRY_COMPARE_WITH_TIMEOUT(received.count(), 1, 5000);
    const auto token = received.first().at(1).value<OAuthToken>();
    QCOMPARE(token.accessToken, QStringLiteral("access-one"));
    QCOMPARE(token.refreshToken, QStringLiteral("refresh-one"));
    QCOMPARE(token.tokenType, QStringLiteral("Bearer"));
    QVERIFY(token.expiresAt.isValid());
    QVERIFY(token.scope.contains(QStringLiteral("gmail.readonly")));

    QCOMPARE(server.requests.size(), 1);
    const QUrlQuery exchangeForm = formBody(server.requests.first().body);
    QCOMPARE(exchangeForm.queryItemValue(QStringLiteral("grant_type")),
             QStringLiteral("authorization_code"));
    QCOMPARE(exchangeForm.queryItemValue(QStringLiteral("code")), QStringLiteral("code-123"));
    QCOMPARE(exchangeForm.queryItemValue(QStringLiteral("code_verifier")),
             QStringLiteral("verifier-456"));
    QCOMPARE(exchangeForm.queryItemValue(QStringLiteral("redirect_uri")),
             QStringLiteral("http://127.0.0.1:1234/callback"));
    QCOMPARE(exchangeForm.queryItemValue(QStringLiteral("client_id")),
             QStringLiteral("test-client.apps.googleusercontent.com"));
    QCOMPARE(exchangeForm.queryItemValue(QStringLiteral("client_secret")),
             QStringLiteral("test-secret"));
    QCOMPARE(server.requests.first().method, QByteArrayLiteral("POST"));

    client.refreshAccessToken(QStringLiteral("refresh"), QStringLiteral("refresh-one"));
    QTRY_COMPARE_WITH_TIMEOUT(received.count(), 2, 5000);
    const auto refreshed = received.at(1).at(1).value<OAuthToken>();
    QCOMPARE(refreshed.accessToken, QStringLiteral("access-two"));
    QVERIFY(refreshed.refreshToken.isEmpty());
    const QUrlQuery refreshForm = formBody(server.requests.at(1).body);
    QCOMPARE(refreshForm.queryItemValue(QStringLiteral("grant_type")),
             QStringLiteral("refresh_token"));
    QCOMPARE(refreshForm.queryItemValue(QStringLiteral("refresh_token")),
             QStringLiteral("refresh-one"));
    QVERIFY(!refreshForm.hasQueryItem(QStringLiteral("code_verifier")));

    client.revokeToken(QStringLiteral("revoke"), QStringLiteral("refresh-one"),
                       QStringLiteral("refresh_token"));
    QTRY_COMPARE_WITH_TIMEOUT(revoked.count(), 1, 5000);
    QCOMPARE(failed.count(), 0);
    const QUrlQuery revokeForm = formBody(server.requests.at(2).body);
    QCOMPARE(revokeForm.queryItemValue(QStringLiteral("token")),
             QStringLiteral("refresh-one"));
    QCOMPARE(revokeForm.queryItemValue(QStringLiteral("token_type_hint")),
             QStringLiteral("refresh_token"));
}

void TestOAuth::tokenClientMapsInvalidGrant()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &, const QByteArray &) {
        return FakeGmailServer::Response{
            400,
            QByteArrayLiteral("{\"error\":\"invalid_grant\","
                              "\"error_description\":\"Token has been expired or revoked.\"}")};
    };

    QNetworkAccessManager network;
    OAuthTokenClient client(&network);
    client.setClientConfig(testClientConfig(server.apiBaseUrl().resolved(QUrl("/"))));
    QSignalSpy received(&client, &OAuthTokenClient::tokenReceived);
    QSignalSpy failed(&client, &OAuthTokenClient::requestFailed);

    client.refreshAccessToken(QStringLiteral("refresh"), QStringLiteral("stale-refresh"));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    QCOMPARE(received.count(), 0);
    const auto error = failed.first().at(1).value<OAuthError>();
    QCOMPARE(error.kind, OAuthErrorKind::InvalidGrant);
    QCOMPARE(error.httpStatus, 400);
    QVERIFY(error.requiresReauthorization());
    QVERIFY(error.message.contains(QStringLiteral("Reauthorize")));
    QVERIFY(!error.message.contains(QStringLiteral("stale-refresh")));
}

void TestOAuth::gmailProfileFetchesEmail()
{
    FakeGmailServer server;
    QVERIFY(server.start());
    server.handler = [](const QUrl &url, const QByteArray &authorization) {
        if (url.path() == QStringLiteral("/gmail/v1/users/me/profile")
            && authorization == QByteArrayLiteral("Bearer profile-token")) {
            return FakeGmailServer::Response{
                200, QByteArrayLiteral("{\"emailAddress\":\"User@Example.com\","
                                       "\"historyId\":\"742\"}")};
        }
        return FakeGmailServer::Response{401, QByteArrayLiteral("{}")};
    };

    QNetworkAccessManager network;
    GmailClient client(&network);
    client.setApiBaseUrl(server.apiBaseUrl());
    client.setAccessToken(QStringLiteral("profile-token"));
    QSignalSpy ready(&client, &GmailClient::profileReady);
    QSignalSpy failed(&client, &GmailClient::requestFailed);

    client.fetchProfile();
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
    QCOMPARE(ready.first().at(0).toString(), QStringLiteral("User@Example.com"));
    QCOMPARE(ready.first().at(1).toLongLong(), qint64(742));
    QCOMPARE(failed.count(), 0);
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().url.query(),
             QStringLiteral("fields=emailAddress,historyId"));
}

QTEST_GUILESS_MAIN(TestOAuth)
#include "test_oauth.moc"
