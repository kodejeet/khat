#pragma once

#include "sync/BackoffPolicy.h"
#include "sync/SyncService.h"

#include <QList>
#include <QObject>
#include <QSet>
#include <QTimer>

class SyncScheduler : public QObject
{
    Q_OBJECT

public:
    explicit SyncScheduler(SyncService *sync, QObject *parent = nullptr);

    // Returns false for intervals outside 5–1440 minutes.
    bool setIntervalMinutes(int minutes);
    int intervalMinutes() const { return m_intervalMinutes; }

    void setAccounts(const QList<SyncAccountRequest> &accounts);
    QList<SyncAccountRequest> accounts() const { return m_accounts; }

    bool start();
    void stop();
    bool checkNow();

    bool isRunning() const { return m_active; }
    bool isCheckRunning() const { return m_checkRunning; }
    qint64 lastCheckEpoch() const { return m_lastCheckEpoch; }
    qint64 lastSuccessEpoch() const { return m_lastSuccessEpoch; }
    qint64 nextCheckEpoch() const { return m_nextCheckEpoch; }
    QString lastError() const { return m_lastError; }
    QList<SyncResult> lastResults() const { return m_lastResults; }
    int backoffFailureCount() const { return m_backoff.failureCount(); }

signals:
    void checkStarted();
    void checkFinished(const QList<SyncResult> &results);
    void scheduleChanged();
    void runningChanged();

private slots:
    void runCheck();
    void handleSyncFinished(const SyncResult &result);

private:
    void finishCheck();
    void scheduleNext(qint64 delayMs);

    SyncService *m_sync;
    QTimer m_timer;
    QList<SyncAccountRequest> m_accounts;
    QSet<QString> m_pendingAccounts;
    QList<SyncResult> m_results;
    QList<SyncResult> m_lastResults;
    BackoffPolicy m_backoff;
    int m_intervalMinutes = 180;
    bool m_active = false;
    bool m_checkRunning = false;
    qint64 m_lastCheckEpoch = 0;
    qint64 m_lastSuccessEpoch = 0;
    qint64 m_nextCheckEpoch = 0;
    QString m_lastError;
};

Q_DECLARE_METATYPE(QList<SyncResult>)
