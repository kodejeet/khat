#include "sync/SyncService.h"

#include "gmail/GmailClient.h"
#include "gmail/GmailQuery.h"
#include "state/StateDatabase.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QSet>

namespace {
constexpr int kPageSize = 100;
constexpr int kMaxPages = 1000;
constexpr qint64 kOverlapSeconds = 24 * 60 * 60;

QString labelFilterError(const MessageSummary &message, bool includeUpdates)
{
    if (!message.labelIds.isEmpty()) {
        if (!message.labelIds.contains(QStringLiteral("INBOX"))) {
            return QStringLiteral("not in inbox");
        }
        if (message.labelIds.contains(QStringLiteral("SPAM"))) {
            return QStringLiteral("spam");
        }
        if (message.labelIds.contains(QStringLiteral("TRASH"))) {
            return QStringLiteral("trash");
        }
    }

    const QStringList excludedCategories = {
        QStringLiteral("CATEGORY_PROMOTIONS"),
        QStringLiteral("CATEGORY_SOCIAL"),
        QStringLiteral("CATEGORY_FORUMS"),
    };
    for (const QString &label : excludedCategories) {
        if (message.labelIds.contains(label)) {
            return QStringLiteral("excluded category");
        }
    }
    if (!includeUpdates && message.labelIds.contains(QStringLiteral("CATEGORY_UPDATES"))) {
        return QStringLiteral("updates disabled");
    }
    return QString();
}
} // namespace

struct SyncService::Job
{
    enum class Stage
    {
        Listing,
        Metadata,
    };

    SyncAccountRequest request;
    GmailClient *client = nullptr;
    Stage stage = Stage::Listing;
    bool baseline = false;
    qint64 lastSyncEpoch = 0;
    QString pageToken;
    QSet<QString> seenPageTokens;
    QList<MessageSummary> listedMessages;
    QSet<QString> listedIds;
    QList<MessageSummary> candidates;
    QList<MessageSummary> fetchedMessages;
    int metadataIndex = 0;
    int pagesFetched = 0;
};

SyncService::SyncService(StateDatabase *state, QNetworkAccessManager *network,
                         QObject *parent)
    : QObject(parent)
    , m_state(state)
    , m_network(network)
    , m_apiBaseUrl(QStringLiteral("https://gmail.googleapis.com/gmail/v1"))
{
    qRegisterMetaType<QList<MessageSummary>>("QList<MessageSummary>");
    qRegisterMetaType<SyncResult>("SyncResult");
}

SyncService::~SyncService()
{
    for (Job *job : std::as_const(m_jobs)) {
        delete job->client;
        delete job;
    }
    m_jobs.clear();
}

void SyncService::setApiBaseUrl(const QUrl &baseUrl)
{
    m_apiBaseUrl = baseUrl;
}

bool SyncService::isSyncing(const QString &accountId) const
{
    return m_jobs.contains(accountId);
}

bool SyncService::syncAccount(const SyncAccountRequest &request, QString *error)
{
    if (request.accountId.isEmpty() || request.email.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Account ID and email are required.");
        }
        return false;
    }
    if (request.accessToken.isEmpty()) {
        if (error) {
            *error = QStringLiteral("No access token is available for this account.");
        }
        return false;
    }
    if (!m_state || !m_state->isOpen()) {
        if (error) {
            *error = QStringLiteral("State database is not open.");
        }
        return false;
    }
    if (m_jobs.contains(request.accountId)) {
        if (error) {
            *error = QStringLiteral("A sync is already running for this account.");
        }
        return false;
    }

    QString databaseError;
    if (!m_state->ensureAccount(request.accountId, request.email, &databaseError)) {
        if (error) {
            *error = databaseError;
        }
        return false;
    }

    AccountState state;
    if (!m_state->accountState(request.accountId, &state, &databaseError)) {
        if (error) {
            *error = databaseError;
        }
        return false;
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!m_state->markSyncAttempt(request.accountId, now, &databaseError)) {
        if (error) {
            *error = databaseError;
        }
        return false;
    }

    auto *job = new Job;
    job->request = request;
    job->baseline = !state.baselineDone;
    job->lastSyncEpoch = state.lastSyncEpoch;
    job->client = new GmailClient(m_network, this);
    job->client->setAccessToken(request.accessToken);
    job->client->setApiBaseUrl(m_apiBaseUrl);

    connect(job->client, &GmailClient::listPageReady, this,
            [this, job](const GmailListPage &page) { handleListPage(job, page); });
    connect(job->client, &GmailClient::messageReady, this,
            [this, job](const MessageSummary &message) { handleMetadata(job, message); });
    connect(job->client, &GmailClient::requestFailed, this,
            [this, job](const GmailError &requestError) {
                if (job->stage == Job::Stage::Metadata) {
                    handleMetadataError(job, requestError);
                } else {
                    handleListError(job, requestError);
                }
            });

    m_jobs.insert(request.accountId, job);
    requestNextListPage(job);
    return true;
}

void SyncService::requestNextListPage(Job *job)
{
    if (!m_jobs.contains(job->request.accountId)
        || m_jobs.value(job->request.accountId) != job) {
        return;
    }

    QDate afterDate;
    if (!job->baseline && job->lastSyncEpoch > 0) {
        afterDate = QDateTime::fromSecsSinceEpoch(job->lastSyncEpoch - kOverlapSeconds).date();
    }
    job->client->listMessages(
        buildGmailQuery(job->request.includeUpdates, afterDate), kPageSize, job->pageToken);
}

void SyncService::handleListPage(Job *job, const GmailListPage &page)
{
    if (!m_jobs.contains(job->request.accountId)
        || m_jobs.value(job->request.accountId) != job) {
        return;
    }

    job->pagesFetched++;
    for (const MessageSummary &message : page.messages) {
        if (!job->listedIds.contains(message.id)) {
            job->listedIds.insert(message.id);
            job->listedMessages.append(message);
        }
    }

    if (!page.nextPageToken.isEmpty()) {
        if (job->pagesFetched >= kMaxPages
            || job->seenPageTokens.contains(page.nextPageToken)) {
            failJob(job, GmailErrorKind::InvalidResponse,
                    QStringLiteral("Gmail pagination did not terminate."));
            return;
        }
        job->seenPageTokens.insert(page.nextPageToken);
        job->pageToken = page.nextPageToken;
        requestNextListPage(job);
        return;
    }

    if (job->baseline) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QString databaseError;
        if (!m_state->applyBaseline(job->request.accountId, job->listedMessages, now,
                                    &databaseError)) {
            failJob(job, GmailErrorKind::Internal, databaseError);
            return;
        }
        SyncResult result;
        result.accountId = job->request.accountId;
        result.success = true;
        result.baseline = true;
        result.pagesFetched = job->pagesFetched;
        completeJob(job, result);
        return;
    }

    // Only fetch metadata for messages that have not been recorded yet.
    for (const MessageSummary &message : job->listedMessages) {
        bool seen = false;
        QString databaseError;
        if (!m_state->hasSeenMessage(job->request.accountId, message.id, &seen,
                                     &databaseError)) {
            failJob(job, GmailErrorKind::Internal, databaseError);
            return;
        }
        if (!seen) {
            job->candidates.append(message);
        }
    }

    if (job->candidates.isEmpty()) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QString databaseError;
        if (!m_state->markSyncSuccess(job->request.accountId, now, &databaseError)) {
            failJob(job, GmailErrorKind::Internal, databaseError);
            return;
        }
        SyncResult result;
        result.accountId = job->request.accountId;
        result.success = true;
        result.pagesFetched = job->pagesFetched;
        completeJob(job, result);
        return;
    }

    beginMetadataPhase(job);
}

void SyncService::handleListError(Job *job, const GmailError &error)
{
    failJob(job, error.kind, error.message);
}

void SyncService::beginMetadataPhase(Job *job)
{
    job->stage = Job::Stage::Metadata;
    job->metadataIndex = 0;
    job->fetchedMessages.clear();
    requestNextMetadata(job);
}

void SyncService::requestNextMetadata(Job *job)
{
    if (!m_jobs.contains(job->request.accountId)
        || m_jobs.value(job->request.accountId) != job) {
        return;
    }
    if (job->metadataIndex >= job->candidates.size()) {
        finalizeMetadata(job);
        return;
    }
    job->client->fetchMessage(job->candidates.at(job->metadataIndex).id);
}

void SyncService::handleMetadata(Job *job, const MessageSummary &message)
{
    if (!m_jobs.contains(job->request.accountId)
        || m_jobs.value(job->request.accountId) != job) {
        return;
    }

    // The Gmail list query already filters, but re-check labels from the fetched
    // message so a concurrently archived/categorized message cannot notify.
    const QString filterReason = labelFilterError(message, job->request.includeUpdates);
    if (filterReason.isEmpty()) {
        job->fetchedMessages.append(message);
    }

    job->metadataIndex++;
    requestNextMetadata(job);
}

void SyncService::handleMetadataError(Job *job, const GmailError &error)
{
    if (error.kind == GmailErrorKind::NotFound) {
        // The message disappeared between listing and metadata fetch.
        job->metadataIndex++;
        requestNextMetadata(job);
        return;
    }
    failJob(job, error.kind, error.message);
}

void SyncService::finalizeMetadata(Job *job)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QList<MessageSummary> inserted;
    QString databaseError;
    if (!m_state->recordMessages(job->request.accountId, job->fetchedMessages, now,
                                 &inserted, &databaseError)) {
        failJob(job, GmailErrorKind::Internal, databaseError);
        return;
    }
    if (!m_state->markSyncSuccess(job->request.accountId, now, &databaseError)) {
        failJob(job, GmailErrorKind::Internal, databaseError);
        return;
    }

    SyncResult result;
    result.accountId = job->request.accountId;
    result.success = true;
    result.baseline = false;
    result.newMessageCount = inserted.size();
    result.pagesFetched = job->pagesFetched;

    if (!inserted.isEmpty()) {
        emit newMessages(job->request.accountId, inserted);
    }
    completeJob(job, result);
}

void SyncService::failJob(Job *job, GmailErrorKind kind, const QString &message)
{
    if (!m_jobs.contains(job->request.accountId)
        || m_jobs.value(job->request.accountId) != job) {
        return;
    }

    QString databaseError;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!m_state->markSyncError(job->request.accountId, now, message, &databaseError)) {
        // Keep the original error; the status is still visible in the result.
    }

    SyncResult result;
    result.accountId = job->request.accountId;
    result.success = false;
    result.baseline = job->baseline;
    result.pagesFetched = job->pagesFetched;
    result.errorKind = kind;
    result.error = message;
    completeJob(job, result);
}

void SyncService::completeJob(Job *job, const SyncResult &result)
{
    const QString accountId = job->request.accountId;
    if (!m_jobs.contains(accountId) || m_jobs.value(accountId) != job) {
        return;
    }
    m_jobs.remove(accountId);
    job->client->deleteLater();
    delete job;

    emit syncFinished(result);
}
