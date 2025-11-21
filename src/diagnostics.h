#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <stdarg.h>

namespace fallout {

enum class DiagnosticsLevel {
    Off = 0,
    Info = 1,
    Trace = 2,
};

void diagnosticsInit(bool enabled, DiagnosticsLevel level, const char* logFilePath = nullptr);
void diagnosticsShutdown();
void diagnosticsSetEnabled(bool enabled);
bool diagnosticsIsEnabled();
DiagnosticsLevel diagnosticsGetLevel();
bool diagnosticsIsLevelEnabled(DiagnosticsLevel level);
bool diagnosticsWouldLog(DiagnosticsLevel level);
const char* diagnosticsGetLogPath();
const char* diagnosticsLevelToString(DiagnosticsLevel level);
DiagnosticsLevel diagnosticsParseLevel(const char* value);
void diagnosticsLog(DiagnosticsLevel level, const char* category, const char* format, ...);
void diagnosticsLogV(DiagnosticsLevel level, const char* category, const char* format, va_list args);

} // namespace fallout

#endif /* DIAGNOSTICS_H */
