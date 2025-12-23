#include "logger.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace fallout {
namespace renderer {

FILE* Logger::mLogFile = nullptr;

void Logger::Init(const std::string& logPath) {
    if (mLogFile) return;
    mLogFile = fopen(logPath.c_str(), "w");
}

void Logger::Log(LogLevel level, const char* format, ...) {
    if (!mLogFile) return;

    // Timestamp
    time_t now = time(nullptr);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));

    // Level string
    const char* levelStr = "INFO";
    if (level == LogLevel::Warning) levelStr = "WARN";
    if (level == LogLevel::Error) levelStr = "ERROR";

    fprintf(mLogFile, "[%s] [%s] ", timestamp, levelStr);

    va_list args;
    va_start(args, format);
    vfprintf(mLogFile, format, args);
    va_end(args);

    fprintf(mLogFile, "\n");
    fflush(mLogFile);
}

void Logger::Shutdown() {
    if (mLogFile) {
        fclose(mLogFile);
        mLogFile = nullptr;
    }
}

} // namespace renderer
} // namespace fallout
