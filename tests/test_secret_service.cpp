#include "auth/SecretServiceTokenStore.h"
#include "auth/SecretServiceTypes.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSignalSpy>
#include <QtTest>

#include <memory>

namespace {
constexpr auto kServiceName = "org.freedesktop.secrets";
constexpr auto kServicePath = "/org/freedesktop/secrets";
constexpr auto kCollectionPath = "/org/freedesktop/secrets/collection/test";
constexpr auto kSessionPath = "/org/freedesktop/secrets/session/test";

struct FakeItem
{
    QString path;
    SecretServiceAttributes attributes;
    QString label;
    QByteArray value;
    QString contentType;
    bool locked = false;
};

class FakeCollection;

class FakeSecretItem : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Item")

public:
    FakeSecretItem(FakeCollection *collection, QString path, QObject *parent = nullptr);

public slots:
    QDBusObjectPath Delete();
    SecretServiceSecret GetSecret(const QDBusObjectPath &session);

private:
    FakeCollection *m_collection;
    QString m_path;
};

class FakeCollection : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Collection")

public:
    explicit FakeCollection(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    const QMap<QString, FakeItem> &items() const { return m_items; }
    int createCalls() const { return m_createCalls; }

    void addLockedItem(const QString &path, const QString &accountId, const QByteArray &value)
    {
        FakeItem item;
        item.path = path;
        item.attributes.insert(QStringLiteral("application"), QStringLiteral("gmail-notifier"));
        item.attributes.insert(QStringLiteral("account"), accountId);
        item.attributes.insert(QStringLiteral("type"), QStringLiteral("oauth-token"));
        item.label = QStringLiteral("locked test item");
        item.value = value;
        item.contentType = QStringLiteral("application/json");
        item.locked = true;
        m_items.insert(path, item);
    }

    bool unlockAll(const QList<QDBusObjectPath> &paths)
    {
        for (const QDBusObjectPath &path : paths) {
            auto it = m_items.find(path.path());
            if (it != m_items.end()) {
                it->locked = false;
            }
        }
        return true;
    }

    bool removeItem(const QString &path)
    {
        return m_items.remove(path) > 0;
    }

    const FakeItem *item(const QString &path) const
    {
        const auto it = m_items.constFind(path);
        return it == m_items.constEnd() ? nullptr : &it.value();
    }

public slots:
    QDBusObjectPath CreateItem(const QVariantMap &properties,
                               const SecretServiceSecret &secret, bool replace,
                               QDBusObjectPath &prompt);
    QList<QDBusObjectPath> SearchItems(const SecretServiceAttributes &attributes,
                                       QList<QDBusObjectPath> &locked);

private:
    QMap<QString, FakeItem> m_items;
    int m_createCalls = 0;
};

class FakeSecretService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Service")

public:
    explicit FakeSecretService(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    FakeCollection *collection() { return &m_collection; }
    bool failUnlock = false;
    int getSecretsCalls = 0;

public slots:
    QDBusVariant OpenSession(const QString &algorithm, const QDBusVariant &input,
                             QDBusObjectPath &result);
    QList<QDBusObjectPath> SearchItems(const SecretServiceAttributes &attributes,
                                       QList<QDBusObjectPath> &locked);
    QList<QDBusObjectPath> Unlock(const QList<QDBusObjectPath> &objects,
                                  QDBusObjectPath &prompt);
    SecretServiceSecrets GetSecrets(const QList<QDBusObjectPath> &items,
                                    const QDBusObjectPath &session);
    QDBusObjectPath ReadAlias(const QString &alias);

private:
    FakeCollection m_collection;
};

FakeSecretItem::FakeSecretItem(FakeCollection *collection, QString path, QObject *parent)
    : QObject(parent)
    , m_collection(collection)
    , m_path(std::move(path))
{
}

QDBusObjectPath FakeSecretItem::Delete()
{
    m_collection->removeItem(m_path);
    return QDBusObjectPath(QStringLiteral("/"));
}

SecretServiceSecret FakeSecretItem::GetSecret(const QDBusObjectPath &session)
{
    SecretServiceSecret secret;
    secret.session = session;
    if (const FakeItem *item = m_collection->item(m_path)) {
        secret.value = item->value;
        secret.contentType = item->contentType;
    }
    return secret;
}

QDBusObjectPath FakeCollection::CreateItem(const QVariantMap &properties,
                                           const SecretServiceSecret &secret, bool replace,
                                           QDBusObjectPath &prompt)
{
    prompt = QDBusObjectPath(QStringLiteral("/"));
    ++m_createCalls;

    const QVariant attributesValue =
        properties.value(QStringLiteral("org.freedesktop.Secret.Item.Attributes"));
    SecretServiceAttributes attributes;
    if (attributesValue.metaType().id() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument argument = attributesValue.value<QDBusArgument>();
        argument >> attributes;
    }
    const QString label =
        properties.value(QStringLiteral("org.freedesktop.Secret.Item.Label")).toString();

    if (replace) {
        for (auto it = m_items.begin(); it != m_items.end(); ++it) {
            if (it->attributes == attributes) {
                it->label = label;
                it->value = secret.value;
                it->contentType = secret.contentType;
                return QDBusObjectPath(it->path);
            }
        }
    }

    FakeItem item;
    item.path = QStringLiteral("/org/freedesktop/secrets/item/%1").arg(m_items.size() + 1);
    item.attributes = attributes;
    item.label = label;
    item.value = secret.value;
    item.contentType = secret.contentType;
    m_items.insert(item.path, item);

    auto *object = new FakeSecretItem(this, item.path, this);
    QDBusConnection::sessionBus().registerObject(item.path, object,
                                                 QDBusConnection::ExportAllSlots);
    return QDBusObjectPath(item.path);
}

QList<QDBusObjectPath> FakeCollection::SearchItems(
    const SecretServiceAttributes &attributes, QList<QDBusObjectPath> &locked)
{
    locked.clear();
    QList<QDBusObjectPath> found;
    for (const FakeItem &item : std::as_const(m_items)) {
        bool matches = true;
        for (auto it = attributes.cbegin(); it != attributes.cend(); ++it) {
            if (item.attributes.value(it.key()) != it.value()) {
                matches = false;
                break;
            }
        }
        if (matches) {
            if (item.locked) {
                locked.append(QDBusObjectPath(item.path));
            } else {
                found.append(QDBusObjectPath(item.path));
            }
        }
    }
    return found;
}

QDBusVariant FakeSecretService::OpenSession(const QString &algorithm,
                                            const QDBusVariant &input,
                                            QDBusObjectPath &result)
{
    Q_UNUSED(algorithm)
    Q_UNUSED(input)
    result = QDBusObjectPath(QString::fromLatin1(kSessionPath));
    return QDBusVariant(QString());
}

QList<QDBusObjectPath> FakeSecretService::SearchItems(
    const SecretServiceAttributes &attributes, QList<QDBusObjectPath> &locked)
{
    return m_collection.SearchItems(attributes, locked);
}

QList<QDBusObjectPath> FakeSecretService::Unlock(const QList<QDBusObjectPath> &objects,
                                                 QDBusObjectPath &prompt)
{
    prompt = QDBusObjectPath(QStringLiteral("/"));
    if (failUnlock) {
        return {};
    }
    m_collection.unlockAll(objects);
    return objects;
}

SecretServiceSecrets FakeSecretService::GetSecrets(const QList<QDBusObjectPath> &items,
                                                   const QDBusObjectPath &session)
{
    getSecretsCalls++;
    SecretServiceSecrets secrets;
    for (const QDBusObjectPath &path : items) {
        if (const FakeItem *item = m_collection.item(path.path())) {
            SecretServiceSecret secret;
            secret.session = session;
            secret.value = item->value;
            secret.contentType = item->contentType;
            secrets.insert(path, secret);
        }
    }
    return secrets;
}

QDBusObjectPath FakeSecretService::ReadAlias(const QString &alias)
{
    if (alias == QStringLiteral("default")) {
        return QDBusObjectPath(QString::fromLatin1(kCollectionPath));
    }
    return QDBusObjectPath(QStringLiteral("/"));
}

struct FakeEnvironment
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    std::unique_ptr<FakeSecretService> service = std::make_unique<FakeSecretService>();
    bool registered = false;

    bool start(QString *error)
    {
        registerSecretServiceTypes();
        if (!bus.isConnected()) {
            if (error) {
                *error = QStringLiteral("no session bus");
            }
            return false;
        }
        if (!bus.registerService(QString::fromLatin1(kServiceName))) {
            if (error) {
                *error = bus.lastError().message();
            }
            return false;
        }
        if (!bus.registerObject(QString::fromLatin1(kServicePath), service.get(),
                                QDBusConnection::ExportAllSlots)) {
            if (error) {
                *error = bus.lastError().message();
            }
            return false;
        }
        if (!bus.registerObject(QString::fromLatin1(kCollectionPath),
                                service->collection(),
                                QDBusConnection::ExportAllSlots)) {
            if (error) {
                *error = bus.lastError().message();
            }
            return false;
        }
        registered = true;
        return true;
    }

    ~FakeEnvironment()
    {
        if (!registered) {
            return;
        }
        for (const FakeItem &item : service->collection()->items()) {
            bus.unregisterObject(item.path);
        }
        bus.unregisterObject(QString::fromLatin1(kCollectionPath));
        bus.unregisterObject(QString::fromLatin1(kServicePath));
        bus.unregisterService(QString::fromLatin1(kServiceName));
    }
};

OAuthToken testToken(const QString &access, const QString &refresh)
{
    OAuthToken token;
    token.accessToken = access;
    token.refreshToken = refresh;
    token.tokenType = QStringLiteral("Bearer");
    token.scope = QStringLiteral("https://www.googleapis.com/auth/gmail.readonly");
    token.expiresAt = QDateTime::currentDateTimeUtc().addSecs(3600);
    return token;
}
} // namespace

class TestSecretService : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void savesLoadsReplacesAndRemoves();
    void missingServiceFailsGracefully();
    void lockedKeyringReportsError();
};

void TestSecretService::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("GMAIL_NOTIFIER_TEST_PRIVATE_BUS")) {
        QSKIP("Run through ctest under dbus-run-session (private session bus only).");
    }
    QVERIFY2(QDBusConnection::sessionBus().isConnected(), "private session bus unavailable");
}

void TestSecretService::savesLoadsReplacesAndRemoves()
{
    FakeEnvironment environment;
    QString environmentError;
    QVERIFY2(environment.start(&environmentError), qPrintable(environmentError));

    SecretServiceTokenStore store(environment.bus, false);
    QSignalSpy saved(&store, &SecretServiceTokenStore::saved);
    QSignalSpy loaded(&store, &SecretServiceTokenStore::loaded);
    QSignalSpy removed(&store, &SecretServiceTokenStore::removed);

    const OAuthToken first = testToken(QStringLiteral("access-one"),
                                       QStringLiteral("refresh-one"));
    store.save(QStringLiteral("account-one"), first);
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 5000);
    QVERIFY2(saved.first().at(1).toString().isEmpty(),
             qPrintable(saved.first().at(1).toString()));
    QCOMPARE(environment.service->collection()->items().size(), 1);
    QCOMPARE(environment.service->collection()->createCalls(), 1);

    const FakeItem &stored = environment.service->collection()->items().first();
    QCOMPARE(stored.attributes.value(QStringLiteral("application")),
             QStringLiteral("gmail-notifier"));
    QCOMPARE(stored.attributes.value(QStringLiteral("account")),
             QStringLiteral("account-one"));
    QCOMPARE(stored.contentType, QStringLiteral("application/json"));
    const QJsonObject storedJson = QJsonDocument::fromJson(stored.value).object();
    QCOMPARE(storedJson.value(QStringLiteral("access_token")).toString(),
             QStringLiteral("access-one"));
    QCOMPARE(storedJson.value(QStringLiteral("refresh_token")).toString(),
             QStringLiteral("refresh-one"));

    store.load(QStringLiteral("account-one"));
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 5000);
    QCOMPARE(loaded.first().at(1).toBool(), true);
    QVERIFY(loaded.first().at(3).toString().isEmpty());
    const OAuthToken loadedToken = loaded.first().at(2).value<OAuthToken>();
    QCOMPARE(loadedToken.accessToken, QStringLiteral("access-one"));
    QCOMPARE(loadedToken.refreshToken, QStringLiteral("refresh-one"));
    QVERIFY(loadedToken.expiresAt.isValid());
    QVERIFY(loadedToken.scope.contains(QStringLiteral("gmail.readonly")));

    const OAuthToken second = testToken(QStringLiteral("access-two"),
                                        QStringLiteral("refresh-two"));
    store.save(QStringLiteral("account-one"), second);
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 2, 5000);
    QVERIFY(saved.at(1).at(1).toString().isEmpty());
    QCOMPARE(environment.service->collection()->items().size(), 1);
    QCOMPARE(environment.service->collection()->createCalls(), 2);

    loaded.clear();
    store.load(QStringLiteral("account-one"));
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 5000);
    QCOMPARE(loaded.first().at(2).value<OAuthToken>().accessToken,
             QStringLiteral("access-two"));

    store.remove(QStringLiteral("account-one"));
    QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 5000);
    QVERIFY(removed.first().at(1).toString().isEmpty());
    QCOMPARE(environment.service->collection()->items().size(), 0);

    loaded.clear();
    store.load(QStringLiteral("account-one"));
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 5000);
    QCOMPARE(loaded.first().at(1).toBool(), false);
    QVERIFY(loaded.first().at(3).toString().isEmpty());
}

void TestSecretService::missingServiceFailsGracefully()
{
    QDBusConnection missing(QStringLiteral("gmail-notifier-test-unconnected"));
    SecretServiceTokenStore store(missing, false);
    QSignalSpy loaded(&store, &SecretServiceTokenStore::loaded);

    store.load(QStringLiteral("account-one"));
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 5000);
    QCOMPARE(loaded.first().at(1).toBool(), false);
    QVERIFY(!loaded.first().at(3).toString().isEmpty());
}

void TestSecretService::lockedKeyringReportsError()
{
    FakeEnvironment environment;
    QString environmentError;
    QVERIFY2(environment.start(&environmentError), qPrintable(environmentError));

    const QByteArray value = QJsonDocument(oauthTokenToJson(
                                                  testToken(QStringLiteral("locked-access"),
                                                            QStringLiteral("locked-refresh"))))
                                 .toJson(QJsonDocument::Compact);
    environment.service->collection()->addLockedItem(
        QStringLiteral("/org/freedesktop/secrets/item/locked"),
        QStringLiteral("account-locked"), value);
    environment.service->failUnlock = true;

    SecretServiceTokenStore store(environment.bus, false);
    QSignalSpy loaded(&store, &SecretServiceTokenStore::loaded);
    store.load(QStringLiteral("account-locked"));
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 5000);
    QCOMPARE(loaded.first().at(1).toBool(), false);
    QVERIFY2(loaded.first().at(3).toString().contains(QStringLiteral("locked")),
             qPrintable(loaded.first().at(3).toString()));
    QCOMPARE(environment.service->getSecretsCalls, 0);

    QSignalSpy removed(&store, &SecretServiceTokenStore::removed);
    store.remove(QStringLiteral("account-locked"));
    QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 5000);
    QVERIFY(removed.first().at(1).toString().contains(QStringLiteral("locked")));
    QCOMPARE(environment.service->collection()->items().size(), 1);
}

QTEST_GUILESS_MAIN(TestSecretService)
#include "test_secret_service.moc"
