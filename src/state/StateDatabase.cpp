#include "state/StateDatabase.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>
#include <utility>

namespace {
constexpr int kSchemaVersion = 1;

std::atomic<quint64> g_connectionCounter{0};

QString nextConnectionName()
{
    return QStringLiteral("khat-state-%1")
        .arg(g_connectionCounter.fetch_add(1));
}
} // namespace

StateDatabase::StateDatabase(QString path)
    : m_path(std::move(path))
    , m_connectionName(nextConnectionName())
{
}

StateDatabase::~StateDatabase()
{
    if (m_db.isValid()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

void StateDatabase::setError(QString *error, const QString &message) const
{
    if (error) {
        *error = message;
    }
}

bool StateDatabase::exec(const QString &sql, QString *error)
{
    QSqlQuery query(m_db);
    if (!query.exec(sql)) {
        setError(error, QStringLiteral("Database error: %1").arg(query.lastError().text()));
        return false;
    }
    return true;
}

bool StateDatabase::open(QString *error)
{
    if (m_db.isValid() && m_db.isOpen()) {
        return true;
    }

    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        setError(error, QStringLiteral("Qt SQLite driver (QSQLITE) is not available."));
        return false;
    }

    if (m_path != QStringLiteral(":memory:")) {
        const QFileInfo info(m_path);
        QDir directory = info.absoluteDir();
        if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
            setError(error, QStringLiteral("Cannot create state directory: %1")
                                 .arg(directory.absolutePath()));
            return false;
        }
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(m_path);
    if (!m_db.open()) {
        setError(error, QStringLiteral("Cannot open state database %1: %2")
                             .arg(m_path, m_db.lastError().text()));
        return false;
    }

    if (!exec(QStringLiteral("PRAGMA foreign_keys = ON"), error)
        || !exec(QStringLiteral("PRAGMA busy_timeout = 5000"), error)
        || !exec(QStringLiteral("PRAGMA journal_mode = WAL"), error)
        || !exec(QStringLiteral("PRAGMA synchronous = FULL"), error)) {
        return false;
    }

    if (!checkSchemaVersion(error) || !createSchema(error)) {
        return false;
    }

    if (!m_path.endsWith(QStringLiteral(":memory:"))) {
        QFile::setPermissions(m_path, QFile::ReadOwner | QFile::WriteOwner);
    }
    return true;
}

bool StateDatabase::isOpen() const
{
    return m_db.isValid() && m_db.isOpen();
}

bool StateDatabase::checkSchemaVersion(QString *error)
{
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
        setError(error, QStringLiteral("Cannot read state schema version: %1")
                             .arg(query.lastError().text()));
        return false;
    }
    const int version = query.value(0).toInt();
    if (version > kSchemaVersion) {
        setError(error, QStringLiteral("State database schema version %1 is newer than %2.")
                             .arg(version)
                             .arg(kSchemaVersion));
        return false;
    }
    return true;
}

bool StateDatabase::createSchema(QString *error)
{
    if (!exec(QStringLiteral(
                  "CREATE TABLE IF NOT EXISTS accounts ("
                  "  account_id TEXT PRIMARY KEY,"
                  "  email TEXT NOT NULL,"
                  "  baseline_done INTEGER NOT NULL DEFAULT 0,"
                  "  last_sync_epoch INTEGER,"
                  "  last_attempt_epoch INTEGER,"
                  "  last_error TEXT NOT NULL DEFAULT ''"
                  ")"),
              error)) {
        return false;
    }

    if (!exec(QStringLiteral(
                  "CREATE TABLE IF NOT EXISTS seen_messages ("
                  "  account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,"
                  "  message_id TEXT NOT NULL,"
                  "  internal_date INTEGER NOT NULL,"
                  "  first_seen_epoch INTEGER NOT NULL,"
                  "  notified INTEGER NOT NULL DEFAULT 0,"
                  "  PRIMARY KEY (account_id, message_id)"
                  ")"),
              error)) {
        return false;
    }

    if (!exec(QStringLiteral(
                  "CREATE INDEX IF NOT EXISTS idx_seen_messages_date "
                  "ON seen_messages(account_id, internal_date)"),
              error)) {
        return false;
    }

    return exec(QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion), error);
}

bool StateDatabase::ensureAccount(const QString &accountId, const QString &email,
                                  QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO accounts(account_id, email) VALUES(?, ?) "
        "ON CONFLICT(account_id) DO UPDATE SET email = excluded.email"));
    query.addBindValue(accountId);
    query.addBindValue(email);
    if (!query.exec()) {
        setError(error, QStringLiteral("Cannot store account: %1")
                             .arg(query.lastError().text()));
        return false;
    }
    return true;
}

bool StateDatabase::accountState(const QString &accountId, AccountState *state,
                                 QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT account_id, email, baseline_done, last_sync_epoch, last_attempt_epoch, "
        "last_error FROM accounts WHERE account_id = ?"));
    query.addBindValue(accountId);
    if (!query.exec()) {
        setError(error, QStringLiteral("Cannot read account state: %1")
                             .arg(query.lastError().text()));
        return false;
    }
    if (!query.next()) {
        setError(error, QStringLiteral("Account is not initialized in the state database."));
        return false;
    }

    state->accountId = query.value(0).toString();
    state->email = query.value(1).toString();
    state->baselineDone = query.value(2).toInt() != 0;
    state->lastSyncEpoch = query.value(3).isNull() ? 0 : query.value(3).toLongLong();
    state->lastAttemptEpoch = query.value(4).isNull() ? 0 : query.value(4).toLongLong();
    state->lastError = query.value(5).toString();
    return true;
}

QList<AccountState> StateDatabase::accountStates(QString *error)
{
    QList<AccountState> states;
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return states;
    }

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral(
            "SELECT account_id, email, baseline_done, last_sync_epoch, "
            "last_attempt_epoch, last_error FROM accounts ORDER BY email"))) {
        setError(error, QStringLiteral("Cannot list account state: %1")
                             .arg(query.lastError().text()));
        return states;
    }
    while (query.next()) {
        AccountState state;
        state.accountId = query.value(0).toString();
        state.email = query.value(1).toString();
        state.baselineDone = query.value(2).toInt() != 0;
        state.lastSyncEpoch = query.value(3).isNull() ? 0 : query.value(3).toLongLong();
        state.lastAttemptEpoch = query.value(4).isNull() ? 0 : query.value(4).toLongLong();
        state.lastError = query.value(5).toString();
        states.append(state);
    }
    return states;
}

bool StateDatabase::deleteAccount(const QString &accountId, QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM accounts WHERE account_id = ?"));
    query.addBindValue(accountId);
    if (!query.exec()) {
        setError(error, QStringLiteral("Cannot remove account state: %1")
                             .arg(query.lastError().text()));
        return false;
    }
    return true;
}

bool StateDatabase::applyBaseline(const QString &accountId,
                                  const QList<MessageSummary> &messages,
                                  qint64 syncEpoch, QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return false;
    }
    if (!m_db.transaction()) {
        setError(error, QStringLiteral("Cannot start state transaction: %1")
                             .arg(m_db.lastError().text()));
        return false;
    }

    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO seen_messages"
        "(account_id, message_id, internal_date, first_seen_epoch, notified) "
        "VALUES(?, ?, ?, ?, 0)"));
    for (const MessageSummary &message : messages) {
        if (message.id.isEmpty()) {
            m_db.rollback();
            setError(error, QStringLiteral("Refusing to baseline a message with an empty ID."));
            return false;
        }
        insert.addBindValue(accountId);
        insert.addBindValue(message.id);
        insert.addBindValue(message.internalDate);
        insert.addBindValue(syncEpoch);
        if (!insert.exec()) {
            m_db.rollback();
            setError(error, QStringLiteral("Cannot record baseline message: %1")
                                 .arg(insert.lastError().text()));
            return false;
        }
        insert.finish();
    }

    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE accounts SET baseline_done = 1, last_sync_epoch = ?, "
        "last_attempt_epoch = ?, last_error = '' WHERE account_id = ?"));
    update.addBindValue(syncEpoch);
    update.addBindValue(syncEpoch);
    update.addBindValue(accountId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        m_db.rollback();
        setError(error, QStringLiteral("Cannot mark baseline complete: %1")
                             .arg(update.lastError().text()));
        return false;
    }

    if (!m_db.commit()) {
        setError(error, QStringLiteral("Cannot commit baseline: %1")
                             .arg(m_db.lastError().text()));
        return false;
    }
    return true;
}

bool StateDatabase::recordMessages(const QString &accountId,
                                   const QList<MessageSummary> &messages,
                                   qint64 firstSeenEpoch,
                                   QList<MessageSummary> *inserted, QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return false;
    }
    if (!m_db.transaction()) {
        setError(error, QStringLiteral("Cannot start state transaction: %1")
                             .arg(m_db.lastError().text()));
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO seen_messages"
        "(account_id, message_id, internal_date, first_seen_epoch, notified) "
        "VALUES(?, ?, ?, ?, 0)"));
    for (const MessageSummary &message : messages) {
        if (message.id.isEmpty()) {
            m_db.rollback();
            setError(error, QStringLiteral("Refusing to record a message with an empty ID."));
            return false;
        }
        query.addBindValue(accountId);
        query.addBindValue(message.id);
        query.addBindValue(message.internalDate);
        query.addBindValue(firstSeenEpoch);
        if (!query.exec()) {
            m_db.rollback();
            setError(error, QStringLiteral("Cannot record message: %1")
                                 .arg(query.lastError().text()));
            return false;
        }
        if (query.numRowsAffected() == 1 && inserted) {
            inserted->append(message);
        }
        query.finish();
    }

    if (!m_db.commit()) {
        setError(error, QStringLiteral("Cannot commit messages: %1")
                             .arg(m_db.lastError().text()));
        return false;
    }
    return true;
}

bool StateDatabase::markSyncAttempt(const QString &accountId, qint64 attemptEpoch,
                                    QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE accounts SET last_attempt_epoch = ? WHERE account_id = ?"));
    query.addBindValue(attemptEpoch);
    query.addBindValue(accountId);
    if (!query.exec()) {
        setError(error, QStringLiteral("Cannot record sync attempt: %1")
                             .arg(query.lastError().text()));
        return false;
    }
    return true;
}

bool StateDatabase::markSyncSuccess(const QString &accountId, qint64 syncEpoch,
                                    QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE accounts SET last_sync_epoch = ?, last_attempt_epoch = ?, "
        "last_error = '' WHERE account_id = ?"));
    query.addBindValue(syncEpoch);
    query.addBindValue(syncEpoch);
    query.addBindValue(accountId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(error, QStringLiteral("Cannot record sync success: %1")
                             .arg(query.lastError().text()));
        return false;
    }
    return true;
}

bool StateDatabase::markSyncError(const QString &accountId, qint64 attemptEpoch,
                                  const QString &message, QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE accounts SET last_attempt_epoch = ?, last_error = ? "
        "WHERE account_id = ?"));
    query.addBindValue(attemptEpoch);
    query.addBindValue(message.left(500));
    query.addBindValue(accountId);
    if (!query.exec()) {
        setError(error, QStringLiteral("Cannot record sync error: %1")
                             .arg(query.lastError().text()));
        return false;
    }
    return true;
}

bool StateDatabase::hasSeenMessage(const QString &accountId, const QString &messageId,
                                   bool *seen, QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM seen_messages WHERE account_id = ? AND message_id = ?"));
    query.addBindValue(accountId);
    query.addBindValue(messageId);
    if (!query.exec()) {
        setError(error, QStringLiteral("Cannot query message state: %1")
                             .arg(query.lastError().text()));
        return false;
    }
    *seen = query.next();
    return true;
}

bool StateDatabase::messageWasNotified(const QString &accountId, const QString &messageId,
                                       bool *notified, QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT notified FROM seen_messages WHERE account_id = ? AND message_id = ?"));
    query.addBindValue(accountId);
    query.addBindValue(messageId);
    if (!query.exec()) {
        setError(error, QStringLiteral("Cannot query notification state: %1")
                             .arg(query.lastError().text()));
        return false;
    }
    if (!query.next()) {
        setError(error, QStringLiteral("Message is not recorded."));
        return false;
    }
    *notified = query.value(0).toInt() != 0;
    return true;
}

bool StateDatabase::markNotified(const QString &accountId, const QString &messageId,
                                 QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE seen_messages SET notified = 1 WHERE account_id = ? AND message_id = ?"));
    query.addBindValue(accountId);
    query.addBindValue(messageId);
    if (!query.exec()) {
        setError(error, QStringLiteral("Cannot mark notification delivered: %1")
                             .arg(query.lastError().text()));
        return false;
    }
    return true;
}

int StateDatabase::countSeenMessages(const QString &accountId, QString *error)
{
    if (!isOpen()) {
        setError(error, QStringLiteral("State database is not open."));
        return -1;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM seen_messages WHERE account_id = ?"));
    query.addBindValue(accountId);
    if (!query.exec() || !query.next()) {
        setError(error, QStringLiteral("Cannot count messages: %1")
                             .arg(query.lastError().text()));
        return -1;
    }
    return query.value(0).toInt();
}
