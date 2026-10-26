#pragma once

#include "model/MessageSummary.h"

#include <QString>

// Text shown in a GNOME notification. Only a short Gmail snippet is ever
// included; full message bodies are never fetched or displayed.
struct NotificationContent
{
    QString messageId;
    QString url;
    QString summary;
    QString body;
    QString actionLabel;
    int urgency = 1; // Freedesktop urgency: 0 low, 1 normal, 2 critical.
};

constexpr int kNotificationSenderLength = 120;
constexpr int kNotificationSubjectLength = 200;
constexpr int kNotificationPreviewLength = 180;

QString notificationSingleLine(const QString &text);
QString notificationTruncate(const QString &text, int maxLength);
QString notificationSenderDisplay(const QString &fromHeader);
QString notificationEscapeMarkup(const QString &text);

// `showAccount` is intended for configurations with more than one account.
NotificationContent buildNotificationContent(const MessageSummary &message,
                                             const QString &accountEmail,
                                             bool showPreview,
                                             bool showAccount);
