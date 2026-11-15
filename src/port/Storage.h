/**
 * @file Storage.h
 * @brief The port's data folder: where the game finds its archive, mods and
 *        saves, and where the transfer page puts them.
 *
 * Layout of the data folder (on the TV: $HOME/data, the app's private,
 * persistent storage):
 *   ship/                     Ghostship's app directory (the port sets SHIP_HOME to it)
 *     sm64.o2r                the game's assets, made on the TV from the ROM
 *     mods/                   mods (.o2r/.otr archives); mods/usb-<drive>/ holds
 *                             links to (or a bind mount of) a USB drive's mods
 *     saves/                  the save files
 *     ghostship.cfg.json      settings and controls
 *     logs/Ghostship.log      the game's log
 *   rom/baserom.z64           the player's ROM, in the .z64 byte order (kept so the
 *                             archive can be made again)
 *   incoming/                 a save backup received while the game ran, applied at
 *                             the next launch; temporary files of uploads
 *   transfer_pin.txt          the transfer page's PIN
 *   port-log.txt              the port's log (port-log-previous.txt: the last launch's)
 *
 * The game's own files that are not data (ghostship.o2r, config.yml and assets/,
 * the ROM extractor's descriptions) are in the app, next to the executable.
 */
#ifndef WM64_STORAGE_H
#define WM64_STORAGE_H

#include <cstdint>
#include <string>
#include <vector>

namespace wm64
{

/** A mod archive (or a folder) in a mods folder. */
struct ModEntry {
    std::string name;   ///< File or folder name in the mods folder.
    std::uint64_t size; ///< Bytes (0 for a folder).
    bool folder;        ///< A folder rather than an archive.
};

/** A file in the saves, relative to Ghostship's app directory. */
struct SaveFile {
    std::string path;   ///< e.g. "saves/file1.sav"
    std::uint64_t size; ///< Bytes.
    std::int64_t mtime; ///< Last change, seconds since the Unix epoch.
};

class Storage
{
public:
    /** Space kept free on top of any upload, so the TV is never filled to the brim. */
    static constexpr std::uint64_t kSafetyMarginBytes = 32ull * 1024ull * 1024ull;
    /** Space the archive needs when it is made from the ROM (it is about 10 MB; with room to spare). */
    static constexpr std::uint64_t kArchiveSpaceBytes = 96ull * 1024ull * 1024ull;
    /** Largest mod archive accepted (texture packs can be large). */
    static constexpr std::uint64_t kMaxModBytes = 2048ull * 1024ull * 1024ull;
    /** Largest save backup accepted (saves are a few KB). */
    static constexpr std::uint64_t kMaxBackupBytes = 64ull * 1024ull * 1024ull;

    /**
     * Picks the data folder: $WM64_DATA_DIR if set, else $HOME/data on the TV
     * ($HOME/.local/share/WebMario64 on a desktop), else a temporary folder
     * (then nothing survives a restart). Creates it.
     */
    static Storage Resolve();

    /** Absolute path of the data folder, without a trailing slash. */
    const std::string &Root() const { return root_; }

    /** Whether the data folder survives restarts. */
    bool Persistent() const { return persistent_; }

    /** Absolute path of @p relative inside the data folder. */
    std::string PathFor(const std::string &relative) const;

    /** Ghostship's app directory (SHIP_HOME). */
    std::string ShipDir() const { return PathFor("ship"); }

    /** The game's asset archive, made from the ROM. */
    std::string ArchivePath() const { return ShipDir() + "/sm64.o2r"; }

    /** The mods folder. */
    std::string ModsDir() const { return ShipDir() + "/mods"; }

    /** The save files' folder. */
    std::string SavesDir() const { return ShipDir() + "/saves"; }

    /** Ghostship's settings file. */
    std::string ConfigPath() const { return ShipDir() + "/ghostship.cfg.json"; }

    /** The game's log. */
    std::string GameLogPath() const { return ShipDir() + "/logs/Ghostship.log"; }

    /** Where the ROM is kept (.z64 byte order). */
    std::string RomPath() const { return PathFor("rom/baserom.z64"); }

    /** Where a save backup received while the game runs waits for the next launch, and uploads are assembled. */
    std::string IncomingDir() const { return PathFor("incoming"); }

    /** The port's log (port-log.txt), or the last launch's (port-log-previous.txt) when @p previous. */
    std::string PortLogPath(bool previous) const { return PathFor(previous ? "port-log-previous.txt" : "port-log.txt"); }

    /** A file the crash handler leaves (Log::SetCrashMarkerPath): there at a launch means the last one crashed. */
    std::string CrashMarkerPath() const { return PathFor("last-crash"); }

    /** Where a save backup received while the game runs is kept until the next launch. */
    std::string PendingBackupPath() const { return IncomingDir() + "/saves-backup.zip"; }

    /**
     * A file asking for the archive to be made again from the ROM at the next
     * launch (a new ROM arrived while the game ran, or the page asked for it):
     * the running game holds the archive open.
     */
    std::string RebuildMarkerPath() const { return PathFor("rebuild-archive"); }

    /** Creates the folders above. */
    void CreateFolders() const;

    /** Whether the archive is there (the game can start). */
    bool HasArchive() const { return FileSize(ArchivePath()) > 0; }

    /** Whether a ROM is kept (the archive can be made). */
    bool HasRom() const;

    /** Bytes available to the app on the filesystem holding @p path (default: the data folder). */
    std::uint64_t FreeBytes(const std::string &path = std::string()) const;

    /** Size of the file at @p path in bytes (following links), or 0 if it does not exist. */
    static std::uint64_t FileSize(const std::string &path);

    /** Whether @p path is a regular file itself (not a link, not missing). */
    static bool IsPlainFile(const std::string &path);

    /**
     * Whether @p name may be used for a mod uploaded through the page: a plain
     * file name ending in .o2r or .otr (the archives Ghostship loads from mods/).
     */
    static bool IsAllowedModName(const std::string &name);

    /** Whether @p name is a mod archive's (.o2r or .otr, any case). */
    static bool IsModArchiveName(const std::string &name);

    /**
     * The mods in the mods folder at @p dir, sorted by name: archives and
     * folders, but not the usb-* folders, Ghostship's own ~ folders or its
     * placeholder text file.
     */
    static std::vector<ModEntry> ListMods(const std::string &dir);

    /** The files a save backup holds (see SaveBackup.h). */
    std::vector<SaveFile> ListSaves() const;

    /**
     * Applies a save backup received while the game ran (incoming/), before
     * the game starts. @return True if one was applied.
     */
    bool ApplyPendingBackup() const;

private:
    Storage(std::string root, bool persistent);

    std::string root_;
    bool persistent_ = true;
};

/** Formats a byte count for people ("1.5 GB"). */
std::string HumanBytes(std::uint64_t bytes);

/** Whether @p name ends with @p suffix, ignoring case (ASCII). */
bool EndsWithNoCase(const std::string &name, const std::string &suffix);

/** @p text in lower case (ASCII). */
std::string Lower(std::string text);

} // namespace wm64

#endif // WM64_STORAGE_H
