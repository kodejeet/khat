#include "app/AppController.h"
#include "app/SingleInstance.h"
#include "app/UnixSignalHandler.h"
#include "config/AppPaths.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QIcon>
#include <QSessionManager>
#include <QSize>
#include <QTextStream>

enum class RunMode
{
    Settings,
    Background,
    CheckNow,
};

namespace {
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
} // namespace

int main(int argc, char *argv[])
{
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
        QStringLiteral("Run without opening a window."));
    QCommandLineOption checkNowOption(
        {QStringLiteral("c"), QStringLiteral("check-now")},
        QStringLiteral("Ask a running instance to check for mail immediately."));
    parser.addOptions({settingsOption, backgroundOption, checkNowOption});
    parser.process(app);

    const int selectedModes = parser.isSet(settingsOption) + parser.isSet(backgroundOption)
        + parser.isSet(checkNowOption);
    if (selectedModes > 1) {
        return fail(QStringLiteral("--settings, --background, and --check-now are mutually "
                                   "exclusive."));
    }

    RunMode mode = RunMode::Settings;
    if (parser.isSet(backgroundOption)) {
        mode = RunMode::Background;
    } else if (parser.isSet(checkNowOption)) {
        mode = RunMode::CheckNow;
    }

    const AppPaths paths = AppPaths::defaults();
    SingleInstance instance(paths.runtimeDir);
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
        if (!response.isEmpty() && mode != RunMode::Background) {
            QTextStream(stdout) << response << '\n';
        }
        return 0;
    }

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
            if (command == QStringLiteral("ping")) {
                if (message) {
                    *message = QStringLiteral("pong");
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
