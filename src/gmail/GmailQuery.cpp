#include "gmail/GmailQuery.h"

#include <QUrl>

QString buildGmailQuery(bool includeUpdates, const QDate &afterDate)
{
    QString query = QStringLiteral(
        "in:inbox -category:promotions -category:social -category:forums");
    if (!includeUpdates) {
        query += QStringLiteral(" -category:updates");
    }
    if (afterDate.isValid()) {
        query += QStringLiteral(" after:%1").arg(afterDate.toString(QStringLiteral("yyyy/MM/dd")));
    }
    return query;
}

QString gmailMessageUrl(const QString &messageId, const QString &accountEmail)
{
    const QString encodedId = QString::fromLatin1(QUrl::toPercentEncoding(messageId));
    if (accountEmail.isEmpty()) {
        return QStringLiteral("https://mail.google.com/mail/u/0/#inbox/%1").arg(encodedId);
    }

    const QString encodedAccount =
        QString::fromLatin1(QUrl::toPercentEncoding(accountEmail));
    return QStringLiteral("https://mail.google.com/mail/?authuser=%1#inbox/%2")
        .arg(encodedAccount, encodedId);
}
