/**
 * @file UsbAccess.h
 * @brief Root access, USB drives, and the game files kept on them.
 *
 * webOS mounts USB drives under /tmp/usb/<disk>/<partition>. An app normally
 * runs inside a jail as an unprivileged user, so whether it can see, read or
 * write a drive depends on the firmware. Everything here therefore tries the
 * direct route first and falls back to root commands, which are available
 * when the app itself runs as root or when the Homebrew Channel's
 * luna://org.webosbrew.hbchannel.service/exec accepts the app's calls (rooted
 * TVs; newer firmwares may refuse calls from apps).
 *
 * A drive keeps the game's files in its "Super Mario 64" folder:
 *   Super Mario 64/<rom>.z64, .n64 or .v64   the ROM (any supported 8 MB dump there;
 *                                            baserom.z64, the name the app gives it, first)
 *   Super Mario 64/sm64.o2r                  the game's archive, made from that ROM
 *   Super Mario 64/mods/                     mods (.o2r/.otr archives, or folders)
 * The ROM and the archive are used where they are (GameFiles.h decides where
 * the game files live and links the archive in). A
 * drive's mods appear in the game's mods folder as mods/usb-<drive>/: a real
 * folder holding links to the drive's mods, or, when the app cannot read the
 * drive itself, a root bind mount of the drive's mods folder. (Ghostship
 * looks into folders in mods/ but does not follow folder links.)
 *
 * With no ROM on the TV or in a drive's "Super Mario 64" folder, the app also
 * looks in the RetroArch-style system
 * folders roms/n64 and roms/nintendo64 at the root of each drive (any
 * capitalisation, and two folder levels below them), at .z64, .n64 and .v64
 * files with "mario" in their name, as Webmon does. Nothing in those folders
 * is changed: a supported ROM found there is copied to the TV. Searching a
 * large collection takes a while, so this runs on its own thread
 * (StartRomSearch()), while the setup screen is up.
 */
#ifndef WM64_USB_ACCESS_H
#define WM64_USB_ACCESS_H

#include "Storage.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace wm64
{

/** What kind of root access the app has. */
struct RootStatus {
    bool runningAsRoot = false;   ///< The app process itself is root.
    bool homebrewChannel = false; ///< Homebrew Channel exec works from this app.
    std::string detail;           ///< Short explanation for the page (e.g. why exec failed).

    bool Available() const { return runningAsRoot || homebrewChannel; }
};

/** A mounted USB drive, and the game files in its "Super Mario 64" folder. */
struct UsbDrive {
    std::string mountPoint;           ///< e.g. /tmp/usb/sda/sda1
    std::string label;                ///< Short name for people, e.g. "sda1".
    std::uint64_t freeBytes = 0;
    std::uint64_t totalBytes = 0;
    bool spaceKnown = false;          ///< statvfs (or root df) succeeded.
    bool readable = false;            ///< The app can list it directly.
    bool writable = false;            ///< The app can create files on it directly.
    std::string romFile;              ///< A ROM-sized file in "Super Mario 64", empty when it has none.
    std::uint64_t archiveBytes = 0;   ///< Size of "Super Mario 64/sm64.o2r" (0: none).
    bool modsFolder = false;          ///< "Super Mario 64/mods" exists.
    std::vector<ModEntry> mods;       ///< The mods in it.
};

class UsbAccess
{
public:
    UsbAccess() = default;
    /** Stops a ROM search that is still running (it ends at its next file). */
    ~UsbAccess();
    UsbAccess(const UsbAccess &)            = delete;
    UsbAccess &operator=(const UsbAccess &) = delete;

    /** The game's folder on a drive, relative to its mount point. */
    static constexpr const char *kDriveFolder = "Super Mario 64";
    /** The mods folder on a drive, relative to its mount point. */
    static constexpr const char *kDriveModsFolder = "Super Mario 64/mods";
    /** The game's archive in a drive's game folder. */
    static constexpr const char *kDriveArchiveName = "sm64.o2r";
    /** The name the app gives a ROM it puts in a drive's game folder (.z64 order). */
    static constexpr const char *kDriveRomName = "baserom.z64";

    /** Probes root access (once; it takes up to a few seconds). */
    void ProbeRoot();

    /** Last probe result. */
    RootStatus Root() const;

    /** Lists mounted USB drives with their space, access and game files. */
    std::vector<UsbDrive> ListDrives();

    /** Fills @p drive for the drive mounted at @p mountPoint. @return False if it is not a mounted USB drive. */
    bool FindDrive(const std::string &mountPoint, UsbDrive *drive);

    /** Whether @p mountPoint is currently a mounted USB drive (guards upload targets). */
    bool IsUsbMount(const std::string &mountPoint);

    /**
     * Starts looking for the ROM on the drives on a thread: in each drive's
     * "Super Mario 64" folder first, then in its RetroArch folders (see the
     * file comment). Does nothing if a search is running or the TV has a ROM.
     * A supported ROM found is copied to @p storage's ROM path. A ROM in a
     * drive's game folder is not copied (it is used where it is): with one
     * there, there is nothing to search for.
     */
    void StartRomSearch(const Storage &storage);

    /** Whether a ROM search is running. */
    bool RomSearchRunning() const;

    /** Where the ROM was copied from this session ("USB sda1: roms/n64"), or empty. */
    std::string RomImportedFrom() const;

    /** The mounted USB drives' mount points joined: changes when a drive is plugged in or out. */
    std::string MountSignature();

    /**
     * Makes the game see the drives' mods (see the file comment). Removes the
     * links and bind mounts of earlier sessions first. Called at launch, and
     * again by the transfer page after it changed what a drive holds.
     */
    void PlaceContent(const Storage &storage);

    /** The name a drive's mods have in the game's mods folder: "usb-<label>". */
    static std::string ModLinkName(const std::string &label);

    /** The drives whose mods are linked into the mods folder (mount points). */
    std::vector<std::string> LinkedModDrives() const;

    /**
     * Creates @p relative (a folder path) on the drive mounted at
     * @p mountPoint, directly or with root.
     * @return Empty string on success, otherwise the reason.
     */
    std::string CreateDriveFolder(const std::string &mountPoint, const std::string &relative);

    /**
     * Moves @p sourcePath (a finished upload in the data folder) to
     * @p destination on a USB drive, creating its folders, using root.
     * @return Empty string on success, otherwise the reason.
     */
    std::string MoveWithRoot(const std::string &sourcePath, const std::string &destination);

    /**
     * Copies @p source to @p destination (between the TV and a drive, or two
     * drives): directly, reporting the bytes copied so far to @p onProgress,
     * when the app may read the one and write the other's folder, else with
     * root. @return Empty string on success, otherwise the reason.
     */
    std::string CopyFileTo(const std::string &source, const std::string &destination,
                           const std::function<void(std::uint64_t)> &onProgress);

    /** Whether @p path is a mount point (spots the app's own bind mounts). */
    static bool IsMountPoint(const std::string &path);

    /** Removes the file at @p path, directly or with root. @return True if it is gone. */
    bool RemoveFile(const std::string &path);

    /** Removes the folder at @p path and everything in it, directly or with root. @return True if it is gone. */
    bool RemoveFolder(const std::string &path);

    /** Renames @p from to @p to (same file system), directly or with root. @return True on success. */
    bool RenameFile(const std::string &from, const std::string &to);

    /**
     * Reads the file at @p path on a drive, refusing more than @p maxBytes:
     * directly, or, when the app cannot read the drive, from a copy root makes
     * in @p tempDir (removed afterwards).
     */
    bool ReadDriveFile(const std::string &path, bool readable, std::uint64_t maxBytes, const std::string &tempDir,
                       std::vector<std::uint8_t> *out);

    /**
     * Runs @p command as root (sh -c). Requires Root().Available().
     * @return True if it exited successfully; @p output receives stdout.
     */
    bool RunAsRoot(const std::string &command, std::string *output = nullptr);

    /** Quotes @p text for a POSIX shell (single quotes). */
    static std::string ShellQuote(const std::string &text);

private:
    /** Mount points that look like USB drives, from /proc/mounts (or root's view). */
    std::vector<std::string> UsbMountPoints();

    /** Fills the game file fields of @p drive. */
    void InspectDrive(UsbDrive *drive);

    /** The entries of the folder @p path on a drive, read directly or with root; sorted by name. */
    std::vector<ModEntry> ListDriveFolder(const std::string &path, bool readable);

    /** Looks for the ROM on @p drives (see StartRomSearch) and copies it. @return True if one was copied. */
    bool ImportRom(const Storage &storage, const std::vector<UsbDrive> &drives);

    /** The ROM-sized files with "mario" in their name in a drive's RetroArch folders. */
    std::vector<std::string> RetroArchCandidates(const UsbDrive &drive);

    /** Removes the usb-* folders, links and bind mounts in the mods folder. */
    void ReleaseModLinks(const Storage &storage);

    mutable std::mutex mutex_;
    RootStatus root_;
    std::vector<std::string> modDrives_;
    std::string romImportedFrom_;
    std::thread searchThread_;
    std::mutex searchMutex_;              ///< Guards starting searchThread_.
    std::atomic<bool> searching_{ false };
    std::atomic<bool> stopSearch_{ false };
    /** Serialises PlaceContent() and ROM imports (launch, the search thread, the page's threads). */
    std::recursive_mutex placeMutex_;
};

} // namespace wm64

#endif // WM64_USB_ACCESS_H
