#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/ConfigStore.h"
#include "config/OAuthClientConfig.h"
#include "config/OAuthClientStore.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class TestConfig : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAreValid();
    void intervalBounds();
    void duplicateAccountsRejected();
    void configRoundTrip();
    void corruptConfigKeepsPreviousValues();
    void missingConfigIsFirstLaunch();
    void oauthPlaceholdersRejected();
    void oauthValidationRejectsBroadScope();
    void oauthValidationRejectsRemoteHttp();
    void oauthRoundTrip();
    void oauthConfigRejectsTokens();
    void oauthAcceptsDownloadedClientFormat();
    void oauthDownloadedFormatRejectsTokens();
    void appPathsAreNamespaced();
};

static AccountConfig account(const QString &id, const QString &email)
{
    AccountConfig account;
    account.id = id;
    account.email = email;
    account.displayName = QStringLiteral("Test");
    return account;
}

void TestConfig::defaultsAreValid()
{
    const AppConfig config = defaultAppConfig();
    QCOMPARE(config.notifications.pollIntervalMinutes, 180);
    QVERIFY(config.notifications.enabled);
    QVERIFY(config.notifications.includeUpdates);
    QVERIFY(config.notifications.showPreview);
    QVERIFY(config.accounts.isEmpty());
    QVERIFY(validateAppConfig(config).isEmpty());
}

void TestConfig::intervalBounds()
{
    AppConfig config = defaultAppConfig();
    config.notifications.pollIntervalMinutes = 4;
    QVERIFY(!validateAppConfig(config).isEmpty());
    config.notifications.pollIntervalMinutes = 5;
    QVERIFY(validateAppConfig(config).isEmpty());
    config.notifications.pollIntervalMinutes = 1440;
    QVERIFY(validateAppConfig(config).isEmpty());
    config.notifications.pollIntervalMinutes = 1441;
    QVERIFY(!validateAppConfig(config).isEmpty());
}

void TestConfig::duplicateAccountsRejected()
{
    AppConfig config = defaultAppConfig();
    config.accounts << account(QStringLiteral("a"), QStringLiteral("User@example.com"));
    config.accounts << account(QStringLiteral("b"), QStringLiteral("user@example.com"));
    QVERIFY(!validateAppConfig(config).isEmpty());

    config.accounts.clear();
    config.accounts << account(QStringLiteral("same"), QStringLiteral("one@example.com"));
    config.accounts << account(QStringLiteral("same"), QStringLiteral("two@example.com"));
    QVERIFY(!validateAppConfig(config).isEmpty());
}

void TestConfig::configRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nested/config.json"));

    ConfigStore store(path);
    QVERIFY2(store.load(), "missing config should be accepted as first launch");

    AppConfig config = defaultAppConfig();
    config.accounts << account(QStringLiteral("acct_1"), QStringLiteral("user@example.com"));
    config.notifications.pollIntervalMinutes = 30;
    config.notifications.includeUpdates = false;
    config.notifications.showPreview = false;
    config.notifications.enabled = false;

    QString error;
    QVERIFY2(store.save(config, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(path));

    const QFile::Permissions permissions = QFileInfo(path).permissions();
    QVERIFY(permissions.testFlag(QFile::ReadOwner));
    QVERIFY(permissions.testFlag(QFile::WriteOwner));
    QVERIFY(!permissions.testFlag(QFile::ReadGroup));
    QVERIFY(!permissions.testFlag(QFile::ReadOther));

    ConfigStore reloaded(path);
    QVERIFY2(reloaded.load(&error), qPrintable(error));
    QCOMPARE(reloaded.config().accounts.size(), 1);
    QCOMPARE(reloaded.config().accounts.first().email, QStringLiteral("user@example.com"));
    QCOMPARE(reloaded.config().notifications.pollIntervalMinutes, 30);
    QVERIFY(!reloaded.config().notifications.includeUpdates);
    QVERIFY(!reloaded.config().notifications.showPreview);
    QVERIFY(!reloaded.config().notifications.enabled);
}

void TestConfig::corruptConfigKeepsPreviousValues()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("config.json"));

    ConfigStore store(path);
    AppConfig config = defaultAppConfig();
    config.notifications.pollIntervalMinutes = 90;
    QString error;
    QVERIFY2(store.save(config, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("{ not json");
    file.close();

    QVERIFY(!store.load(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.config().notifications.pollIntervalMinutes, 90);
}

void TestConfig::missingConfigIsFirstLaunch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ConfigStore store(dir.filePath(QStringLiteral("config.json")));
    QString error;
    QVERIFY2(store.load(&error), qPrintable(error));
    QCOMPARE(store.config().notifications.pollIntervalMinutes, 180);
}

void TestConfig::oauthPlaceholdersRejected()
{
    OAuthClientConfig config = defaultOAuthClientConfig();
    config.clientId = QStringLiteral("REPLACE_WITH_CLIENT.apps.googleusercontent.com");
    config.clientSecret = QStringLiteral("REPLACE_WITH_SECRET");
    QVERIFY(!validateOAuthClientConfig(config).isEmpty());
    QVERIFY(isPlaceholderValue(QStringLiteral("YOUR_CLIENT_ID")));
}

void TestConfig::oauthValidationRejectsBroadScope()
{
    OAuthClientConfig config = defaultOAuthClientConfig();
    config.clientId = QStringLiteral("123456-demo.apps.googleusercontent.com");
    config.clientSecret = QStringLiteral("not-a-real-secret");
    config.scope = QStringLiteral("https://mail.google.com/");
    QVERIFY(!validateOAuthClientConfig(config).isEmpty());
    QVERIFY(validateOAuthClientConfig(config).join(' ').contains(QStringLiteral("scope")));
}

void TestConfig::oauthValidationRejectsRemoteHttp()
{
    OAuthClientConfig config = defaultOAuthClientConfig();
    config.clientId = QStringLiteral("123456-demo.apps.googleusercontent.com");
    config.clientSecret = QStringLiteral("not-a-real-secret");
    config.tokenUri = QUrl(QStringLiteral("http://example.com/token"));
    QVERIFY(!validateOAuthClientConfig(config).isEmpty());

    config.tokenUri = QUrl(QStringLiteral("http://127.0.0.1:1234/token"));
    QVERIFY(validateOAuthClientConfig(config).isEmpty());
}

void TestConfig::oauthRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("oauth.json"));
    OAuthClientStore store(path);

    QString error;
    QVERIFY(!store.load(&error));
    QVERIFY(error.contains(QStringLiteral("not found")));

    OAuthClientConfig config = defaultOAuthClientConfig();
    config.clientId = QStringLiteral("123456-demo.apps.googleusercontent.com");
    config.clientSecret = QStringLiteral("local-installed-app-secret");
    QVERIFY2(store.save(config, &error), qPrintable(error));

    OAuthClientStore reloaded(path);
    QVERIFY2(reloaded.load(&error), qPrintable(error));
    QCOMPARE(reloaded.config().clientId, config.clientId);
    QCOMPARE(reloaded.config().scope, config.scope);
}

void TestConfig::oauthConfigRejectsTokens()
{
    QJsonObject object;
    object.insert(QStringLiteral("client_id"),
                  QStringLiteral("123456-demo.apps.googleusercontent.com"));
    object.insert(QStringLiteral("client_secret"), QStringLiteral("local-installed-app-secret"));
    object.insert(QStringLiteral("refresh_token"), QStringLiteral("must-not-live-here"));

    OAuthClientConfig parsed;
    QStringList errors;
    QVERIFY(!oauthClientConfigFromJson(object, &parsed, &errors));
    QVERIFY(errors.join(' ').contains(QStringLiteral("refresh_token")));
}

void TestConfig::oauthAcceptsDownloadedClientFormat()
{
    QJsonObject installed;
    installed.insert(QStringLiteral("client_id"),
                     QStringLiteral("123456-demo.apps.googleusercontent.com"));
    installed.insert(QStringLiteral("client_secret"),
                     QStringLiteral("local-installed-app-secret"));
    installed.insert(QStringLiteral("auth_uri"),
                     QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth"));
    installed.insert(QStringLiteral("token_uri"),
                     QStringLiteral("https://oauth2.googleapis.com/token"));
    installed.insert(QStringLiteral("project_id"), QStringLiteral("demo-project"));
    installed.insert(QStringLiteral("redirect_uris"),
                     QJsonArray{QStringLiteral("http://localhost")});

    QJsonObject object;
    object.insert(QStringLiteral("installed"), installed);

    OAuthClientConfig parsed;
    QStringList errors;
    QVERIFY2(oauthClientConfigFromJson(object, &parsed, &errors),
             qPrintable(errors.join(' ')));
    QCOMPARE(parsed.clientId, QStringLiteral("123456-demo.apps.googleusercontent.com"));
    QCOMPARE(parsed.clientSecret, QStringLiteral("local-installed-app-secret"));
    QCOMPARE(parsed.authUri, QUrl(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth")));
    QCOMPARE(parsed.tokenUri, QUrl(QStringLiteral("https://oauth2.googleapis.com/token")));
    QCOMPARE(parsed.revokeUri, QUrl(QStringLiteral("https://oauth2.googleapis.com/revoke")));
    QCOMPARE(parsed.scope, QStringLiteral("https://www.googleapis.com/auth/gmail.readonly"));
}

void TestConfig::oauthDownloadedFormatRejectsTokens()
{
    QJsonObject installed;
    installed.insert(QStringLiteral("client_id"),
                     QStringLiteral("123456-demo.apps.googleusercontent.com"));
    installed.insert(QStringLiteral("client_secret"),
                     QStringLiteral("local-installed-app-secret"));
    installed.insert(QStringLiteral("refresh_token"), QStringLiteral("must-not-live-here"));

    QJsonObject object;
    object.insert(QStringLiteral("installed"), installed);

    OAuthClientConfig parsed;
    QStringList errors;
    QVERIFY(!oauthClientConfigFromJson(object, &parsed, &errors));
    QVERIFY(errors.join(' ').contains(QStringLiteral("refresh_token")));
}

void TestConfig::appPathsAreNamespaced()
{
    const AppPaths paths = AppPaths::fromConfigDir(QStringLiteral("/tmp/example-config"));
    QCOMPARE(paths.configFile, QStringLiteral("/tmp/example-config/config.json"));
    QCOMPARE(paths.oauthClientFile, QStringLiteral("/tmp/example-config/oauth-client.json"));
    QVERIFY(paths.stateFile.endsWith(QStringLiteral("/khat/state.db")));
    QVERIFY(paths.runtimeDir.endsWith(QStringLiteral("/khat")));
}

QTEST_GUILESS_MAIN(TestConfig)
#include "test_config.moc"
