#include <QApplication>
#include <QCommandLineParser>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("gmail-notifier"));
    QCoreApplication::setApplicationVersion(QStringLiteral(GMAIL_NOTIFIER_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("GmailNotifier"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Minimal native Gmail notifier for GNOME"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);

    QTextStream(stdout) << "gmail-notifier " << QCoreApplication::applicationVersion()
                        << " scaffold\n";
    return 0;
}
