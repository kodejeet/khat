#pragma once

#include "auth/AccessTokenProvider.h"
#include "auth/OAuthPkce.h"
#include "auth/OAuthToken.h"
#include "config/AppConfig.h"
#include "config/OAuthClientConfig.h"
#include "gmail/GmailError.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QUrl>

#include <functional>
#include <memory>

class GmailClient;
class OAuthLoopbackServer;
class OAuthTokenClient;
class OAuthTokenStore;
class QNetworkAccessManager;

// Coordinates interactive Google authorization, the in-memory access-token
// cache, Secret Service persistence, profile verification, and background
// refresh. OAuth client configuration and tokens never appear in log messages.
class OAuthManager : public QObject, public AccessTokenProvider
{
    Q_OBJECT

public:
    explicit OAuthManager(std::unique_ptr<OAuthTokenStore> tokenStore,
                          QNetworkAccessManager *network, QObject *parent = nullptr);
    ~OAuthManager() override;

    void setOAuthClientPath(const QString &path);
    QString oauthClientPath() const { return m_clientPath; }

    // Tests and injected configurations can bypass the file. Returns false and
    // leaves the previous configuration untouched when validation fails.
    bool setOAuthClientConfig(const OAuthClientConfig &config, QString *error = nullptr);
    bool loadOAuthClientConfig(QString *error = nullptr);
    QString oauthClientConfigError() const { return m_clientConfigError; }

    void setGmailApiBaseUrl(const QUrl &url);
    void setBrowserOpener(std::function<bool(const QUrl &)> opener);

    // Existing configured accounts are used to resolve duplicate email
    // addresses during Add Account. Tokens for removed accounts stay cached
    // until removeAccountToken() completes so remote revocation can use them.
    void setAccounts(const QList<AccountConfig> &accounts);
    QList<AccountConfig> accounts() const { return m_accounts; }

    // Starts asynchronous Secret Service loads for all configured accounts.
    // initialTokenLoadFinished() is emitted after every load has settled.
    void loadTokens();

    bool accessToken(const QString &accountId, QString *token) const override;

    bool isAuthorizing() const { return m_flow != nullptr; }
    bool beginAddAccount();
    bool beginReauthorizeAccount(const QString &accountId);
    void cancelAuthorization();

    // Refreshes an expired/rejected access token using its stored refresh token.
    // Returns false for an unknown account, missing refresh token, or an
    // already-running refresh for that account.
    bool refreshAccessToken(const QString &accountId);

    // Revokes the Google grant when possible, then deletes the Secret Service
    // item. Local deletion still proceeds if revocation cannot be performed.
    void removeAccountToken(const QString &accountId);

    static QString normalizedEmail(const QString &email);
    static QString accountIdForEmail(const QString &email);

signals:
    void initialTokenLoadFinished();
    void tokenCacheChanged();
    void tokenError(const QString &accountId, const QString &error);
    void authorizationStarted();
    void authorizationBusyChanged(bool busy);
    // Emitted before opening the browser. Contains the PKCE challenge/state
    // query but never a token or client secret.
    void authorizationUrlReady(const QUrl &url);
    void authorizationFinished(const QString &accountId, const QString &email,
                               bool newAccount);
    void authorizationFailed(const QString &error);
    void authorizationRequired(const QString &accountId, const QString &error);
    void tokenRefreshed(const QString &accountId);
    void tokenRefreshFailed(const QString &accountId, const QString &error);
    void tokenStorageError(const QString &accountId, const QString &error);
    void tokenRemoved(const QString &accountId, const QString &warning);

private:
    enum class FlowPhase
    {
        WaitingForCallback,
        ExchangingCode,
        FetchingProfile,
        SavingToken,
    };

    struct AuthorizationFlow
    {
        FlowPhase phase = FlowPhase::WaitingForCallback;
        QString accountId;
        QString expectedEmail;
        QString email;
        QString redirectUri;
        OAuthPkce pkce;
        OAuthToken token;
        bool newAccount = false;
    };

    enum class RequestKind
    {
        AuthorizationCode,
        Refresh,
        Revocation,
    };

    struct RequestContext
    {
        RequestKind kind = RequestKind::AuthorizationCode;
        QString accountId;
    };

    bool ensureOAuthClientConfig(QString *error);
    void startAuthorization(const QString &reauthorizeAccountId);
    void handleLoopbackCallback(const QString &code, const QString &state);
    void handleTokenReceived(const QString &requestId, const OAuthToken &token);
    void handleTokenRevoked(const QString &requestId);
    void handleTokenRequestFailed(const QString &requestId, const OAuthError &error);
    void handleProfileReady(const QString &email);
    void handleProfileError(const GmailError &error);
    void handleTokenLoaded(const QString &accountId, bool found, const OAuthToken &token,
                           const QString &error);
    void handleTokenSaved(const QString &accountId, const QString &error);
    void handleTokenRemoved(const QString &accountId, const QString &error);
    void finishAuthorization();
    void failAuthorization(const QString &error);
    void clearProfileClient();
    void clearAuthorizationRequest();
    void refreshFinished(const QString &accountId, const OAuthToken &token);
    void refreshFailed(const QString &accountId, const OAuthError &error);
    void markReauthorizationRequired(const QString &accountId, const QString &error);
    void startCachedTokenRemoval(const QString &accountId, const OAuthToken &token);
    void deleteStoredToken(const QString &accountId, const QString &warning);
    void finishTokenRemoval(const QString &accountId, QString warning);
    QString nextRequestId(const QString &prefix);
    const AccountConfig *findAccountByEmail(const QString &email) const;
    const AccountConfig *findAccountById(const QString &accountId) const;
    void maybeFinishInitialTokenLoad();

    std::unique_ptr<OAuthTokenStore> m_store;
    QNetworkAccessManager *m_network;
    OAuthTokenClient *m_tokenClient;
    OAuthLoopbackServer *m_loopback;
    GmailClient *m_profileClient;
    QUrl m_gmailApiBaseUrl;
    QString m_clientPath;
    OAuthClientConfig m_clientConfig;
    bool m_clientConfigReady = false;
    QString m_clientConfigError;
    std::function<bool(const QUrl &)> m_browserOpener;

    QList<AccountConfig> m_accounts;
    QHash<QString, OAuthToken> m_tokens;
    QSet<QString> m_needsReauthorization;
    QSet<QString> m_loadingTokens;
    QSet<QString> m_initialLoadPending;
    bool m_initialLoadActive = false;
    bool m_initialLoadFinished = false;

    std::unique_ptr<AuthorizationFlow> m_flow;
    QHash<QString, RequestContext> m_requests;
    QSet<QString> m_refreshingAccounts;

    QSet<QString> m_removalLoadPending;
    QSet<QString> m_removalDeletePending;
    QHash<QString, QString> m_removalWarnings;
    quint64 m_requestCounter = 0;
};
