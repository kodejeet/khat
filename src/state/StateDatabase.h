#pragma once

#include "model/MessageSummary.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>

struct AccountState
{
    QString accountId;
    QString email;
    bool baselineDone = false;
    qint64 lastSyncEpoch = 0;
    qint64 lastAttemptEpoch = 0;
    QString lastError;
};

class StateDatabase
{
public:
    explicit StateDatabase(QString path);
    ~StateDatabase();

    StateDatabase(const StateDatabase &) = delete;
    StateDatabase &operator=(const StateDatabase &) = delete;

    bool open(QString *error = nullptr);
    bool isOpen() const;
    QString path() const { return m_path; }

    bool ensureAccount(const QString &accountId, const QString &email,
                       QString *error = nullptr);
    bool accountState(const QString &accountId, AccountState *state,
                      QString *error = nullptr);
    QList<AccountState> accountStates(QString *error = nullptr);
    bool deleteAccount(const QString &accountId, QString *error = nullptr);

    // First-launch synchronization: record every currently matching message as
    // seen and mark the account baseline complete without notifying.
    bool applyBaseline(const QString &accountId, const QList<MessageSummary> &messages,
                       qint64 syncEpoch, QString *error = nullptr);

    // Inserts only unseen messages. `inserted` receives the new records in input
    // order; existing IDs are ignored. Does not advance lastSyncEpoch.
    bool recordMessages(const QString &accountId, const QList<MessageSummary> &messages,
                        qint64 firstSeenEpoch, QList<MessageSummary> *inserted = nullptr,
                        QString *error = nullptr);

    bool markSyncAttempt(const QString &accountId, qint64 attemptEpoch,
                         QString *error = nullptr);
    bool markSyncSuccess(const QString &accountId, qint64 syncEpoch,
                         QString *error = nullptr);
    bool markSyncError(const QString &accountId, qint64 attemptEpoch,
                       const QString &message, QString *error = nullptr);

    bool hasSeenMessage(const QString &accountId, const QString &messageId, bool *seen,
                        QString *error = nullptr);
    bool messageWasNotified(const QString &accountId, const QString &messageId,
                            bool *notified, QString *error = nullptr);
    bool markNotified(const QString &accountId, const QString &messageId,
                      QString *error = nullptr);
    int countSeenMessages(const QString &accountId, QString *error = nullptr);

private:
    bool createSchema(QString *error);
    bool checkSchemaVersion(QString *error);
    bool exec(const QString &sql, QString *error);
    void setError(QString *error, const QString &message) const;

    QString m_path;
    QString m_connectionName;
    QSqlDatabase m_db;
};
