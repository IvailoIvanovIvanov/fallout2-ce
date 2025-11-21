#include "diagnostics.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <time.h>

#include <SDL.h>

#include "platform_compat.h"

namespace fallout {

namespace {

constexpr uintmax_t kDiagnosticsMaxFileSize = 5 * 1024 * 1024;

std::mutex gDiagnosticsMutex;
FILE* gDiagnosticsFile = nullptr;
bool gDiagnosticsEnabled = false;
bool gDiagnosticsRegisteredAtExit = false;
DiagnosticsLevel gDiagnosticsLevel = DiagnosticsLevel::Off;
std::string gDiagnosticsPath = "diagnostics.log";
uint64_t gDiagnosticsStartTicks = 0;
std::atomic<bool> gDiagnosticsFastEnabled(false);
std::atomic<int> gDiagnosticsFastLevel(0);

const char* kLevelStrings[] = {
    "off",
    "info",
    "trace",
};

void diagnosticsRotateLogLocked()
{
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(gDiagnosticsPath, ec)) {
        return;
    }

    uintmax_t size = fs::file_size(gDiagnosticsPath, ec);
    if (ec || size <= kDiagnosticsMaxFileSize) {
        return;
    }

    std::string backupPath = gDiagnosticsPath + ".1";
    fs::remove(backupPath, ec);
    fs::rename(gDiagnosticsPath, backupPath, ec);
}

uint64_t getElapsedTicksLocked()
{
    if (gDiagnosticsStartTicks == 0) {
        gDiagnosticsStartTicks = SDL_GetTicks64();
    }
    uint64_t now = SDL_GetTicks64();
    return now - gDiagnosticsStartTicks;
}

} // namespace

void diagnosticsShutdown()
{
    std::lock_guard<std::mutex> guard(gDiagnosticsMutex);
    if (gDiagnosticsFile != nullptr) {
        fprintf(gDiagnosticsFile, "---- diagnostics shutdown ----\n");
        fclose(gDiagnosticsFile);
        gDiagnosticsFile = nullptr;
    }
    gDiagnosticsFastEnabled.store(false, std::memory_order_relaxed);
}

void diagnosticsInit(bool enabled, DiagnosticsLevel level, const char* logFilePath)
{
    std::lock_guard<std::mutex> guard(gDiagnosticsMutex);

    if (logFilePath != nullptr && logFilePath[0] != '\0') {
        gDiagnosticsPath = logFilePath;
    } else if (gDiagnosticsPath.empty()) {
        gDiagnosticsPath = "diagnostics.log";
    }

    gDiagnosticsLevel = level;
    gDiagnosticsEnabled = enabled;
    gDiagnosticsFastLevel.store(static_cast<int>(level), std::memory_order_relaxed);
    gDiagnosticsFastEnabled.store(enabled, std::memory_order_relaxed);

    if (gDiagnosticsFile != nullptr) {
        fclose(gDiagnosticsFile);
        gDiagnosticsFile = nullptr;
    }

    if (!gDiagnosticsEnabled) {
        return;
    }

    diagnosticsRotateLogLocked();

    gDiagnosticsFile = compat_fopen(gDiagnosticsPath.c_str(), "at");
    if (gDiagnosticsFile == nullptr) {
        gDiagnosticsEnabled = false;
        gDiagnosticsFastEnabled.store(false, std::memory_order_relaxed);
        return;
    }

    if (!gDiagnosticsRegisteredAtExit) {
        atexit(diagnosticsShutdown);
        gDiagnosticsRegisteredAtExit = true;
    }

    fprintf(gDiagnosticsFile, "---- diagnostics start (level=%s, file=%s) ----\n", diagnosticsLevelToString(gDiagnosticsLevel), gDiagnosticsPath.c_str());
    fflush(gDiagnosticsFile);
}

void diagnosticsSetEnabled(bool enabled)
{
    std::lock_guard<std::mutex> guard(gDiagnosticsMutex);
    gDiagnosticsEnabled = enabled;
    gDiagnosticsFastEnabled.store(enabled && gDiagnosticsFile != nullptr, std::memory_order_relaxed);
}

bool diagnosticsIsEnabled()
{
    std::lock_guard<std::mutex> guard(gDiagnosticsMutex);
    return gDiagnosticsEnabled && gDiagnosticsFile != nullptr;
}

DiagnosticsLevel diagnosticsGetLevel()
{
    std::lock_guard<std::mutex> guard(gDiagnosticsMutex);
    return gDiagnosticsLevel;
}

bool diagnosticsIsLevelEnabled(DiagnosticsLevel level)
{
    if (!diagnosticsWouldLog(level)) {
        return false;
    }

    std::lock_guard<std::mutex> guard(gDiagnosticsMutex);
    if (!gDiagnosticsEnabled || gDiagnosticsFile == nullptr) {
        return false;
    }

    return static_cast<int>(level) <= static_cast<int>(gDiagnosticsLevel);
}

bool diagnosticsWouldLog(DiagnosticsLevel level)
{
    if (!gDiagnosticsFastEnabled.load(std::memory_order_relaxed)) {
        return false;
    }

    return static_cast<int>(level) <= gDiagnosticsFastLevel.load(std::memory_order_relaxed);
}

const char* diagnosticsGetLogPath()
{
    std::lock_guard<std::mutex> guard(gDiagnosticsMutex);
    return gDiagnosticsPath.c_str();
}

const char* diagnosticsLevelToString(DiagnosticsLevel level)
{
    const int index = std::clamp(static_cast<int>(level), 0, 2);
    return kLevelStrings[index];
}

DiagnosticsLevel diagnosticsParseLevel(const char* value)
{
    if (value == nullptr) {
        return DiagnosticsLevel::Info;
    }

    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (lowered == "trace") {
        return DiagnosticsLevel::Trace;
    }

    if (lowered == "off") {
        return DiagnosticsLevel::Off;
    }

    return DiagnosticsLevel::Info;
}

void diagnosticsLog(DiagnosticsLevel level, const char* category, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    diagnosticsLogV(level, category, format, args);
    va_end(args);
}

void diagnosticsLogV(DiagnosticsLevel level, const char* category, const char* format, va_list args)
{
    std::lock_guard<std::mutex> guard(gDiagnosticsMutex);

    if (!gDiagnosticsEnabled || gDiagnosticsFile == nullptr) {
        return;
    }

    if (static_cast<int>(level) > static_cast<int>(gDiagnosticsLevel)) {
        return;
    }

    char message[1024];
    vsnprintf(message, sizeof(message), format, args);

    uint64_t elapsedMs = getElapsedTicksLocked();

    std::time_t now = std::time(nullptr);
    std::tm timeInfo;
#if defined(_WIN32)
    localtime_s(&timeInfo, &now);
#else
    localtime_r(&now, &timeInfo);
#endif
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeInfo);

    fprintf(gDiagnosticsFile, "[%s][%010llu ms][%s][%s] %s\n",
        timestamp,
        static_cast<unsigned long long>(elapsedMs),
        category != nullptr ? category : "GENERAL",
        diagnosticsLevelToString(level),
        message);

    fflush(gDiagnosticsFile);
}

} // namespace fallout
