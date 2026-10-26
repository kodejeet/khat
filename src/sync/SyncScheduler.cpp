#include "sync/SyncScheduler.h"

#include <QDateTime>
#include <QSet>

#include <utility>

namespace {
constexpr int kMinIntervalMinutes = 5;
constexpr int kMaxIntervalMinutes = 24 * 60;
constexpr qint64 kMinuteMs = 60 * 1000;
} // namespace

SyncScheduler::SyncScheduler(SyncService *sync, QObject *parent)
    : QObject(parent)
    , m_sync(sync)
{
    qRegisterMetaType<QList<SyncResult>>("QList<SyncResult>");
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &SyncScheduler::runCheck);
    if (m_sync) {
        connect(m_sync, &SyncService::syncFinished, this,
                &SyncScheduler::handleSyncFinished);
    }
}

bool SyncScheduler::setIntervalMinutes(int minutes)
{
    if (minutes < kMinIntervalMinutes || minutes > kMaxIntervalMinutes) {
        return false;
    }
    m_intervalMinutes = minutes;
    if (m_active && !m_checkRunning && m_timer.isActive()) {
        scheduleNext(static_cast<qint64>(m_intervalMinutes) * kMinuteMs);
    }
    return true;
}

void SyncScheduler::setAccounts(const QList<SyncAccountRequest> &accounts)
{
    QList<SyncAccountRequest> deduped;
    QSet<QString> ids;
    for (const SyncAccountRequest &account : accounts) {
        if (account.accountId.isEmpty() || ids.contains(account.accountId)) {
            continue;
        }
        ids.insert(account.accountId);
        deduped.append(account);
    }
    m_accounts = deduped;
}

bool SyncScheduler::start()
{
    if (m_active || !m_sync) {
        return false;
    }
    m_active = true;
    m_backoff.reset();
    emit runningChanged();
    runCheck();
    return true;
}

void SyncScheduler::stop()
{
    const bool wasActive = m_active;
    m_active = false;
    m_timer.stop();
    m_nextCheckEpoch = 0;
    if (wasActive) {
        emit runningChanged();
        emit scheduleChanged();
    }
}

bool SyncScheduler::checkNow()
{
    if (!m_active || m_checkRunning) {
        return false;
    }
    m_timer.stop();
    runCheck();
    return true;
}

void SyncScheduler::runCheck()
{
    if (!m_active || m_checkRunning || !m_sync) {
        return;
    }

    m_timer.stop();
    m_checkRunning = true;
    m_pendingAccounts.clear();
    m_results.clear();
    m_lastCheckEpoch = QDateTime::currentSecsSinceEpoch();
    m_nextCheckEpoch = 0;
    m_lastError.clear();
    emit scheduleChanged();
    emit checkStarted();

    if (m_accounts.isEmpty()) {
        finishCheck();
        return;
    }

    for (const SyncAccountRequest &account : std::as_const(m_accounts)) {
        QString startError;
        if (m_sync->syncAccount(account, &startError)) {
            m_pendingAccounts.insert(account.accountId);
            continue;
        }

        SyncResult failure;
        failure.accountId = account.accountId;
        failure.success = false;
        failure.error = startError;
        failure.errorKind = account.accessToken.isEmpty()
            ? GmailErrorKind::NoToken
            : GmailErrorKind::Internal;
        m_results.append(failure);
        if (m_lastError.isEmpty()) {
            m_lastError = startError;
        }
    }

    if (m_pendingAccounts.isEmpty()) {
        finishCheck();
    }
}

void SyncScheduler::handleSyncFinished(const SyncResult &result)
{
    if (!m_checkRunning || !m_pendingAccounts.contains(result.accountId)) {
        return;
    }
    m_pendingAccounts.remove(result.accountId);
    m_results.append(result);
    if (m_pendingAccounts.isEmpty()) {
        finishCheck();
    }
}

void SyncScheduler::finishCheck()
{
    if (!m_checkRunning) {
        return;
    }
    m_checkRunning = false;
    m_lastResults = m_results;

    bool allSuccess = true;
    int retryAfter = 0;
    for (const SyncResult &result : std::as_const(m_results)) {
        if (!result.success) {
            allSuccess = false;
            retryAfter = qMax(retryAfter, result.retryAfterSeconds);
            if (m_lastError.isEmpty()) {
                m_lastError = result.error;
            }
        }
    }

    if (allSuccess) {
        m_backoff.reset();
        m_lastSuccessEpoch = QDateTime::currentSecsSinceEpoch();
        m_lastError.clear();
        scheduleNext(static_cast<qint64>(m_intervalMinutes) * kMinuteMs);
    } else {
        scheduleNext(m_backoff.nextDelayMs(retryAfter));
    }

    emit checkFinished(m_lastResults);
}

void SyncScheduler::scheduleNext(qint64 delayMs)
{
    if (!m_active) {
        m_nextCheckEpoch = 0;
        emit scheduleChanged();
        return;
    }
    delayMs = qBound<qint64>(1000, delayMs, 2'147'483'647LL);
    m_nextCheckEpoch = QDateTime::currentSecsSinceEpoch() + qMax<qint64>(1, delayMs / 1000);
    m_timer.start(static_cast<int>(delayMs));
    emit scheduleChanged();
}
