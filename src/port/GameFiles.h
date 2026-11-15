/**
 * @file GameFiles.h
 * @brief Where the game files live, on the TV or on a USB drive, and moving
 *        them between the two.
 *
 * The game files are the player's ROM and the archive Ghostship's extractor
 * makes from it (sm64.o2r, about 10 MB, plus the 8 MB ROM kept to make it
 * again). They live together, either
 *  - on the TV: rom/baserom.z64 and ship/sm64.o2r in the data folder, or
 *  - on a USB drive, in its "Super Mario 64" folder: the ROM (any name; the
 *    app names the ones it puts there baserom.z64) and sm64.o2r.
 *
 * Ghostship reads its archive only from ship/sm64.o2r. When the archive is on
 * a drive, Place() puts a link to it there: a symbolic link when the app can
 * read the drive, otherwise a root bind mount of the file over an empty one.
 * The TV's own archive, when there is one, always wins.
 *
 * Which ROM the archive is made from: the TV's, else the first one in a
 * drive's game folder. The archive is made where that ROM is: on the TV, or
 * next to it on the drive (written there directly, or made on the TV and
 * moved there by root when the app cannot write to the drive).
 *
 * All methods are thread-safe: the setup screen, the launcher and the
 * transfer page's threads use one instance.
 */
#ifndef WM64_GAME_FILES_H
#define WM64_GAME_FILES_H

#include "Storage.h"
#include "UsbAccess.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace wm64
{

/** Where one of the game files is. */
struct GameFilePlace {
    bool present = false;       ///< The file exists (and is reachable).
    std::string drive;          ///< Mount point of the drive holding it; empty for the TV.
    std::string driveLabel;     ///< The drive's short name ("sda1"), for people.
    std::string path;           ///< Absolute path of the file itself (on the TV or the drive).
    bool readable = true;       ///< The app can read the file itself (else only root can).
    std::uint64_t bytes = 0;    ///< Its size.

    /** "the TV" or "USB sda1", for people. */
    std::string Describe() const { return drive.empty() ? std::string("the TV") : "USB " + driveLabel; }
};

/** A snapshot of the game files. */
struct GameFilesState {
    GameFilePlace rom;      ///< The ROM the archive is (or would be) made from.
    GameFilePlace archive;  ///< The archive the game reads.
};

/** How an extraction runs: which ROM it reads and where it writes the archive. */
struct ExtractionPlan {
    std::string romPath;        ///< The ROM to read (.z64 order; may be a temporary copy).
    bool temporaryRom = false;  ///< romPath is a copy made for this extraction (removed afterwards).
    std::string outputDir;      ///< Where the extractor writes sm64.o2r.
    std::string drive;          ///< The drive the archive goes on; empty for the TV.
    std::string driveLabel;     ///< Its short name.
    bool moveWithRoot = false;  ///< Made on the TV first, then moved onto the drive by root.
};

class GameFiles
{
public:
    GameFiles(const Storage &storage, UsbAccess &usb);
    GameFiles(const GameFiles &)            = delete;
    GameFiles &operator=(const GameFiles &) = delete;

    /**
     * Makes ship/sm64.o2r the archive the game should read: the TV's own, or
     * a link to a drive's (see the file comment). Drops links to drives that
     * are gone. Called at launch, when drives change and after a move.
     */
    void Place();

    /** Where the ROM and the archive are now. Lists the drives (may run root commands). */
    GameFilesState State();

    /** State() for drives the caller has just listed (UsbAccess::ListDrives()). */
    GameFilesState StateFor(const std::vector<UsbDrive> &drives);

    /**
     * The state found by the last State()/StateFor()/Place() call, without
     * looking at the drives again (for the menu, which must not wait on root commands).
     */
    GameFilesState LastState();

    /** Whether there is a ROM to make the archive from (on the TV or in a drive's game folder). */
    bool HasRom();

    /** A key that changes when the ROM used changes (its place, size and time), empty without one. */
    std::string RomKey();

    /**
     * Prepares making the archive from the ROM (see the file comment): copies
     * a drive's ROM to the TV when needed, clears the old archive and the
     * links. @return False with @p error set when it cannot be done.
     */
    bool PrepareExtraction(ExtractionPlan *plan, std::string *error);

    /**
     * After an extraction: moves the archive onto the drive if root has to,
     * removes temporary files and links the result in (only cleans up when
     * @p succeeded is false). @return Empty string on success, otherwise the reason.
     */
    std::string FinishExtraction(const ExtractionPlan &plan, bool succeeded);

    /**
     * Keeps a ROM uploaded through the page (any byte order, checked here)
     * on the TV (@p drive empty) or in a drive's game folder; a ROM on the TV
     * is removed when the new one goes to a drive, so the new one is used.
     * @param name Receives the dump's name.
     * @return Empty string on success, otherwise the reason.
     */
    std::string StoreRom(std::vector<std::uint8_t> data, const std::string &drive, std::string *name);

    /** Removes the ROM in use (the TV's, else the drive's). @return Empty string on success, otherwise the reason. */
    std::string RemoveRom();

    /**
     * Moves the ROM and the archive to the TV (@p drive empty) or to a
     * drive's game folder, reporting the bytes copied so far. The running
     * game keeps reading the archive it opened; the next start uses the moved one.
     * @return Empty string on success, otherwise the reason.
     */
    std::string MoveTo(const std::string &drive, const std::function<void(std::uint64_t done, std::uint64_t total)> &onProgress);

private:
    /** The drive's game folder ("<mount>/Super Mario 64"). */
    static std::string DriveFolder(const std::string &mountPoint);

    /** Whether ship/sm64.o2r is the TV's own archive (a plain, non-empty file, not a bind mount). */
    bool ArchiveIsOnTv() const;

    /** Removes the link (symbolic link, bind mount or its empty placeholder) at ship/sm64.o2r, if any. */
    void ReleaseLink();

    /** Links @p drive's archive at ship/sm64.o2r. @return True if the game can read it there. */
    bool Link(const UsbDrive &drive);

    /** Reads the ROM at @p place (converted to .z64 order and checked) into @p z64. */
    bool ReadRom(const GameFilePlace &place, std::vector<std::uint8_t> *z64, std::string *error);

    const Storage &storage_;
    UsbAccess &usb_;
    std::recursive_mutex mutex_;
    GameFilesState last_; ///< See LastState().
};

} // namespace wm64

#endif // WM64_GAME_FILES_H
