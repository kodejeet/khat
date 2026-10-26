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
    explicit AppController(
        AppControllerOptions options,
        std::shared_ptr<AccessTokenProvider> tokenProvider = {},
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

    AppControllerOptions m_options;
    ConfigStore m_config;
    StateDatabase m_state;
    QNetworkAccessManager m_network;
    ConnectivityMonitor m_connectivity;
    SyncService m_sync;
    std::shared_ptr<AccessTokenProvider> m_tokenProvider;
    SyncScheduler m_scheduler;
    std::unique_ptr<NotificationBackend> m_notificationBackend;
    std::unique_ptr<NotificationService> m_notificationService;
    std::unique_ptr<NotificationDispatcher> m_dispatcher;
    std::unique_ptr<PowerMonitor> m_powerMonitor;
    SettingsWindow *m_settingsWindow = nullptr;
    QList<SyncAccountRequest> m_accountRequests;
    int m_accountsAwaitingAuthorization = 0;
    bool m_initialized = false;
    bool m_suspended = false;
    bool m_checkAfterCurrent = false;
};

Q_DECLARE_METATYPE(AppConfig)
