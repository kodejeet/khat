#pragma once

#include "notify/NotificationContent.h"

#include <QObject>
#include <QUrl>

#include <functional>

class NotificationBackend;

class NotificationService : public QObject
{
    Q_OBJECT

public:
    using UrlOpener = std::function<bool(const QUrl &)>;

    explicit NotificationService(NotificationBackend *backend, QObject *parent = nullptr);

    // Injectable for tests. The default opens the URL with QDesktopServices.
    void setUrlOpener(UrlOpener opener);

    bool notify(const NotificationContent &content, QString *error = nullptr);

signals:
    void notificationFailed(const QString &error);
    void urlOpenFailed(const QUrl &url);

private slots:
    void handleActivated(const QString &url);

private:
    NotificationBackend *m_backend;
    UrlOpener m_urlOpener;
};
