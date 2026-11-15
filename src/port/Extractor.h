/**
 * @file Extractor.h
 * @brief Makes the game's asset archive (sm64.o2r) from the player's ROM, on the TV.
 *
 * Ghostship ships no game data: its extractor (GameExtractor, built on Torch)
 * reads the ROM and writes sm64.o2r, which the game then loads. The port runs
 * it before the game starts, in a separate process (this executable started
 * again with --wm64-extract), as Ghostship's Android launcher does:
 *  - the extractor keeps the ROM and every decoded asset in memory, and its
 *    cleanup is not safe to run, so the process ending is what frees it;
 *  - a failure cannot take the app with it.
 * The child reports its progress on its standard output, one line at a time:
 *   "progress <done> <total>", then "done" or "error <message>".
 */
#ifndef WM64_EXTRACTOR_H
#define WM64_EXTRACTOR_H

#include <sys/types.h>

#include <cstddef>
#include <string>

namespace wm64
{

class Storage;

/** Where an extraction is. */
struct ExtractionStatus {
    bool running  = false;   ///< The child is working.
    bool finished = false;   ///< It ended (see succeeded).
    bool succeeded = false;  ///< sm64.o2r was written.
    std::size_t done  = 0;   ///< Assets processed so far.
    std::size_t total = 0;   ///< Assets to process (0 while the ROM is being parsed).
    std::string error;       ///< Why it failed, for people.
};

class Extraction
{
public:
    Extraction() = default;
    /** Stops a child still running (the archive it was writing is removed). */
    ~Extraction();
    Extraction(const Extraction &)            = delete;
    Extraction &operator=(const Extraction &) = delete;

    /**
     * Starts making @p storage's archive from its ROM.
     * @return False (with the reason in Poll().error) if the child could not start.
     */
    bool Start(const Storage &storage);

    /** Reads the child's progress and checks whether it ended. Call regularly. */
    ExtractionStatus Poll();

private:
    void ReadLines();
    void HandleLine(const std::string &line);

    pid_t child_ = -1;
    int pipe_    = -1;
    std::string pending_;   ///< Output not yet ending in a newline.
    std::string archive_;   ///< The archive being written (removed if the child fails).
    ExtractionStatus status_;
};

/**
 * The child side: if @p argv asks for an extraction (--wm64-extract <rom>
 * <assets dir> <output dir>), runs it and returns the process exit code in
 * @p exitCode. @return False when this is a normal launch.
 */
bool RunExtractionChild(int argc, char **argv, int *exitCode);

} // namespace wm64

#endif // WM64_EXTRACTOR_H
