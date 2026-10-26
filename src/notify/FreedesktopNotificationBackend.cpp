#include "notify/FreedesktopNotificationBackend.h"

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QStringList>
#include <QVariantMap>

namespace {
constexpr int kCallTimeoutMs = 5000;
constexpr int kDefaultTimeoutMs = -1;
constexpr auto kInterface = "org.freedesktop.Notifications";
} // namespace

FreedesktopNotificationBackend::FreedesktopNotificationBackend(QObject *parent)
    : FreedesktopNotificationBackend(QDBusConnection::sessionBus(), true, parent)
{
}

FreedesktopNotificationBackend::FreedesktopNotificationBackend(
    const QDBusConnection &connection, bool autoStartService, QObject *parent)
    : NotificationBackend(parent)
    , m_connection(connection)
    , m_autoStartService(autoStartService)
{
    connectToServer();
}

FreedesktopNotificationBackend::~FreedesktopNotificationBackend() = default;

void FreedesktopNotificationBackend::connectToServer()
{
    if (!m_connection.isConnected()) {
        return;
    }

    m_connection.connect(m_serviceName, m_objectPath, QString::fromLatin1(kInterface),
                         QStringLiteral("ActionInvoked"), this,
                         SLOT(handleActionInvoked(uint, QString)));
    m_connection.connect(m_serviceName, m_objectPath, QString::fromLatin1(kInterface),
                         QStringLiteral("NotificationClosed"), this,
                         SLOT(handleNotificationClosed(uint, uint)));
}

bool FreedesktopNotificationBackend::notify(const NotificationContent &content,
                                            QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (!m_connection.isConnected()) {
        return fail(QStringLiteral("The session D-Bus is not available."));
    }
    if (content.summary.isEmpty()) {
        return fail(QStringLiteral("Notification summary is empty."));
    }
    if (content.url.isEmpty()) {
        return fail(QStringLiteral("Notification has no Gmail URL."));
    }

    QDBusMessage message =
        QDBusMessage::createMethodCall(m_serviceName, m_objectPath,
                                       QString::fromLatin1(kInterface),
                                       QStringLiteral("Notify"));

    QVariantMap hints;
    hints.insert(QStringLiteral("urgency"),
                 QVariant::fromValue<uchar>(static_cast<uchar>(qBound(0, content.urgency, 2))));
    hints.insert(QStringLiteral("category"), QStringLiteral("email"));
    hints.insert(QStringLiteral("desktop-entry"), QStringLiteral("gmail-notifier"));

    const QString actionLabel = content.actionLabel.isEmpty()
        ? QStringLiteral("Open in Gmail")
        : content.actionLabel;
    const QStringList actions{QStringLiteral("default"), actionLabel};

    message << QStringLiteral("Gmail Notifier") << 0u << QStringLiteral("mail-unread")
            << content.summary << content.body << actions << hints << kDefaultTimeoutMs;
    message.setAutoStartService(m_autoStartService);

    const QString url = content.url;
    auto *watcher =
        new QDBusPendingCallWatcher(m_connection.asyncCall(message, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, url] {
                QDBusPendingReply<uint> reply(*watcher);
                watcher->deleteLater();
                if (reply.isError()) {
                    emit failed(QStringLiteral("Notification server rejected the message: %1")
                                    .arg(reply.error().message()));
                    return;
                }

                const uint id = reply.value();
                m_urls.insert(id, url);
                emit notificationSent(id);
            });

    return true;
}

void FreedesktopNotificationBackend::handleActionInvoked(uint id, const QString &action)
{
    if (action != QStringLiteral("default")) {
        return;
    }
    const QString url = m_urls.take(id);
    if (!url.isEmpty()) {
        emit activated(url);
    }
}

void FreedesktopNotificationBackend::handleNotificationClosed(uint id, uint)
{
    m_urls.remove(id);
}
