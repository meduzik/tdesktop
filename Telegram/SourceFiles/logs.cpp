/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2015 John Preston, https://desktop.telegram.org
*/
#include "stdafx.h"
#include <iostream>
#include "pspecific.h"

// see https://blog.inventic.eu/2012/08/qt-and-google-breakpad/
#ifdef Q_OS_WIN
#include "client/windows/handler/exception_handler.h"
#elif defined Q_OS_MAC
#include "client/mac/handler/exception_handler.h"
#elif defined Q_OS_LINUX64 || defined Q_OS_LINUX32
#include "client/linux/handler/exception_handler.h"
#endif

enum LogDataType {
	LogDataMain,
	LogDataDebug,
	LogDataTcp,
	LogDataMtp,

	LogDataCount
};

QMutex *_logsMutex(LogDataType type, bool clear = false) {
	static QMutex *LogsMutexes = 0;
	if (clear) {
		delete[] LogsMutexes;
		LogsMutexes = 0;
	} else if (!LogsMutexes) {
		LogsMutexes = new QMutex[LogDataCount];
	}
	return &LogsMutexes[type];
}

QString _logsFilePath(LogDataType type, const QString &postfix = QString()) {
	QString path(cWorkingDir());
	switch (type) {
	case LogDataMain: path += qstr("log") + postfix + qstr(".txt"); break;
	case LogDataDebug: path += qstr("DebugLogs/log") + postfix + qstr(".txt"); break;
	case LogDataTcp: path += qstr("DebugLogs/tcp") + postfix + qstr(".txt"); break;
	case LogDataMtp: path += qstr("DebugLogs/mtp") + postfix + qstr(".txt"); break;
	}
	return path;
}

int32 LogsStartIndexChosen = -1;
QString _logsEntryStart() {
	static int32 index = 0;
	QDateTime tm(QDateTime::currentDateTime());

	QThread *thread = QThread::currentThread();
	MTPThread *mtpThread = qobject_cast<MTPThread*>(thread);
	uint threadId = mtpThread ? mtpThread->getThreadId() : 0;

	return QString("[%1 %2-%3]").arg(tm.toString("hh:mm:ss.zzz")).arg(QString("%1").arg(threadId, 2, 10, QChar('0'))).arg(++index, 7, 10, QChar('0'));
}

class LogsDataFields {
public:

	LogsDataFields() {
		for (int32 i = 0; i < LogDataCount; ++i) {
			files[i].reset(new QFile());
		}
	}

	bool openMain() {
		return reopen(LogDataMain, 0, qsl("start"));
	}

	bool instanceChecked() {
		return reopen(LogDataMain, 0, QString());
	}

	QString full() {
		if (!streams[LogDataMain].device()) {
			return QString();
		}

		QFile out(files[LogDataMain]->fileName());
		if (out.open(QIODevice::ReadOnly)) {
			return QString::fromUtf8(out.readAll());
		}
		return QString();
	}

	void write(LogDataType type, const QString &msg) {
		QMutexLocker lock(_logsMutex(type));
		if (type != LogDataMain) reopenDebug();
		if (!streams[type].device()) return;

		streams[type] << msg;
		streams[type].flush();
	}

private:

	QSharedPointer<QFile> files[LogDataCount];
	QTextStream streams[LogDataCount];

	int32 part = -1;

	bool reopen(LogDataType type, int32 dayIndex, const QString &postfix) {
		if (streams[type].device()) {
			if (type == LogDataMain) {
				if (!postfix.isEmpty()) {
					return true;
				}
			} else {
				streams[type].setDevice(0);
				files[type]->close();
			}
		}

		QFlags<QIODevice::OpenModeFlag> mode = QIODevice::WriteOnly | QIODevice::Text;
		if (type == LogDataMain) { // we can call LOG() in LogDataMain reopen - mutex not locked
			if (postfix.isEmpty()) { // instance checked, need to move to log.txt
				t_assert(!files[type]->fileName().isEmpty()); // one of log_startXX.txt should've been opened already

				QSharedPointer<QFile> to(new QFile(_logsFilePath(type, postfix)));
				if (to->exists() && !to->remove()) {
					LOG(("Could not delete '%1' file to start new logging!").arg(to->fileName()));
					return false;
				}
				if (!QFile(files[type]->fileName()).copy(to->fileName())) { // don't close files[type] yet
					LOG(("Could not copy '%1' to '%2' to start new logging!").arg(files[type]->fileName()).arg(to->fileName()));
					return false;
				}
				if (to->open(mode | QIODevice::Append)) {
					qSwap(files[type], to);
					streams[type].setDevice(files[type].data());
					streams[type].setCodec("UTF-8");
					LOG(("Moved logging from '%1' to '%2'!").arg(to->fileName()).arg(files[type]->fileName()));
					to->remove();

					LogsStartIndexChosen = -1;

					QDir working(cWorkingDir()); // delete all other log_startXX.txt that we can
					QStringList oldlogs = working.entryList(QStringList("log_start*.txt"), QDir::Files);
					for (QStringList::const_iterator i = oldlogs.cbegin(), e = oldlogs.cend(); i != e; ++i) {
						QString oldlog = cWorkingDir() + *i, oldlogend = i->mid(qstr("log_start").size());
						if (oldlogend.size() == 1 + qstr(".txt").size() && oldlogend.at(0).isDigit() && oldlogend.midRef(1) == qstr(".txt")) {
							bool removed = QFile(*i).remove();
							LOG(("Old start log '%1' found, deleted: %2").arg(*i).arg(Logs::b(removed)));
						}
					}

					return true;
				}
				LOG(("Could not open '%1' file to start new logging!").arg(to->fileName()));
				return false;
			} else {
				bool found = false;
				int32 oldest = -1; // find not existing log_startX.txt or pick the oldest one (by lastModified)
				QDateTime oldestLastModified;
				for (int32 i = 0; i < 10; ++i) {
					QString trying = _logsFilePath(type, qsl("_start%1").arg(i));
					files[type]->setFileName(trying);
					if (!files[type]->exists()) {
						LogsStartIndexChosen = i;
						found = true;
						break;
					}
					QDateTime lastModified = QFileInfo(trying).lastModified();
					if (oldest < 0 || lastModified < oldestLastModified) {
						oldestLastModified = lastModified;
						oldest = i;
					}
				}
				if (!found) {
					files[type]->setFileName(_logsFilePath(type, qsl("_start%1").arg(oldest)));
					LogsStartIndexChosen = oldest;
				}
			}
		} else {
			files[type]->setFileName(_logsFilePath(type, postfix));
			if (files[type]->exists()) {
				if (files[type]->open(QIODevice::ReadOnly | QIODevice::Text)) {
					if (QString::fromUtf8(files[type]->readLine()).toInt() == dayIndex) {
						mode |= QIODevice::Append;
					}
					files[type]->close();
				}
			} else {
				QDir().mkdir(cWorkingDir() + qstr("DebugLogs"));
			}
		}
		if (files[type]->open(mode)) {
			streams[type].setDevice(files[type].data());
			streams[type].setCodec("UTF-8");

			if (type != LogDataMain) {
				streams[type] << ((mode & QIODevice::Append) ? qsl("----------------------------------------------------------------\nNEW LOGGING INSTANCE STARTED!!!\n----------------------------------------------------------------\n") : qsl("%1\n").arg(dayIndex));
				streams[type].flush();
			}

			return true;
		} else if (type != LogDataMain) {
			LOG(("Could not open debug log '%1'!").arg(files[type]->fileName()));
		}
		return false;
	}

	void reopenDebug() {
		time_t t = time(NULL);
		struct tm tm;
		mylocaltime(&tm, &t);

		static const int switchEach = 15; // minutes
		int32 newPart = (tm.tm_min + tm.tm_hour * 60) / switchEach;
		if (newPart == part) return;

		part = newPart;

		int32 dayIndex = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
		QString postfix = QString("_%4_%5").arg((part * switchEach) / 60, 2, 10, QChar('0')).arg((part * switchEach) % 60, 2, 10, QChar('0'));

		reopen(LogDataDebug, dayIndex, postfix);
		reopen(LogDataTcp, dayIndex, postfix);
		reopen(LogDataMtp, dayIndex, postfix);
	}

};

LogsDataFields *LogsData = 0;

typedef QList<QPair<LogDataType, QString> > LogsInMemoryList;
LogsInMemoryList *LogsInMemory = 0;
LogsInMemoryList *DeletedLogsInMemory = SharedMemoryLocation<LogsInMemoryList, 0>();

QString LogsBeforeSingleInstanceChecked; // LogsInMemory already dumped in LogsData, but LogsData is about to be deleted

void _logsWrite(LogDataType type, const QString &msg) {
	if (LogsData && (type == LogDataMain || LogsStartIndexChosen < 0)) {
		if (type == LogDataMain || cDebug()) {
			LogsData->write(type, msg);
		}
	} else if (LogsInMemory != DeletedLogsInMemory) {
		if (!LogsInMemory) {
			LogsInMemory = new LogsInMemoryList;
		}
		LogsInMemory->push_back(qMakePair(type, msg));
	} else if (!LogsBeforeSingleInstanceChecked.isEmpty() && type == LogDataMain) {
		LogsBeforeSingleInstanceChecked += msg;
	}
}

void _moveOldDataFiles(const QString &from);

namespace SignalHandlers {
	void StartBreakpad();
	void FinishBreakpad();
}

namespace Logs {

	Initializer::Initializer() {
		t_assert(LogsData == 0);

		if (!Global::CheckBetaVersionDir()) {
			return;
		}
		bool workingDirChosen = cBetaVersion();

		QString initialWorkingDir = QDir(cWorkingDir()).absolutePath() + '/', moveOldDataFrom;
		if (cBetaVersion()) {
			cSetDebug(true);
#if (defined Q_OS_MAC || defined Q_OS_LINUX)
		} else {
#ifdef _DEBUG
			cForceWorkingDir(cExeDir());
#else
			if (cWorkingDir().isEmpty()) {
				cForceWorkingDir(psAppDataPath());
			}
#endif
			workingDirChosen = true;

#if (defined Q_OS_LINUX && !defined _DEBUG) // fix first version
			moveOldDataFrom = initialWorkingDir;
#endif

#endif
		}

		LogsData = new LogsDataFields();
		if (!workingDirChosen) {
			cForceWorkingDir(cWorkingDir());
			if (!LogsData->openMain()) {
				cForceWorkingDir(cExeDir());
				if (!LogsData->openMain()) {
					cForceWorkingDir(psAppDataPath());
				}
			}
		}

		cForceWorkingDir(QDir(cWorkingDir()).absolutePath() + '/');
		QDir().setCurrent(cWorkingDir());
		QDir().mkpath(cWorkingDir() + qstr("tdata"));

		Global::WorkingDirReady();
		SignalHandlers::StartBreakpad();

		if (!LogsData->openMain()) {
			delete LogsData;
			LogsData = 0;
		}

		LOG(("Launched version: %1, dev: %2, beta: %3, debug mode: %4, test dc: %5").arg(AppVersion).arg(Logs::b(cDevVersion())).arg(cBetaVersion()).arg(Logs::b(cDebug())).arg(Logs::b(cTestMode())));
		LOG(("Executable dir: %1, name: %2").arg(cExeDir()).arg(cExeName()));
		LOG(("Initial working dir: %1").arg(initialWorkingDir));
		LOG(("Working dir: %1").arg(cWorkingDir()));
		LOG(("Arguments: %1").arg(cArguments()));

		if (!LogsData) {
			LOG(("Could not open '%1' for writing log!").arg(_logsFilePath(LogDataMain, qsl("_startXX"))));
			return;
		}

#ifdef Q_OS_WIN
		if (cWorkingDir() == psAppDataPath()) { // fix old "Telegram Win (Unofficial)" version
			moveOldDataFrom = psAppDataPathOld();
		}
#endif
		if (!moveOldDataFrom.isEmpty()) {
			_moveOldDataFiles(moveOldDataFrom);
		}

		if (LogsInMemory) {
			t_assert(LogsInMemory != DeletedLogsInMemory);
			LogsInMemoryList list = *LogsInMemory;
			for (LogsInMemoryList::const_iterator i = list.cbegin(), e = list.cend(); i != e; ++i) {
				if (i->first == LogDataMain) {
					_logsWrite(i->first, i->second);
					LOG(("First: %1, %2").arg(i->first).arg(i->second));
				}
			}
		}

		LOG(("Logs started"));
	}

	Initializer::~Initializer() {
		delete LogsData;
		LogsData = 0;

		if (LogsInMemory && LogsInMemory != DeletedLogsInMemory) {
			delete LogsInMemory;
		}
		LogsInMemory = DeletedLogsInMemory;

		_logsMutex(LogDataMain, true);

		SignalHandlers::FinishBreakpad();
	}

	bool started() {
		return LogsData != 0;
	}

	bool instanceChecked() {
		if (!LogsData) return false;

		if (!LogsData->instanceChecked()) {
			LogsBeforeSingleInstanceChecked = Logs::full();

			delete LogsData;
			LogsData = 0;
			LOG(("Could not move logging to '%1'!").arg(_logsFilePath(LogDataMain)));
			return false;
		}

		if (LogsInMemory) {
			t_assert(LogsInMemory != DeletedLogsInMemory);
			LogsInMemoryList list = *LogsInMemory;
			for (LogsInMemoryList::const_iterator i = list.cbegin(), e = list.cend(); i != e; ++i) {
				if (i->first != LogDataMain) {
					_logsWrite(i->first, i->second);
				}
			}
		}
		if (LogsInMemory) {
			t_assert(LogsInMemory != DeletedLogsInMemory);
			delete LogsInMemory;
		}
		LogsInMemory = DeletedLogsInMemory;

		DEBUG_LOG(("Debug logs started."));
		LogsBeforeSingleInstanceChecked.clear();
		return true;
	}

	void multipleInstances() {
		if (LogsInMemory) {
			t_assert(LogsInMemory != DeletedLogsInMemory);
			delete LogsInMemory;
		}
		LogsInMemory = DeletedLogsInMemory;

		if (cDebug()) {
			LOG(("WARNING: debug logs are not written in multiple instances mode!"));
		}
		LogsBeforeSingleInstanceChecked.clear();
	}

	void writeMain(const QString &v) {
		time_t t = time(NULL);
		struct tm tm;
		mylocaltime(&tm, &t);

		QString msg(QString("[%1.%2.%3 %4:%5:%6] %7\n").arg(tm.tm_year + 1900).arg(tm.tm_mon + 1, 2, 10, QChar('0')).arg(tm.tm_mday, 2, 10, QChar('0')).arg(tm.tm_hour, 2, 10, QChar('0')).arg(tm.tm_min, 2, 10, QChar('0')).arg(tm.tm_sec, 2, 10, QChar('0')).arg(v));
		_logsWrite(LogDataMain, msg);

		QString debugmsg(QString("%1 %2\n").arg(_logsEntryStart()).arg(v));
		_logsWrite(LogDataDebug, debugmsg);
	}

	void writeDebug(const char *file, int32 line, const QString &v) {
		const char *last = strstr(file, "/"), *found = 0;
		while (last) {
			found = last;
			last = strstr(last + 1, "/");
		}
		last = strstr(file, "\\");
		while (last) {
			found = last;
			last = strstr(last + 1, "\\");
		}
		if (found) {
			file = found + 1;
		}

		QString msg(QString("%1 %2 (%3 : %4)\n").arg(_logsEntryStart()).arg(v).arg(file).arg(line));
		_logsWrite(LogDataDebug, msg);

#ifdef Q_OS_WIN
		//OutputDebugString(reinterpret_cast<const wchar_t *>(msg.utf16()));
#elif defined Q_OS_MAC
		//objc_outputDebugString(msg);
#elif defined Q_OS_LINUX && defined _DEBUG
		//std::cout << msg.toUtf8().constData();
#endif
	}

	void writeTcp(const QString &v) {
		QString msg(QString("%1 %2\n").arg(_logsEntryStart()).arg(v));
		_logsWrite(LogDataTcp, msg);
	}

	void writeMtp(int32 dc, const QString &v) {
		QString msg(QString("%1 (dc:%2) %3\n").arg(_logsEntryStart()).arg(dc).arg(v));
		_logsWrite(LogDataMtp, msg);
	}

	QString full() {
		if (LogsData) {
			return LogsData->full();
		}
		if (!LogsInMemory || LogsInMemory == DeletedLogsInMemory) {
			return LogsBeforeSingleInstanceChecked;
		}

		int32 size = 0;
		for (LogsInMemoryList::const_iterator i = LogsInMemory->cbegin(), e = LogsInMemory->cend(); i != e; ++i) {
			if (i->first == LogDataMain) {
				size += i->second.size();
			}
		}
		QString result;
		result.reserve(size);
		for (LogsInMemoryList::const_iterator i = LogsInMemory->cbegin(), e = LogsInMemory->cend(); i != e; ++i) {
			if (i->first == LogDataMain) {
				result += i->second;
			}
		}
		return result;
	}

	QString vector(const QVector<MTPlong> &ids) {
		if (!ids.size()) return "[]";
		QString idsStr = QString("[%1").arg(ids.cbegin()->v);
		for (QVector<MTPlong>::const_iterator i = ids.cbegin() + 1, e = ids.cend(); i != e; ++i) {
			idsStr += QString(", %2").arg(i->v);
		}
		return idsStr + "]";
	}

	QString vector(const QVector<uint64> &ids) {
		if (!ids.size()) return "[]";
		QString idsStr = QString("[%1").arg(*ids.cbegin());
		for (QVector<uint64>::const_iterator i = ids.cbegin() + 1, e = ids.cend(); i != e; ++i) {
			idsStr += QString(", %2").arg(*i);
		}
		return idsStr + "]";
	}

}

void _moveOldDataFiles(const QString &wasDir) {
	QFile data(wasDir + "data"), dataConfig(wasDir + "data_config"), tdataConfig(wasDir + "tdata/config");
	if (data.exists() && dataConfig.exists() && !QFileInfo(cWorkingDir() + "data").exists() && !QFileInfo(cWorkingDir() + "data_config").exists()) { // move to home dir
		LOG(("Copying data to home dir '%1' from '%2'").arg(cWorkingDir()).arg(wasDir));
		if (data.copy(cWorkingDir() + "data")) {
			LOG(("Copied 'data' to home dir"));
			if (dataConfig.copy(cWorkingDir() + "data_config")) {
				LOG(("Copied 'data_config' to home dir"));
				bool tdataGood = true;
				if (tdataConfig.exists()) {
					tdataGood = false;
					QDir().mkpath(cWorkingDir() + "tdata");
					if (tdataConfig.copy(cWorkingDir() + "tdata/config")) {
						LOG(("Copied 'tdata/config' to home dir"));
						tdataGood = true;
					} else {
						LOG(("Copied 'data' and 'data_config', but could not copy 'tdata/config'!"));
					}
				}
				if (tdataGood) {
					if (data.remove()) {
						LOG(("Removed 'data'"));
					} else {
						LOG(("Could not remove 'data'"));
					}
					if (dataConfig.remove()) {
						LOG(("Removed 'data_config'"));
					} else {
						LOG(("Could not remove 'data_config'"));
					}
					if (!tdataConfig.exists() || tdataConfig.remove()) {
						LOG(("Removed 'tdata/config'"));
						LOG(("Could not remove 'tdata/config'"));
					} else {
					}
					QDir().rmdir(wasDir + "tdata");
				}
			} else {
				LOG(("Copied 'data', but could not copy 'data_config'!!"));
			}
		} else {
			LOG(("Could not copy 'data'!"));
		}
	}
}

#if defined Q_OS_MAC || defined Q_OS_LINUX32 || defined Q_OS_LINUX64

#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>

#ifdef Q_OS_MAC

#include <dlfcn.h>

#endif

#endif

namespace SignalHandlers {

	QByteArray CrashDumpPath;
	FILE *CrashDumpFile = 0;
	int CrashDumpFileNo = 0;
	char LaunchedDateTimeStr[32] = { 0 };
	char LaunchedBinaryName[256] = { 0 };

	void _writeChar(char ch) {
		fwrite(&ch, 1, 1, CrashDumpFile);
	}

	dump::~dump() {
		if (CrashDumpFile) {
			fflush(CrashDumpFile);
		}
	}

	const dump &operator<<(const dump &stream, const char *str) {
		if (!CrashDumpFile) return stream;

		fwrite(str, 1, strlen(str), CrashDumpFile);
		return stream;
	}

    const dump &operator<<(const dump &stream, const wchar_t *str) {
        if (!CrashDumpFile) return stream;

        for (int i = 0, l = wcslen(str); i < l; ++i) {
            if (str[i] >= 0 && str[i] < 128) {
                _writeChar(char(str[i]));
            } else {
                _writeChar('?');
            }
        }
        return stream;
    }

	template <typename Type>
	const dump &_writeNumber(const dump &stream, Type number) {
		if (!CrashDumpFile) return stream;

		if (number < 0) {
			_writeChar('-');
			number = -number;
		}
		Type upper = 1, prev = number / 10;
		while (prev >= upper) {
			upper *= 10;
		}
		while (upper > 0) {
			int digit = (number / upper);
			_writeChar('0' + digit);
			number -= digit * upper;
			upper /= 10;
		}
		return stream;
	}

	const dump &operator<<(const dump &stream, int num) {
		return _writeNumber(stream, num);
	}

	const dump &operator<<(const dump &stream, unsigned int num) {
		return _writeNumber(stream, num);
	}

	const dump &operator<<(const dump &stream, unsigned long num) {
		return _writeNumber(stream, num);
	}

	const dump &operator<<(const dump &stream, unsigned long long num) {
		return _writeNumber(stream, num);
	}

	const dump &operator<<(const dump &stream, double num) {
		if (num < 0) {
			_writeChar('-');
			num = -num;
		}
		_writeNumber(stream, uint64(floor(num)));
		_writeChar('.');
		num -= floor(num);
		for (int i = 0; i < 4; ++i) {
			num *= 10;
			int digit = int(floor(num));
			_writeChar('0' + digit);
			num -= digit;
		}
		return stream;
	}

	Qt::HANDLE LoggingCrashThreadId = 0;
	bool LoggingCrashHeaderWritten = false;
	QMutex LoggingCrashMutex;

    const char *BreakpadDumpPath = 0;
    const wchar_t *BreakpadDumpPathW = 0;

// see https://github.com/benbjohnson/bandicoot
#if defined Q_OS_MAC || defined Q_OS_LINUX32 || defined Q_OS_LINUX64
	struct sigaction SIG_def[32];

	void Handler(int signum, siginfo_t *info, void *ucontext) {
		if (signum > 0) {
			sigaction(signum, &SIG_def[signum], 0);
		}

#else
	void Handler(int signum) {
#endif

		const char* name = 0;
		switch (signum) {
		case SIGABRT: name = "SIGABRT"; break;
		case SIGSEGV: name = "SIGSEGV"; break;
		case SIGILL: name = "SIGILL"; break;
		case SIGFPE: name = "SIGFPE"; break;
#ifndef Q_OS_WIN
		case SIGBUS: name = "SIGBUS"; break;
		case SIGSYS: name = "SIGSYS"; break;
#endif
		}

		Qt::HANDLE thread = QThread::currentThreadId();
		if (thread == LoggingCrashThreadId) return;

		QMutexLocker lock(&LoggingCrashMutex);
		LoggingCrashThreadId = thread;

		if (!LoggingCrashHeaderWritten) {
			LoggingCrashHeaderWritten = true;
			dump() << "Binary: " << LaunchedBinaryName << "\n";
			dump() << "ApiId: " << ApiId << "\n";
			if (cBetaVersion()) {
				dump() << "Version: " << cBetaVersion() << " beta\n";
			} else {
				dump() << "Version: " << AppVersion;
				if (cDevVersion()) {
					dump() << " dev\n";
				} else {
					dump() << "\n";
				}
			}
			dump() << "Launched: " << LaunchedDateTimeStr << "\n";
			dump() << "Platform: ";
			switch (cPlatform()) {
			case dbipWindows: dump() << "win"; break;
			case dbipMac: dump() << "mac"; break;
			case dbipMacOld: dump() << "macold"; break;
			case dbipLinux64: dump() << "linux64"; break;
			case dbipLinux32: dump() << "linux32"; break;
			}
			dump() << "\n";
			psWriteDump();
			dump() << "\n";
		}
		if (name) {
			dump() << "Caught signal " << signum << " (" << name << ") in thread " << uint64(thread) << "\n";
		} else if (signum == -1) {
            dump() << "Google Breakpad caught a crash, minidump written in thread " << uint64(thread) << "\n";
            if (BreakpadDumpPath) {
                dump() << "Minidump: " << BreakpadDumpPath << "\n";
            } else if (BreakpadDumpPathW) {
                dump() << "Minidump: " << BreakpadDumpPathW << "\n";
            }
        } else {
			dump() << "Caught signal " << signum << " in thread " << uint64(thread) << "\n";
		}

#if defined Q_OS_MAC || defined Q_OS_LINUX32 || defined Q_OS_LINUX64
		ucontext_t *uc = (ucontext_t*)ucontext;

		void *caller = 0;
		if (uc) {
#if defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
			/* OSX < 10.6 */
#if defined(__x86_64__)
			caller = (void*)uc->uc_mcontext->__ss.__rip;
#elif defined(__i386__)
			caller = (void*)uc->uc_mcontext->__ss.__eip;
#else
			caller = (void*)uc->uc_mcontext->__ss.__srr0;
#endif
#elif defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
			/* OSX >= 10.6 */
#if defined(_STRUCT_X86_THREAD_STATE64) && !defined(__i386__)
			caller = (void*)uc->uc_mcontext->__ss.__rip;
#else
			caller = (void*)uc->uc_mcontext->__ss.__eip;
#endif
#elif defined(__linux__)
			/* Linux */
#if defined(__i386__)
			caller = (void*)uc->uc_mcontext.gregs[14]; /* Linux 32 */
#elif defined(__X86_64__) || defined(__x86_64__)
			caller = (void*)uc->uc_mcontext.gregs[16]; /* Linux 64 */
#elif defined(__ia64__) /* Linux IA64 */
			caller = (void*)uc->uc_mcontext.sc_ip;
#endif

#endif
		}

        void *addresses[132] = { 0 };
		size_t size = backtrace(addresses, 128);

		/* overwrite sigaction with caller's address */
        if (caller) {
            for (int i = size; i > 1; --i) {
                addresses[i + 3] = addresses[i];
            }
            addresses[2] = (void*)0x1;
            addresses[3] = caller;
            addresses[4] = (void*)0x1;
        }

#ifdef Q_OS_MAC
		dump() << "\nBase image addresses:\n";
		for (size_t i = 0; i < size; ++i) {
			Dl_info info;
			dump() << i << " ";
			if (dladdr(addresses[i], &info)) {
				dump() << uint64(info.dli_fbase) << " (" << info.dli_fname << ")\n";
			} else {
				dump() << "_unknown_module_\n";
			}
		}
#endif

		dump() << "\nBacktrace:\n";

		backtrace_symbols_fd(addresses, size, CrashDumpFileNo);

#else
		dump() << "\nBacktrace:\n";

		psWriteStackTrace();
#endif

		dump() << "\n";

		LoggingCrashThreadId = 0;
	}

	google_breakpad::ExceptionHandler* BreakpadExceptionHandler = 0;

#ifdef Q_OS_WIN
	bool DumpCallback(const wchar_t* _dump_dir, const wchar_t* _minidump_id, void* context, EXCEPTION_POINTERS* exinfo, MDRawAssertionInfo* assertion, bool success)
#elif defined Q_OS_MAC
	bool DumpCallback(const char* _dump_dir, const char* _minidump_id, void *context, bool success)
#elif defined Q_OS_LINUX64 || defined Q_OS_LINUX32
	bool DumpCallback(const google_breakpad::MinidumpDescriptor &md, void *context, bool success)
#endif
	{
#ifdef Q_OS_WIN
        BreakpadDumpPathW = _minidump_id;
        Handler(-1);
#else

#ifdef Q_OS_MAC
        BreakpadDumpPath = _minidump_id;
#else
        BreakpadDumpPath = md.path();
#endif
		Handler(-1, 0, 0);
#endif
		return success;
	}

	void StartBreakpad() {
		QString dumpPath = cWorkingDir() + qsl("tdumps");
		QDir().mkpath(dumpPath);

#ifdef Q_OS_WIN
		BreakpadExceptionHandler = new google_breakpad::ExceptionHandler(
			dumpPath.toStdWString(),
			/*FilterCallback*/ 0,
			DumpCallback,
			/*context*/	0,
			true
		);
#elif defined Q_OS_MAC
		BreakpadExceptionHandler = new google_breakpad::ExceptionHandler(
			dumpPath.toStdString(),
			/*FilterCallback*/ 0,
			DumpCallback,
			/*context*/ 0,
			true,
			0
		);
#elif defined Q_OS_LINUX64 || defined Q_OS_LINUX32
		BreakpadExceptionHandler = new google_breakpad::ExceptionHandler(
			google_breakpad::MinidumpDescriptor(dumpPath.toStdString()),
			/*FilterCallback*/ 0,
			DumpCallback,
			/*context*/ 0,
			true,
			-1
		);
#endif
	}

	void FinishBreakpad() {
		if (BreakpadExceptionHandler) {
			google_breakpad::ExceptionHandler *h = BreakpadExceptionHandler;
			BreakpadExceptionHandler = 0;
			delete h;
		}
	}

	Status start() {
		CrashDumpPath = (cWorkingDir() + qsl("tdata/working")).toUtf8();
		if (FILE *f = fopen(CrashDumpPath.constData(), "rb")) {
			QByteArray lastdump;
			char buffer[64 * 1024] = { 0 };
			int32 read = 0;
			while ((read = fread(buffer, 1, 64 * 1024, f)) > 0) {
				lastdump.append(buffer, read);
			}
			fclose(f);

			Global::SetLastCrashDump(lastdump);

			LOG(("Opened '%1' for reading, the previous Telegram Desktop launch was not finished properly :( Crash log size: %2").arg(QString::fromUtf8(CrashDumpPath)).arg(lastdump.size()));

			return LastCrashed;
		}
		return restart();
	}

	Status restart() {
		if (CrashDumpFile) {
			return Started;
		}

		CrashDumpFile = fopen(CrashDumpPath.constData(), "wb");
		if (CrashDumpFile) {
			CrashDumpFileNo = fileno(CrashDumpFile);

			QByteArray launchedDateTime = QDateTime::currentDateTime().toString("dd.MM.yyyy hh:mm:ss").toUtf8();
			t_assert(launchedDateTime.size() < int(sizeof(LaunchedDateTimeStr)));
			memcpy(LaunchedDateTimeStr, launchedDateTime.constData(), launchedDateTime.size());

			QByteArray launchedBinaryName = cExeName().toUtf8();
			t_assert(launchedBinaryName.size() < int(sizeof(LaunchedBinaryName)));
			memcpy(LaunchedBinaryName, launchedBinaryName.constData(), launchedBinaryName.size());

#ifndef Q_OS_WIN
			struct sigaction sigact;

			sigact.sa_sigaction = SignalHandlers::Handler;
			sigemptyset(&sigact.sa_mask);
			sigact.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;

#ifndef Q_OS_MAC // let breakpad handle this
			sigaction(SIGABRT, &sigact, &SIG_def[SIGABRT]);
#endif
			sigaction(SIGSEGV, &sigact, &SIG_def[SIGSEGV]);
			sigaction(SIGILL, &sigact, &SIG_def[SIGILL]);
			sigaction(SIGFPE, &sigact, &SIG_def[SIGFPE]);
			sigaction(SIGBUS, &sigact, &SIG_def[SIGBUS]);
			sigaction(SIGSYS, &sigact, &SIG_def[SIGSYS]);
#else
			signal(SIGABRT, SignalHandlers::Handler);
			signal(SIGSEGV, SignalHandlers::Handler);
			signal(SIGILL, SignalHandlers::Handler);
			signal(SIGFPE, SignalHandlers::Handler);
#endif
			return Started;
		}

		LOG(("Could not open '%1' for writing!").arg(QString::fromUtf8(CrashDumpPath)));

		return CantOpen;
	}

	void finish() {
		if (BreakpadExceptionHandler) {
			delete BreakpadExceptionHandler;
			BreakpadExceptionHandler = 0;
		}
		if (CrashDumpFile) {
			fclose(CrashDumpFile);
			unlink(CrashDumpPath.constData());
		}
	}

}
