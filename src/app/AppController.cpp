#include "app/AppController.h"

#include "app/PowerMonitor.h"
#include "auth/AccessTokenProvider.h"
#include "auth/OAuthManager.h"
#include "auth/SecretServiceTokenStore.h"
#include "notify/FreedesktopNotificationBackend.h"
#include "notify/NotificationBackend.h"
#include "notify/NotificationDispatcher.h"
#include "notify/NotificationService.h"
#include "ui/SettingsWindow.h"

#include <QDateTime>
#include <QDebug>

#include <utility>

namespace {
bool sameAccountRequests(const QList<SyncAccountRequest> &left,
                         const QList<SyncAccountRequest> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (int i = 0; i < left.size(); ++i) {
        const SyncAccountRequest &a = left.at(i);
        const SyncAccountRequest &b = right.at(i);
        if (a.accountId != b.accountId || a.email != b.email
            || a.accessToken != b.accessToken || a.includeUpdates != b.includeUpdates) {
            return false;
        }
    }
    return true;
}
} // namespace

AppController::AppController(
    AppControllerOptions options,
    std::shared_ptr<AccessTokenProvider> tokenProvider,
    std::shared_ptr<OAuthManager> oauthManager,
    QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_config(m_options.paths.configFile)
    , m_state(m_options.paths.stateFile)
    , m_sync(&m_state, &m_network)
    , m_oauthManager(std::move(oauthManager))
    , m_tokenProvider(std::move(tokenProvider))
    , m_scheduler(&m_sync)
{
    if (!m_tokenProvider) {
        if (!m_oauthManager) {
            m_oauthManager = std::make_shared<OAuthManager>(
                std::make_unique<SecretServiceTokenStore>(), &m_network);
            m_oauthManager->setOAuthClientPath(m_options.paths.oauthClientFile);
        }
        m_oauthManager->setGmailApiBaseUrl(m_options.gmailApiBaseUrl);
        m_tokenProvider = m_oauthManager;
    } else {
        m_oauthManager.reset();
    }

    m_notificationBackend = std::make_unique<FreedesktopNotificationBackend>();
    m_notificationService =
        std::make_unique<NotificationService>(m_notificationBackend.get());
    m_dispatcher = std::make_unique<NotificationDispatcher>(
        &m_sync, m_notificationService.get(), &m_state);

    connect(m_notificationService.get(), &NotificationService::notificationFailed,
            this, [](const QString &error) {
                qWarning().noquote() << "Notification delivery failed:" << error;
            });
    connect(m_notificationService.get(), &NotificationService::urlOpenFailed, this,
            [] {
                qWarning().noquote()
                    << "Could not open the Gmail message in a browser.";
            });
    connect(m_dispatcher.get(), &NotificationDispatcher::notificationFailed, this,
            [](const QString &, const QString &, const QString &error) {
                qWarning().noquote() << "Notification could not be prepared:" << error;
            });
}

AppController::~AppController()
{
    shutdown();
    delete m_settingsWindow;
    m_settingsWindow = nullptr;
}

bool AppController::initialize(QString *error)
{
    if (m_initialized) {
        return true;
    }
    if (m_options.paths.configFile.isEmpty() || m_options.paths.stateFile.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Application configuration and state paths are required.");
        }
        return false;
    }

    QString localError;
    if (!m_config.load(&localError)) {
        if (error) {
            *error = QStringLiteral("Cannot load configuration: %1").arg(localError);
        }
        return false;
    }
    if (!m_state.open(&localError)) {
        if (error) {
            *error = localError;
        }
        return false;
    }

    // A missing network-information backend is not fatal: requests fail with
    // normal backoff and users can still use manual checks.
    if (!m_connectivity.initialize()) {
        qInfo().noquote()
            << "Network reachability information is unavailable; checks will fail open.";
    }

    m_sync.setApiBaseUrl(m_options.gmailApiBaseUrl);
    m_scheduler.setConnectivityMonitor(&m_connectivity);
    m_dispatcher->setConfig(m_config.config().notifications);

    connect(&m_scheduler, &SyncScheduler::checkFinished, this, [this] {
        if (m_suspended) {
            if (m_scheduler.isRunning() && !m_scheduler.isCheckRunning()) {
                m_scheduler.stop();
            }
            return;
        }
        if (m_checkAfterCurrent && !m_accountRequests.isEmpty()
            && !m_scheduler.isCheckRunning()) {
            m_checkAfterCurrent = false;
            m_scheduler.checkNow();
        }
    });

    setupOAuth();
    setupSleepMonitoring();

    m_initialized = true;
    applyConfiguration(m_config.config());

    if (m_oauthManager) {
        // Do not start polling until every configured account's stored token
        // has had a chance to load from Secret Service.
        m_oauthInitialLoadPending = true;
        m_oauthManager->loadTokens();
    } else {
        logAccountState();
    }
    return true;
}

void AppController::shutdown()
{
    if (!m_initialized) {
        return;
    }
    m_scheduler.stop();
    if (m_settingsWindow) {
        m_settingsWindow->hide();
    }
    m_initialized = false;
}

void AppController::reloadAccounts()
{
    if (m_initialized) {
        applyConfiguration(m_config.config());
    }
}

void AppController::setupSleepMonitoring()
{
    if (!m_options.subscribeToSleep) {
        return;
    }
    m_powerMonitor = std::make_unique<PowerMonitor>();
    connect(m_powerMonitor.get(), &PowerMonitor::aboutToSuspend, this,
            &AppController::suspend);
    connect(m_powerMonitor.get(), &PowerMonitor::resumed, this, &AppController::resume);

    QString sleepError;
    if (!m_powerMonitor->subscribe(&sleepError)) {
        qInfo().noquote() << "Suspend/resume monitoring is unavailable:" << sleepError;
        m_powerMonitor.reset();
    }
}

void AppController::setupOAuth()
{
    if (!m_oauthManager || m_oauthConnected) {
        return;
    }
    m_oauthConnected = true;
    OAuthManager *manager = m_oauthManager.get();

    connect(manager, &OAuthManager::initialTokenLoadFinished, this, [this] {
        if (!m_oauthInitialLoadPending) {
            return;
        }
        m_oauthInitialLoadPending = false;
        applyConfiguration(m_config.config());
        logAccountState();
    });
    connect(manager, &OAuthManager::tokenCacheChanged, this, [this] {
        if (m_initialized && !m_oauthInitialLoadPending) {
            applyConfiguration(m_config.config());
        }
    });
    connect(manager, &OAuthManager::tokenError, this,
            [this](const QString &accountId, const QString &error) {
                const QString label = emailForAccount(accountId);
                qWarning().noquote()
                    << "Could not load the saved Google authorization for"
                    << (label.isEmpty() ? accountId : label) << ":" << error;
                showAccountMessage(
                    QStringLiteral("Could not load the saved Google authorization for %1: %2")
                        .arg(label.isEmpty() ? accountId : label, error),
                    true);
            });
    connect(manager, &OAuthManager::authorizationStarted, this, [this] {
        showAccountMessage(
            QStringLiteral("Waiting for Google authorization in your browser..."));
    });
    connect(manager, &OAuthManager::authorizationBusyChanged, this, [this](bool busy) {
        if (m_settingsWindow) {
            m_settingsWindow->setAuthorizationBusy(busy);
        }
    });
    connect(manager, &OAuthManager::authorizationFinished, this,
            &AppController::handleAuthorizationFinished);
    connect(manager, &OAuthManager::authorizationFailed, this,
            &AppController::handleAuthorizationFailed);
    connect(manager, &OAuthManager::authorizationRequired, this,
            &AppController::handleAuthorizationRequired);
    connect(manager, &OAuthManager::tokenRefreshFailed, this,
            [this](const QString &accountId, const QString &error) {
                const QString label = emailForAccount(accountId);
                qWarning().noquote() << "Could not refresh Google authorization for"
                                     << (label.isEmpty() ? accountId : label) << ":"
                                     << error;
                showAccountMessage(
                    QStringLiteral("Could not refresh Google authorization for %1: %2. "
                                   "The next check will retry.")
                        .arg(label.isEmpty() ? accountId : label, error),
                    true);
            });
    connect(manager, &OAuthManager::tokenStorageError, this,
            [this](const QString &accountId, const QString &error) {
                const QString label = emailForAccount(accountId);
                qWarning().noquote() << "Secure token storage error for"
                                     << (label.isEmpty() ? accountId : label) << ":"
                                     << error;
                showAccountMessage(
                    QStringLiteral("Secure token storage reported an error for %1: %2")
                        .arg(label.isEmpty() ? accountId : label, error),
                    true);
            });
    connect(manager, &OAuthManager::tokenRemoved, this,
            [this](const QString &, const QString &warning) {
                if (warning.isEmpty()) {
                    showAccountMessage(QStringLiteral("Stored Google authorization removed."));
                } else {
                    qWarning().noquote() << "Token removal warning:" << warning;
                    showAccountMessage(
                        QStringLiteral("The local Google authorization was removed. %1")
                            .arg(warning),
                        true);
                }
                if (m_settingsWindow) {
                    m_settingsWindow->refresh();
                }
            });
    connect(&m_sync, &SyncService::syncFinished, this,
            &AppController::handleSyncFinished);
}

void AppController::logAccountState()
{
    if (m_accountRequests.isEmpty() && m_accountsAwaitingAuthorization > 0) {
        qInfo().noquote()
            << "Accounts are configured but not authorized; background checks are idle.";
    } else if (m_accountRequests.isEmpty() && m_config.config().accounts.isEmpty()) {
        qInfo().noquote() << "No accounts are configured; background checks are idle.";
    } else if (m_accountRequests.isEmpty()) {
        qInfo().noquote()
            << "No enabled accounts are authorized; background checks are idle.";
    }
}

void AppController::requestAddAccount()
{
    emit addAccountRequested();
    if (!m_oauthManager) {
        showAccountMessage(
            QStringLiteral("Google authorization is not available in this instance."), true);
        return;
    }
    if (m_oauthManager->beginAddAccount()) {
        showAccountMessage(
            QStringLiteral("Waiting for Google authorization in your browser..."));
    }
}

void AppController::requestReauthorizeAccount(const QString &accountId)
{
    emit reauthorizeAccountRequested(accountId);
    if (!m_oauthManager) {
        showAccountMessage(
            QStringLiteral("Google authorization is not available in this instance."), true);
        return;
    }
    if (m_oauthManager->beginReauthorizeAccount(accountId)) {
        showAccountMessage(
            QStringLiteral("Waiting for Google authorization in your browser..."));
    }
}

void AppController::handleAccountRemoval(const QString &accountId)
{
    if (m_oauthManager) {
        m_oauthManager->setAccounts(m_config.config().accounts);
        m_oauthManager->removeAccountToken(accountId);
    }
    emit accountRemoved(accountId);
}

void AppController::handleAuthorizationFinished(const QString &accountId,
                                                const QString &email,
                                                bool newAccount)
{
    Q_UNUSED(newAccount)

    AppConfig config = m_config.config();
    int existing = -1;
    for (int i = 0; i < config.accounts.size(); ++i) {
        if (config.accounts.at(i).id == accountId) {
            existing = i;
            break;
        }
    }
    const bool added = existing < 0;
    if (added) {
        config.accounts.append(AccountConfig{accountId, email, QString(), true});
    }

    QString error;
    if (!m_config.save(config, &error)) {
        qWarning().noquote() << "Could not save the authorized account:" << error;
        if (added && m_oauthManager) {
            // Do not leave a stored token behind when its account could not be
            // written to the configuration.
            m_oauthManager->removeAccountToken(accountId);
        }
        showAccountMessage(
            QStringLiteral("Google authorization succeeded, but the account could not be "
                           "saved: %1")
                .arg(error),
            true);
        return;
    }

    if (m_oauthManager) {
        m_oauthManager->setAccounts(config.accounts);
    }
    QString stateError;
    if (!m_state.ensureAccount(accountId, email, &stateError)) {
        qWarning().noquote() << "Cannot initialize local state for the account:"
                             << stateError;
    }
    applyConfiguration(config);
    if (m_settingsWindow) {
        m_settingsWindow->refresh();
    }
    if (added) {
        showAccountMessage(QStringLiteral("Added %1. The first synchronization runs "
                                          "without notifications.")
                               .arg(email));
    } else {
        showAccountMessage(QStringLiteral("Reauthorized %1.").arg(email));
    }
}

void AppController::handleAuthorizationFailed(const QString &error)
{
    qWarning().noquote() << "Google authorization failed:" << error;
    showAccountMessage(QStringLiteral("Google authorization failed: %1").arg(error), true);
}

void AppController::handleAuthorizationRequired(const QString &accountId,
                                                const QString &error)
{
    const QString label = emailForAccount(accountId);
    const QString accountLabel = label.isEmpty() ? accountId : label;
    qWarning().noquote() << "Google authorization requires reauthorization for"
                         << accountLabel << ":" << error;

    QString stateError;
    if (!m_state.markSyncError(
            accountId, QDateTime::currentSecsSinceEpoch(),
            QStringLiteral("Google authorization expired. Reauthorize the account. %1")
                .arg(error),
            &stateError)) {
        qWarning().noquote() << "Could not record the authorization error:" << stateError;
    }
    applyConfiguration(m_config.config());
    if (m_settingsWindow) {
        m_settingsWindow->refresh();
    }
    showAccountMessage(
        QStringLiteral("Google authorization for %1 requires reauthorization: %2")
            .arg(accountLabel, error),
        true);
}

void AppController::handleSyncFinished(const SyncResult &result)
{
    if (!m_oauthManager || result.errorKind != GmailErrorKind::Unauthorized) {
        return;
    }
    const QString label = emailForAccount(result.accountId);
    qInfo().noquote() << "Gmail rejected the access token for"
                      << (label.isEmpty() ? result.accountId : label)
                      << "; attempting a token refresh.";
    m_oauthManager->refreshAccessToken(result.accountId);
}

void AppController::showAccountMessage(const QString &message, bool error)
{
    m_accountMessage = message;
    m_accountMessageIsError = error;
    if (m_settingsWindow) {
        m_settingsWindow->setAccountMessage(message, error);
    }
}

QString AppController::emailForAccount(const QString &accountId) const
{
    for (const AccountConfig &account : m_config.config().accounts) {
        if (account.id == accountId) {
            return account.email;
        }
    }
    return QString();
}

void AppController::applyConfiguration(const AppConfig &config)
{
    if (m_oauthManager) {
        m_oauthManager->setAccounts(config.accounts);
    }
    m_dispatcher->setConfig(config.notifications);
    if (!m_scheduler.setIntervalMinutes(config.notifications.pollIntervalMinutes)) {
        qWarning().noquote() << "Ignoring an invalid polling interval.";
    }

    for (const AccountConfig &account : config.accounts) {
        QString stateError;
        if (!m_state.ensureAccount(account.id, account.email, &stateError)) {
            qWarning().noquote() << "Cannot initialize local state for an account:"
                                 << stateError;
        }
    }

    QList<SyncAccountRequest> requests;
    int missingTokens = 0;
    for (const AccountConfig &account : config.accounts) {
        if (!account.enabled) {
            continue;
        }

        QString token;
        if (!m_tokenProvider->accessToken(account.id, &token) || token.isEmpty()) {
            ++missingTokens;
            continue;
        }

        SyncAccountRequest request;
        request.accountId = account.id;
        request.email = account.email;
        request.accessToken = token;
        request.includeUpdates = config.notifications.includeUpdates;
        requests.append(request);
    }

    const bool accountsChanged = !sameAccountRequests(m_accountRequests, requests);
    m_scheduler.setAccounts(requests);
    m_accountRequests = requests;
    m_accountsAwaitingAuthorization = missingTokens;

    if (requests.isEmpty()) {
        m_checkAfterCurrent = false;
        m_scheduler.stop();
        return;
    }

    if (m_suspended) {
        if (m_scheduler.isRunning() && !m_scheduler.isCheckRunning()) {
            m_scheduler.stop();
        }
        return;
    }

    if (!m_scheduler.isRunning()) {
        m_scheduler.start();
        return;
    }

    if (accountsChanged) {
        if (m_scheduler.isCheckRunning()) {
            m_checkAfterCurrent = true;
        } else {
            m_scheduler.checkNow();
        }
    }
}

void AppController::suspend()
{
    if (!m_initialized || m_suspended) {
        return;
    }
    m_suspended = true;
    if (!m_scheduler.isCheckRunning()) {
        m_scheduler.stop();
    }
    qInfo().noquote() << "Session suspend requested; polling paused.";
}

void AppController::resume()
{
    if (!m_suspended) {
        return;
    }
    m_suspended = false;
    if (!m_initialized || m_accountRequests.isEmpty()) {
        return;
    }
    if (!m_scheduler.isRunning()) {
        m_scheduler.start();
    }
    qInfo().noquote() << "Session resumed; polling resumed.";
}

bool AppController::checkNow(QString *message)
{
    auto setMessage = [message](const QString &text) {
        if (message) {
            *message = text;
        }
    };

    if (!m_initialized) {
        setMessage(QStringLiteral("The notifier is not initialized."));
        return false;
    }
    if (m_suspended) {
        setMessage(QStringLiteral("The session is suspended."));
        return false;
    }
    if (m_oauthInitialLoadPending) {
        setMessage(QStringLiteral("Saved Google authorizations are still loading."));
        return false;
    }
    if (m_accountRequests.isEmpty()) {
        setMessage(QStringLiteral("No authorized accounts are available. Add or "
                                  "reauthorize an account first."));
        return false;
    }
    if (m_scheduler.isCheckRunning()) {
        setMessage(QStringLiteral("A Gmail check is already running."));
        return true;
    }
    if (!m_connectivity.isOnline()) {
        setMessage(QStringLiteral("The computer is offline. The check will run after "
                                  "reconnection."));
        return false;
    }
    if (!m_scheduler.isRunning()) {
        if (!m_scheduler.start()) {
            setMessage(QStringLiteral("A Gmail check could not be started."));
            return false;
        }
        setMessage(QStringLiteral("Gmail check started."));
        return true;
    }
    if (!m_scheduler.checkNow()) {
        setMessage(QStringLiteral("A Gmail check could not be started."));
        return false;
    }
    setMessage(QStringLiteral("Gmail check started."));
    return true;
}

bool AppController::showSettings()
{
    if (!m_initialized) {
        return false;
    }

    if (!m_settingsWindow) {
        m_settingsWindow = new SettingsWindow(&m_config, &m_state);
        m_settingsWindow->setAttribute(Qt::WA_QuitOnClose, false);
        m_settingsWindow->setScheduler(&m_scheduler);
        m_settingsWindow->setConnectivityMonitor(&m_connectivity);

        connect(m_settingsWindow, &SettingsWindow::configurationChanged, this,
                [this](const AppConfig &config) {
                    applyConfiguration(config);
                });
        connect(m_settingsWindow, &SettingsWindow::addAccountRequested, this,
                &AppController::requestAddAccount);
        connect(m_settingsWindow, &SettingsWindow::reauthorizeAccountRequested, this,
                &AppController::requestReauthorizeAccount);
        connect(m_settingsWindow, &SettingsWindow::accountRemoved, this,
                &AppController::handleAccountRemoval);
    }

    if (m_oauthManager) {
        m_settingsWindow->setAuthorizationBusy(m_oauthManager->isAuthorizing());
    }
    if (!m_accountMessage.isEmpty()) {
        m_settingsWindow->setAccountMessage(m_accountMessage, m_accountMessageIsError);
    }
    m_settingsWindow->refresh();
    if (m_settingsWindow->isMinimized()) {
        m_settingsWindow->showNormal();
    } else {
        m_settingsWindow->show();
    }
    m_settingsWindow->raise();
    m_settingsWindow->activateWindow();
    return true;
}
