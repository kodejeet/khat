#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QLockFile;

// One process owns a lock and a private Unix socket in the user runtime
// directory. Additional launches can forward "show-settings" or "check-now"
// requests to that process instead of starting a second notifier.
class SingleInstance : public QObject
{
    Q_OBJECT

public:
    using CommandHandler = std::function<bool(const QString &command, QString *message)>;

    explicit SingleInstance(QString runtimeDir, QObject *parent = nullptr);
    ~SingleInstance() override;

    // Creates the runtime directory and acquires both the lock and socket.
    // A false return normally means another instance owns the lock.
    bool acquire(QString *error = nullptr);
    bool isPrimary() const;
    QString socketPath() const { return m_socketPath; }

    void setCommandHandler(CommandHandler handler);

    // Sends one request to the primary instance. The call spins a local event
    // loop while waiting, so it works from a secondary process before its main
    // event loop starts (and from same-process tests).
    bool sendCommand(const QString &command, QString *response = nullptr,
                     QString *error = nullptr, int timeoutMs = 3000);

private:
    void handleNewConnection();
    void handleCommand(QLocalSocket *socket, const QByteArray &line);

    QString m_runtimeDir;
    QString m_socketPath;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer *m_server = nullptr;
    CommandHandler m_commandHandler;
};
