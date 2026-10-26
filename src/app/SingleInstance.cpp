#include "app/SingleInstance.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QTimer>

#include <memory>
#include <utility>

namespace {
constexpr int kMaxCommandBytes = 1024;
constexpr int kMaxResponseBytes = 4096;
constexpr int kReconnectDelayMs = 50;

QString sanitizeMessage(const QString &message)
{
    QString clean = message;
    clean.replace(QLatin1Char('\r'), QLatin1Char(' '));
    clean.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (clean.size() > 512) {
        clean = clean.left(512);
    }
    return clean.trimmed();
}
} // namespace

SingleInstance::SingleInstance(QString runtimeDir, QObject *parent)
    : QObject(parent)
    , m_runtimeDir(QDir::cleanPath(std::move(runtimeDir)))
    , m_socketPath(m_runtimeDir + QStringLiteral("/instance.sock"))
{
}

SingleInstance::~SingleInstance()
{
    if (m_server) {
        m_server->close();
        QLocalServer::removeServer(m_socketPath);
    }
}

bool SingleInstance::acquire(QString *error)
{
    if (isPrimary()) {
        return true;
    }
    if (m_runtimeDir.isEmpty() || m_runtimeDir == QStringLiteral(".")) {
        if (error) {
            *error = QStringLiteral("The user runtime directory is unavailable.");
        }
        return false;
    }

    QDir directory(m_runtimeDir);
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        if (error) {
            *error = QStringLiteral("Cannot create the runtime directory: %1")
                         .arg(m_runtimeDir);
        }
        return false;
    }
    const QString absoluteRuntimeDir = directory.absolutePath();
    if (!QFile::setPermissions(absoluteRuntimeDir,
                               QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        if (error) {
            *error = QStringLiteral("Cannot secure the runtime directory: %1")
                         .arg(absoluteRuntimeDir);
        }
        return false;
    }
    m_runtimeDir = absoluteRuntimeDir;
    m_socketPath = m_runtimeDir + QStringLiteral("/instance.sock");

    m_lock = std::make_unique<QLockFile>(
        m_runtimeDir + QStringLiteral("/instance.lock"));
    if (!m_lock->tryLock(0)) {
        if (error) {
            *error = m_lock->error() == QLockFile::LockFailedError
                ? QStringLiteral("Another gmail-notifier instance is already running.")
                : QStringLiteral("Cannot acquire the gmail-notifier instance lock: %1")
                      .arg(m_lock->error() == QLockFile::PermissionError
                               ? QStringLiteral("permission denied")
                               : QStringLiteral("unavailable"));
        }
        m_lock.reset();
        return false;
    }

    // Safe now that this process owns the lock: any previous socket belongs to
    // a process that exited without cleaning up.
    QLocalServer::removeServer(m_socketPath);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_socketPath)) {
        if (error) {
            *error = QStringLiteral("Cannot listen for gmail-notifier commands: %1")
                         .arg(m_server->errorString());
        }
        delete m_server;
        m_server = nullptr;
        m_lock.reset();
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this,
            &SingleInstance::handleNewConnection);
    return true;
}

bool SingleInstance::isPrimary() const
{
    return m_lock && m_lock->isLocked() && m_server && m_server->isListening();
}

void SingleInstance::setCommandHandler(CommandHandler handler)
{
    m_commandHandler = std::move(handler);
}

bool SingleInstance::sendCommand(const QString &command, QString *response,
                                 QString *error, int timeoutMs)
{
    if (response) {
        response->clear();
    }
    if (error) {
        error->clear();
    }
    if (command.isEmpty() || command.size() > 128 || command.contains(QLatin1Char('\n'))
        || command.contains(QLatin1Char('\r'))) {
        if (error) {
            *error = QStringLiteral("The instance command is invalid.");
        }
        return false;
    }

    QLocalSocket socket;
    QEventLoop loop;
    QTimer deadline;
    QTimer reconnect;
    QByteArray buffer;
    QElapsedTimer elapsed;
    bool done = false;
    bool success = false;
    QString responseText;
    QString errorText;

    timeoutMs = qBound(100, timeoutMs, 30000);
    deadline.setSingleShot(true);
    reconnect.setSingleShot(true);
    elapsed.start();

    auto finish = [&](bool ok, const QString &text) {
        if (done) {
            return;
        }
        done = true;
        success = ok;
        if (ok) {
            responseText = text;
        } else {
            errorText = text;
        }
        loop.quit();
    };

    auto attempt = [&] {
        if (done) {
            return;
        }
        if (elapsed.elapsed() >= timeoutMs) {
            finish(false, QStringLiteral("Timed out waiting for the gmail-notifier instance."));
            return;
        }
        socket.abort();
        buffer.clear();
        socket.connectToServer(m_socketPath);
    };

    connect(&socket, &QLocalSocket::connected, &loop, [&] {
        reconnect.stop();
        socket.write(command.toUtf8() + '\n');
        socket.flush();
    });

    connect(&socket, &QLocalSocket::readyRead, &loop, [&] {
        buffer.append(socket.readAll());
        if (buffer.size() > kMaxResponseBytes) {
            finish(false, QStringLiteral("The gmail-notifier instance response was too large."));
            return;
        }
        const int newline = buffer.indexOf('\n');
        if (newline < 0) {
            return;
        }

        QByteArray line = buffer.left(newline);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        const QString text = QString::fromUtf8(line).trimmed();
        if (text == QStringLiteral("ok")) {
            finish(true, QString());
        } else if (text.startsWith(QStringLiteral("ok "))) {
            finish(true, text.mid(3));
        } else if (text == QStringLiteral("error")) {
            finish(false, QStringLiteral("The gmail-notifier instance rejected the command."));
        } else if (text.startsWith(QStringLiteral("error "))) {
            finish(false, text.mid(6));
        } else {
            finish(false, QStringLiteral("The gmail-notifier instance sent an invalid response."));
        }
    });

    connect(&socket, &QLocalSocket::errorOccurred, &loop,
            [&](QLocalSocket::LocalSocketError socketError) {
                if (done) {
                    return;
                }
                const bool retryable = socketError == QLocalSocket::ConnectionRefusedError
                    || socketError == QLocalSocket::ServerNotFoundError;
                if (retryable && elapsed.elapsed() + kReconnectDelayMs < timeoutMs) {
                    reconnect.start(kReconnectDelayMs);
                    return;
                }
                if (socketError == QLocalSocket::PeerClosedError && buffer.isEmpty()) {
                    finish(false,
                           QStringLiteral("The gmail-notifier instance closed the connection "
                                          "without a response."));
                    return;
                }
                finish(false, QStringLiteral("Cannot contact the gmail-notifier instance: %1")
                                  .arg(socket.errorString()));
            });

    connect(&reconnect, &QTimer::timeout, &loop, attempt);
    connect(&deadline, &QTimer::timeout, &loop, [&] {
        finish(false, QStringLiteral("Timed out waiting for the gmail-notifier instance."));
    });

    deadline.start(timeoutMs);
    attempt();
    loop.exec();

    deadline.stop();
    reconnect.stop();
    socket.abort();

    if (response) {
        *response = responseText;
    }
    if (!success && error) {
        *error = errorText;
    }
    return success;
}

void SingleInstance::handleNewConnection()
{
    if (!m_server) {
        return;
    }
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QLocalSocket::disconnected, this,
                [socket] { socket->deleteLater(); });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, buffer] {
            if (socket->property("commandHandled").toBool()) {
                return;
            }
            buffer->append(socket->readAll());
            if (buffer->size() > kMaxCommandBytes + 2) {
                socket->setProperty("commandHandled", true);
                handleCommand(socket, QByteArray());
                return;
            }

            const int newline = buffer->indexOf('\n');
            if (newline < 0) {
                return;
            }
            QByteArray line = buffer->left(newline);
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            socket->setProperty("commandHandled", true);
            handleCommand(socket, line);
        });
    }
}

void SingleInstance::handleCommand(QLocalSocket *socket, const QByteArray &line)
{
    const QString command = QString::fromUtf8(line).trimmed();
    bool accepted = false;
    QString message;

    if (command.isEmpty()) {
        message = QStringLiteral("The command is empty or too long.");
    } else if (!m_commandHandler) {
        message = QStringLiteral("The notifier is still starting.");
    } else {
        accepted = m_commandHandler(command, &message);
        if (!accepted && message.isEmpty()) {
            message = QStringLiteral("The command failed.");
        }
    }

    QByteArray response = accepted ? QByteArrayLiteral("ok") : QByteArrayLiteral("error");
    const QString clean = sanitizeMessage(message);
    if (!clean.isEmpty()) {
        response += ' ';
        response += clean.toUtf8();
    }
    response += '\n';

    socket->write(response);
    socket->flush();
    socket->disconnectFromServer();
}
