#pragma once

#include "gmail/GmailError.h"
#include "model/MessageSummary.h"

#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QUrl>

struct GmailListPage
{
    QList<MessageSummary> messages;
    QString nextPageToken;
    int resultSizeEstimate = 0;
};

// One page of users.history.list. `added` holds messages the history stream
// reports as added since startHistoryId; `historyId` is the cursor to continue
// from after this page.
struct GmailHistoryPage
{
    QList<MessageSummary> added;
    QString nextPageToken;
    qint64 historyId = 0;
};

class GmailClient : public QObject
{
    Q_OBJECT

public:
    explicit GmailClient(QNetworkAccessManager *network, QObject *parent = nullptr);

    void setAccessToken(const QString &accessToken);
    void setApiBaseUrl(const QUrl &baseUrl);
    QUrl apiBaseUrl() const { return m_apiBaseUrl; }

    // One request at a time. The caller advances pagination after listPageReady.
    void listMessages(const QString &query, int maxResults = 100,
                      const QString &pageToken = QString());
    void fetchMessage(const QString &messageId);

    // Incremental change feed: messages added to `labelId=INBOX` since the
    // stored historyId. Google returns HTTP 404 once the starting id has aged
    // out; the caller then falls back to a full listing.
    void listHistory(qint64 startHistoryId, int maxResults = 500,
                     const QString &pageToken = QString());

    // Minimal users.getProfile request used after authorization to learn the
    // connected Gmail address and historyId. Uses the same access token and
    // network manager.
    void fetchProfile();

signals:
    void historyPageReady(const GmailHistoryPage &page);
    void listPageReady(const GmailListPage &page);
    void messageReady(const MessageSummary &message);
    void profileReady(const QString &emailAddress, qint64 historyId);
    void requestFailed(const GmailError &error);

private:
    QNetworkRequest makeRequest(const QUrl &url) const;
    QUrl endpoint(const QString &suffix) const;
    void handleHistoryReply(QNetworkReply *reply);
    void handleListReply(QNetworkReply *reply);
    void handleMessageReply(QNetworkReply *reply);
    void handleProfileReply(QNetworkReply *reply);
    GmailError errorFromReply(QNetworkReply *reply, const QByteArray &body) const;

    QNetworkAccessManager *m_network;
    QString m_accessToken;
    QUrl m_apiBaseUrl;
};

Q_DECLARE_METATYPE(GmailListPage)
Q_DECLARE_METATYPE(GmailHistoryPage)
Q_DECLARE_METATYPE(GmailError)
