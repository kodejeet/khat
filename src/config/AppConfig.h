#pragma once

#include <QList>
#include <QString>
#include <QStringList>

struct AccountConfig
{
    QString id;
    QString email;
    QString displayName;
    bool enabled = true;
};

struct NotificationConfig
{
    bool enabled = true;
    int pollIntervalMinutes = 180;
    bool includeUpdates = true;
    bool showPreview = true;
};

struct AppConfig
{
    int schemaVersion = 1;
    QList<AccountConfig> accounts;
    NotificationConfig notifications;
};

constexpr int kDefaultPollIntervalMinutes = 180;
constexpr int kMinPollIntervalMinutes = 5;
constexpr int kMaxPollIntervalMinutes = 24 * 60;

AppConfig defaultAppConfig();

// Returns an empty list when the configuration is valid.
QStringList validateAppConfig(const AppConfig &config);
