#pragma once

#include <QDate>
#include <QString>

// Gmail excludes Spam and Trash from normal message searches by default.
// Updates are included unless the user disables them.
QString buildGmailQuery(bool includeUpdates, const QDate &afterDate = QDate());

QString gmailMessageUrl(const QString &messageId);
