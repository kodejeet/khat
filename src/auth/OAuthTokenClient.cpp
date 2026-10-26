#include "auth/OAuthTokenClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace {
constexpr int kMaxErrorBodyBytes = 64 * 1024;
constexpr int kTransferTimeoutMs = 45'000;

QString safeDescription(const QString &description)
{
    return description.simplified().left(200);
}
} // namespace

OAuthTokenClient::OAuthTokenClient(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent)
    , m_network(network)
{
    qRegisterMetaType<OAuthToken>("OAuthToken");
    qRegisterMetaType<OAuthError>("OAuthError");
}

void OAuthTokenClient::setClientConfig(const OAuthClientConfig &config)
{
    m_config = config;
}

QNetworkRequest OAuthTokenClient::makeRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Content-Type", "application/x-www-form-urlencoded");
    request.setRawHeader("User-Agent", "gmail-notifier/0.1");
    request.setTransferTimeout(kTransferTimeoutMs);
    return request;
}

void OAuthTokenClient::exchangeAuthorizationCode(const QString &requestId,
                                                 const QString &code,
                                                 const QString &codeVerifier,
                                                 const QString &redirectUri)
{
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    form.addQueryItem(QStringLiteral("code"), code);
    form.addQueryItem(QStringLiteral("client_id"), m_config.clientId);
    form.addQueryItem(QStringLiteral("client_secret"), m_config.clientSecret);
    form.addQueryItem(QStringLiteral("redirect_uri"), redirectUri);
    form.addQueryItem(QStringLiteral("code_verifier"), codeVerifier);
    postForm(m_config.tokenUri, form, requestId, false);
}

void OAuthTokenClient::refreshAccessToken(const QString &requestId,
                                          const QString &refreshToken)
{
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    form.addQueryItem(QStringLiteral("refresh_token"), refreshToken);
    form.addQueryItem(QStringLiteral("client_id"), m_config.clientId);
    form.addQueryItem(QStringLiteral("client_secret"), m_config.clientSecret);
    postForm(m_config.tokenUri, form, requestId, false);
}

void OAuthTokenClient::revokeToken(const QString &requestId, const QString &token,
                                   const QString &tokenTypeHint)
{
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("token"), token);
    if (!tokenTypeHint.isEmpty()) {
        form.addQueryItem(QStringLiteral("token_type_hint"), tokenTypeHint);
    }
    postForm(m_config.revokeUri, form, requestId, true);
}

void OAuthTokenClient::postForm(const QUrl &url, const QUrlQuery &form,
                                const QString &requestId, bool revoke)
{
    if (m_network == nullptr) {
        OAuthError error;
        error.kind = OAuthErrorKind::Network;
        error.message = QStringLiteral("No network manager is available.");
        emit requestFailed(requestId, error);
        return;
    }

    const QByteArray body = form.toString(QUrl::FullyEncoded).toUtf8();
    QNetworkReply *reply = m_network->post(makeRequest(url), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId, revoke] {
        handleReply(reply, requestId, revoke);
        reply->deleteLater();
    });
}

OAuthError OAuthTokenClient::errorFromReply(QNetworkReply *reply,
                                            const QByteArray &body) const
{
    OAuthError error;
    error.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError && error.httpStatus == 0) {
        error.kind = OAuthErrorKind::Network;
        error.message = QStringLiteral("Could not reach the Google OAuth endpoint: %1")
                            .arg(reply->errorString());
        return error;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
        const QJsonObject root = document.object();
        error.code = root.value(QStringLiteral("error")).toString();
        const QString description =
            safeDescription(root.value(QStringLiteral("error_description")).toString());
        if (error.code == QStringLiteral("invalid_grant")) {
            error.kind = OAuthErrorKind::InvalidGrant;
            error.message = QStringLiteral("Google rejected the saved authorization "
                                           "(invalid_grant). Reauthorize the account.");
            return error;
        }
        if (!description.isEmpty()) {
            error.message = description;
        }
    }

    if (error.httpStatus >= 500) {
        error.kind = OAuthErrorKind::Server;
        if (error.message.isEmpty()) {
            error.message = QStringLiteral("Google OAuth server error (HTTP %1).")
                                .arg(error.httpStatus);
        }
    } else {
        error.kind = OAuthErrorKind::InvalidRequest;
        if (error.message.isEmpty()) {
            error.message = QStringLiteral("Google rejected the OAuth request (HTTP %1).")
                                .arg(error.httpStatus);
        }
    }
    return error;
}

void OAuthTokenClient::handleReply(QNetworkReply *reply, const QString &requestId,
                                   bool revoke)
{
    const QByteArray body = reply->readAll().left(kMaxErrorBodyBytes);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool success = reply->error() == QNetworkReply::NoError && status >= 200
        && status <= 299;

    if (revoke) {
        if (!success) {
            OAuthError error = errorFromReply(reply, body);
            error.kind = error.kind == OAuthErrorKind::InvalidGrant
                ? OAuthErrorKind::Revoked
                : error.kind;
            emit requestFailed(requestId, error);
            return;
        }
        emit tokenRevoked(requestId);
        return;
    }

    if (!success) {
        emit requestFailed(requestId, errorFromReply(reply, body));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit requestFailed(requestId,
                           {OAuthErrorKind::InvalidResponse, status, QString(),
                            QStringLiteral("Google returned malformed OAuth JSON.")});
        return;
    }

    const QJsonObject root = document.object();
    OAuthToken token;
    token.accessToken = root.value(QStringLiteral("access_token")).toString();
    token.refreshToken = root.value(QStringLiteral("refresh_token")).toString();
    token.tokenType = root.value(QStringLiteral("token_type")).toString();
    if (token.tokenType.isEmpty()) {
        token.tokenType = QStringLiteral("Bearer");
    }
    token.scope = root.value(QStringLiteral("scope")).toString();

    const QJsonValue expiresIn = root.value(QStringLiteral("expires_in"));
    if (expiresIn.isDouble() && expiresIn.toInt() > 0) {
        token.expiresAt = QDateTime::currentDateTimeUtc().addSecs(expiresIn.toInt());
    } else if (expiresIn.isString() && expiresIn.toString().toInt() > 0) {
        token.expiresAt =
            QDateTime::currentDateTimeUtc().addSecs(expiresIn.toString().toInt());
    }

    if (!token.hasAccessToken()) {
        emit requestFailed(requestId,
                           {OAuthErrorKind::InvalidResponse, status, QString(),
                            QStringLiteral("Google returned an OAuth response without an "
                                           "access token.")});
        return;
    }

    emit tokenReceived(requestId, token);
}
