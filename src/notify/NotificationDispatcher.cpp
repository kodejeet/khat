#include "notify/NotificationDispatcher.h"

#include "notify/NotificationContent.h"
#include "notify/NotificationService.h"
#include "state/StateDatabase.h"
#include "sync/SyncService.h"

NotificationDispatcher::NotificationDispatcher(SyncService *sync,
                                               NotificationService *service,
                                               StateDatabase *state, QObject *parent)
    : QObject(parent)
    , m_sync(sync)
    , m_service(service)
    , m_state(state)
{
    if (m_sync) {
        connect(m_sync, &SyncService::newMessages, this,
                &NotificationDispatcher::handleNewMessages);
    }
}

void NotificationDispatcher::handleNewMessages(const QString &accountId,
                                               const QList<MessageSummary> &messages)
{
    processMessages(accountId, messages);
}

void NotificationDispatcher::processMessages(const QString &accountId,
                                             const QList<MessageSummary> &messages)
{
    if (!m_config.enabled || !m_service || !m_state) {
        return;
    }

    AccountState account;
    QString error;
    if (!m_state->accountState(accountId, &account, &error)) {
        for (const MessageSummary &message : messages) {
            fail(accountId, message.id, error);
        }
        return;
    }

    QList<AccountState> accounts = m_state->accountStates(&error);
    const bool showAccount = accounts.size() > 1;

    for (const MessageSummary &message : messages) {
        if (message.id.isEmpty()) {
            fail(accountId, QString(), QStringLiteral("Message has no ID."));
            continue;
        }

        bool alreadyNotified = false;
        if (!m_state->messageWasNotified(accountId, message.id, &alreadyNotified, &error)) {
            fail(accountId, message.id, error);
            continue;
        }
        if (alreadyNotified) {
            continue;
        }

        const NotificationContent content = buildNotificationContent(
            message, account.email, m_config.showPreview, showAccount);
        if (!m_service->notify(content, &error)) {
            fail(accountId, message.id, error);
            continue;
        }

        // Delivery was accepted. Marking only after the backend accepts keeps
        // failed notifications retryable if a message is ever re-emitted, while
        // the seen_messages row still guarantees at-most-once across restarts.
        if (!m_state->markNotified(accountId, message.id, &error)) {
            fail(accountId, message.id, error);
            continue;
        }

        emit notificationDelivered(accountId, message.id);
    }
}

void NotificationDispatcher::fail(const QString &accountId, const QString &messageId,
                                  const QString &error)
{
    emit notificationFailed(accountId, messageId, error);
}
