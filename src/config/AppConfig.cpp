#include "config/AppConfig.h"

#include <QRegularExpression>
#include <QSet>

AppConfig defaultAppConfig()
{
    AppConfig config;
    config.schemaVersion = 1;
    config.notifications.enabled = true;
    config.notifications.pollIntervalMinutes = kDefaultPollIntervalMinutes;
    config.notifications.includeUpdates = true;
    config.notifications.showPreview = true;
    return config;
}

QStringList validateAppConfig(const AppConfig &config)
{
    QStringList errors;

    if (config.schemaVersion != 1) {
        errors << QStringLiteral("Unsupported schema_version %1 (expected 1).")
                      .arg(config.schemaVersion);
    }

    const int interval = config.notifications.pollIntervalMinutes;
    if (interval < kMinPollIntervalMinutes || interval > kMaxPollIntervalMinutes) {
        errors << QStringLiteral("poll_interval_minutes must be between %1 and %2.")
                      .arg(kMinPollIntervalMinutes)
                      .arg(kMaxPollIntervalMinutes);
    }

    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9._-]{1,128}$"));
    static const QRegularExpression emailPattern(
        QStringLiteral("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$"));

    QSet<QString> ids;
    QSet<QString> emails;
    for (const AccountConfig &account : config.accounts) {
        if (!idPattern.match(account.id).hasMatch()) {
            errors << QStringLiteral("Account id is empty or contains unsupported characters.");
        } else if (ids.contains(account.id)) {
            errors << QStringLiteral("Duplicate account id: %1").arg(account.id);
        }
        ids.insert(account.id);

        if (account.email.size() > 320 || !emailPattern.match(account.email).hasMatch()) {
            errors << QStringLiteral("Account %1 has an invalid email address.").arg(account.id);
        }
        const QString normalizedEmail = account.email.toCaseFolded();
        if (emails.contains(normalizedEmail)) {
            errors << QStringLiteral("Duplicate account email: %1").arg(account.email);
        }
        emails.insert(normalizedEmail);

        if (account.displayName.size() > 256) {
            errors << QStringLiteral("Account %1 display_name is too long.").arg(account.id);
        }
    }

    return errors;
}
