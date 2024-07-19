#include <QObject>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QCommandLineParser>
#include <QStandardPaths>

#include "logger.h"

QFile *logFile = nullptr;
QTextStream *logStream = nullptr;

void tankMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    QString logMessage;
    switch (type) {
    case QtDebugMsg:
        logMessage = "[D] ";
        break;
    case QtInfoMsg:
        logMessage = "[I] ";
        break;
    case QtWarningMsg:
        logMessage = "[W] ";
        break;
    case QtCriticalMsg:
        logMessage = "[C] ";
        break;
    default:
        logMessage = "[U] ";
        break;
    }

    auto fn = QString(context.file);
    auto fnspl = fn.split("/");
    logMessage += QString("[%1::%2] %3").arg(fnspl.last()).arg(context.line).arg(msg);

    // to console
    QTextStream stream(type != QtInfoMsg ? stderr : stdout);
    stream << logMessage << "\n";

    // to file
    if(logFile != nullptr && logFile->isOpen()) {
      if(logStream == nullptr)
        logStream = new QTextStream(logFile);
      *logStream << logMessage << endl;
    }
}
