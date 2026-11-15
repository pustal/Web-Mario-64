/**
 * @file Log.h
 * @brief The port's own log: each line goes to stdout and to <data folder>/port-log.txt.
 *
 * On the TV a native app's stdout goes nowhere a player can see, so the lines
 * are also kept in a file (started afresh at each launch), which the transfer
 * page offers next to the game's own log. That makes a start-up that hangs or
 * fails on a TV possible to diagnose.
 */
#ifndef WM64_LOG_H
#define WM64_LOG_H

#include <string>

namespace wm64
{
namespace Log
{

/** Starts the log file at @p path (emptying it). Lines logged before go to stdout only. */
void Open(const std::string &path);

/** The log file's path, or empty before Open(). */
std::string Path();

/**
 * Logs fatal signals (segmentation fault, abort, ...) to the log file, with
 * the addresses of the calls that led there (the executable is not
 * position-independent: `addr2line -e <build>/ghostship/Ghostship <address>` on
 * the unstripped build names them), then lets the signal end the app as
 * before, without a core dump: the kernel would write the whole process
 * image (hundreds of MB) as core.<pid> into the working directory, in the
 * app's own storage. Call after Open().
 */
void InstallCrashHandler();

/**
 * Removes core dumps ("core" or "core.<pid>" files) left in @p dir by crashes
 * of versions that did not turn them off. Not recursive.
 * @return The bytes freed.
 */
unsigned long long RemoveCoreDumps(const std::string &dir);

/**
 * The file the crash handler creates when the app crashes (a fatal signal), so
 * the next launch knows the last one crashed. An app the system closes (or one
 * killed otherwise) leaves no such file.
 */
void SetCrashMarkerPath(const std::string &path);

/**
 * Logs one line, printf-style; "[WebMario64] " and the time since launch are
 * put in front, and a newline is added. Thread-safe; flushed at once, so the
 * file shows how far a start-up got even if the app is killed.
 */
void Printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

} // namespace Log
} // namespace wm64

#endif // WM64_LOG_H
