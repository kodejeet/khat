#include "gmail/GmailClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrlQuery>

namespace {
constexpr int kMaxErrorBodyBytes = 64 * 1024;

QString headerValue(const QJsonObject &payload, const QString &name)
{
    const QJsonArray headers = payload.value(QStringLiteral("headers")).toArray();
    for (const QJsonValue &value : headers) {
        const QJsonObject header = value.toObject();
        if (header.value(QStringLiteral("name")).toString().compare(name,
                                                                    Qt::CaseInsensitive)
            == 0) {
            return header.value(QStringLiteral("value")).toString();
        }
    }
    return QString();
}

QStringList errorReasons(const QByteArray &body)
{
    QStringList reasons;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return reasons;
    }
    const QJsonArray errors = document.object()
                                  .value(QStringLiteral("error"))
                                  .toObject()
                                  .value(QStringLiteral("errors"))
                                  .toArray();
    for (const QJsonValue &value : errors) {
        const QString reason = value.toObject().value(QStringLiteral("reason")).toString();
        if (!reason.isEmpty()) {
            reasons.append(reason);
        }
    }
    return reasons;
}
} // namespace

GmailClient::GmailClient(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent)
    , m_network(network)
    , m_apiBaseUrl(QStringLiteral("https://gmail.googleapis.com/gmail/v1"))
{
    qRegisterMetaType<GmailListPage>("GmailListPage");
    qRegisterMetaType<MessageSummary>("MessageSummary");
    qRegisterMetaType<GmailError>("GmailError");
}

void GmailClient::setAccessToken(const QString &accessToken)
{
    m_accessToken = accessToken;
}

void GmailClient::setApiBaseUrl(const QUrl &baseUrl)
{
    m_apiBaseUrl = baseUrl;
}

QUrl GmailClient::endpoint(const QString &suffix) const
{
    QUrl url = m_apiBaseUrl;
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    url.setPath(path + QLatin1Char('/') + suffix);
    return url;
}

QNetworkRequest GmailClient::makeRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "gmail-notifier/0.1");
    if (!m_accessToken.isEmpty()) {
        request.setRawHeader("Authorization",
                             QByteArrayLiteral("Bearer ") + m_accessToken.toUtf8());
    }
    request.setTransferTimeout(45'000);
    return request;
}

void GmailClient::listMessages(const QString &query, int maxResults,
                               const QString &pageToken)
{
    if (m_accessToken.isEmpty()) {
        emit requestFailed({GmailErrorKind::NoToken, 0, 0,
                            QStringLiteral("No Gmail access token is available.")});
        return;
    }

    QUrl url = endpoint(QStringLiteral("users/me/messages"));
    QUrlQuery urlQuery;
    urlQuery.addQueryItem(QStringLiteral("q"), query);
    urlQuery.addQueryItem(QStringLiteral("maxResults"),
                          QString::number(qBound(1, maxResults, 500)));
    urlQuery.addQueryItem(QStringLiteral("fields"),
                          QStringLiteral("messages(id,threadId),nextPageToken,"
                                         "resultSizeEstimate"));
    if (!pageToken.isEmpty()) {
        urlQuery.addQueryItem(QStringLiteral("pageToken"), pageToken);
    }
    url.setQuery(urlQuery);

    QNetworkReply *reply = m_network->get(makeRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleListReply(reply);
        reply->deleteLater();
    });
}

void GmailClient::fetchMessage(const QString &messageId)
{
    if (m_accessToken.isEmpty()) {
        emit requestFailed({GmailErrorKind::NoToken, 0, 0,
                            QStringLiteral("No Gmail access token is available.")});
        return;
    }

    const QString encodedId = QString::fromLatin1(QUrl::toPercentEncoding(messageId));
    QUrl url = endpoint(QStringLiteral("users/me/messages/") + encodedId);
    QUrlQuery urlQuery;
    urlQuery.addQueryItem(QStringLiteral("format"), QStringLiteral("metadata"));
    urlQuery.addQueryItem(QStringLiteral("metadataHeaders"), QStringLiteral("From"));
    urlQuery.addQueryItem(QStringLiteral("metadataHeaders"), QStringLiteral("Subject"));
    urlQuery.addQueryItem(QStringLiteral("fields"),
                          QStringLiteral("id,threadId,internalDate,labelIds,snippet,"
                                         "payload/headers"));
    url.setQuery(urlQuery);

    QNetworkReply *reply = m_network->get(makeRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleMessageReply(reply);
        reply->deleteLater();
    });
}

GmailError GmailClient::errorFromReply(QNetworkReply *reply, const QByteArray &body) const
{
    GmailError error;
    error.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    error.retryAfterSeconds = reply->rawHeader("Retry-After").toInt();

    if (reply->error() != QNetworkReply::NoError && error.httpStatus == 0) {
        error.kind = GmailErrorKind::Network;
        error.message = QStringLiteral("Network error: %1").arg(reply->errorString());
        return error;
    }

    const QStringList reasons = errorReasons(body);
    const QString reasonText = reasons.isEmpty()
        ? QString()
        : QStringLiteral(" (%1)").arg(reasons.join(QStringLiteral(", ")));

    switch (error.httpStatus) {
    case 400:
        error.kind = GmailErrorKind::InvalidRequest;
        error.message = QStringLiteral("Gmail API rejected the request (HTTP 400)%1.")
                            .arg(reasonText);
        break;
    case 401:
        error.kind = GmailErrorKind::Unauthorized;
        error.message = QStringLiteral("Gmail authorization expired or was revoked (HTTP 401).");
        break;
    case 403:
        if (reasons.contains(QStringLiteral("rateLimitExceeded"))
            || reasons.contains(QStringLiteral("userRateLimitExceeded"))) {
            error.kind = GmailErrorKind::RateLimited;
            error.message = QStringLiteral("Gmail API rate limit reached (HTTP 403)%1.")
                                .arg(reasonText);
        } else if (reasons.contains(QStringLiteral("quotaExceeded"))
                   || reasons.contains(QStringLiteral("dailyLimitExceeded"))) {
            error.kind = GmailErrorKind::QuotaExceeded;
            error.message = QStringLiteral("Gmail API quota exceeded (HTTP 403)%1.")
                                .arg(reasonText);
        } else {
            error.kind = GmailErrorKind::Forbidden;
            error.message = QStringLiteral("Gmail API access denied (HTTP 403)%1.")
                                .arg(reasonText);
        }
        break;
    case 404:
        error.kind = GmailErrorKind::NotFound;
        error.message = QStringLiteral("Gmail message no longer exists (HTTP 404).");
        break;
    case 429:
        error.kind = GmailErrorKind::RateLimited;
        error.message = QStringLiteral("Gmail API rate limit reached (HTTP 429).");
        break;
    default:
        if (error.httpStatus >= 500 && error.httpStatus <= 599) {
            error.kind = GmailErrorKind::Server;
            error.message = QStringLiteral("Gmail API server error (HTTP %1).")
                                .arg(error.httpStatus);
        } else if (error.httpStatus >= 200 && error.httpStatus <= 299) {
            error.kind = GmailErrorKind::InvalidResponse;
            error.message = QStringLiteral("Gmail API returned an invalid response.");
        } else {
            error.kind = GmailErrorKind::InvalidResponse;
            error.message = QStringLiteral("Unexpected Gmail API response (HTTP %1).")
                                .arg(error.httpStatus);
        }
        break;
    }
    return error;
}

void GmailClient::handleListReply(QNetworkReply *reply)
{
    const QByteArray body = reply->readAll().left(kMaxErrorBodyBytes);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError || status < 200 || status > 299) {
        emit requestFailed(errorFromReply(reply, body));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit requestFailed({GmailErrorKind::InvalidResponse, status, 0,
                            QStringLiteral("Gmail API returned malformed message-list JSON.")});
        return;
    }

    const QJsonObject root = document.object();
    GmailListPage page;
    const QJsonValue messagesValue = root.value(QStringLiteral("messages"));
    if (!messagesValue.isUndefined()) {
        if (!messagesValue.isArray()) {
            emit requestFailed({GmailErrorKind::InvalidResponse, status, 0,
                                QStringLiteral("Gmail message list was not an array.")});
            return;
        }
        const QJsonArray messages = messagesValue.toArray();
        for (const QJsonValue &value : messages) {
            const QJsonObject object = value.toObject();
            const QString id = object.value(QStringLiteral("id")).toString();
            if (id.isEmpty()) {
                emit requestFailed({GmailErrorKind::InvalidResponse, status, 0,
                                    QStringLiteral("Gmail message list contains a message "
                                                   "without an ID.")});
                return;
            }
            MessageSummary summary;
            summary.id = id;
            summary.threadId = object.value(QStringLiteral("threadId")).toString();
            page.messages.append(summary);
        }
    }

    page.nextPageToken = root.value(QStringLiteral("nextPageToken")).toString();
    page.resultSizeEstimate = root.value(QStringLiteral("resultSizeEstimate")).toInt();
    emit listPageReady(page);
}

void GmailClient::handleMessageReply(QNetworkReply *reply)
{
    const QByteArray body = reply->readAll().left(kMaxErrorBodyBytes);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError || status < 200 || status > 299) {
        emit requestFailed(errorFromReply(reply, body));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit requestFailed({GmailErrorKind::InvalidResponse, status, 0,
                            QStringLiteral("Gmail API returned malformed message JSON.")});
        return;
    }

    const QJsonObject root = document.object();
    MessageSummary message;
    message.id = root.value(QStringLiteral("id")).toString();
    if (message.id.isEmpty()) {
        emit requestFailed({GmailErrorKind::InvalidResponse, status, 0,
                            QStringLiteral("Gmail API returned a message without an ID.")});
        return;
    }
    message.threadId = root.value(QStringLiteral("threadId")).toString();
    const QJsonValue internalDate = root.value(QStringLiteral("internalDate"));
    if (internalDate.isString()) {
        message.internalDate = internalDate.toString().toLongLong();
    } else if (internalDate.isDouble()) {
        message.internalDate = static_cast<qint64>(internalDate.toDouble());
    }
    message.snippet = root.value(QStringLiteral("snippet")).toString();

    const QJsonArray labels = root.value(QStringLiteral("labelIds")).toArray();
    for (const QJsonValue &label : labels) {
        if (label.isString()) {
            message.labelIds.append(label.toString());
        }
    }

    const QJsonObject payload = root.value(QStringLiteral("payload")).toObject();
    message.sender = headerValue(payload, QStringLiteral("From"));
    message.subject = headerValue(payload, QStringLiteral("Subject"));

    emit messageReady(message);
}
