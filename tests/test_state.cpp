#include "state/StateDatabase.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

class TestState : public QObject
{
    Q_OBJECT

private slots:
    void baselineAndDedupe();
    void restartPreservesState();
    void accountsAreIsolated();
    void syncStatusPersists();
    void emptyMessageIdRollsBack();
    void deleteAccountCascades();
    void futureSchemaRejected();
    void schemaV1MigratesToV2();
    void fileIsOwnerOnly();
};

static MessageSummary message(const QString &id, qint64 date = 1000)
{
    MessageSummary summary;
    summary.id = id;
    summary.threadId = id + QStringLiteral("-thread");
    summary.internalDate = date;
    summary.sender = QStringLiteral("Sender <sender@example.com>");
    summary.subject = QStringLiteral("Subject");
    summary.snippet = QStringLiteral("Preview");
    return summary;
}

void TestState::baselineAndDedupe()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    QVERIFY2(db.ensureAccount(QStringLiteral("acct"), QStringLiteral("user@example.com"), &error),
             qPrintable(error));

    AccountState state;
    QVERIFY2(db.accountState(QStringLiteral("acct"), &state, &error), qPrintable(error));
    QVERIFY(!state.baselineDone);
    QCOMPARE(state.lastSyncEpoch, 0);

    const QList<MessageSummary> oldMessages{message(QStringLiteral("old-1")),
                                            message(QStringLiteral("old-2"))};
    QVERIFY2(db.applyBaseline(QStringLiteral("acct"), oldMessages, 1000, &error),
             qPrintable(error));

    QVERIFY2(db.accountState(QStringLiteral("acct"), &state, &error), qPrintable(error));
    QVERIFY(state.baselineDone);
    QCOMPARE(state.lastSyncEpoch, 1000);
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 2);

    bool seen = false;
    QVERIFY(db.hasSeenMessage(QStringLiteral("acct"), QStringLiteral("old-1"), &seen));
    QVERIFY(seen);
    bool notified = true;
    QVERIFY(db.messageWasNotified(QStringLiteral("acct"), QStringLiteral("old-1"), &notified));
    QVERIFY(!notified);

    QList<MessageSummary> inserted;
    QVERIFY2(db.recordMessages(QStringLiteral("acct"), {message(QStringLiteral("new-1"), 2000)},
                               2000, &inserted, &error),
             qPrintable(error));
    QCOMPARE(inserted.size(), 1);
    QCOMPARE(inserted.first().id, QStringLiteral("new-1"));

    inserted.clear();
    QVERIFY2(db.recordMessages(QStringLiteral("acct"), {message(QStringLiteral("new-1"), 2000)},
                               2001, &inserted, &error),
             qPrintable(error));
    QVERIFY(inserted.isEmpty());
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 3);

    QVERIFY(db.markNotified(QStringLiteral("acct"), QStringLiteral("new-1"), &error));
    QVERIFY(db.messageWasNotified(QStringLiteral("acct"), QStringLiteral("new-1"), &notified));
    QVERIFY(notified);
}

void TestState::restartPreservesState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("state.db"));

    {
        StateDatabase db(path);
        QString error;
        QVERIFY2(db.open(&error), qPrintable(error));
        QVERIFY(db.ensureAccount(QStringLiteral("acct"), QStringLiteral("user@example.com")));
        QVERIFY(db.applyBaseline(QStringLiteral("acct"), {message(QStringLiteral("old"))}, 100));
        QList<MessageSummary> inserted;
        QVERIFY(db.recordMessages(QStringLiteral("acct"), {message(QStringLiteral("new"), 200)},
                                  200, &inserted));
        QCOMPARE(inserted.size(), 1);
        QVERIFY(db.markSyncSuccess(QStringLiteral("acct"), 200));
    }

    StateDatabase reopened(path);
    QString error;
    QVERIFY2(reopened.open(&error), qPrintable(error));
    AccountState state;
    QVERIFY2(reopened.accountState(QStringLiteral("acct"), &state, &error), qPrintable(error));
    QVERIFY(state.baselineDone);
    QCOMPARE(state.lastSyncEpoch, 200);
    QCOMPARE(reopened.countSeenMessages(QStringLiteral("acct")), 2);

    QList<MessageSummary> inserted;
    QVERIFY2(reopened.recordMessages(QStringLiteral("acct"),
                                     {message(QStringLiteral("new"), 200)}, 300, &inserted,
                                     &error),
             qPrintable(error));
    QVERIFY(inserted.isEmpty());
}

void TestState::accountsAreIsolated()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    QVERIFY(db.ensureAccount(QStringLiteral("one"), QStringLiteral("one@example.com")));
    QVERIFY(db.ensureAccount(QStringLiteral("two"), QStringLiteral("two@example.com")));

    QList<MessageSummary> inserted;
    QVERIFY(db.recordMessages(QStringLiteral("one"), {message(QStringLiteral("same"))}, 10,
                              &inserted));
    QCOMPARE(inserted.size(), 1);
    QVERIFY(db.recordMessages(QStringLiteral("two"), {message(QStringLiteral("same"))}, 10,
                              &inserted));
    QCOMPARE(inserted.size(), 2);
    QCOMPARE(db.countSeenMessages(QStringLiteral("one")), 1);
    QCOMPARE(db.countSeenMessages(QStringLiteral("two")), 1);
    QCOMPARE(db.accountStates().size(), 2);
}

void TestState::syncStatusPersists()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    QVERIFY(db.ensureAccount(QStringLiteral("acct"), QStringLiteral("user@example.com")));
    QVERIFY(db.applyBaseline(QStringLiteral("acct"), {}, 100));

    QVERIFY(db.markSyncAttempt(QStringLiteral("acct"), 200));
    QVERIFY(db.markSyncError(QStringLiteral("acct"), 200,
                             QStringLiteral("HTTP 503: temporarily unavailable")));

    AccountState state;
    QVERIFY(db.accountState(QStringLiteral("acct"), &state));
    QCOMPARE(state.lastSyncEpoch, 100);
    QCOMPARE(state.lastAttemptEpoch, 200);
    QVERIFY(state.lastError.contains(QStringLiteral("503")));

    QVERIFY(db.markSyncSuccess(QStringLiteral("acct"), 300));
    QVERIFY(db.accountState(QStringLiteral("acct"), &state));
    QCOMPARE(state.lastSyncEpoch, 300);
    QCOMPARE(state.lastAttemptEpoch, 300);
    QVERIFY(state.lastError.isEmpty());
}

void TestState::emptyMessageIdRollsBack()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    QVERIFY(db.ensureAccount(QStringLiteral("acct"), QStringLiteral("user@example.com")));
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 0);

    QList<MessageSummary> inserted;
    const QList<MessageSummary> messages{message(QStringLiteral("valid")), MessageSummary{}};
    QVERIFY(!db.recordMessages(QStringLiteral("acct"), messages, 100, &inserted, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 0);
}

void TestState::deleteAccountCascades()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StateDatabase db(dir.filePath(QStringLiteral("state.db")));
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));
    QVERIFY(db.ensureAccount(QStringLiteral("acct"), QStringLiteral("user@example.com")));
    QVERIFY(db.recordMessages(QStringLiteral("acct"), {message(QStringLiteral("one"))}, 100));
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 1);
    QVERIFY2(db.deleteAccount(QStringLiteral("acct"), &error), qPrintable(error));
    QVERIFY(db.accountStates().isEmpty());
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 0);
}

void TestState::futureSchemaRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("future.db"));

    const QString connectionName = QStringLiteral("future-schema-test");
    {
        QSqlDatabase future = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        future.setDatabaseName(path);
        QVERIFY(future.open());
        QSqlQuery query(future);
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 99")));
        future.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    StateDatabase db(path);
    QString error;
    QVERIFY(!db.open(&error));
    QVERIFY(error.contains(QStringLiteral("newer")));
}

// A version-1 database predates the Gmail history cursor. Opening it must
// migrate the schema in place: user_version advances, the stored account rows
// survive untouched, and the new column is immediately writable.
void TestState::schemaV1MigratesToV2()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("legacy.db"));

    const QString connectionName = QStringLiteral("legacy-schema-test");
    {
        QSqlDatabase legacy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        legacy.setDatabaseName(path);
        QVERIFY(legacy.open());
        QSqlQuery query(legacy);
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE accounts ("
            "  account_id TEXT PRIMARY KEY,"
            "  email TEXT NOT NULL,"
            "  baseline_done INTEGER NOT NULL DEFAULT 0,"
            "  last_sync_epoch INTEGER,"
            "  last_attempt_epoch INTEGER,"
            "  last_error TEXT NOT NULL DEFAULT ''"
            ")")));
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE seen_messages ("
            "  account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,"
            "  message_id TEXT NOT NULL,"
            "  internal_date INTEGER NOT NULL,"
            "  first_seen_epoch INTEGER NOT NULL,"
            "  notified INTEGER NOT NULL DEFAULT 0,"
            "  PRIMARY KEY (account_id, message_id)"
            ")")));
        QVERIFY(query.exec(QStringLiteral(
            "INSERT INTO accounts (account_id, email, baseline_done, last_sync_epoch)"
            " VALUES ('acct', 'user@example.com', 1, 1234)")));
        QVERIFY(query.exec(QStringLiteral(
            "INSERT INTO seen_messages (account_id, message_id, internal_date,"
            " first_seen_epoch) VALUES ('acct', 'old-1', 42, 43)")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 1")));
        legacy.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    StateDatabase db(path);
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));

    AccountState state;
    QVERIFY2(db.accountState(QStringLiteral("acct"), &state, &error), qPrintable(error));
    QVERIFY(state.baselineDone);
    QCOMPARE(state.lastSyncEpoch, qint64(1234));
    QCOMPARE(state.email, QStringLiteral("user@example.com"));
    QCOMPARE(state.historyId, qint64(0));
    QCOMPARE(db.countSeenMessages(QStringLiteral("acct")), 1);

    QVERIFY2(db.setAccountHistoryId(QStringLiteral("acct"), 900, &error),
             qPrintable(error));
    QVERIFY2(db.accountState(QStringLiteral("acct"), &state, &error), qPrintable(error));
    QCOMPARE(state.historyId, qint64(900));

    // New accounts still get the migrated table with the cursor column.
    QVERIFY2(db.ensureAccount(QStringLiteral("acct-two"), QStringLiteral("x@example.com"),
                              &error),
             qPrintable(error));
    QVERIFY2(db.accountState(QStringLiteral("acct-two"), &state, &error), qPrintable(error));
    QCOMPARE(state.historyId, qint64(0));
}

void TestState::fileIsOwnerOnly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("state.db"));
    StateDatabase db(path);
    QString error;
    QVERIFY2(db.open(&error), qPrintable(error));

    const QFile::Permissions permissions = QFileInfo(path).permissions();
    QVERIFY(permissions.testFlag(QFile::ReadOwner));
    QVERIFY(permissions.testFlag(QFile::WriteOwner));
    QVERIFY(!permissions.testFlag(QFile::ReadGroup));
    QVERIFY(!permissions.testFlag(QFile::ReadOther));
}

QTEST_GUILESS_MAIN(TestState)
#include "test_state.moc"
