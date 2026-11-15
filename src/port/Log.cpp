/**
 * @file Log.cpp
 * @brief Implementation of the port's log (see Log.h).
 */
#include "Log.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace wm64
{
namespace Log
{
namespace
{

std::mutex g_mutex;
std::FILE *g_file = nullptr;
std::string g_path;
/** The log file's descriptor, for the crash handler (which may not use stdio). */
volatile sig_atomic_t g_crashFd = -1;
/** Where the crash handler leaves its marker (fixed storage: the handler may not allocate). */
std::array<char, 1024> g_crashMarker{};

/** Writes @p text to the crash descriptor (async-signal-safe). */
void CrashWrite(const char *text)
{
    if (g_crashFd >= 0)
        (void)!write(g_crashFd, text, std::strlen(text));
}

/** A fatal signal's name (a literal: strsignal() is not async-signal-safe). */
const char *SignalName(int signal)
{
    switch (signal) {
        case SIGSEGV: return "segmentation fault (SIGSEGV)";
        case SIGBUS: return "bus error (SIGBUS)";
        case SIGILL: return "illegal instruction (SIGILL)";
        case SIGFPE: return "arithmetic error (SIGFPE)";
        case SIGABRT: return "aborted (SIGABRT)";
        default: return "fatal signal";
    }
}

/** The fatal signal handler: what happened and where, then the default action. */
void OnFatalSignal(int signal)
{
    CrashWrite("CRASH: ");
    CrashWrite(SignalName(signal));
    CrashWrite("; call addresses (innermost first):\n");
    std::array<void *, 64> frames{};
    const int count = backtrace(frames.data(), static_cast<int>(frames.size()));
    if (g_crashFd >= 0)
        backtrace_symbols_fd(frames.data(), count, g_crashFd);
    if (g_crashFd >= 0)
        fsync(g_crashFd);
    if (g_crashMarker[0] != '\0') {
        const int marker = open(g_crashMarker.data(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (marker >= 0) {
            const char *name = SignalName(signal);
            (void)!write(marker, name, std::strlen(name));
            close(marker);
        }
    }
    ::signal(signal, SIG_DFL);
    raise(signal);
}
const std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

} // namespace

void Open(const std::string &path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
        std::fclose(g_file);
    g_file    = std::fopen(path.c_str(), "w");
    g_path    = g_file ? path : std::string();
    g_crashFd = g_file ? fileno(g_file) : -1;
}

void SetCrashMarkerPath(const std::string &path)
{
    if (path.size() < g_crashMarker.size())
        std::memcpy(g_crashMarker.data(), path.c_str(), path.size() + 1);
}

void InstallCrashHandler()
{
    // The first backtrace() loads the unwinder; do it now, not inside a crash.
    std::array<void *, 4> warmUp{};
    backtrace(warmUp.data(), static_cast<int>(warmUp.size()));
    for (const int signal : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT })
        ::signal(signal, OnFatalSignal);
}

std::string Path()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_path;
}

void Printf(const char *format, ...)
{
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    std::lock_guard<std::mutex> lock(g_mutex);
    std::printf("[WebMario64] %8.3f %s\n", seconds, message);
    std::fflush(stdout);
    if (g_file) {
        std::fprintf(g_file, "%8.3f %s\n", seconds, message);
        std::fflush(g_file);
    }
}

} // namespace Log
} // namespace wm64
