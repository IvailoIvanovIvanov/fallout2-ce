#ifndef FALLOUT_RENDERER_LOGGER_H
#define FALLOUT_RENDERER_LOGGER_H

#include <string>

namespace fallout {
namespace renderer {

enum class LogLevel {
    Info,
    Warning,
    Error
};

class Logger {
public:
    static void Init(const std::string& logPath);
    static void Log(LogLevel level, const char* format, ...);
    static void Shutdown();

private:
    static FILE* mLogFile;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_LOGGER_H
