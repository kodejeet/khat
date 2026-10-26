#include "auth/OAuthPkce.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QUrlQuery>

namespace {
QString randomUrlSafeToken(int byteCount)
{
    QByteArray bytes(byteCount, Qt::Uninitialized);
    auto *generator = QRandomGenerator::system();
    for (int i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>(generator->bounded(256));
    }
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
} // namespace

OAuthPkce OAuthPkce::generate()
{
    OAuthPkce pkce;
    // 32 random bytes encode to 43 characters, within RFC 7636's 43–128 range.
    pkce.verifier = randomUrlSafeToken(32);
    pkce.state = randomUrlSafeToken(32);
    pkce.challenge = codeChallenge(pkce.verifier);
    return pkce;
}

QString OAuthPkce::codeChallenge(const QString &verifier)
{
    const QByteArray digest =
        QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256);
    return QString::fromLatin1(
        digest.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QUrl buildOAuthAuthorizationUrl(const OAuthClientConfig &client,
                                const QString &redirectUri, const OAuthPkce &pkce)
{
    QUrl url = client.authUri;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("client_id"), client.clientId);
    query.addQueryItem(QStringLiteral("redirect_uri"), redirectUri);
    query.addQueryItem(QStringLiteral("scope"), client.scope);
    query.addQueryItem(QStringLiteral("code_challenge"), pkce.challenge);
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("state"), pkce.state);
    query.addQueryItem(QStringLiteral("access_type"), QStringLiteral("offline"));
    // Google only returns a refresh token when consent is actually granted. The
    // notifier needs one to keep polling without asking again.
    query.addQueryItem(QStringLiteral("prompt"), QStringLiteral("consent"));
    url.setQuery(query);
    return url;
}
