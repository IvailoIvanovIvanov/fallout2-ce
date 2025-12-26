#ifndef FALLOUT_RENDERER_LOGGER_H
#define FALLOUT_RENDERER_LOGGER_H

/**
 * @file logger.h
 * @brief Logging utilities for the renderer subsystem.
 *
 * Provides a simple file-based logging system with severity levels
 * for debugging and diagnostics during rendering operations.
 */

#include <string>

namespace fallout {
namespace renderer {

/**
 * @enum LogLevel
 * @brief Severity levels for log messages.
 */
enum class LogLevel {
    Info,    ///< Informational messages for normal operation
    Warning, ///< Warning messages for recoverable issues
    Error    ///< Error messages for failures
};

/**
 * @class Logger
 * @brief Static logging class for file-based message output.
 *
 * Thread-safety: Not thread-safe. All logging should occur from main thread.
 */
class Logger {
public:
    /**
     * @brief Initializes the logger and opens the log file.
     * @param logPath Path to the log file.
     */
    static void Init(const std::string& logPath);

    /**
     * @brief Logs a formatted message with the specified severity level.
     * @param level Severity level of the message.
     * @param format printf-style format string.
     * @param ... Variable arguments for format string.
     */
    static void Log(LogLevel level, const char* format, ...);

    /**
     * @brief Closes the log file and shuts down the logger.
     */
    static void Shutdown();

private:
    /**
     * @brief Writes the current timestamp to the log file.
     */
    static void WriteTimestamp();
    
    /**
     * @brief Writes the log level prefix to the log file.
     */
    static void WriteLevelPrefix(LogLevel level);
    
    static FILE* mLogFile;  ///< Handle to the open log file
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_LOGGER_H
