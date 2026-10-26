#include "notify/NotificationService.h"

#include "notify/NotificationBackend.h"

#include <QDesktopServices>

NotificationService::NotificationService(NotificationBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    if (m_backend) {
        connect(m_backend, &NotificationBackend::activated, this,
                &NotificationService::handleActivated);
        connect(m_backend, &NotificationBackend::failed, this,
                &NotificationService::notificationFailed);
    }
}

void NotificationService::setUrlOpener(UrlOpener opener)
{
    m_urlOpener = std::move(opener);
}

bool NotificationService::notify(const NotificationContent &content, QString *error)
{
    if (!m_backend) {
        if (error) {
            *error = QStringLiteral("No notification backend is configured.");
        }
        return false;
    }
    if (content.url.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Notification has no Gmail URL.");
        }
        return false;
    }

    const QUrl url(content.url);
    if (!url.isValid() || url.scheme() != QStringLiteral("https")
        || url.host() != QStringLiteral("mail.google.com")) {
        if (error) {
            *error = QStringLiteral("Refusing a non-Gmail notification URL.");
        }
        return false;
    }

    return m_backend->notify(content, error);
}

void NotificationService::handleActivated(const QString &urlText)
{
    const QUrl url(urlText);
    if (!url.isValid() || url.scheme() != QStringLiteral("https")
        || url.host() != QStringLiteral("mail.google.com")) {
        emit urlOpenFailed(url);
        return;
    }

    const bool opened = m_urlOpener ? m_urlOpener(url) : QDesktopServices::openUrl(url);
    if (!opened) {
        emit urlOpenFailed(url);
    }
}
