#include "auth/OAuthManager.h"

#include "auth/OAuthLoopbackServer.h"
#include "auth/OAuthTokenClient.h"
#include "auth/OAuthTokenStore.h"
#include "config/OAuthClientStore.h"
#include "gmail/GmailClient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

#include <utility>

namespace {
QString safeError(const QString &error)
{
    return error.simplified().left(300);
}

// Refresh slightly before a token's expiry so a scheduled check never starts
// with a token that is about to be rejected.
constexpr qint64 kRefreshAheadMs = 5 * 60 * 1000;
constexpr int kRefreshCooldownMs = 60 * 1000;
constexpr int kRefreshCooldownCapMs = 30 * 60 * 1000;

bool constantTimeEquals(const QString &left, const QString &right)
{
    const QByteArray a = left.toUtf8();
    const QByteArray b = right.toUtf8();
    unsigned char difference = static_cast<unsigned char>(a.size() != b.size());
    const int length = qMin(a.size(), b.size());
    for (int i = 0; i < length; ++i) {
        difference |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    }
    return difference == 0;
}

bool validEmail(const QString &email)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$"));
    return email.size() <= 320 && pattern.match(email).hasMatch();
}
} // namespace

OAuthManager::OAuthManager(std::unique_ptr<OAuthTokenStore> tokenStore,
                           QNetworkAccessManager *network, QObject *parent)
    : QObject(parent)
    , m_store(std::move(tokenStore))
    , m_network(network)
    , m_tokenClient(new OAuthTokenClient(network, this))
    , m_loopback(new OAuthLoopbackServer(this))
    , m_profileClient(nullptr)
    , m_gmailApiBaseUrl(QStringLiteral("https://gmail.googleapis.com/gmail/v1"))
    , m_clientConfig(defaultOAuthClientConfig())
{
    m_browserOpener = [](const QUrl &url) { return QDesktopServices::openUrl(url); };

    if (m_store) {
        connect(m_store.get(), &OAuthTokenStore::loaded, this,
                &OAuthManager::handleTokenLoaded);
        connect(m_store.get(), &OAuthTokenStore::saved, this,
                &OAuthManager::handleTokenSaved);
        connect(m_store.get(), &OAuthTokenStore::removed, this,
                &OAuthManager::handleTokenRemoved);
    }

    connect(m_tokenClient, &OAuthTokenClient::tokenReceived, this,
            &OAuthManager::handleTokenReceived);
    connect(m_tokenClient, &OAuthTokenClient::tokenRevoked, this,
            &OAuthManager::handleTokenRevoked);
    connect(m_tokenClient, &OAuthTokenClient::requestFailed, this,
            &OAuthManager::handleTokenRequestFailed);
    connect(m_loopback, &OAuthLoopbackServer::callbackReceived, this,
            &OAuthManager::handleLoopbackCallback);
    connect(m_loopback, &OAuthLoopbackServer::failed, this, [this](const QString &error) {
        if (m_flow) {
            failAuthorization(error);
        }
    });
}

OAuthManager::~OAuthManager()
{
    if (m_loopback) {
        m_loopback->stop();
    }
}

void OAuthManager::setOAuthClientPath(const QString &path)
{
    m_clientPath = path;
}

bool OAuthManager::setOAuthClientConfig(const OAuthClientConfig &config,
                                        QString *error)
{
    const QStringList errors = validateOAuthClientConfig(config);
    if (!errors.isEmpty()) {
        if (error) {
            *error = errors.join(QStringLiteral(" "));
        }
        return false;
    }
    m_clientConfig = config;
    m_clientConfigReady = true;
    m_clientConfigError.clear();
    m_tokenClient->setClientConfig(config);
    return true;
}

bool OAuthManager::loadOAuthClientConfig(QString *error)
{
    if (m_clientPath.isEmpty()) {
        const QString message =
            QStringLiteral("No OAuth client configuration file path is available.");
        m_clientConfigReady = false;
        m_clientConfigError = message;
        if (error) {
            *error = message;
        }
        return false;
    }

    OAuthClientStore store(m_clientPath);
    QString localError;
    if (!store.load(&localError)) {
        m_clientConfigReady = false;
        m_clientConfigError = safeError(localError);
        if (error) {
            *error = m_clientConfigError;
        }
        return false;
    }

    if (!setOAuthClientConfig(store.config(), &localError)) {
        m_clientConfigReady = false;
        m_clientConfigError = safeError(localError);
        if (error) {
            *error = m_clientConfigError;
        }
        return false;
    }
    return true;
}

bool OAuthManager::ensureOAuthClientConfig(QString *error)
{
    if (!m_clientPath.isEmpty()) {
        return loadOAuthClientConfig(error);
    }
    if (m_clientConfigReady) {
        return true;
    }
    const QString message = m_clientConfigError.isEmpty()
        ? QStringLiteral("OAuth client configuration is not available. See "
                         "config/oauth-client.example.json.")
        : m_clientConfigError;
    if (error) {
        *error = message;
    }
    return false;
}

void OAuthManager::setGmailApiBaseUrl(const QUrl &url)
{
    m_gmailApiBaseUrl = url;
    if (m_profileClient) {
        m_profileClient->setApiBaseUrl(url);
    }
}

void OAuthManager::setBrowserOpener(std::function<bool(const QUrl &)> opener)
{
    if (opener) {
        m_browserOpener = std::move(opener);
    } else {
        m_browserOpener = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
    }
}

void OAuthManager::setAccounts(const QList<AccountConfig> &accounts)
{
    m_accounts = accounts;
}

void OAuthManager::loadTokens()
{
    m_initialLoadActive = true;
    m_initialLoadFinished = false;
    m_initialLoadPending.clear();

    for (const AccountConfig &account : std::as_const(m_accounts)) {
        m_initialLoadPending.insert(account.id);
    }

    if (!m_store) {
        QTimer::singleShot(0, this, &OAuthManager::maybeFinishInitialTokenLoad);
        return;
    }

    for (const AccountConfig &account : std::as_const(m_accounts)) {
        if (m_loadingTokens.contains(account.id)) {
            continue;
        }
        m_loadingTokens.insert(account.id);
        m_store->load(account.id);
    }

    // Some test stores answer synchronously. Check on the next event-loop turn
    // so every initial load has had a chance to register.
    QTimer::singleShot(0, this, &OAuthManager::maybeFinishInitialTokenLoad);
}

void OAuthManager::maybeFinishInitialTokenLoad()
{
    if (!m_initialLoadActive || !m_initialLoadPending.isEmpty()) {
        return;
    }
    m_initialLoadActive = false;
    m_initialLoadFinished = true;
    emit initialTokenLoadFinished();
}

bool OAuthManager::accessToken(const QString &accountId, QString *token) const
{
    if (token) {
        token->clear();
    }
    if (m_needsReauthorization.contains(accountId)) {
        return false;
    }
    const auto it = m_tokens.constFind(accountId);
    if (it == m_tokens.cend() || !it->hasAccessToken()) {
        return false;
    }
    // Google access tokens live for about an hour. An expired token is never
    // handed out: housekeeping refreshes it proactively and the account simply
    // becomes usable when the refreshed token replaces the cache entry. This
    // avoids starting every session with a guaranteed HTTP 401.
    if (it->isAccessTokenExpired()) {
        return false;
    }
    if (token) {
        *token = it->accessToken;
    }
    return true;
}

bool OAuthManager::beginAddAccount()
{
    if (m_flow) {
        emit authorizationFailed(
            QStringLiteral("A Google authorization is already in progress."));
        return false;
    }
    startAuthorization(QString());
    return m_flow != nullptr;
}

bool OAuthManager::beginReauthorizeAccount(const QString &accountId)
{
    if (m_flow) {
        emit authorizationFailed(
            QStringLiteral("A Google authorization is already in progress."));
        return false;
    }
    if (!findAccountById(accountId)) {
        emit authorizationFailed(QStringLiteral("The account no longer exists."));
        return false;
    }
    startAuthorization(accountId);
    return m_flow != nullptr;
}

void OAuthManager::startAuthorization(const QString &reauthorizeAccountId)
{
    QString configError;
    if (!ensureOAuthClientConfig(&configError)) {
        emit authorizationFailed(configError);
        return;
    }

    QString serverError;
    if (!m_loopback->start(&serverError)) {
        emit authorizationFailed(serverError);
        return;
    }

    auto flow = std::make_unique<AuthorizationFlow>();
    flow->pkce = OAuthPkce::generate();
    flow->accountId = reauthorizeAccountId;
    if (!reauthorizeAccountId.isEmpty()) {
        const AccountConfig *account = findAccountById(reauthorizeAccountId);
        if (!account) {
            m_loopback->stop();
            emit authorizationFailed(QStringLiteral("The account no longer exists."));
            return;
        }
        flow->expectedEmail = account->email;
    }
    m_flow = std::move(flow);
    m_profileClient = new GmailClient(m_network, this);
    m_profileClient->setApiBaseUrl(m_gmailApiBaseUrl);
    connect(m_profileClient, &GmailClient::profileReady, this,
            &OAuthManager::handleProfileReady);
    connect(m_profileClient, &GmailClient::requestFailed, this,
            &OAuthManager::handleProfileError);

    const QString redirectUri = m_loopback->redirectUri();
    m_flow->redirectUri = redirectUri;
    const QUrl url =
        buildOAuthAuthorizationUrl(m_clientConfig, redirectUri, m_flow->pkce);

    emit authorizationStarted();
    emit authorizationBusyChanged(true);
    emit authorizationUrlReady(url);
    if (!m_browserOpener(url)) {
        failAuthorization(
            QStringLiteral("Could not open the browser for Google authorization."));
    }
}

void OAuthManager::cancelAuthorization()
{
    if (m_flow) {
        failAuthorization(QStringLiteral("Google authorization was cancelled."));
    }
}

void OAuthManager::handleLoopbackCallback(const QString &code, const QString &state)
{
    if (!m_flow || m_flow->phase != FlowPhase::WaitingForCallback) {
        return;
    }
    if (!constantTimeEquals(state, m_flow->pkce.state)) {
        failAuthorization(QStringLiteral("The Google authorization response failed its "
                                         "security state check. Try again."));
        return;
    }
    if (code.isEmpty()) {
        failAuthorization(
            QStringLiteral("Google did not return an authorization code."));
        return;
    }

    m_loopback->stop();
    const QString requestId = nextRequestId(QStringLiteral("auth"));
    RequestContext context;
    context.kind = RequestKind::AuthorizationCode;
    m_requests.insert(requestId, context);
    m_flow->phase = FlowPhase::ExchangingCode;
    m_tokenClient->exchangeAuthorizationCode(requestId, code, m_flow->pkce.verifier,
                                             m_flow->redirectUri);
}

void OAuthManager::handleTokenReceived(const QString &requestId, const OAuthToken &token)
{
    const auto request = m_requests.find(requestId);
    if (request == m_requests.end()) {
        return;
    }
    const RequestContext context = request.value();
    m_requests.erase(request);

    switch (context.kind) {
    case RequestKind::AuthorizationCode:
        if (!m_flow || m_flow->phase != FlowPhase::ExchangingCode) {
            return;
        }
        if (!token.hasAccessToken()) {
            failAuthorization(QStringLiteral("Google returned no usable access token."));
            return;
        }
        if (!token.hasRefreshToken()) {
            failAuthorization(
                QStringLiteral("Google did not return a refresh token. Remove the app's "
                               "access at myaccount.google.com/permissions, then try "
                               "again."));
            return;
        }
        if (!m_profileClient) {
            failAuthorization(QStringLiteral("The Gmail profile client is unavailable."));
            return;
        }
        m_flow->token = token;
        m_flow->phase = FlowPhase::FetchingProfile;
        m_profileClient->setApiBaseUrl(m_gmailApiBaseUrl);
        m_profileClient->setAccessToken(token.accessToken);
        m_profileClient->fetchProfile();
        break;
    case RequestKind::Refresh:
        refreshFinished(context.accountId, token);
        break;
    case RequestKind::Revocation:
        break;
    }
}

void OAuthManager::handleTokenRevoked(const QString &requestId)
{
    const auto request = m_requests.find(requestId);
    if (request == m_requests.end()) {
        return;
    }
    const RequestContext context = request.value();
    m_requests.erase(request);
    if (context.kind != RequestKind::Revocation) {
        return;
    }
    finishTokenRemoval(context.accountId, QString());
}

void OAuthManager::handleTokenRequestFailed(const QString &requestId,
                                            const OAuthError &error)
{
    const auto request = m_requests.find(requestId);
    if (request == m_requests.end()) {
        return;
    }
    const RequestContext context = request.value();
    m_requests.erase(request);

    switch (context.kind) {
    case RequestKind::AuthorizationCode:
        failAuthorization(safeError(error.message));
        break;
    case RequestKind::Refresh:
        refreshFailed(context.accountId, error);
        break;
    case RequestKind::Revocation:
        finishTokenRemoval(
            context.accountId,
            QStringLiteral("The Google access grant could not be revoked: %1")
                .arg(safeError(error.message)));
        break;
    }
}

void OAuthManager::handleProfileReady(const QString &emailAddress, qint64 historyId)
{
    Q_UNUSED(historyId)
    if (!m_flow || m_flow->phase != FlowPhase::FetchingProfile
        || sender() != m_profileClient) {
        return;
    }

    const QString email = emailAddress.trimmed();
    if (!validEmail(email)) {
        failAuthorization(
            QStringLiteral("Google returned an invalid Gmail profile address."));
        return;
    }

    if (!m_flow->accountId.isEmpty()) {
        const AccountConfig *account = findAccountById(m_flow->accountId);
        if (!account) {
            failAuthorization(QStringLiteral("The account was removed during authorization."));
            return;
        }
        if (normalizedEmail(email) != normalizedEmail(account->email)) {
            failAuthorization(
                QStringLiteral("Google authorized %1, but this account is %2. "
                               "Reauthorize and choose the correct Google account.")
                    .arg(email, account->email));
            return;
        }
        m_flow->newAccount = false;
    } else {
        const AccountConfig *existing = findAccountByEmail(email);
        m_flow->newAccount = existing == nullptr;
        m_flow->accountId =
            existing ? existing->id : accountIdForEmail(email);
        if (m_flow->accountId.isEmpty()) {
            failAuthorization(
                QStringLiteral("Could not derive an account identifier for %1.").arg(email));
            return;
        }
        if (!existing && findAccountById(m_flow->accountId)) {
            failAuthorization(
                QStringLiteral("The derived account identifier is already in use."));
            return;
        }
    }

    if (!m_flow->token.hasRefreshToken()) {
        failAuthorization(
            QStringLiteral("Google did not return a durable refresh token."));
        return;
    }
    if (!m_store) {
        failAuthorization(QStringLiteral("Secure token storage is unavailable."));
        return;
    }

    m_flow->email = email;
    m_flow->phase = FlowPhase::SavingToken;
    m_store->save(m_flow->accountId, m_flow->token);
}

void OAuthManager::handleProfileError(const GmailError &error)
{
    if (!m_flow || m_flow->phase != FlowPhase::FetchingProfile
        || sender() != m_profileClient) {
        return;
    }
    failAuthorization(
        QStringLiteral("Could not verify the Gmail account after authorization: %1")
            .arg(safeError(error.message)));
}

void OAuthManager::finishAuthorization()
{
    if (!m_flow) {
        return;
    }
    std::unique_ptr<AuthorizationFlow> flow = std::move(m_flow);
    m_loopback->stop();
    clearProfileClient();
    emit authorizationBusyChanged(false);
    emit authorizationFinished(flow->accountId, flow->email, flow->newAccount);
}

void OAuthManager::clearProfileClient()
{
    if (!m_profileClient) {
        return;
    }
    // Deferred deletion is safe when called from the client's own signal; the
    // sender check above makes any already-queued reply from the old client a
    // no-op for a later authorization flow.
    m_profileClient->deleteLater();
    m_profileClient = nullptr;
}

void OAuthManager::failAuthorization(const QString &error)
{
    if (!m_flow) {
        emit authorizationFailed(safeError(error));
        return;
    }
    m_flow.reset();
    m_loopback->stop();
    clearProfileClient();
    clearAuthorizationRequest();
    emit authorizationBusyChanged(false);
    emit authorizationFailed(safeError(error));
}

void OAuthManager::clearAuthorizationRequest()
{
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        if (it->kind == RequestKind::AuthorizationCode) {
            it = m_requests.erase(it);
        } else {
            ++it;
        }
    }
}

void OAuthManager::handleTokenLoaded(const QString &accountId, bool found,
                                     const OAuthToken &token, const QString &error)
{
    m_loadingTokens.remove(accountId);
    const bool awaitingRemoval = m_removalLoadPending.remove(accountId);
    m_initialLoadPending.remove(accountId);

    if (awaitingRemoval) {
        if (found && error.isEmpty()) {
            startCachedTokenRemoval(accountId, token);
        } else {
            deleteStoredToken(accountId, error.isEmpty() ? QString() : safeError(error));
        }
        maybeFinishInitialTokenLoad();
        return;
    }

    if (!findAccountById(accountId)) {
        maybeFinishInitialTokenLoad();
        return;
    }
    if (!error.isEmpty()) {
        emit tokenError(accountId, safeError(error));
        maybeFinishInitialTokenLoad();
        return;
    }
    if (!found) {
        m_tokens.remove(accountId);
        stopProactiveRefresh(accountId);
        m_needsReauthorization.remove(accountId);
        emit tokenCacheChanged();
        maybeFinishInitialTokenLoad();
        return;
    }

    m_tokens.insert(accountId, token);
    m_needsReauthorization.remove(accountId);
    // Refresh immediately instead of letting the first Gmail request die with
    // an HTTP 401: a token that is expired when it is loaded means the daemon
    // slept or restarted past the access-token lifetime.
    scheduleProactiveRefresh(accountId);
    emit tokenCacheChanged();
    // Cached tokens must be visible before the application treats initial
    // loading as complete and starts polling.
    maybeFinishInitialTokenLoad();
}

void OAuthManager::handleTokenSaved(const QString &accountId, const QString &error)
{
    const bool authorizationSave = m_flow && m_flow->phase == FlowPhase::SavingToken
        && m_flow->accountId == accountId;
    if (!error.isEmpty()) {
        if (authorizationSave) {
            failAuthorization(
                QStringLiteral("Could not store the account token securely: %1")
                    .arg(safeError(error)));
        } else {
            emit tokenStorageError(accountId, safeError(error));
        }
        return;
    }

    if (!authorizationSave) {
        return;
    }
    m_tokens.insert(accountId, m_flow->token);
    m_needsReauthorization.remove(accountId);
    scheduleProactiveRefresh(accountId);
    emit tokenCacheChanged();
    finishAuthorization();
}

void OAuthManager::handleTokenRemoved(const QString &accountId, const QString &error)
{
    const bool awaitingDelete = m_removalDeletePending.remove(accountId);
    if (!awaitingDelete) {
        if (!error.isEmpty()) {
            emit tokenStorageError(accountId, safeError(error));
        }
        return;
    }

    QString warning = m_removalWarnings.take(accountId);
    if (!error.isEmpty()) {
        const QString deleteError =
            QStringLiteral("The local Secret Service token could not be deleted: %1")
                .arg(safeError(error));
        warning = warning.isEmpty() ? deleteError
                                    : warning + QStringLiteral(" ") + deleteError;
    }
    m_tokens.remove(accountId);
    m_needsReauthorization.remove(accountId);
    emit tokenRemoved(accountId, warning);
}

void OAuthManager::scheduleProactiveRefresh(const QString &accountId, int retryDelayMs)
{
    QTimer *timer = m_refreshTimers.value(accountId, nullptr);
    if (!timer) {
        if (!findAccountById(accountId)) {
            return;
        }
        timer = new QTimer(this);
        timer->setSingleShot(true);
        const QString target = accountId;
        connect(timer, &QTimer::timeout, timer, [this, target] {
            refreshAccessToken(target);
        });
        m_refreshTimers.insert(accountId, timer);
    }

    // A reauthorization requirement or a removed token makes later requests
    // pointless; refresh only helps for a token that can still be replaced.
    if (m_needsReauthorization.contains(accountId)) {
        stopProactiveRefresh(accountId);
        return;
    }
    const OAuthToken token = m_tokens.value(accountId);
    if (!token.hasRefreshToken() || !token.hasAccessToken()) {
        stopProactiveRefresh(accountId);
        return;
    }
    if (!token.expiresAt.isValid()) {
        // Google always reports a lifetime for these tokens; if it is absent,
        // rely on the Gmail-401 driven refresh instead.
        stopProactiveRefresh(accountId);
        return;
    }

    qint64 delayMs = token.expiresAt.toMSecsSinceEpoch()
        - QDateTime::currentMSecsSinceEpoch() - kRefreshAheadMs;
    if (delayMs <= 0) {
        delayMs = retryDelayMs > 0 ? retryDelayMs : 0;
    }
    static const qint64 kMaxTimerMs = qint64(INT_MAX);
    timer->start(static_cast<int>(qBound<qint64>(0, delayMs, kMaxTimerMs)));
}

void OAuthManager::stopProactiveRefresh(const QString &accountId)
{
    m_refreshRetryDelayMs.remove(accountId);
    if (QTimer *timer = m_refreshTimers.take(accountId)) {
        timer->stop();
        timer->deleteLater();
    }
}

bool OAuthManager::refreshAccessToken(const QString &accountId)
{
    if (m_refreshingAccounts.contains(accountId)) {
        return false;
    }
    const auto it = m_tokens.constFind(accountId);
    if (it == m_tokens.cend() || !it->hasRefreshToken()) {
        markReauthorizationRequired(
            accountId, QStringLiteral("No refresh token is available. Reauthorize the "
                                      "account."));
        return false;
    }

    QString configError;
    if (!ensureOAuthClientConfig(&configError)) {
        emit tokenRefreshFailed(accountId, configError);
        return false;
    }

    const QString requestId = nextRequestId(QStringLiteral("refresh"));
    RequestContext context;
    context.kind = RequestKind::Refresh;
    context.accountId = accountId;
    m_requests.insert(requestId, context);
    m_refreshingAccounts.insert(accountId);
    m_tokenClient->refreshAccessToken(requestId, it->refreshToken);
    return true;
}

void OAuthManager::refreshFinished(const QString &accountId, const OAuthToken &token)
{
    m_refreshingAccounts.remove(accountId);
    if (!findAccountById(accountId)) {
        return;
    }

    const OAuthToken previous = m_tokens.value(accountId);
    OAuthToken updated = token;
    if (updated.refreshToken.isEmpty()) {
        updated.refreshToken = previous.refreshToken;
    }
    if (updated.scope.isEmpty()) {
        updated.scope = previous.scope;
    }
    if (!updated.hasAccessToken() || !updated.hasRefreshToken()) {
        markReauthorizationRequired(
            accountId, QStringLiteral("Google returned an incomplete refreshed token. "
                                      "Reauthorize the account."));
        return;
    }

    m_tokens.insert(accountId, updated);
    m_needsReauthorization.remove(accountId);
    m_refreshRetryDelayMs.remove(accountId);
    emit tokenCacheChanged();
    scheduleProactiveRefresh(accountId);
    if (m_store) {
        m_store->save(accountId, updated);
    }
    emit tokenRefreshed(accountId);
}

void OAuthManager::refreshFailed(const QString &accountId, const OAuthError &error)
{
    m_refreshingAccounts.remove(accountId);
    if (error.requiresReauthorization()) {
        markReauthorizationRequired(accountId, safeError(error.message));
        return;
    }
    // Retry the transparent refresh with a growing cooldown so a flaky network
    // cannot hammer the endpoint, and so the scheduler keeps the account usable
    // through its normal 401 fallback in the meantime.
    int delay = m_refreshRetryDelayMs.value(accountId, kRefreshCooldownMs) * 2;
    delay = qBound(kRefreshCooldownMs, delay, kRefreshCooldownCapMs);
    m_refreshRetryDelayMs.insert(accountId, delay);
    scheduleProactiveRefresh(accountId, delay);
    emit tokenRefreshFailed(accountId, safeError(error.message));
}

void OAuthManager::markReauthorizationRequired(const QString &accountId,
                                               const QString &error)
{
    m_refreshingAccounts.remove(accountId);
    m_tokens.remove(accountId);
    stopProactiveRefresh(accountId);
    m_needsReauthorization.insert(accountId);
    if (m_store) {
        m_store->remove(accountId);
    }
    emit tokenCacheChanged();
    emit authorizationRequired(accountId, safeError(error));
}

void OAuthManager::removeAccountToken(const QString &accountId)
{
    stopProactiveRefresh(accountId);
    if (m_flow && m_flow->accountId == accountId) {
        failAuthorization(
            QStringLiteral("Authorization was cancelled because the account was removed."));
    }
    // A refresh that is already in flight must not restore the token after it
    // has been removed. Dropping its request context makes the late reply a
    // no-op.
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        if (it->kind == RequestKind::Refresh && it->accountId == accountId) {
            it = m_requests.erase(it);
        } else {
            ++it;
        }
    }
    m_refreshingAccounts.remove(accountId);
    m_needsReauthorization.remove(accountId);

    const auto it = m_tokens.constFind(accountId);
    if (it != m_tokens.cend()) {
        const OAuthToken token = it.value();
        m_tokens.erase(it);
        emit tokenCacheChanged();
        startCachedTokenRemoval(accountId, token);
        return;
    }

    if (m_loadingTokens.contains(accountId)) {
        m_removalLoadPending.insert(accountId);
        return;
    }

    m_removalLoadPending.insert(accountId);
    if (!m_store) {
        m_removalLoadPending.remove(accountId);
        deleteStoredToken(accountId, QStringLiteral("Secure token storage is unavailable."));
        return;
    }
    m_loadingTokens.insert(accountId);
    m_store->load(accountId);
}

void OAuthManager::startCachedTokenRemoval(const QString &accountId,
                                           const OAuthToken &token)
{
    m_tokens.remove(accountId);
    stopProactiveRefresh(accountId);
    m_needsReauthorization.remove(accountId);
    emit tokenCacheChanged();

    if (!token.hasRefreshToken()) {
        deleteStoredToken(accountId, QString());
        return;
    }

    QString configError;
    if (!ensureOAuthClientConfig(&configError)) {
        deleteStoredToken(
            accountId,
            QStringLiteral("The Google access grant was not revoked because the OAuth "
                           "client configuration is unavailable: %1")
                .arg(configError));
        return;
    }

    const QString requestId = nextRequestId(QStringLiteral("revoke"));
    RequestContext context;
    context.kind = RequestKind::Revocation;
    context.accountId = accountId;
    m_requests.insert(requestId, context);
    m_tokenClient->revokeToken(requestId, token.refreshToken,
                               QStringLiteral("refresh_token"));
}

void OAuthManager::deleteStoredToken(const QString &accountId,
                                     const QString &warning)
{
    m_tokens.remove(accountId);
    stopProactiveRefresh(accountId);
    m_needsReauthorization.remove(accountId);
    emit tokenCacheChanged();
    if (!m_store) {
        emit tokenRemoved(accountId, warning);
        return;
    }
    m_removalDeletePending.insert(accountId);
    m_removalWarnings.insert(accountId, warning);
    m_store->remove(accountId);
}

void OAuthManager::finishTokenRemoval(const QString &accountId, QString warning)
{
    deleteStoredToken(accountId, warning);
}

QString OAuthManager::nextRequestId(const QString &prefix)
{
    return QStringLiteral("%1-%2").arg(prefix).arg(++m_requestCounter);
}

const AccountConfig *OAuthManager::findAccountByEmail(const QString &email) const
{
    const QString normalized = normalizedEmail(email);
    for (const AccountConfig &account : m_accounts) {
        if (normalizedEmail(account.email) == normalized) {
            return &account;
        }
    }
    return nullptr;
}

const AccountConfig *OAuthManager::findAccountById(const QString &accountId) const
{
    for (const AccountConfig &account : m_accounts) {
        if (account.id == accountId) {
            return &account;
        }
    }
    return nullptr;
}

QString OAuthManager::normalizedEmail(const QString &email)
{
    return email.trimmed().toCaseFolded();
}

QString OAuthManager::accountIdForEmail(const QString &email)
{
    const QByteArray digest = QCryptographicHash::hash(
        normalizedEmail(email).toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toHex());
}
