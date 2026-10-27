#include "notify/NotificationContent.h"

#include "gmail/GmailQuery.h"

#include <QStringList>

QString notificationSingleLine(const QString &text)
{
    QString result;
    result.reserve(text.size());
    bool pendingSpace = false;
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if (character.isSpace()) {
            pendingSpace = !result.isEmpty();
            continue;
        }
        if (code < 0x20 || (code >= 0x7f && code <= 0x9f)) {
            pendingSpace = !result.isEmpty();
            continue;
        }
        if (pendingSpace) {
            result.append(QLatin1Char(' '));
            pendingSpace = false;
        }
        result.append(character);
    }
    return result.trimmed();
}

QString notificationTruncate(const QString &text, int maxLength)
{
    if (maxLength <= 0) {
        return QString();
    }
    if (text.size() <= maxLength) {
        return text;
    }

    int end = maxLength;
    if (text.at(end - 1).isHighSurrogate()) {
        --end;
    }
    return text.left(end).trimmed() + QChar(0x2026);
}

// Bodies are cut at a sentence boundary when one exists inside the limit, so
// the notification shows whole sentences and never a floating word. Short
// snippets pass through untouched: the banner then only ever grows as large as
// the text behind it.
QString notificationTruncateSentences(const QString &text, int maxLength)
{
    if (maxLength <= 0) {
        return QString();
    }
    if (text.size() <= maxLength) {
        return text;
    }

    int cut = -1;
    for (int i = 0; i < maxLength; ++i) {
        const QChar character = text.at(i);
        const bool terminator = character == QLatin1Char('.')
            || character == QLatin1Char('!')
            || character == QLatin1Char('?')
            || character == QChar(0x2026);
        if (terminator && i + 1 < text.size() && text.at(i + 1).isSpace()) {
            // Abbreviations such as "U.S. President" do not end in whitespace
            // after the dot, so they are not treated as sentence breaks.
            cut = i + 1;
        }
    }
    if (cut > 0) {
        QString truncated = text.left(cut).trimmed();
        // A sentence may end on the ellipsis character itself; do not stack
        // another one right after it.
        if (!truncated.endsWith(QChar(0x2026))) {
            truncated.append(QChar(0x2026));
        }
        return truncated;
    }
    return notificationTruncate(text, maxLength);
}

QString notificationSenderDisplay(const QString &fromHeader)
{
    const QString cleaned = notificationSingleLine(fromHeader);
    const int angleOpen = cleaned.lastIndexOf(QLatin1Char('<'));
    const int angleClose = cleaned.lastIndexOf(QLatin1Char('>'));
    if (angleOpen >= 0 && angleClose > angleOpen) {
        QString name = cleaned.left(angleOpen).trimmed();
        const QString email = cleaned.mid(angleOpen + 1, angleClose - angleOpen - 1).trimmed();
        if (name.size() >= 2 && name.startsWith(QLatin1Char('"'))
            && name.endsWith(QLatin1Char('"'))) {
            name = name.mid(1, name.size() - 2).trimmed();
        }
        if (!name.isEmpty()) {
            return name;
        }
        if (!email.isEmpty()) {
            return email;
        }
    }
    return cleaned;
}

QString notificationEscapeMarkup(const QString &text)
{
    // GNOME treats the body as Pango markup, so user-controlled text must be
    // escaped even though the app itself adds no formatting.
    return text.toHtmlEscaped();
}

NotificationContent buildNotificationContent(const MessageSummary &message,
                                             const QString &accountEmail,
                                             bool showPreview,
                                             bool showAccount)
{
    NotificationContent content;
    content.messageId = message.id;
    content.url = gmailMessageUrl(message.id, accountEmail);
    content.actionLabel = QStringLiteral("Open in Gmail");
    content.urgency = 1;

    QString sender = notificationSenderDisplay(message.sender);
    if (sender.isEmpty()) {
        sender = QStringLiteral("New Gmail message");
    }
    content.summary =
        notificationTruncate(notificationSingleLine(sender), kNotificationSenderLength);

    const QString subject = notificationSingleLine(message.subject);
    QStringList lines;
    lines.append(subject.isEmpty() ? QStringLiteral("(No subject)")
                                   : notificationTruncate(subject,
                                                          kNotificationSubjectLength));

    if (showPreview) {
        const QString preview = notificationTruncateSentences(
            notificationSingleLine(message.snippet), kNotificationPreviewLength);
        if (!preview.isEmpty()) {
            lines.append(preview);
        }
    }

    if (showAccount && !accountEmail.isEmpty()) {
        lines.append(QStringLiteral("Account: %1").arg(accountEmail));
    }

    content.body = notificationEscapeMarkup(lines.join(QLatin1Char('\n')));
    return content;
}
