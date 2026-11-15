/**
 * @file Extractor.cpp
 * @brief Implementation of the archive extraction (see Extractor.h).
 */
#include "Extractor.h"

#include "Log.h"
#include "Storage.h"

// Ghostship's extractor (src/port/GameExtractor.h, Torch underneath).
#include "port/GameExtractor.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <thread>
#include <vector>

namespace wm64
{
namespace
{

constexpr const char *kChildFlag = "--wm64-extract";

/** The folder holding the executable: config.yml and assets/ (Torch's descriptions) are next to it. */
std::string ExecutableDir()
{
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string(".") : self.parent_path().string();
}

} // namespace

Extraction::~Extraction()
{
    if (child_ > 0) {
        kill(child_, SIGKILL);
        waitpid(child_, nullptr, 0);
        std::error_code ec;
        std::filesystem::remove(archive_, ec);
    }
    if (pipe_ >= 0)
        close(pipe_);
}

bool Extraction::Start(const Storage &storage)
{
    status_  = ExtractionStatus{};
    archive_ = storage.ArchivePath();
    std::error_code ec;
    std::filesystem::remove(archive_, ec);

    int fds[2];
    if (pipe(fds) != 0) {
        status_.finished = true;
        status_.error    = "Could not start the extraction (no pipe).";
        return false;
    }

    // Everything the child needs is prepared before fork(): it only calls
    // async-signal-safe functions until execv().
    const std::string self   = "/proc/self/exe";
    const std::string rom    = storage.RomPath();
    const std::string assets = ExecutableDir();
    const std::string output = storage.ShipDir();
    std::vector<char *> args = { const_cast<char *>(self.c_str()), const_cast<char *>(kChildFlag), const_cast<char *>(rom.c_str()),
                                 const_cast<char *>(assets.c_str()), const_cast<char *>(output.c_str()), nullptr };

    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execv(self.c_str(), args.data());
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        status_.finished = true;
        status_.error    = "Could not start the extraction.";
        return false;
    }
    child_ = pid;
    pipe_  = fds[0];
    fcntl(pipe_, F_SETFL, fcntl(pipe_, F_GETFL) | O_NONBLOCK);
    status_.running = true;
    Log::Printf("extraction started (pid %d)", static_cast<int>(pid));
    return true;
}

void Extraction::HandleLine(const std::string &line)
{
    unsigned long done = 0, total = 0;
    if (std::sscanf(line.c_str(), "progress %lu %lu", &done, &total) == 2) {
        // Torch resets its counter between passes, so the count can step back:
        // the bar only moves forward while the total stays the same.
        if (total != status_.total)
            status_.done = 0;
        status_.total = total;
        status_.done  = std::min<std::size_t>(std::max<std::size_t>(status_.done, done), total);
    }
    else if (line == "done") {
        status_.succeeded = true;
    }
    else if (line.rfind("error ", 0) == 0) {
        status_.error = line.substr(6);
    }
}

void Extraction::ReadLines()
{
    if (pipe_ < 0)
        return;
    char buffer[512];
    ssize_t length;
    while ((length = read(pipe_, buffer, sizeof(buffer))) > 0) {
        pending_.append(buffer, static_cast<std::size_t>(length));
        std::size_t newline;
        while ((newline = pending_.find('\n')) != std::string::npos) {
            HandleLine(pending_.substr(0, newline));
            pending_.erase(0, newline + 1);
        }
    }
}

ExtractionStatus Extraction::Poll()
{
    ReadLines();
    if (child_ > 0) {
        int status       = 0;
        const pid_t done = waitpid(child_, &status, WNOHANG);
        if (done == child_) {
            ReadLines();
            child_          = -1;
            status_.running = false;
            status_.finished = true;
            const bool exitedOk = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            status_.succeeded   = status_.succeeded && exitedOk && Storage::FileSize(archive_) > 0;
            if (!status_.succeeded) {
                if (status_.error.empty()) {
                    status_.error = WIFSIGNALED(status) ? "The extractor crashed (signal " + std::to_string(WTERMSIG(status)) + ")."
                                                        : "The extractor stopped without writing the archive.";
                }
                std::error_code ec;
                std::filesystem::remove(archive_, ec);
            }
            Log::Printf("extraction %s%s%s", status_.succeeded ? "finished" : "failed",
                        status_.error.empty() ? "" : ": ", status_.error.c_str());
            close(pipe_);
            pipe_ = -1;
        }
    }
    return status_;
}

bool RunExtractionChild(int argc, char **argv, int *exitCode)
{
    if (argc < 5 || std::strcmp(argv[1], kChildFlag) != 0)
        return false;
    const std::string rom    = argv[2];
    const std::string assets = argv[3];
    const std::string output = argv[4];
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    const auto report = [](const std::string &line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };

    GameExtractor extractor;
    if (!extractor.RunStandalone(rom)) {
        report("error The ROM could not be read.");
        *exitCode = 1;
        return true;
    }

    // Counting first gives the progress bar its total.
    std::atomic<std::size_t> total{ 0 }, done{ 0 };
    std::atomic<bool> working{ true };
    std::thread reporter([&] {
        while (working.load()) {
            std::printf("progress %zu %zu\n", done.load(), total.load());
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });

    std::string error;
    try {
        if (!extractor.Parse(total, "sm64"))
            error = GameExtractor::sLastError.empty() ? "The ROM could not be parsed." : GameExtractor::sLastError;
        else if (!extractor.GenerateOTRTo(done, assets, output))
            error = GameExtractor::sLastError.empty() ? "The archive could not be written." : GameExtractor::sLastError;
    } catch (const std::exception &exception) {
        error = exception.what();
    } catch (...) {
        error = "The extractor failed.";
    }
    working.store(false);
    reporter.join();

    // Init() logs rather than throws for some failures: check the archive really landed.
    if (error.empty() && Storage::FileSize(output + "/sm64.o2r") == 0)
        error = "The extractor finished without writing sm64.o2r.";
    // Torch leaves its cache of the ROM's hashes next to the archive; the game does not use it.
    std::error_code ignored;
    std::filesystem::remove(output + "/torch.hash.yml", ignored);
    std::printf("progress %zu %zu\n", done.load(), total.load());
    report(error.empty() ? "done" : "error " + error);
    std::fflush(stdout);
    // The extractor's state is not safe to destroy (see Ghostship's AndroidBridge.cpp): leave without it.
    *exitCode = error.empty() ? 0 : 1;
    _exit(*exitCode);
}

} // namespace wm64
