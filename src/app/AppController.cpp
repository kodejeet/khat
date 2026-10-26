#include "app/AppController.h"

#include "app/PowerMonitor.h"
#include "auth/AccessTokenProvider.h"
#include "notify/FreedesktopNotificationBackend.h"
#include "notify/NotificationBackend.h"
#include "notify/NotificationDispatcher.h"
#include "notify/NotificationService.h"
#include "ui/SettingsWindow.h"

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
    QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_config(m_options.paths.configFile)
    , m_state(m_options.paths.stateFile)
    , m_sync(&m_state, &m_network)
    , m_tokenProvider(std::move(tokenProvider))
    , m_scheduler(&m_sync)
{
    if (!m_tokenProvider) {
        m_tokenProvider = std::make_shared<EmptyAccessTokenProvider>();
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

    setupSleepMonitoring();

    m_initialized = true;
    applyConfiguration(m_config.config());

    if (m_accountRequests.isEmpty() && m_accountsAwaitingAuthorization > 0) {
        qInfo().noquote()
            << "Accounts are configured but not authorized; background checks are idle.";
    } else if (m_accountRequests.isEmpty() && m_config.config().accounts.isEmpty()) {
        qInfo().noquote() << "No accounts are configured; background checks are idle.";
    } else if (m_accountRequests.isEmpty()) {
        qInfo().noquote()
            << "No enabled accounts are authorized; background checks are idle.";
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

void AppController::applyConfiguration(const AppConfig &config)
{
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
                &AppController::addAccountRequested);
        connect(m_settingsWindow, &SettingsWindow::reauthorizeAccountRequested, this,
                &AppController::reauthorizeAccountRequested);
        connect(m_settingsWindow, &SettingsWindow::accountRemoved, this,
                &AppController::accountRemoved);
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
