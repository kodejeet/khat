#pragma once

#include "config/AppConfig.h"

#include <QWidget>

#include <functional>

class ConfigStore;
class ConnectivityMonitor;
class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class StateDatabase;
class SyncScheduler;

class SettingsWindow : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsWindow(ConfigStore *config, StateDatabase *state,
                            QWidget *parent = nullptr);

    // Optional live objects. Without them the status tab shows "Not running"
    // and "Unknown" rather than failing.
    void setScheduler(SyncScheduler *scheduler);
    void setConnectivityMonitor(ConnectivityMonitor *monitor);

    // Test seam; the default asks the user with QMessageBox.
    void setRemovalConfirmation(std::function<bool(const QString &accountEmail)> handler);

    void refresh();

signals:
    void configurationChanged(const AppConfig &config);
    void addAccountRequested();
    void reauthorizeAccountRequested(const QString &accountId);
    void accountRemoved(const QString &accountId);

private slots:
    void saveNotificationSettings();
    void refreshAccounts();
    void refreshStatus();
    void updateAccountButtons();
    void removeSelectedAccount();
    void checkNow();
    void updateIntervalHint();

private:
    void buildUi();
    void populateNotifications();
    void populateAccounts();
    QString selectedAccountId() const;

    ConfigStore *m_config;
    StateDatabase *m_state;
    SyncScheduler *m_scheduler = nullptr;
    ConnectivityMonitor *m_connectivity = nullptr;
    std::function<bool(const QString &accountEmail)> m_confirmRemoval;
    QMetaObject::Connection m_scheduleConnection;
    QMetaObject::Connection m_runningConnection;
    QMetaObject::Connection m_checkStartedConnection;
    QMetaObject::Connection m_checkFinishedConnection;
    QMetaObject::Connection m_connectivityConnection;

    QTreeWidget *m_accounts = nullptr;
    QPushButton *m_addAccount = nullptr;
    QPushButton *m_reauthorize = nullptr;
    QPushButton *m_removeAccount = nullptr;
    QLabel *m_accountMessage = nullptr;

    QCheckBox *m_notificationsEnabled = nullptr;
    QSpinBox *m_pollInterval = nullptr;
    QLabel *m_intervalHint = nullptr;
    QCheckBox *m_includeUpdates = nullptr;
    QCheckBox *m_showPreview = nullptr;
    QLabel *m_settingsError = nullptr;

    QLabel *m_lastSync = nullptr;
    QLabel *m_nextCheck = nullptr;
    QLabel *m_connection = nullptr;
    QLabel *m_lastError = nullptr;
    QLabel *m_statusMessage = nullptr;
    QPushButton *m_checkNow = nullptr;
};
