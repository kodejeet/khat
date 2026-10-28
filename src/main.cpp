#include "app/AppController.h"
#include "app/SingleInstance.h"
#include "app/UnixSignalHandler.h"
#include "config/AppPaths.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLoggingCategory>
#include <QLocalSocket>
#include <QSessionManager>
#include <QSize>
#include <QTextStream>
#include <QThread>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum class RunMode
{
    Settings,
    Background,
    CheckNow,
    Exit,
};

namespace {
// The detached grandchild reports readiness through this inherited file
// descriptor (see launchDetachedBackground).
constexpr const char kReadinessFdEnv[] = "KHAT_BACKGROUND_READINESS_FD";

int fail(const QString &message)
{
    QTextStream(stderr) << "khat: " << message << '\n';
    return 1;
}

QIcon applicationIcon()
{
    const QIcon themed = QIcon::fromTheme(QStringLiteral("khat"));
    if (!themed.isNull()) {
        return themed;
    }

    QIcon fallback;
    for (const int size : {16, 24, 32, 48, 64, 128, 256, 512}) {
        fallback.addFile(QStringLiteral(":/icons/khat-%1.png").arg(size),
                         QSize(size, size));
    }
    return fallback;
}

// The xdg-desktop-portal app registry rejects the registration of further
// instances whose application ID is already claimed, and Qt prints that
// warning on every secondary launch. It is expected and harmless here.
QtMessageHandler previousMessageHandler = nullptr;

void messageFilter(QtMsgType type, const QMessageLogContext &context,
                   const QString &message)
{
    if (message.contains(QStringLiteral("Failed to register with host portal"))) {
        return;
    }
    if (previousMessageHandler) {
        previousMessageHandler(type, context, message);
    }
}

void installPortalNoiseFilter()
{
    previousMessageHandler = qInstallMessageHandler(&messageFilter);
}

bool runningUnderSystemdUnit()
{
    return qEnvironmentVariableIsSet("INVOCATION_ID");
}

// Called by the detached child after the single-instance slot is secured.
void confirmDetachedLaunch(qint64 pid)
{
    const int descriptor = qEnvironmentVariableIntValue(kReadinessFdEnv);
    if (descriptor <= 0) {
        return;
    }
    ::signal(SIGPIPE, SIG_IGN);
    const QByteArray payload = QByteArrayLiteral("K") + QByteArray::number(pid) + '\n';
    (void)::write(descriptor, payload.constData(), static_cast<size_t>(payload.size()));
    ::close(descriptor);
    ::unsetenv(kReadinessFdEnv);
}

void reportDetachedLaunchFailure()
{
    const int descriptor = qEnvironmentVariableIntValue(kReadinessFdEnv);
    if (descriptor <= 0) {
        return;
    }
    ::signal(SIGPIPE, SIG_IGN);
    (void)::write(descriptor, "E", 1);
    ::close(descriptor);
}

enum class LaunchOutcome
{
    Started,
    AlreadyRunning,
    Failed,
};

// Launches a fully detached copy of khat and waits for its readiness
// handshake, so the shell can print a meaningful confirmation. Re-executes
// /proc/self/exe (instead of continuing in the forked process) so the daemon
// starts with a clean Qt image.
LaunchOutcome launchDetachedBackground(const QString &socketPath, QString *message)
{
    QLocalSocket probe;
    probe.connectToServer(socketPath);
    if (probe.waitForConnected(250)) {
        probe.abort();
        return LaunchOutcome::AlreadyRunning;
    }

    int descriptors[2];
    if (::pipe(descriptors) != 0) {
        if (message) {
            *message = QStringLiteral("could not create the readiness pipe: %1")
                           .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        return LaunchOutcome::Failed;
    }

    const pid_t middle = ::fork();
    if (middle < 0) {
        if (message) {
            *message = QStringLiteral("could not fork: %1")
                           .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        return LaunchOutcome::Failed;
    }
    if (middle == 0) {
        ::setsid();
        const pid_t daemon = ::fork();
        if (daemon == 0) {
            ::close(descriptors[0]);
            const int nullDevice = ::open("/dev/null", O_RDWR);
            if (nullDevice >= 0) {
                ::dup2(nullDevice, STDIN_FILENO);
                ::dup2(nullDevice, STDOUT_FILENO);
                ::dup2(nullDevice, STDERR_FILENO);
                if (nullDevice > STDERR_FILENO) {
                    ::close(nullDevice);
                }
            }
            if (descriptors[1] != 3) {
                ::dup2(descriptors[1], 3);
            }
            ::setenv(kReadinessFdEnv, "3", 1);
            // Execute the resolved binary (not /proc/self/exe: the kernel
            // derives the process name from the executed path, and "exe"
            // would make pgrep/ps/pkill unable to find the daemon).
            const QByteArray selfPath =
                QCoreApplication::applicationFilePath().toLocal8Bit();
            QByteArray selfName =
                QFileInfo(selfPath.constData()).fileName().toLocal8Bit();
            // Re-run with the same mode: without this argument the new
            // process would default to the settings window.
            char *const arguments[] = {selfName.data(),
                                       const_cast<char *>("--background"), nullptr};
            ::execv(selfPath.constData(), arguments);
            _exit(127);
        }
        _exit(daemon < 0 ? 126 : 0);
    }

    int status = 0;
    ::waitpid(middle, &status, 0);
    ::close(descriptors[1]);
    char payload[32] = {};
    const ssize_t received = ::read(descriptors[0], payload, sizeof(payload) - 1);
    ::close(descriptors[0]);

    if (received >= 2 && payload[0] == 'K') {
        payload[received] = '\0';
        const qint64 pid = QByteArray(payload + 1).simplified().toLongLong();
        if (message) {
            *message = QStringLiteral(
                           "khat is running in the background (PID %1). Stop it with: "
                           "khat --exit")
                           .arg(pid);
        }
        return LaunchOutcome::Started;
    }
    if (received >= 1 && payload[0] == 'E') {
        if (message) {
            *message = QStringLiteral("khat could not start in the background.");
        }
        return LaunchOutcome::Failed;
    }
    if (message) {
        *message = QStringLiteral("khat exited before reporting readiness.");
    }
    return LaunchOutcome::Failed;
}

// All khat processes of this user, by scanning /proc (an `--exit` call may
// also have to sweep a process whose socket is no longer reachable).
QList<pid_t> khatProcesses()
{
    QList<pid_t> result;
    const QDir procDir(QStringLiteral("/proc"));
    const QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    const uid_t ownUser = ::geteuid();
    const pid_t self = ::getpid();
    for (const QString &entry : entries) {
        bool isNumber = false;
        const pid_t pid = entry.toInt(&isNumber);
        if (!isNumber || pid <= 0 || pid == self) {
            continue;
        }
        struct stat status = {};
        if (::stat(QFile::encodeName(QStringLiteral("/proc/%1").arg(pid)).constData(),
                   &status)
                != 0
            || status.st_uid != ownUser) {
            continue;
        }
        QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
        if (!comm.open(QIODevice::ReadOnly)) {
            continue;
        }
        if (comm.readLine().trimmed() == QLatin1String("khat")) {
            result.append(pid);
        }
    }
    return result;
}

int stopKhat(const AppPaths &paths)
{
    bool confirmedRunning = false;
    bool socketWasPresent = false;
    const QString socketPath = paths.runtimeDir + QStringLiteral("/instance.sock");

    // 1) Ask the primary instance to leave through the regular shutdown path
    //    (the same sequence Ctrl+Q and SIGTERM use).
    if (QFileInfo::exists(socketPath)) {
        socketWasPresent = true;
        QString response;
        QString error;
        SingleInstance target(paths.runtimeDir);
        confirmedRunning = target.sendCommand(QStringLiteral("quit"), &response,
                                              &error, 3000);

        // 2) Give the primary a moment to release the socket.
        const QDeadlineTimer deadline(3000);
        while (deadline.remainingTime() > 0 && QFileInfo::exists(socketPath)) {
            QThread::msleep(100);
        }
    }

    // 3) Sweep any process the socket could not reach (this also catches an
    //    instance that exited through an unclean path and left the socket).
    QList<pid_t> targets = khatProcesses();
    if (targets.isEmpty() && !confirmedRunning && socketWasPresent
        && QFileInfo::exists(socketPath)) {
        QTextStream(stdout) << "khat is not running." << '\n';
        return 0;
    }
    bool ranAnywhere = confirmedRunning || !targets.isEmpty() || socketWasPresent;
    for (const pid_t pid : targets) {
        ::kill(pid, SIGTERM);
    }

    const QDeadlineTimer gracefulDeadline(3000);
    while (!targets.isEmpty() && gracefulDeadline.remainingTime() > 0) {
        QThread::msleep(150);
        targets = khatProcesses();
    }
    int forced = 0;
    for (const pid_t pid : targets) {
        ::kill(pid, SIGKILL);
        ++forced;
    }
    const QDeadlineTimer killDeadline(2000);
    while (!khatProcesses().isEmpty() && killDeadline.remainingTime() > 0) {
        QThread::msleep(150);
    }

    if (!ranAnywhere) {
        QTextStream(stdout) << "khat is not running." << '\n';
        return 0;
    }

    QTextStream(stdout) << "khat stopped." << '\n';
    if (forced > 0) {
        QTextStream(stdout) << "khat had to force-kill " << forced << " process(es)."
                            << '\n';
    }
    return 0;
}
} // namespace

int main(int argc, char *argv[])
{
    installPortalNoiseFilter();

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("khat"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KHAT_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("khat"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("khat"));
    QGuiApplication::setDesktopFileName(QStringLiteral("khat"));
    QApplication::setQuitOnLastWindowClosed(false);
    app.setWindowIcon(applicationIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("khat: minimal native Gmail notifications for GNOME"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption settingsOption(
        {QStringLiteral("s"), QStringLiteral("settings")},
        QStringLiteral("Open the settings window (default)."));
    QCommandLineOption backgroundOption(
        {QStringLiteral("b"), QStringLiteral("background")},
        QStringLiteral("Run without a window: detached from the terminal unless "
                       "--foreground is given or systemd started khat."));
    QCommandLineOption checkNowOption(
        {QStringLiteral("c"), QStringLiteral("check-now")},
        QStringLiteral("Ask a running instance to check for mail immediately."));
    QCommandLineOption exitOption(
        {QStringLiteral("e"), QStringLiteral("exit")},
        QStringLiteral("Stop all running khat instances (same sequence as Ctrl+Q; "
                       "stray processes are terminated as a fallback)."));
    QCommandLineOption foregroundOption(
        {QStringLiteral("f"), QStringLiteral("foreground")},
        QStringLiteral("With --background: do not detach (this is how the systemd "
                       "unit runs khat)."));
    parser.addOptions({settingsOption, backgroundOption, checkNowOption, exitOption,
                       foregroundOption});
    parser.process(app);

    const int selectedModes = parser.isSet(settingsOption) + parser.isSet(backgroundOption)
        + parser.isSet(checkNowOption) + parser.isSet(exitOption);
    if (selectedModes > 1) {
        return fail(QStringLiteral("--settings, --background, --check-now, and --exit "
                                   "are mutually exclusive."));
    }
    if (parser.isSet(foregroundOption) && !parser.isSet(backgroundOption)) {
        return fail(QStringLiteral("--foreground only applies to --background."));
    }

    RunMode mode = RunMode::Settings;
    if (parser.isSet(backgroundOption)) {
        mode = RunMode::Background;
    } else if (parser.isSet(checkNowOption)) {
        mode = RunMode::CheckNow;
    } else if (parser.isSet(exitOption)) {
        mode = RunMode::Exit;
    }

    const AppPaths paths = AppPaths::defaults();

    if (mode == RunMode::Exit) {
        return stopKhat(paths);
    }

    // A manual --background launch detaches so the terminal prompt returns.
    // A systemd unit runs khat as its own main process (no detach), and the
    // detached child itself must not re-exec a third time.
    SingleInstance instance(paths.runtimeDir);
    if (mode == RunMode::Background && !parser.isSet(foregroundOption)
        && !runningUnderSystemdUnit()
        && !qEnvironmentVariableIsSet(kReadinessFdEnv)) {
        QString detachMessage;
        const LaunchOutcome outcome =
            launchDetachedBackground(instance.socketPath(), &detachMessage);
        switch (outcome) {
        case LaunchOutcome::Started:
            QTextStream(stdout) << detachMessage << '\n';
            return 0;
        case LaunchOutcome::AlreadyRunning: {
            // Someone already serves the socket; let it introduce itself.
            QString response;
            QString sendError;
            if (instance.sendCommand(QStringLiteral("ping"), &response, &sendError)
                && !response.isEmpty()) {
                QTextStream(stdout) << response << '\n';
            } else {
                QTextStream(stdout) << "khat is already running. Stop it with: "
                                       "khat --exit"
                                    << '\n';
            }
            return 0;
        }
        case LaunchOutcome::Failed:
            return fail(detachMessage);
        }
    }

    QString instanceError;
    if (!instance.acquire(&instanceError)) {
        QString command;
        switch (mode) {
        case RunMode::Settings:
            command = QStringLiteral("show-settings");
            break;
        case RunMode::CheckNow:
            command = QStringLiteral("check-now");
            break;
        case RunMode::Background:
            // A second background launch is a no-op if the primary is healthy.
            command = QStringLiteral("ping");
            break;
        case RunMode::Exit:
            break; // handled above
        }

        QString response;
        QString sendError;
        if (!instance.sendCommand(command, &response, &sendError)) {
            if (!instanceError.isEmpty()
                && instanceError != QStringLiteral("Another khat instance is already "
                                                   "running.")) {
                return fail(instanceError + QStringLiteral(" ") + sendError);
            }
            return fail(sendError);
        }
        if (!response.isEmpty()) {
            QTextStream(stdout) << response << '\n';
        }
        return 0;
    }

    // The detached child reports readiness to its launcher, if there is one.
    confirmDetachedLaunch(app.applicationPid());
    // Local-socket writes must not kill the daemon when a reader vanished.
    ::signal(SIGPIPE, SIG_IGN);

    UnixSignalHandler signalHandler;
    QString signalError;
    if (!signalHandler.install(&signalError)) {
        QTextStream(stderr) << "khat: warning: " << signalError << '\n';
    }

    AppControllerOptions options;
    options.paths = paths;
    AppController controller(options);

    QString initializationError;
    if (!controller.initialize(&initializationError)) {
        return fail(initializationError);
    }

    instance.setCommandHandler(
        [&controller](const QString &command, QString *message) {
            if (command == QStringLiteral("show-settings")) {
                if (!controller.showSettings()) {
                    if (message) {
                        *message = QStringLiteral("The settings window is unavailable.");
                    }
                    return false;
                }
                if (message) {
                    *message = QStringLiteral("Settings opened.");
                }
                return true;
            }
            if (command == QStringLiteral("check-now")) {
                return controller.checkNow(message);
            }
            if (command == QStringLiteral("quit")) {
                if (message) {
                    *message = QStringLiteral("Stopping khat.");
                }
                // Exactly the Ctrl+Q/SIGTERM sequence: regular shutdown,
                // then leave the event loop.
                controller.shutdown();
                qGuiApp->quit();
                return true;
            }
            if (command == QStringLiteral("ping")) {
                if (message) {
                    *message = QStringLiteral("khat is already running (PID %1).")
                                   .arg(qGuiApp->applicationPid());
                }
                return true;
            }
            if (message) {
                *message = QStringLiteral("Unknown command.");
            }
            return false;
        });

    QObject::connect(&signalHandler, &UnixSignalHandler::terminationRequested, &app,
                     [&app, &instance, &controller](int) {
                         instance.setCommandHandler({});
                         controller.shutdown();
                         app.quit();
                     });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &controller,
                     &AppController::shutdown);
    QObject::connect(&app, &QGuiApplication::commitDataRequest, &app,
                     [&controller](QSessionManager &) { controller.shutdown(); });
    QObject::connect(&app, &QGuiApplication::saveStateRequest, &app,
                     [&controller](QSessionManager &) { controller.shutdown(); });

    if (mode == RunMode::Settings) {
        if (!controller.showSettings()) {
            return fail(QStringLiteral("The settings window could not be opened."));
        }
    } else if (mode == RunMode::CheckNow) {
        QString message;
        if (!controller.checkNow(&message)) {
            return fail(message);
        }
        if (!message.isEmpty()) {
            QTextStream(stdout) << message << '\n';
        }
    }

    return app.exec();
}
