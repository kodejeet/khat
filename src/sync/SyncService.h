#pragma once

#include "gmail/GmailError.h"
#include "model/MessageSummary.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QUrl>

class GmailListPage;
class GmailClient;
class QNetworkAccessManager;
class StateDatabase;

struct SyncAccountRequest
{
    QString accountId;
    QString email;
    QString accessToken;
    bool includeUpdates = true;
};

struct SyncResult
{
    QString accountId;
    bool success = false;
    bool baseline = false;
    int newMessageCount = 0;
    int pagesFetched = 0;
    int retryAfterSeconds = 0;
    GmailErrorKind errorKind = GmailErrorKind::None;
    QString error;
};

class SyncService : public QObject
{
    Q_OBJECT

public:
    explicit SyncService(StateDatabase *state, QNetworkAccessManager *network,
                         QObject *parent = nullptr);
    ~SyncService() override;

    void setApiBaseUrl(const QUrl &baseUrl);
    QUrl apiBaseUrl() const { return m_apiBaseUrl; }

    // Starts an asynchronous sync. Returns false if the same account is already
    // syncing, the database is unavailable, or the request is incomplete.
    bool syncAccount(const SyncAccountRequest &request, QString *error = nullptr);
    bool isSyncing(const QString &accountId) const;
    int activeSyncs() const { return m_jobs.size(); }

signals:
    void newMessages(const QString &accountId, const QList<MessageSummary> &messages);
    void syncFinished(const SyncResult &result);

private:
    struct Job;

    void requestNextListPage(Job *job);
    void handleListPage(Job *job, const GmailListPage &page);
    void handleListError(Job *job, const GmailError &error);
    void beginMetadataPhase(Job *job);
    void requestNextMetadata(Job *job);
    void handleMetadata(Job *job, const MessageSummary &message);
    void handleMetadataError(Job *job, const GmailError &error);
    void finalizeMetadata(Job *job);
    void failJob(Job *job, GmailErrorKind kind, const QString &message,
                 int retryAfterSeconds = 0);
    void completeJob(Job *job, const SyncResult &result);

    StateDatabase *m_state;
    QNetworkAccessManager *m_network;
    QUrl m_apiBaseUrl;
    QHash<QString, Job *> m_jobs;
};

Q_DECLARE_METATYPE(SyncResult)
