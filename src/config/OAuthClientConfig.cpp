#include "config/OAuthClientConfig.h"

#include <QStringList>

namespace {
constexpr auto kGmailReadonlyScope = "https://www.googleapis.com/auth/gmail.readonly";

bool isSecureEndpoint(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty()) {
        return false;
    }
    if (url.scheme() == QStringLiteral("https")) {
        return true;
    }
    // Loopback HTTP is only tolerated for local test servers.
    return url.scheme() == QStringLiteral("http")
        && (url.host() == QStringLiteral("127.0.0.1")
            || url.host() == QStringLiteral("localhost"));
}
} // namespace

OAuthClientConfig defaultOAuthClientConfig()
{
    OAuthClientConfig config;
    config.authUri = QUrl(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth"));
    config.tokenUri = QUrl(QStringLiteral("https://oauth2.googleapis.com/token"));
    config.revokeUri = QUrl(QStringLiteral("https://oauth2.googleapis.com/revoke"));
    config.scope = QString::fromLatin1(kGmailReadonlyScope);
    return config;
}

bool isPlaceholderValue(const QString &value)
{
    const QString upper = value.toUpper();
    return value.trimmed().isEmpty()
        || upper.contains(QStringLiteral("REPLACE"))
        || upper.contains(QStringLiteral("YOUR_"))
        || upper.contains(QStringLiteral("CHANGEME"))
        || upper.contains(QStringLiteral("PLACEHOLDER"))
        || upper.contains(QStringLiteral("EXAMPLE_CLIENT"));
}

QStringList validateOAuthClientConfig(const OAuthClientConfig &config)
{
    QStringList errors;

    if (isPlaceholderValue(config.clientId)) {
        errors << QStringLiteral("client_id is missing or still a placeholder.");
    } else if (!config.clientId.endsWith(QStringLiteral(".apps.googleusercontent.com"))) {
        errors << QStringLiteral("client_id must be a Google OAuth client ID ending in "
                                 ".apps.googleusercontent.com.");
    }

    if (isPlaceholderValue(config.clientSecret)) {
        errors << QStringLiteral("client_secret is missing or still a placeholder.");
    }

    if (!isSecureEndpoint(config.authUri)) {
        errors << QStringLiteral("auth_uri must be an HTTPS URL (or loopback HTTP for tests).");
    }
    if (!isSecureEndpoint(config.tokenUri)) {
        errors << QStringLiteral("token_uri must be an HTTPS URL (or loopback HTTP for tests).");
    }
    if (!isSecureEndpoint(config.revokeUri)) {
        errors << QStringLiteral("revoke_uri must be an HTTPS URL (or loopback HTTP for tests).");
    }

    if (config.scope != QString::fromLatin1(kGmailReadonlyScope)) {
        errors << QStringLiteral("scope must be exactly %1; broader Gmail scopes are not "
                                 "permitted.")
                      .arg(QString::fromLatin1(kGmailReadonlyScope));
    }

    return errors;
}
