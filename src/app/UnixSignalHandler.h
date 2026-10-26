#pragma once

#include <QObject>

class QSocketNotifier;

// Turns SIGTERM, SIGINT, and SIGHUP into a normal Qt event so the application
// can stop the scheduler, release the single-instance lock, and close SQLite
// cleanly instead of being terminated abruptly.
class UnixSignalHandler : public QObject
{
    Q_OBJECT

public:
    explicit UnixSignalHandler(QObject *parent = nullptr);
    ~UnixSignalHandler() override;

    bool install(QString *error = nullptr);
    void uninstall();

signals:
    void terminationRequested(int signalNumber);

private:
    void handleReadable();

    QSocketNotifier *m_notifier = nullptr;
    int m_readFd = -1;
    int m_writeFd = -1;
    bool m_installed = false;
};
