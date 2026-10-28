#pragma once

#include "config/AppPaths.h"
#include "config/ConfigStore.h"
#include "connectivity/ConnectivityMonitor.h"
#include "state/StateDatabase.h"
#include "sync/SyncScheduler.h"
#include "sync/SyncService.h"

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

#include <memory>

class AccessTokenProvider;
class NotificationBackend;
class NotificationDispatcher;
class NotificationService;
class OAuthManager;
class PowerMonitor;
class SettingsWindow;

struct AppControllerOptions
{
    AppPaths paths;
    QUrl gmailApiBaseUrl =
        QUrl(QStringLiteral("https://gmail.googleapis.com/gmail/v1"));
    bool subscribeToSleep = true;
};

// Owns the long-lived background application stack. The settings window is
// created only when requested and may be closed without stopping polling.
class AppController : public QObject
{
    Q_OBJECT

public:
    // A provided token provider bypasses the production OAuth manager. Tests
    // may inject an OAuthManager configured with fake endpoints and storage.
    explicit AppController(
        AppControllerOptions options,
        std::shared_ptr<AccessTokenProvider> tokenProvider = {},
        std::shared_ptr<OAuthManager> oauthManager = {},
        QObject *parent = nullptr);
    ~AppController() override;

    bool initialize(QString *error = nullptr);
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // Rebuilds account requests after authorization/token changes.
    void reloadAccounts();

    bool showSettings();
    bool checkNow(QString *message = nullptr);

    bool isSuspended() const { return m_suspended; }
    bool isWaitingForAuthorization() const
    {
        return m_accountsAwaitingAuthorization > 0;
    }
    int accountsAwaitingAuthorization() const
    {
        return m_accountsAwaitingAuthorization;
    }

    ConfigStore *configStore() { return &m_config; }
    StateDatabase *stateDatabase() { return &m_state; }
    ConnectivityMonitor *connectivityMonitor() { return &m_connectivity; }
    SyncScheduler *scheduler() { return &m_scheduler; }
    OAuthManager *oauthManager() const { return m_oauthManager.get(); }
    SettingsWindow *settingsWindow() const { return m_settingsWindow; }

public slots:
    void suspend();
    void resume();

signals:
    void addAccountRequested();
    void reauthorizeAccountRequested(const QString &accountId);
    void accountRemoved(const QString &accountId);

private:
    void applyConfiguration(const AppConfig &config);
    void setupSleepMonitoring();
    void setupOAuth();
    void logAccountState();
    void requestAddAccount();
    void requestReauthorizeAccount(const QString &accountId);
    void handleAccountRemoval(const QString &accountId);
    void handleAuthorizationFinished(const QString &accountId, const QString &email,
                                     bool newAccount);
    void handleAuthorizationFailed(const QString &error);
    void handleAuthorizationRequired(const QString &accountId, const QString &error);
    void handleSyncFinished(const SyncResult &result);
    void showAccountMessage(const QString &message, bool error = false);
    QString emailForAccount(const QString &accountId) const;

    AppControllerOptions m_options;
    ConfigStore m_config;
    StateDatabase m_state;
    QNetworkAccessManager m_network;
    ConnectivityMonitor m_connectivity;
    SyncService m_sync;
    std::shared_ptr<OAuthManager> m_oauthManager;
    std::shared_ptr<AccessTokenProvider> m_tokenProvider;
    SyncScheduler m_scheduler;
    std::unique_ptr<NotificationBackend> m_notificationBackend;
    std::unique_ptr<NotificationService> m_notificationService;
    std::unique_ptr<NotificationDispatcher> m_dispatcher;
    std::unique_ptr<PowerMonitor> m_powerMonitor;
    SettingsWindow *m_settingsWindow = nullptr;
    QList<SyncAccountRequest> m_accountRequests;
    QString m_accountMessage;
    bool m_accountMessageIsError = false;
    int m_accountsAwaitingAuthorization = 0;
    bool m_initialized = false;
    bool m_suspended = false;
    bool m_checkAfterCurrent = false;
    bool m_checkAfterInitialLoad = false;
    bool m_oauthConnected = false;
    bool m_oauthInitialLoadPending = false;
};

Q_DECLARE_METATYPE(AppConfig)
