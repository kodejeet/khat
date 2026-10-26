#include "ui/SettingsWindow.h"

#include "config/ConfigStore.h"
#include "connectivity/ConnectivityMonitor.h"
#include "state/StateDatabase.h"
#include "sync/SyncScheduler.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <utility>

namespace {
QString formatEpoch(qint64 seconds)
{
    if (seconds <= 0) {
        return QStringLiteral("Never");
    }
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(seconds), QLocale::ShortFormat);
}

QString intervalDescription(int minutes)
{
    if (minutes % 60 == 0) {
        const int hours = minutes / 60;
        return hours == 1 ? QStringLiteral("1 hour") : QStringLiteral("%1 hours").arg(hours);
    }
    if (minutes > 60) {
        return QStringLiteral("%1 min (%2 h %3 min)")
            .arg(minutes)
            .arg(minutes / 60)
            .arg(minutes % 60);
    }
    return QStringLiteral("%1 minutes").arg(minutes);
}
} // namespace

SettingsWindow::SettingsWindow(ConfigStore *config, StateDatabase *state, QWidget *parent)
    : QWidget(parent)
    , m_config(config)
    , m_state(state)
{
    setWindowTitle(QStringLiteral("Gmail Notifier Settings"));
    setWindowIcon(QApplication::windowIcon());
    setMinimumSize(620, 440);
    resize(700, 500);
    buildUi();
    populateNotifications();
    populateAccounts();
    refreshStatus();
}

void SettingsWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    root->addWidget(tabs);

    auto *accountsPage = new QWidget(tabs);
    auto *accountsLayout = new QVBoxLayout(accountsPage);
    auto *accountsHint = new QLabel(
        QStringLiteral("Notifications are checked independently for each connected account."),
        accountsPage);
    accountsHint->setWordWrap(true);
    accountsLayout->addWidget(accountsHint);

    m_accounts = new QTreeWidget(accountsPage);
    m_accounts->setObjectName(QStringLiteral("accountsTree"));
    m_accounts->setColumnCount(2);
    m_accounts->setHeaderLabels(
        {QStringLiteral("Account"), QStringLiteral("Last synchronization")});
    m_accounts->setRootIsDecorated(false);
    m_accounts->setAlternatingRowColors(true);
    m_accounts->setSelectionMode(QAbstractItemView::SingleSelection);
    m_accounts->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_accounts->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_accounts->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    accountsLayout->addWidget(m_accounts, 1);

    auto *accountButtons = new QHBoxLayout;
    m_addAccount = new QPushButton(QStringLiteral("Add Account..."), accountsPage);
    m_addAccount->setObjectName(QStringLiteral("addAccountButton"));
    m_reauthorize = new QPushButton(QStringLiteral("Reauthorize..."), accountsPage);
    m_reauthorize->setObjectName(QStringLiteral("reauthorizeButton"));
    m_removeAccount = new QPushButton(QStringLiteral("Remove..."), accountsPage);
    m_removeAccount->setObjectName(QStringLiteral("removeAccountButton"));
    accountButtons->addWidget(m_addAccount);
    accountButtons->addWidget(m_reauthorize);
    accountButtons->addWidget(m_removeAccount);
    accountButtons->addStretch();
    accountsLayout->addLayout(accountButtons);

    m_accountMessage = new QLabel(accountsPage);
    m_accountMessage->setObjectName(QStringLiteral("accountMessage"));
    m_accountMessage->setWordWrap(true);
    m_accountMessage->hide();
    accountsLayout->addWidget(m_accountMessage);

    connect(m_addAccount, &QPushButton::clicked, this,
            &SettingsWindow::addAccountRequested);
    connect(m_reauthorize, &QPushButton::clicked, this, [this] {
        const QString id = selectedAccountId();
        if (!id.isEmpty()) {
            emit reauthorizeAccountRequested(id);
        }
    });
    connect(m_removeAccount, &QPushButton::clicked, this,
            &SettingsWindow::removeSelectedAccount);
    connect(m_accounts, &QTreeWidget::itemSelectionChanged, this,
            &SettingsWindow::updateAccountButtons);

    tabs->addTab(accountsPage, QStringLiteral("Accounts"));

    auto *notificationsPage = new QWidget(tabs);
    auto *notificationsLayout = new QVBoxLayout(notificationsPage);
    auto *notificationsForm = new QFormLayout;

    m_notificationsEnabled =
        new QCheckBox(QStringLiteral("Show desktop notifications"), notificationsPage);
    m_notificationsEnabled->setObjectName(QStringLiteral("notificationsEnabledCheck"));
    notificationsForm->addRow(QStringLiteral("Notifications"), m_notificationsEnabled);

    m_pollInterval = new QSpinBox(notificationsPage);
    m_pollInterval->setObjectName(QStringLiteral("pollIntervalSpin"));
    m_pollInterval->setRange(kMinPollIntervalMinutes, kMaxPollIntervalMinutes);
    m_pollInterval->setSuffix(QStringLiteral(" min"));
    m_pollInterval->setKeyboardTracking(false);
    m_intervalHint = new QLabel(notificationsPage);
    m_intervalHint->setObjectName(QStringLiteral("intervalHint"));
    auto *intervalRow = new QHBoxLayout;
    intervalRow->addWidget(m_pollInterval);
    intervalRow->addWidget(m_intervalHint);
    intervalRow->addStretch();
    notificationsForm->addRow(QStringLiteral("Check every"), intervalRow);

    m_includeUpdates = new QCheckBox(
        QStringLiteral("Include the Updates category"), notificationsPage);
    m_includeUpdates->setObjectName(QStringLiteral("includeUpdatesCheck"));
    notificationsForm->addRow(QStringLiteral("Categories"), m_includeUpdates);

    m_showPreview = new QCheckBox(
        QStringLiteral("Show a short message preview"), notificationsPage);
    m_showPreview->setObjectName(QStringLiteral("showPreviewCheck"));
    notificationsForm->addRow(QStringLiteral("Preview"), m_showPreview);

    notificationsLayout->addLayout(notificationsForm);
    auto *notificationsHint = new QLabel(
        QStringLiteral("Changes are saved automatically. The default check interval is "
                       "180 minutes (3 hours)."),
        notificationsPage);
    notificationsHint->setWordWrap(true);
    notificationsLayout->addWidget(notificationsHint);
    notificationsLayout->addStretch();

    m_settingsError = new QLabel(notificationsPage);
    m_settingsError->setObjectName(QStringLiteral("settingsError"));
    m_settingsError->setWordWrap(true);
    m_settingsError->hide();
    notificationsLayout->addWidget(m_settingsError);

    connect(m_notificationsEnabled, &QCheckBox::toggled, this,
            &SettingsWindow::saveNotificationSettings);
    connect(m_pollInterval, &QSpinBox::valueChanged, this,
            &SettingsWindow::saveNotificationSettings);
    connect(m_includeUpdates, &QCheckBox::toggled, this,
            &SettingsWindow::saveNotificationSettings);
    connect(m_showPreview, &QCheckBox::toggled, this,
            &SettingsWindow::saveNotificationSettings);
    connect(m_pollInterval, &QSpinBox::valueChanged, this,
            &SettingsWindow::updateIntervalHint);

    tabs->addTab(notificationsPage, QStringLiteral("Notifications"));

    auto *statusPage = new QWidget(tabs);
    auto *statusLayout = new QVBoxLayout(statusPage);
    auto *statusForm = new QFormLayout;

    m_lastSync = new QLabel(statusPage);
    m_lastSync->setObjectName(QStringLiteral("lastSyncValue"));
    statusForm->addRow(QStringLiteral("Last successful sync:"), m_lastSync);

    m_nextCheck = new QLabel(statusPage);
    m_nextCheck->setObjectName(QStringLiteral("nextCheckValue"));
    statusForm->addRow(QStringLiteral("Next scheduled check:"), m_nextCheck);

    m_connection = new QLabel(statusPage);
    m_connection->setObjectName(QStringLiteral("connectionValue"));
    statusForm->addRow(QStringLiteral("Connection:"), m_connection);

    m_lastError = new QLabel(statusPage);
    m_lastError->setObjectName(QStringLiteral("lastErrorValue"));
    m_lastError->setWordWrap(true);
    m_lastError->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusForm->addRow(QStringLiteral("Last error:"), m_lastError);

    statusLayout->addLayout(statusForm);
    auto *statusButtons = new QHBoxLayout;
    m_checkNow = new QPushButton(QStringLiteral("Check now"), statusPage);
    m_checkNow->setObjectName(QStringLiteral("checkNowButton"));
    statusButtons->addWidget(m_checkNow);
    statusButtons->addStretch();
    statusLayout->addLayout(statusButtons);

    m_statusMessage = new QLabel(statusPage);
    m_statusMessage->setObjectName(QStringLiteral("statusMessage"));
    m_statusMessage->setWordWrap(true);
    m_statusMessage->hide();
    statusLayout->addWidget(m_statusMessage);
    statusLayout->addStretch();

    connect(m_checkNow, &QPushButton::clicked, this, &SettingsWindow::checkNow);
    tabs->addTab(statusPage, QStringLiteral("Status"));

    updateIntervalHint();
    updateAccountButtons();
}

void SettingsWindow::setScheduler(SyncScheduler *scheduler)
{
    for (const QMetaObject::Connection &connection :
         {m_scheduleConnection, m_runningConnection, m_checkStartedConnection,
          m_checkFinishedConnection}) {
        if (connection) {
            disconnect(connection);
        }
    }

    m_scheduler = scheduler;
    if (m_scheduler) {
        m_scheduleConnection = connect(m_scheduler, &SyncScheduler::scheduleChanged, this,
                                       &SettingsWindow::refreshStatus);
        m_runningConnection = connect(m_scheduler, &SyncScheduler::runningChanged, this,
                                      &SettingsWindow::refreshStatus);
        m_checkStartedConnection = connect(m_scheduler, &SyncScheduler::checkStarted, this,
                                           &SettingsWindow::refreshStatus);
        m_checkFinishedConnection = connect(
            m_scheduler, &SyncScheduler::checkFinished, this, [this] {
                refreshAccounts();
                refreshStatus();
            });
    }
    refreshStatus();
}

void SettingsWindow::setConnectivityMonitor(ConnectivityMonitor *monitor)
{
    if (m_connectivityConnection) {
        disconnect(m_connectivityConnection);
        m_connectivityConnection = QMetaObject::Connection();
    }
    m_connectivity = monitor;
    if (m_connectivity) {
        m_connectivityConnection = connect(m_connectivity, &ConnectivityMonitor::stateChanged,
                                           this, &SettingsWindow::refreshStatus);
    }
    refreshStatus();
}

void SettingsWindow::setRemovalConfirmation(
    std::function<bool(const QString &accountEmail)> handler)
{
    m_confirmRemoval = std::move(handler);
}

void SettingsWindow::setAuthorizationBusy(bool busy)
{
    if (m_authorizationBusy == busy) {
        return;
    }
    m_authorizationBusy = busy;
    if (m_addAccount) {
        m_addAccount->setText(busy ? QStringLiteral("Authorizing...")
                                   : QStringLiteral("Add Account..."));
    }
    updateAccountButtons();
}

void SettingsWindow::setAccountMessage(const QString &message, bool error)
{
    if (!m_accountMessage) {
        return;
    }
    m_accountMessage->setText(message);
    QFont font = m_accountMessage->font();
    font.setBold(error);
    m_accountMessage->setFont(font);
    m_accountMessage->setVisible(!message.isEmpty());
}

void SettingsWindow::populateNotifications()
{
    if (!m_config) {
        return;
    }

    const NotificationConfig notifications = m_config->config().notifications;
    const QSignalBlocker enabledBlocker(m_notificationsEnabled);
    const QSignalBlocker intervalBlocker(m_pollInterval);
    const QSignalBlocker updatesBlocker(m_includeUpdates);
    const QSignalBlocker previewBlocker(m_showPreview);

    m_notificationsEnabled->setChecked(notifications.enabled);
    m_pollInterval->setValue(notifications.pollIntervalMinutes);
    m_includeUpdates->setChecked(notifications.includeUpdates);
    m_showPreview->setChecked(notifications.showPreview);
    updateIntervalHint();
}

void SettingsWindow::populateAccounts()
{
    if (!m_accounts) {
        return;
    }

    const QString previousId = selectedAccountId();
    const QSignalBlocker blocker(m_accounts);
    m_accounts->clear();

    QHash<QString, AccountState> states;
    if (m_state) {
        const QList<AccountState> list = m_state->accountStates();
        for (const AccountState &state : list) {
            states.insert(state.accountId, state);
        }
    }

    QTreeWidgetItem *selectedItem = nullptr;
    if (m_config) {
        const QList<AccountConfig> accounts = m_config->config().accounts;
        for (const AccountConfig &account : accounts) {
            QString label = account.displayName.isEmpty() ? account.email
                                                         : account.displayName;
            if (!account.displayName.isEmpty() && account.displayName != account.email) {
                label += QStringLiteral(" <%1>").arg(account.email);
            }
            if (!account.enabled) {
                label.prepend(QStringLiteral("[disabled] "));
            }

            auto *item = new QTreeWidgetItem(m_accounts);
            item->setText(0, label);
            item->setData(0, Qt::UserRole, account.id);
            item->setToolTip(0, account.email);

            QString status = QStringLiteral("Not synced yet");
            if (!account.enabled) {
                status = QStringLiteral("Disabled");
            } else if (states.contains(account.id)) {
                const AccountState state = states.value(account.id);
                if (!state.lastError.isEmpty()) {
                    status = QStringLiteral("Error: %1").arg(
                        state.lastError.simplified().left(80));
                    item->setToolTip(1, state.lastError);
                } else if (state.lastSyncEpoch > 0) {
                    status = QStringLiteral("Synced %1").arg(formatEpoch(state.lastSyncEpoch));
                } else if (state.baselineDone) {
                    status = QStringLiteral("Baseline ready");
                }
            }
            item->setText(1, status);
            m_accounts->addTopLevelItem(item);
            if (account.id == previousId) {
                selectedItem = item;
            }
        }
    }

    if (!selectedItem && m_accounts->topLevelItemCount() > 0) {
        selectedItem = m_accounts->topLevelItem(0);
    }
    if (selectedItem) {
        m_accounts->setCurrentItem(selectedItem);
    }
    updateAccountButtons();
}

void SettingsWindow::refresh()
{
    populateNotifications();
    populateAccounts();
    refreshStatus();
}

void SettingsWindow::refreshAccounts()
{
    populateAccounts();
}

void SettingsWindow::refreshStatus()
{
    if (!m_lastSync || !m_nextCheck || !m_connection || !m_lastError || !m_checkNow) {
        return;
    }

    qint64 lastSuccess = 0;
    QString lastError;
    if (m_state) {
        const QList<AccountState> states = m_state->accountStates();
        for (const AccountState &state : states) {
            lastSuccess = qMax(lastSuccess, state.lastSyncEpoch);
            if (lastError.isEmpty() && !state.lastError.isEmpty()) {
                lastError = state.lastError;
            }
        }
    }

    if (m_scheduler) {
        lastSuccess = qMax(lastSuccess, m_scheduler->lastSuccessEpoch());
        if (!m_scheduler->lastError().isEmpty()) {
            lastError = m_scheduler->lastError();
        }
    }

    m_lastSync->setText(lastSuccess > 0 ? formatEpoch(lastSuccess)
                                        : QStringLiteral("Never"));
    m_connection->setText(m_connectivity ? m_connectivity->stateText()
                                         : QStringLiteral("Unknown"));

    if (!m_scheduler) {
        m_nextCheck->setText(QStringLiteral("Not running"));
        m_checkNow->setEnabled(false);
    } else if (m_scheduler->nextCheckEpoch() > 0) {
        m_nextCheck->setText(formatEpoch(m_scheduler->nextCheckEpoch()));
        m_checkNow->setEnabled(m_scheduler->isRunning()
                               && !m_scheduler->isCheckRunning()
                               && (!m_connectivity || m_connectivity->isOnline()));
    } else if (m_scheduler->isRunning() && m_connectivity
               && !m_connectivity->isOnline()) {
        m_nextCheck->setText(QStringLiteral("Deferred until online"));
        m_checkNow->setEnabled(false);
    } else {
        m_nextCheck->setText(QStringLiteral("Not scheduled"));
        m_checkNow->setEnabled(m_scheduler->isRunning()
                               && !m_scheduler->isCheckRunning()
                               && (!m_connectivity || m_connectivity->isOnline()));
    }

    m_lastError->setText(lastError.isEmpty() ? QStringLiteral("None") : lastError.left(500));
}

void SettingsWindow::updateAccountButtons()
{
    const bool hasSelection = !selectedAccountId().isEmpty();
    if (m_addAccount) {
        m_addAccount->setEnabled(!m_authorizationBusy);
    }
    if (m_reauthorize) {
        m_reauthorize->setEnabled(hasSelection && !m_authorizationBusy);
    }
    if (m_removeAccount) {
        m_removeAccount->setEnabled(hasSelection && !m_authorizationBusy);
    }
}

void SettingsWindow::saveNotificationSettings()
{
    if (!m_config || !m_notificationsEnabled || !m_pollInterval || !m_includeUpdates
        || !m_showPreview) {
        return;
    }

    AppConfig config = m_config->config();
    config.notifications.enabled = m_notificationsEnabled->isChecked();
    config.notifications.pollIntervalMinutes = m_pollInterval->value();
    config.notifications.includeUpdates = m_includeUpdates->isChecked();
    config.notifications.showPreview = m_showPreview->isChecked();

    QString error;
    if (!m_config->save(config, &error)) {
        populateNotifications();
        m_settingsError->setText(
            QStringLiteral("Could not save notification settings: %1").arg(error));
        m_settingsError->show();
        return;
    }

    m_settingsError->hide();
    m_settingsError->clear();
    emit configurationChanged(config);
}

void SettingsWindow::updateIntervalHint()
{
    if (m_intervalHint && m_pollInterval) {
        m_intervalHint->setText(
            QStringLiteral("(%1)").arg(intervalDescription(m_pollInterval->value())));
    }
}

void SettingsWindow::checkNow()
{
    if (!m_scheduler) {
        return;
    }
    m_statusMessage->hide();
    if (!m_scheduler->checkNow()) {
        m_statusMessage->setText(
            QStringLiteral("A check could not start. It may already be running or the "
                           "connection is offline."));
        m_statusMessage->show();
    }
}

QString SettingsWindow::selectedAccountId() const
{
    if (!m_accounts) {
        return QString();
    }
    const QTreeWidgetItem *item = m_accounts->currentItem();
    if (!item) {
        return QString();
    }
    return item->data(0, Qt::UserRole).toString();
}

void SettingsWindow::removeSelectedAccount()
{
    if (!m_config) {
        return;
    }
    const QString accountId = selectedAccountId();
    if (accountId.isEmpty()) {
        return;
    }

    QString email;
    for (const AccountConfig &account : m_config->config().accounts) {
        if (account.id == accountId) {
            email = account.email;
            break;
        }
    }
    const QString label = email.isEmpty() ? accountId : email;

    bool confirmed = false;
    if (m_confirmRemoval) {
        confirmed = m_confirmRemoval(label);
    } else {
        confirmed = QMessageBox::question(
                       this, QStringLiteral("Remove account"),
                       QStringLiteral("Remove %1? Its local synchronization history will "
                                      "also be deleted.")
                           .arg(label),
                       QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes;
    }
    if (!confirmed) {
        return;
    }

    AppConfig config = m_config->config();
    bool found = false;
    for (int i = 0; i < config.accounts.size(); ++i) {
        if (config.accounts.at(i).id == accountId) {
            config.accounts.removeAt(i);
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    QString error;
    if (!m_config->save(config, &error)) {
        m_accountMessage->setText(
            QStringLiteral("Could not remove the account: %1").arg(error));
        m_accountMessage->show();
        return;
    }
    if (m_state) {
        QString stateError;
        if (!m_state->deleteAccount(accountId, &stateError)) {
            m_accountMessage->setText(
                QStringLiteral("Account settings were removed, but local state could not be "
                               "deleted: %1")
                    .arg(stateError));
        } else {
            m_accountMessage->setText(QStringLiteral("Removed %1.").arg(label));
        }
    }
    m_accountMessage->show();

    populateAccounts();
    refreshStatus();
    emit configurationChanged(config);
    emit accountRemoved(accountId);
}
