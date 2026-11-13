#include "filehandler.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSharedPointer>
#include <QTemporaryDir>

FileHandler::FileHandler(QObject *parent) : QObject(parent)
{
}

FileHandler::~FileHandler() {}

void FileHandler::onPushFileRequest(const QString &serial, const QString &file, const QString &devicePath)
{
    qsc::AdbProcess* adb = new qsc::AdbProcess;
    bool isApk = false;
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this, adb, isApk](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        onAdbProcessResult(adb, isApk, processResult);
    });

    adb->push(serial, file, devicePath);
}

void FileHandler::onInstallApkRequest(const QString &serial, const QString &apkFile)
{
    const QString suffix = QFileInfo(apkFile).suffix().toLower();
    if (suffix == "xapk" || suffix == "apks") {
        onInstallArchiveRequest(serial, apkFile);
        return;
    }

    qsc::AdbProcess* adb = new qsc::AdbProcess;
    bool isApk = true;
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this, adb, isApk](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        onAdbProcessResult(adb, isApk, processResult);
    });

    adb->install(serial, apkFile);
}

void FileHandler::onInstallArchiveRequest(const QString &serial, const QString &archiveFile)
{
    auto tempDir = QSharedPointer<QTemporaryDir>::create(QDir::tempPath() + "/qtscrcpy-apks-XXXXXX");
    if (!tempDir->isValid()) {
        qWarning() << "Cannot create temporary directory for APK archive:" << archiveFile;
        emit fileHandlerResult(FAR_ERROR_EXEC, true);
        return;
    }

    auto extractor = new QProcess(this);
    // Retain the directory through process destruction, including device disconnects.
    connect(extractor, &QObject::destroyed, [tempDir]() { Q_UNUSED(tempDir); });
    extractor->setProcessChannelMode(QProcess::MergedChannels);
    connect(extractor, &QProcess::errorOccurred, this, [this, extractor](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            qWarning() << "Cannot start APK archive extractor:" << extractor->errorString();
            emit fileHandlerResult(FAR_ERROR_EXEC, true);
            extractor->deleteLater();
        }
    });
    connect(extractor, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this, extractor, tempDir, serial, archiveFile](int exitCode, QProcess::ExitStatus exitStatus) {
        extractor->deleteLater();
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            qWarning() << "Cannot extract APK archive:" << archiveFile << extractor->readAllStandardOutput();
            emit fileHandlerResult(FAR_ERROR_EXEC, true);
            return;
        }

        QStringList apkFiles;
        QDirIterator files(tempDir->path(), QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString file = files.next();
            if (QFileInfo(file).suffix().compare("apk", Qt::CaseInsensitive) == 0) {
                apkFiles << file;
            }
        }
        apkFiles.sort();
        if (apkFiles.isEmpty()) {
            qWarning() << "No APK files found in archive:" << archiveFile;
            emit fileHandlerResult(FAR_ERROR_EXEC, true);
            return;
        }

        auto adb = new qsc::AdbProcess(this);
        // Keep extracted APKs alive until ADB has finished reading them.
        connect(adb, &QObject::destroyed, [tempDir]() { Q_UNUSED(tempDir); });
        connect(adb, &qsc::AdbProcess::adbProcessResult, this,
                [this, adb](qsc::AdbProcess::ADB_EXEC_RESULT result) {
            onAdbProcessResult(adb, true, result);
        });
        qInfo() << "Installing" << apkFiles.size() << "APKs from" << archiveFile << "on" << serial;
        adb->execute(serial, QStringList() << "install-multiple" << "-r" << apkFiles);
    });

#ifdef Q_OS_WIN32
    // Pass paths through the environment so PowerShell never interprets file names as code.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("QTSCRCPY_APK_ARCHIVE", QFileInfo(archiveFile).absoluteFilePath());
    environment.insert("QTSCRCPY_APK_DIRECTORY", tempDir->path());
    extractor->setProcessEnvironment(environment);
    extractor->start("powershell.exe", QStringList() << "-NoProfile" << "-NonInteractive" << "-Command"
                     << "$ErrorActionPreference = 'Stop'; Add-Type -AssemblyName System.IO.Compression.FileSystem; "
                        "[System.IO.Compression.ZipFile]::ExtractToDirectory($env:QTSCRCPY_APK_ARCHIVE, $env:QTSCRCPY_APK_DIRECTORY)");
#else
    extractor->start("unzip", QStringList() << "-q" << "-o" << QFileInfo(archiveFile).absoluteFilePath()
                     << "-d" << tempDir->path());
#endif
    extractor->closeWriteChannel();
}

void FileHandler::onAdbProcessResult(qsc::AdbProcess *adb, bool isApk, qsc::AdbProcess::ADB_EXEC_RESULT processResult)
{
    switch (processResult) {
    case qsc::AdbProcess::AER_ERROR_START:
    case qsc::AdbProcess::AER_ERROR_EXEC:
    case qsc::AdbProcess::AER_ERROR_MISSING_BINARY:
        emit fileHandlerResult(FAR_ERROR_EXEC, isApk);
        adb->deleteLater();
        break;
    case qsc::AdbProcess::AER_SUCCESS_EXEC:
        emit fileHandlerResult(FAR_SUCCESS_EXEC, isApk);
        adb->deleteLater();
        break;
    default:
        break;
    }
}
