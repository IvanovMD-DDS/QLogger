#include "QLoggerWriter.h"

#include <QAbstractEventDispatcher>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QProcess>
#include <QTextStream>

namespace
{
/**
 * @brief Converts the given level in a QString.
 * @param level The log level in LogLevel format.
 * @return The string with the name of the log level.
 */
QString levelToText(const QLogger::LogLevel &level)
{
    switch (level)
    {
    case QLogger::LogLevel::Trace:
        return "Trace";
    case QLogger::LogLevel::Debug:
        return "Debug";
    case QLogger::LogLevel::Info:
        return "Info";
    case QLogger::LogLevel::Warning:
        return "Warning";
    case QLogger::LogLevel::Error:
        return "Error";
    case QLogger::LogLevel::Fatal:
        return "Fatal";
    }

    return QString();
}
}  // namespace

namespace QLogger
{

QMutex QLoggerWriter::zipLock;
QMutex QLoggerWriter::writeLock;

QLoggerWriter::QLoggerWriter(const QString &fileDestination, LogLevel level, const QString &fileFolderDestination, LogMode mode, LogFileDisplay fileSuffixIfFull, LogMessageDisplays messageOptions)
    : mFileSuffixIfFull(fileSuffixIfFull)
    , mMode(mode)
    , mLevel(level)
    , mMessageOptions(messageOptions)
{
    mFileDestinationFolder = fileFolderDestination.isEmpty() ? QDir::currentPath() + "/logs/" : fileFolderDestination;

    if (!mFileDestinationFolder.endsWith("/"))
        mFileDestinationFolder.append("/");

    mFileDestination = mFileDestinationFolder + fileDestination;

    QDir dir(mFileDestinationFolder);
    if (fileDestination.isEmpty())
    {
        mFileDestination = dir.filePath(QString::fromLatin1("%1.log").arg(
            QDateTime::currentDateTime().date().toString(QString::fromLatin1("yyyy-MM-dd"))));
    }
    else if (!fileDestination.contains(QLatin1Char('.')))
        mFileDestination.append(QString::fromLatin1(".log"));

    if (mMode == LogMode::Full || mMode == LogMode::OnlyFile)
        dir.mkpath(QStringLiteral("."));

    //QLogger commit
    {
        wakeUpTime.start();

        QFileInfo info(mFileDestination);

        if (info.exists())
        {
            currentDate = info.lastModified().date();
        }
        else
        {
            currentDate = QDateTime::currentDateTime().date();
        }
    }
}

void QLoggerWriter::setLogMode(LogMode mode)
{
    mMode = mode;

    if (mMode == LogMode::Full || mMode == LogMode::OnlyFile)
    {
        QDir dir(mFileDestinationFolder);
        dir.mkpath(QStringLiteral("."));
    }

    if (mode != LogMode::Disabled && !this->isRunning())
        start();
}

QString QLoggerWriter::renameFileIfFull()
{
    if (currentDate == QDateTime::currentDateTime().date())
        return QString();

    QFile file(mFileDestination);

    {  //QLogger commit
        QString newName = mFileDestination;
        newName.insert(newName.size() - 4, currentDate.toString("_yyyy_MM_dd"));

        if (!file.rename(mFileDestination, newName))  // не удалось переименовать
            return QString();

        currentDate = QDateTime::currentDateTime().date();

        if (mQuit)  // if quit no zip
            return newName;

        return zipFileCustom(newName);
    }

    //--------
    // Rename file if it's full
    if (file.size() >= mMaxFileSize)
    {
        QString newName;

        const auto fileDestination = mFileDestination.left(mFileDestination.lastIndexOf('.'));
        const auto fileExtension   = mFileDestination.mid(mFileDestination.lastIndexOf('.') + 1);

        if (mFileSuffixIfFull == LogFileDisplay::DateTime)
        {
            newName
                = QString("%1_%2.%3")
                      .arg(fileDestination, QDateTime::currentDateTime().toString("dd_MM_yy__hh_mm_ss"), fileExtension);
        }
        else
            newName = generateDuplicateFilename(fileDestination, fileExtension);

        const auto renamed = file.rename(mFileDestination, newName);

        return renamed ? newName : QString();
    }

    return QString();
}

QString QLoggerWriter::generateDuplicateFilename(const QString &fileDestination, const QString &fileExtension, int fileSuffixNumber)
{
    QString path(fileDestination);
    if (fileSuffixNumber > 1)
        path = QString("%1(%2).%3").arg(fileDestination, QString::number(fileSuffixNumber), fileExtension);
    else
        path.append(QString(".%1").arg(fileExtension));

    // A name already exists, increment the number and check again
    if (QFileInfo::exists(path))
        return generateDuplicateFilename(fileDestination, fileExtension, fileSuffixNumber + 1);

    // No file exists at the given location, so no need to continue
    return path;
}

void QLoggerWriter::write(const QVector<QString> &messages)
{
    // Write data to console
    if (mMode == LogMode::OnlyConsole)
    {
        for (const auto &message : messages)
            qInfo() << message;

        return;
    }

    // Write data to file
    QFile file(mFileDestination);

    const auto prevFilename = renameFileIfFull();

    QMutexLocker write(&writeLock);

    if (file.open(QIODevice::ReadWrite | QIODevice::Text | QIODevice::Append))
    {
        QTextStream out(&file);

        if (!prevFilename.isEmpty())
            out << QString("Previous log %1\n").arg(prevFilename);

        for (const auto &message : messages)
        {
            out << message;

            if (mMode == LogMode::Full)
                qInfo() << message;
        }

        file.close();
    }
}

void QLoggerWriter::enqueue(const QDateTime &date, const QString &threadId, const QString &module, LogLevel level, const QString &function, const QString &fileName, int line, const QString &message)
{
    QMutexLocker locker(&mutex);

    if (mMode == LogMode::Disabled)
        return;

    QString fileLine;
    if (mMessageOptions.testFlag(LogMessageDisplay::File) && mMessageOptions.testFlag(LogMessageDisplay::Line)
        && !fileName.isEmpty() && line > 0 && mLevel <= LogLevel::Debug)
    {
        fileLine = QString("{%1:%2}").arg(fileName, QString::number(line));
    }
    else if (mMessageOptions.testFlag(LogMessageDisplay::File) && mMessageOptions.testFlag(LogMessageDisplay::Function)
             && !fileName.isEmpty() && !function.isEmpty() && mLevel <= LogLevel::Debug)
    {
        fileLine = QString("{%1}{%2}").arg(fileName, function);
    }

    QString text;
    if (mMessageOptions.testFlag(LogMessageDisplay::Default))
    {
        text = QString("[%1][%2][%3][%4]%5 %6")
                   .arg(levelToText(level), module, date.toString("yyyy-MM-dd hh:mm:ss:zzz"), threadId, fileLine, message);
    }
    else
    {
        if (mMessageOptions.testFlag(LogMessageDisplay::LogLevel))
            text.append(QString("[%1]").arg(levelToText(level)));

        if (mMessageOptions.testFlag(LogMessageDisplay::ModuleName))
            text.append(QString("[%1]").arg(module));

        if (mMessageOptions.testFlag(LogMessageDisplay::DateTime))
            text.append(QString("[%1]").arg(date.toString("yyyy-MM-dd hh:mm:ss:zzz")));

        if (mMessageOptions.testFlag(LogMessageDisplay::ThreadId))
            text.append(QString("[%1]").arg(threadId));

        if (!fileLine.isEmpty())
        {
            if (fileLine.startsWith(QChar::Space))
                fileLine = fileLine.right(1);

            text.append(fileLine);
        }
        if (mMessageOptions.testFlag(LogMessageDisplay::Message))
        {
            if (text.isEmpty() || text.endsWith(QChar::Space))
                text.append(QString("%1").arg(message));
            else
                text.append(QString(" %1").arg(message));
        }
    }

    text.append(QString::fromLatin1("\n"));

    mMessages.append(text);

    if (wakeUpTime.elapsed() > writeMSec)
    {
        if (!mIsStop)
            mQueueNotEmpty.wakeAll();

        wakeUpTime.start();
    }
}

void QLoggerWriter::run()
{
    if (!mQuit)
    {
        QMutexLocker locker(&mutex);
        mQueueNotEmpty.wait(&mutex);
    }

    while (!mQuit)
    {
        decltype(mMessages) copy;

        {
            QMutexLocker locker(&mutex);
            std::swap(copy, mMessages);
        }

        write(copy);

        lastActive = QDateTime::currentDateTime();

        if (!mQuit)
        {
            QMutexLocker locker(&mutex);
            mQueueNotEmpty.wait(&mutex);
        }
    }
}

void QLoggerWriter::closeDestination()
{
    QMutexLocker locker(&mutex);

    if (!mMessages.isEmpty())
    {
        decltype(mMessages) copy;

        {
            std::swap(copy, mMessages);
        }

        write(copy);
    }

    QVector<QString> closed(0);
    closed.append(QString("Closed %1 \n").arg(QDateTime::currentDateTime().toString()));
    write(closed);

    mQuit = true;
    mQueueNotEmpty.wakeAll();
}

void QLoggerWriter::forcePush()
{
    if (mMessages.count() > 0)
    {
        auto r = lastActive.secsTo(QDateTime::currentDateTime());

        if (r > 5)
            mQueueNotEmpty.wakeAll();
    }
}

QString QLoggerWriter::zipFileCustom(const QString &path)
{
    QMutexLocker zipLock1(&zipLock);

    QElapsedTimer time;
    time.start();

    // cut .log, add .7z
    QString archiveName = path.mid(0, path.size() - 4) + ".7z";

    QStringList param;
    param << "a"
          << "-t7z"
          << "-mx9"
          << archiveName << path;

    QProcess zip;

    QString pathExe = "7z";
    zip.start(pathExe, param);

    bool res = zip.waitForFinished(1000 * 60 * 15);  // 15 min

    auto exitStatus = (bool)zip.exitStatus();

    zip.close();

    QString result = QString("%1 to archive : %2. %3. Time::%4")
                         .arg(path,
                              archiveName,
                              QString("finished: %1, %2").arg(res ? "yes" : "no", exitStatus ? "The process crashed" : "The process exited normall"),
                              QString::number(time.elapsed()));

    return result;
}

}  // namespace QLogger
