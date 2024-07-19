#pragma once

#include <QNetworkAccessManager>
#include <QCommandLineParser>
#include <QStorageInfo>
#include <QCoreApplication>
#include <algorithm>
#include <QFileInfo>
#include <iostream>
#include <QProcess>
#include <QObject>
#include <QTimer>
#include <random>
#include <chrono>
#include <array>

extern QFile *logFile;
extern QTextStream *logStream;

void tankMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg);
