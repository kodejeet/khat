#pragma once

#include "config/AppConfig.h"
#include "model/MessageSummary.h"

#include <QObject>

class NotificationService;
class StateDatabase;
class SyncService;

// Delivers notifications for newly recorded Gmail messages and records the
// at-most-once notification bookkeeping in the state database.
class NotificationDispatcher : public QObject
{
    Q_OBJECT

public:
    NotificationDispatcher(SyncService *sync, NotificationService *service,
                           StateDatabase *state, QObject *parent = nullptr);

    void setConfig(const NotificationConfig &config) { m_config = config; }
    NotificationConfig config() const { return m_config; }

    // Public for deterministic tests; normally invoked by SyncService.
    void processMessages(const QString &accountId, const QList<MessageSummary> &messages);

signals:
    void notificationDelivered(const QString &accountId, const QString &messageId);
    void notificationFailed(const QString &accountId, const QString &messageId,
                            const QString &error);

private slots:
    void handleNewMessages(const QString &accountId,
                           const QList<MessageSummary> &messages);

private:
    void fail(const QString &accountId, const QString &messageId, const QString &error);

    SyncService *m_sync;
    NotificationService *m_service;
    StateDatabase *m_state;
    NotificationConfig m_config = defaultAppConfig().notifications;
};
