#include "config/ConfigStore.h"
#include "connectivity/ConnectivityMonitor.h"
#include "state/StateDatabase.h"
#include "sync/SyncScheduler.h"
#include "sync/SyncService.h"
#include "ui/SettingsWindow.h"

#include <QCheckBox>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

class TestSettingsWindow : public QObject
{
    Q_OBJECT

private slots:
    void notificationsPersist();
    void accountSignalsAndRemoval();
    void authorizationBusyAndMessages();
    void statusReflectsSchedulerAndConnectivity();
};

static bool openDatabase(StateDatabase &db)
{
    QString error;
    return db.open(&error);
}

void TestSettingsWindow::notificationsPersist()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configPath = dir.filePath(QStringLiteral("config.json"));
    ConfigStore store(configPath);
    QString error;
    QVERIFY2(store.load(&error), qPrintable(error));

    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QVERIFY2(openDatabase(db), "cannot open state database");

    SettingsWindow window(&store, &db);
    auto *enabled = window.findChild<QCheckBox *>(QStringLiteral("notificationsEnabledCheck"));
    auto *interval = window.findChild<QSpinBox *>(QStringLiteral("pollIntervalSpin"));
    auto *updates = window.findChild<QCheckBox *>(QStringLiteral("includeUpdatesCheck"));
    auto *preview = window.findChild<QCheckBox *>(QStringLiteral("showPreviewCheck"));
    auto *hint = window.findChild<QLabel *>(QStringLiteral("intervalHint"));
    auto *errorLabel = window.findChild<QLabel *>(QStringLiteral("settingsError"));
    auto *nextCheck = window.findChild<QLabel *>(QStringLiteral("nextCheckValue"));
    auto *connection = window.findChild<QLabel *>(QStringLiteral("connectionValue"));
    auto *checkNow = window.findChild<QPushButton *>(QStringLiteral("checkNowButton"));
    QVERIFY(enabled);
    QVERIFY(interval);
    QVERIFY(updates);
    QVERIFY(preview);
    QVERIFY(hint);
    QVERIFY(errorLabel);
    QVERIFY(nextCheck);
    QVERIFY(connection);
    QVERIFY(checkNow);

    QCOMPARE(enabled->isChecked(), true);
    QCOMPARE(interval->value(), 180);
    QCOMPARE(interval->minimum(), 5);
    QCOMPARE(interval->maximum(), 1440);
    QCOMPARE(updates->isChecked(), true);
    QCOMPARE(preview->isChecked(), true);
    QVERIFY(!errorLabel->isVisible());
    QCOMPARE(nextCheck->text(), QStringLiteral("Not running"));
    QCOMPARE(connection->text(), QStringLiteral("Unknown"));
    QVERIFY(!checkNow->isEnabled());

    int changes = 0;
    connect(&window, &SettingsWindow::configurationChanged, this,
            [&changes](const AppConfig &) { ++changes; });

    enabled->setChecked(false);
    interval->setValue(60);
    updates->setChecked(false);
    preview->setChecked(false);

    QCOMPARE(changes, 4);
    QCOMPARE(store.config().notifications.enabled, false);
    QCOMPARE(store.config().notifications.pollIntervalMinutes, 60);
    QCOMPARE(store.config().notifications.includeUpdates, false);
    QCOMPARE(store.config().notifications.showPreview, false);
    QVERIFY(hint->text().contains(QStringLiteral("1 hour")));

    ConfigStore reloaded(configPath);
    QVERIFY2(reloaded.load(&error), qPrintable(error));
    QCOMPARE(reloaded.config().notifications.enabled, false);
    QCOMPARE(reloaded.config().notifications.pollIntervalMinutes, 60);
    QCOMPARE(reloaded.config().notifications.includeUpdates, false);
    QCOMPARE(reloaded.config().notifications.showPreview, false);
}

void TestSettingsWindow::accountSignalsAndRemoval()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ConfigStore store(dir.filePath(QStringLiteral("config.json")));
    QString error;
    QVERIFY2(store.load(&error), qPrintable(error));

    AppConfig config = defaultAppConfig();
    config.accounts = {
        AccountConfig{QStringLiteral("acct-one"), QStringLiteral("user@example.com"),
                      QStringLiteral("User One"), true},
        AccountConfig{QStringLiteral("acct-two"), QStringLiteral("other@example.com"),
                      QString(), true},
    };
    QVERIFY2(store.save(config, &error), qPrintable(error));

    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QVERIFY2(openDatabase(db), "cannot open state database");
    QVERIFY2(db.ensureAccount(QStringLiteral("acct-one"), QStringLiteral("user@example.com"),
                              &error),
             qPrintable(error));
    QVERIFY2(db.applyBaseline(QStringLiteral("acct-one"), {}, 1000, &error), qPrintable(error));
    QVERIFY2(db.markSyncSuccess(QStringLiteral("acct-one"), 1700000000, &error),
             qPrintable(error));
    QVERIFY2(db.ensureAccount(QStringLiteral("acct-two"), QStringLiteral("other@example.com"),
                              &error),
             qPrintable(error));

    SettingsWindow window(&store, &db);
    auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("accountsTree"));
    auto *add = window.findChild<QPushButton *>(QStringLiteral("addAccountButton"));
    auto *reauth = window.findChild<QPushButton *>(QStringLiteral("reauthorizeButton"));
    auto *remove = window.findChild<QPushButton *>(QStringLiteral("removeAccountButton"));
    QVERIFY(tree);
    QVERIFY(add);
    QVERIFY(reauth);
    QVERIFY(remove);

    QCOMPARE(tree->topLevelItemCount(), 2);
    QCOMPARE(tree->topLevelItem(0)->data(0, Qt::UserRole).toString(),
             QStringLiteral("acct-one"));
    QVERIFY(tree->topLevelItem(0)->text(1).startsWith(QStringLiteral("Synced")));
    QCOMPARE(tree->topLevelItem(1)->data(0, Qt::UserRole).toString(),
             QStringLiteral("acct-two"));
    QCOMPARE(tree->topLevelItem(1)->text(1), QStringLiteral("Not synced yet"));
    QVERIFY(reauth->isEnabled());
    QVERIFY(remove->isEnabled());

    QSignalSpy addSpy(&window, &SettingsWindow::addAccountRequested);
    QSignalSpy reauthSpy(&window, &SettingsWindow::reauthorizeAccountRequested);
    QSignalSpy removedSpy(&window, &SettingsWindow::accountRemoved);
    int configChanges = 0;
    connect(&window, &SettingsWindow::configurationChanged, this,
            [&configChanges](const AppConfig &) { ++configChanges; });

    QVERIFY(add->isEnabled());
    add->click();
    QCOMPARE(addSpy.count(), 1);

    tree->setCurrentItem(tree->topLevelItem(1));
    reauth->click();
    QCOMPARE(reauthSpy.count(), 1);
    QCOMPARE(reauthSpy.first().at(0).toString(), QStringLiteral("acct-two"));

    QString confirmationEmail;
    window.setRemovalConfirmation([&confirmationEmail](const QString &email) {
        confirmationEmail = email;
        return false;
    });
    tree->setCurrentItem(tree->topLevelItem(0));
    remove->click();
    QCOMPARE(confirmationEmail, QStringLiteral("user@example.com"));
    QCOMPARE(store.config().accounts.size(), 2);
    QCOMPARE(removedSpy.count(), 0);

    window.setRemovalConfirmation([&confirmationEmail](const QString &email) {
        confirmationEmail = email;
        return true;
    });
    remove->click();
    QCOMPARE(confirmationEmail, QStringLiteral("user@example.com"));
    QCOMPARE(store.config().accounts.size(), 1);
    QCOMPARE(store.config().accounts.first().id, QStringLiteral("acct-two"));
    QCOMPARE(removedSpy.count(), 1);
    QCOMPARE(removedSpy.first().at(0).toString(), QStringLiteral("acct-one"));
    QCOMPARE(configChanges, 1);
    const QList<AccountState> states = db.accountStates();
    QCOMPARE(states.size(), 1);
    QCOMPARE(states.first().accountId, QStringLiteral("acct-two"));
    QCOMPARE(tree->topLevelItemCount(), 1);
}

void TestSettingsWindow::authorizationBusyAndMessages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ConfigStore store(dir.filePath(QStringLiteral("config.json")));
    QString error;
    QVERIFY2(store.load(&error), qPrintable(error));

    AppConfig config = defaultAppConfig();
    config.accounts = {
        AccountConfig{QStringLiteral("acct-one"), QStringLiteral("user@example.com"),
                      QString(), true},
    };
    QVERIFY2(store.save(config, &error), qPrintable(error));

    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QVERIFY2(openDatabase(db), "cannot open state database");

    SettingsWindow window(&store, &db);
    auto *add = window.findChild<QPushButton *>(QStringLiteral("addAccountButton"));
    auto *reauth = window.findChild<QPushButton *>(QStringLiteral("reauthorizeButton"));
    auto *remove = window.findChild<QPushButton *>(QStringLiteral("removeAccountButton"));
    auto *message = window.findChild<QLabel *>(QStringLiteral("accountMessage"));
    QVERIFY(add);
    QVERIFY(reauth);
    QVERIFY(remove);
    QVERIFY(message);

    QVERIFY(add->isEnabled());
    QVERIFY(reauth->isEnabled());
    QVERIFY(remove->isEnabled());
    QVERIFY(message->isHidden());

    window.setAuthorizationBusy(true);
    QVERIFY(!add->isEnabled());
    QCOMPARE(add->text(), QStringLiteral("Authorizing..."));
    QVERIFY(!reauth->isEnabled());
    QVERIFY(!remove->isEnabled());

    window.setAccountMessage(QStringLiteral("Waiting for Google authorization."));
    QCOMPARE(message->text(), QStringLiteral("Waiting for Google authorization."));
    QVERIFY(!message->isHidden());
    QVERIFY(!message->font().bold());

    window.setAuthorizationBusy(false);
    QVERIFY(add->isEnabled());
    QCOMPARE(add->text(), QStringLiteral("Add Account..."));
    QVERIFY(reauth->isEnabled());
    QVERIFY(remove->isEnabled());

    window.setAccountMessage(QStringLiteral("Authorization failed."), true);
    QVERIFY(message->font().bold());
}

void TestSettingsWindow::statusReflectsSchedulerAndConnectivity()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ConfigStore store(dir.filePath(QStringLiteral("config.json")));
    QString error;
    QVERIFY2(store.load(&error), qPrintable(error));

    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QVERIFY2(openDatabase(db), "cannot open state database");

    QNetworkAccessManager network;
    SyncService sync(&db, &network);
    SyncScheduler scheduler(&sync);
    ConnectivityMonitor monitor;
    monitor.setSimulatedState(ConnectivityMonitor::State::Online);
    scheduler.setConnectivityMonitor(&monitor);
    QVERIFY(scheduler.setIntervalMinutes(180));

    SettingsWindow window(&store, &db);
    window.setScheduler(&scheduler);
    window.setConnectivityMonitor(&monitor);

    auto *lastSync = window.findChild<QLabel *>(QStringLiteral("lastSyncValue"));
    auto *nextCheck = window.findChild<QLabel *>(QStringLiteral("nextCheckValue"));
    auto *connection = window.findChild<QLabel *>(QStringLiteral("connectionValue"));
    auto *lastError = window.findChild<QLabel *>(QStringLiteral("lastErrorValue"));
    auto *checkNow = window.findChild<QPushButton *>(QStringLiteral("checkNowButton"));
    QVERIFY(lastSync);
    QVERIFY(nextCheck);
    QVERIFY(connection);
    QVERIFY(lastError);
    QVERIFY(checkNow);

    QCOMPARE(connection->text(), QStringLiteral("Online"));
    QCOMPARE(lastSync->text(), QStringLiteral("Never"));
    QCOMPARE(lastError->text(), QStringLiteral("None"));

    QVERIFY(scheduler.start());
    QVERIFY(scheduler.lastSuccessEpoch() > 0);
    QVERIFY(nextCheck->text() != QStringLiteral("Not scheduled"));
    QVERIFY(nextCheck->text() != QStringLiteral("Deferred until online"));
    QVERIFY(lastSync->text() != QStringLiteral("Never"));
    QVERIFY(checkNow->isEnabled());

    monitor.setSimulatedState(ConnectivityMonitor::State::Offline);
    QCOMPARE(connection->text(), QStringLiteral("Offline"));
    QCOMPARE(nextCheck->text(), QStringLiteral("Deferred until online"));
    QVERIFY(!checkNow->isEnabled());

    monitor.setSimulatedState(ConnectivityMonitor::State::Online);
    QTRY_VERIFY(nextCheck->text() != QStringLiteral("Deferred until online"));
    QVERIFY(checkNow->isEnabled());
    QCOMPARE(connection->text(), QStringLiteral("Online"));

    scheduler.stop();
    QCOMPARE(nextCheck->text(), QStringLiteral("Not scheduled"));
    QVERIFY(!checkNow->isEnabled());
}

QTEST_MAIN(TestSettingsWindow)
#include "test_settings_window.moc"
