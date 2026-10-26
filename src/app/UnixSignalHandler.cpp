#include "app/UnixSignalHandler.h"

#include <QSocketNotifier>
#include <QString>

#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace {
int g_signalWriteFd = -1;

void handleUnixSignal(int signalNumber)
{
    if (g_signalWriteFd < 0) {
        return;
    }
    const unsigned char value = static_cast<unsigned char>(signalNumber);
    const ssize_t ignored = ::write(g_signalWriteFd, &value, sizeof(value));
    (void)ignored;
}

const int kHandledSignals[] = {SIGTERM, SIGINT, SIGHUP};
} // namespace

UnixSignalHandler::UnixSignalHandler(QObject *parent)
    : QObject(parent)
{
}

UnixSignalHandler::~UnixSignalHandler()
{
    uninstall();
}

bool UnixSignalHandler::install(QString *error)
{
    if (m_installed) {
        return true;
    }
    if (g_signalWriteFd >= 0) {
        if (error) {
            *error = QStringLiteral("A process signal handler is already installed.");
        }
        return false;
    }

    int fds[2] = {-1, -1};
#ifdef __linux__
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
#else
    if (::pipe(fds) != 0) {
#endif
        if (error) {
            *error = QStringLiteral("Cannot create the signal pipe: %1")
                         .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        return false;
    }
#ifndef __linux__
    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(fds[0], F_SETFD, ::fcntl(fds[0], F_GETFD) | FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    ::fcntl(fds[1], F_SETFD, ::fcntl(fds[1], F_GETFD) | FD_CLOEXEC);
#endif

    m_readFd = fds[0];
    m_writeFd = fds[1];
    g_signalWriteFd = m_writeFd;

    struct sigaction action {};
    action.sa_handler = handleUnixSignal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;

    for (const int signalNumber : kHandledSignals) {
        if (::sigaction(signalNumber, &action, nullptr) != 0) {
            if (error) {
                *error = QStringLiteral("Cannot install a signal handler: %1")
                             .arg(QString::fromLocal8Bit(std::strerror(errno)));
            }
            uninstall();
            return false;
        }
    }

    m_notifier = new QSocketNotifier(m_readFd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this,
            &UnixSignalHandler::handleReadable);
    m_installed = true;
    return true;
}

void UnixSignalHandler::uninstall()
{
    // A handler that never installed anything must not reset the process-wide
    // signal actions owned by another live handler.
    if (!m_installed && !m_notifier && m_readFd < 0 && m_writeFd < 0) {
        return;
    }

    if (m_notifier) {
        m_notifier->setEnabled(false);
        delete m_notifier;
        m_notifier = nullptr;
    }

    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    for (const int signalNumber : kHandledSignals) {
        ::sigaction(signalNumber, &action, nullptr);
    }

    g_signalWriteFd = -1;
    if (m_readFd >= 0) {
        ::close(m_readFd);
        m_readFd = -1;
    }
    if (m_writeFd >= 0) {
        ::close(m_writeFd);
        m_writeFd = -1;
    }
    m_installed = false;
}

void UnixSignalHandler::handleReadable()
{
    unsigned char values[64];
    int lastSignal = 0;
    for (;;) {
        const ssize_t count = ::read(m_readFd, values, sizeof(values));
        if (count > 0) {
            for (ssize_t i = 0; i < count; ++i) {
                lastSignal = values[i];
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    if (lastSignal > 0) {
        emit terminationRequested(lastSignal);
    }
}
