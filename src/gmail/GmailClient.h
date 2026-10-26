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

    // Minimal users.getProfile request used after authorization to learn the
    // connected Gmail address. Uses the same access token and network manager.
    void fetchProfile();

signals:
    void listPageReady(const GmailListPage &page);
    void messageReady(const MessageSummary &message);
    void profileReady(const QString &emailAddress);
    void requestFailed(const GmailError &error);

private:
    QNetworkRequest makeRequest(const QUrl &url) const;
    QUrl endpoint(const QString &suffix) const;
    void handleListReply(QNetworkReply *reply);
    void handleMessageReply(QNetworkReply *reply);
    void handleProfileReply(QNetworkReply *reply);
    GmailError errorFromReply(QNetworkReply *reply, const QByteArray &body) const;

    QNetworkAccessManager *m_network;
    QString m_accessToken;
    QUrl m_apiBaseUrl;
};

Q_DECLARE_METATYPE(GmailListPage)
Q_DECLARE_METATYPE(GmailError)
