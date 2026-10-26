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

QString gmailMessageUrl(const QString &messageId)
{
    return QStringLiteral("https://mail.google.com/mail/u/0/#inbox/%1")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(messageId)));
}
