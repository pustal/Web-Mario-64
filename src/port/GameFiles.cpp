/**
 * @file GameFiles.cpp
 * @brief Implementation of the game files' place on the TV or a USB drive
 *        (see GameFiles.h).
 */
#include "GameFiles.h"

#include "Log.h"
#include "RomFile.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace wm64
{
namespace fs = std::filesystem;

namespace
{

/** Where a ROM read from a drive waits while the archive is made from it. */
constexpr const char *kExtractionRomName = "extract-rom.z64";
/** Where an uploaded ROM is checked before it goes to a drive. */
constexpr const char *kUploadRomName = "upload-rom.z64";
/** Where the archive is made when root has to move it onto the drive. */
constexpr const char *kBuildFolderName = "archive-build";

/** The drive mounted at @p mountPoint among @p drives, or null. */
const UsbDrive *FindIn(const std::vector<UsbDrive> &drives, const std::string &mountPoint)
{
    const auto found = std::find_if(drives.begin(), drives.end(),
                                    [&mountPoint](const UsbDrive &drive) { return drive.mountPoint == mountPoint; });
    return found == drives.end() ? nullptr : &*found;
}

} // namespace

GameFiles::GameFiles(const Storage &storage, UsbAccess &usb)
    : storage_(storage), usb_(usb)
{
}

std::string GameFiles::DriveFolder(const std::string &mountPoint) { return mountPoint + "/" + UsbAccess::kDriveFolder; }

bool GameFiles::ArchiveIsOnTv() const
{
    const std::string path = storage_.ArchivePath();
    return Storage::IsPlainFile(path) && Storage::FileSize(path) > 0 && !UsbAccess::IsMountPoint(path);
}

GameFilesState GameFiles::State() { return StateFor(usb_.ListDrives()); }

GameFilesState GameFiles::StateFor(const std::vector<UsbDrive> &drives)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const bool root = usb_.Root().Available();
    GameFilesState state;

    // The ROM: the TV's first, then the first drive's game folder holding one.
    if (storage_.HasRom()) {
        state.rom.present = true;
        state.rom.path    = storage_.RomPath();
        state.rom.bytes   = RomFile::kRomBytes;
    }
    else {
        for (const UsbDrive &drive : drives) {
            if (drive.romFile.empty() || !(drive.readable || root))
                continue;
            state.rom.present    = true;
            state.rom.drive      = drive.mountPoint;
            state.rom.driveLabel = drive.label;
            state.rom.path       = DriveFolder(drive.mountPoint) + "/" + drive.romFile;
            state.rom.readable   = drive.readable;
            state.rom.bytes      = RomFile::kRomBytes;
            break;
        }
    }

    // The archive: the TV's own first, then the first drive's (Place() links that one).
    if (ArchiveIsOnTv()) {
        state.archive.present = true;
        state.archive.path    = storage_.ArchivePath();
        state.archive.bytes   = Storage::FileSize(state.archive.path);
    }
    else {
        for (const UsbDrive &drive : drives) {
            if (drive.archiveBytes == 0 || !(drive.readable || root))
                continue;
            state.archive.present    = true;
            state.archive.drive      = drive.mountPoint;
            state.archive.driveLabel = drive.label;
            state.archive.path       = DriveFolder(drive.mountPoint) + "/" + UsbAccess::kDriveArchiveName;
            state.archive.readable   = drive.readable;
            state.archive.bytes      = drive.archiveBytes;
            break;
        }
    }
    last_ = state;
    return state;
}

GameFilesState GameFiles::LastState()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return last_;
}

bool GameFiles::HasRom() { return State().rom.present; }

std::string GameFiles::RomKey()
{
    const GameFilePlace rom = State().rom;
    if (!rom.present)
        return {};
    struct stat info {};
    const std::string time = rom.readable && stat(rom.path.c_str(), &info) == 0 ? std::to_string(info.st_mtime) : std::string();
    return rom.path + ":" + std::to_string(rom.bytes) + ":" + time;
}

void GameFiles::ReleaseLink()
{
    const std::string path = storage_.ArchivePath();
    // Lazy: detaches the bind mount even while the game still reads through it.
    if (UsbAccess::IsMountPoint(path))
        usb_.RunAsRoot("umount -l " + UsbAccess::ShellQuote(path));
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (fs::is_symlink(status))
        fs::remove(path, ec);
    // The empty placeholder a bind mount covered (the TV's own archive is never empty).
    else if (fs::is_regular_file(status) && Storage::FileSize(path) == 0 && !UsbAccess::IsMountPoint(path))
        fs::remove(path, ec);
}

bool GameFiles::Link(const UsbDrive &drive)
{
    const std::string source = DriveFolder(drive.mountPoint) + "/" + UsbAccess::kDriveArchiveName;
    const std::string path   = storage_.ArchivePath();
    std::error_code ec;
    if (drive.readable) {
        fs::create_symlink(source, path, ec);
        if (!ec && access(path.c_str(), R_OK) == 0)
            return true;
        fs::remove(path, ec);
        return false;
    }
    if (!usb_.Root().Available())
        return false;
    // The app cannot read the drive: root shows the file at ship/sm64.o2r.
    std::ofstream(path).close();
    const bool mounted = usb_.RunAsRoot("mount --bind " + UsbAccess::ShellQuote(source) + " " + UsbAccess::ShellQuote(path));
    if (mounted && access(path.c_str(), R_OK) == 0 && Storage::FileSize(path) > 0)
        return true;
    if (mounted)
        usb_.RunAsRoot("umount " + UsbAccess::ShellQuote(path));
    fs::remove(path, ec);
    return false;
}

void GameFiles::Place()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (ArchiveIsOnTv()) {
        StateFor(usb_.ListDrives()); // keeps LastState() current
        return;
    }
    ReleaseLink();
    const std::vector<UsbDrive> drives = usb_.ListDrives();
    StateFor(drives); // keeps LastState() current
    for (const UsbDrive &drive : drives) {
        if (drive.archiveBytes == 0)
            continue;
        if (Link(drive)) {
            Log::Printf("game archive: on USB %s (%s)", drive.label.c_str(), HumanBytes(drive.archiveBytes).c_str());
            return;
        }
        Log::Printf("game archive: on USB %s, but the game cannot read it there", drive.label.c_str());
    }
}

bool GameFiles::ReadRom(const GameFilePlace &place, std::vector<std::uint8_t> *z64, std::string *error)
{
    if (place.drive.empty())
        return !RomFile::Load(place.path, z64, error).empty();
    if (!usb_.ReadDriveFile(place.path, place.readable, RomFile::kRomBytes, storage_.IncomingDir(), z64)) {
        *error = "The ROM on USB " + place.driveLabel + " could not be read.";
        return false;
    }
    if (!RomFile::ToZ64(*z64) || RomFile::Identify(*z64).empty()) {
        *error = "The ROM on USB " + place.driveLabel + " is not a supported Super Mario 64 ROM.";
        return false;
    }
    return true;
}

bool GameFiles::PrepareExtraction(ExtractionPlan *plan, std::string *error)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::vector<UsbDrive> drives = usb_.ListDrives();
    const GameFilesState state         = StateFor(drives);
    *plan                              = ExtractionPlan{};
    if (!state.rom.present) {
        *error = "There is no ROM to make the game from.";
        return false;
    }
    std::error_code ec;
    fs::create_directories(storage_.IncomingDir(), ec);

    // The archive is made where the ROM is: the TV's archive (or link) goes first.
    ReleaseLink();
    if (state.rom.drive.empty()) {
        if (storage_.FreeBytes() < Storage::kArchiveSpaceBytes + Storage::kSafetyMarginBytes) {
            *error = "Not enough space on the TV to make the game's archive (" + HumanBytes(Storage::kArchiveSpaceBytes)
                     + " needed). Move the ROM to a USB drive, or free some space.";
            return false;
        }
        usb_.RemoveFile(storage_.ArchivePath());
        plan->romPath   = storage_.RomPath();
        plan->outputDir = storage_.ShipDir();
        return true;
    }

    const UsbDrive *drive = FindIn(drives, state.rom.drive);
    if (!drive) {
        *error = "The USB drive with the ROM is gone.";
        return false;
    }
    if (!drive->writable && !usb_.Root().Available()) {
        *error = "The app cannot write to USB " + drive->label + " and has no root access: move the ROM to the TV.";
        return false;
    }
    if (!drive->spaceKnown || drive->freeBytes < Storage::kArchiveSpaceBytes) {
        *error = "Not enough space on USB " + drive->label + " to make the game's archive (" + HumanBytes(Storage::kArchiveSpaceBytes)
                 + " needed).";
        return false;
    }

    // The drive's ROM, converted and checked, waits on the TV for the extractor.
    std::vector<std::uint8_t> z64;
    if (!ReadRom(state.rom, &z64, error))
        return false;
    const std::string romCopy = storage_.IncomingDir() + "/" + kExtractionRomName;
    if (RomFile::Install(std::move(z64), romCopy, error).empty())
        return false;
    plan->romPath      = romCopy;
    plan->temporaryRom = true;
    plan->drive        = drive->mountPoint;
    plan->driveLabel   = drive->label;

    // The TV's own archive would win over the drive's: it goes.
    if (ArchiveIsOnTv())
        usb_.RemoveFile(storage_.ArchivePath());
    if (drive->writable) {
        plan->outputDir = DriveFolder(drive->mountPoint);
        usb_.RemoveFile(plan->outputDir + "/" + UsbAccess::kDriveArchiveName);
    }
    else {
        // Made on the TV, then moved by root (FinishExtraction).
        if (storage_.FreeBytes() < Storage::kArchiveSpaceBytes + Storage::kSafetyMarginBytes) {
            fs::remove(romCopy, ec);
            *error = "Not enough space on the TV to make the archive before root moves it to USB " + drive->label + ".";
            return false;
        }
        plan->outputDir    = storage_.IncomingDir() + "/" + kBuildFolderName;
        plan->moveWithRoot = true;
        fs::remove_all(plan->outputDir, ec);
        fs::create_directories(plan->outputDir, ec);
    }
    const std::string folderError = usb_.CreateDriveFolder(drive->mountPoint, UsbAccess::kDriveFolder);
    if (!folderError.empty()) {
        FinishExtraction(*plan, false);
        *error = folderError;
        return false;
    }
    Log::Printf("the archive is made on USB %s%s", drive->label.c_str(), plan->moveWithRoot ? " (moved there by root)" : "");
    return true;
}

std::string GameFiles::FinishExtraction(const ExtractionPlan &plan, bool succeeded)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::error_code ec;
    if (plan.temporaryRom)
        fs::remove(plan.romPath, ec);
    std::string error;
    if (succeeded && plan.moveWithRoot) {
        error = usb_.MoveWithRoot(plan.outputDir + "/" + UsbAccess::kDriveArchiveName,
                                  DriveFolder(plan.drive) + "/" + UsbAccess::kDriveArchiveName);
    }
    if (plan.moveWithRoot)
        fs::remove_all(plan.outputDir, ec);
    if (!succeeded || !error.empty())
        return error;
    Place();
    if (!storage_.HasArchive())
        return "The archive was made on " + (plan.drive.empty() ? std::string("the TV") : "USB " + plan.driveLabel)
               + ", but the game cannot read it there.";
    return {};
}

std::string GameFiles::StoreRom(std::vector<std::uint8_t> data, const std::string &drive, std::string *name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string error;
    if (drive.empty()) {
        *name = RomFile::Install(std::move(data), storage_.RomPath(), &error);
        return name->empty() ? error : std::string();
    }

    UsbDrive target;
    if (!usb_.FindDrive(drive, &target))
        return "That USB drive is not connected.";
    if (!target.writable && !usb_.Root().Available())
        return "The app cannot write to USB " + target.label + " and has no root access.";

    // Checked (and converted) on the TV first.
    std::error_code ec;
    fs::create_directories(storage_.IncomingDir(), ec);
    const std::string checked = storage_.IncomingDir() + "/" + kUploadRomName;
    *name                     = RomFile::Install(std::move(data), checked, &error);
    if (name->empty())
        return error;

    error = usb_.CreateDriveFolder(drive, UsbAccess::kDriveFolder);
    const std::string destination = DriveFolder(drive) + "/" + UsbAccess::kDriveRomName;
    if (error.empty() && target.writable) {
        error = usb_.CopyFileTo(checked, destination + ".part", nullptr);
        if (error.empty() && !usb_.RenameFile(destination + ".part", destination))
            error = "Could not put the ROM on USB " + target.label + ".";
        if (!error.empty())
            usb_.RemoveFile(destination + ".part");
    }
    else if (error.empty()) {
        error = usb_.MoveWithRoot(checked, destination);
    }
    usb_.RemoveFile(checked);
    if (!error.empty())
        return error;

    // The TV's ROM would be used first: the new one replaces it.
    if (storage_.HasRom())
        usb_.RemoveFile(storage_.RomPath());
    Log::Printf("ROM stored on USB %s", target.label.c_str());
    return {};
}

std::string GameFiles::RemoveRom()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const GameFilePlace rom = State().rom;
    if (!rom.present)
        return "There is no ROM to remove.";
    return usb_.RemoveFile(rom.path) ? std::string() : "Could not remove the ROM from " + rom.Describe() + ".";
}

std::string GameFiles::MoveTo(const std::string &drive, const std::function<void(std::uint64_t, std::uint64_t)> &onProgress)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::vector<UsbDrive> drives = usb_.ListDrives();
    const GameFilesState state         = StateFor(drives);
    if (!state.rom.present && !state.archive.present)
        return "There are no game files to move.";

    const bool romMoves     = state.rom.present && state.rom.drive != drive;
    const bool archiveMoves = state.archive.present && state.archive.drive != drive;
    const std::string where = drive.empty() ? std::string("the TV") : "USB " + fs::path(drive).filename().string();
    if (!romMoves && !archiveMoves)
        return "The game files are already on " + where + ".";
    const std::uint64_t total = (romMoves ? RomFile::kRomBytes : 0) + (archiveMoves ? state.archive.bytes : 0);
    std::uint64_t done        = 0;
    const auto progress       = [&](std::uint64_t copied) {
        if (onProgress)
            onProgress(done + copied, total);
    };
    std::error_code ec;
    std::string error;

    if (drive.empty()) {
        // --- To the TV.
        if (total + Storage::kSafetyMarginBytes > storage_.FreeBytes())
            return "Not enough space on the TV: the game files need " + HumanBytes(total + Storage::kSafetyMarginBytes)
                   + " (including a safety margin), " + HumanBytes(storage_.FreeBytes()) + " is free.";
        const std::string part = storage_.ArchivePath() + ".part";
        if (archiveMoves) {
            error = usb_.CopyFileTo(state.archive.path, part, progress);
            if (!error.empty()) {
                usb_.RemoveFile(part);
                return error;
            }
            done += state.archive.bytes;
        }
        if (romMoves) {
            std::vector<std::uint8_t> z64;
            if (!ReadRom(state.rom, &z64, &error) || RomFile::Install(std::move(z64), storage_.RomPath(), &error).empty()) {
                usb_.RemoveFile(part);
                return error;
            }
            done += RomFile::kRomBytes;
            progress(0);
        }
        if (archiveMoves) {
            ReleaseLink();
            fs::rename(part, storage_.ArchivePath(), ec);
            if (ec) {
                usb_.RemoveFile(part);
                Place();
                return "Could not put the archive in place on the TV: " + ec.message();
            }
            usb_.RemoveFile(state.archive.path);
        }
        if (romMoves)
            usb_.RemoveFile(state.rom.path);
        Place();
        Log::Printf("game files moved to the TV");
        return {};
    }

    // --- To a drive.
    const UsbDrive *target = FindIn(drives, drive);
    if (!target)
        return "That USB drive is not connected.";
    if (!target->writable && !usb_.Root().Available())
        return "The app cannot write to USB " + target->label + " and has no root access.";
    if (!target->spaceKnown)
        return "The free space on USB " + target->label + " could not be read.";
    if (total + Storage::kSafetyMarginBytes > target->freeBytes)
        return "Not enough space on USB " + target->label + ": the game files need " + HumanBytes(total + Storage::kSafetyMarginBytes)
               + " (including a safety margin), " + HumanBytes(target->freeBytes) + " is free.";
    error = usb_.CreateDriveFolder(drive, UsbAccess::kDriveFolder);
    if (!error.empty())
        return error;

    const std::string folder = DriveFolder(drive);
    // Copies with a .part name first, so an interrupted copy is never taken for the real file.
    const auto copyInto = [&](const GameFilePlace &source, const std::string &destination) -> std::string {
        std::string copyError = usb_.CopyFileTo(source.path, destination + ".part", progress);
        if (copyError.empty() && !usb_.RenameFile(destination + ".part", destination))
            copyError = "Could not put " + fs::path(destination).filename().string() + " in place on USB " + target->label + ".";
        if (!copyError.empty())
            usb_.RemoveFile(destination + ".part");
        return copyError;
    };
    if (archiveMoves) {
        error = copyInto(state.archive, folder + "/" + UsbAccess::kDriveArchiveName);
        if (!error.empty())
            return error;
        done += state.archive.bytes;
    }
    std::string romDestination;
    if (romMoves) {
        // The TV's ROM is in .z64 order already; a drive's keeps its name.
        romDestination = folder + "/" + (state.rom.drive.empty() ? std::string(UsbAccess::kDriveRomName)
                                                                 : fs::path(state.rom.path).filename().string());
        error = copyInto(state.rom, romDestination);
        if (!error.empty()) {
            if (archiveMoves)
                usb_.RemoveFile(folder + "/" + UsbAccess::kDriveArchiveName);
            return error;
        }
        done += RomFile::kRomBytes;
    }

    // The copies are in place: the originals go (the running game keeps the archive it opened).
    ReleaseLink();
    if (archiveMoves)
        usb_.RemoveFile(state.archive.path);
    if (romMoves)
        usb_.RemoveFile(state.rom.path);
    Place();
    Log::Printf("game files moved to USB %s", target->label.c_str());
    if (archiveMoves && !storage_.HasArchive())
        return "The game files are on USB " + target->label + " now, but the game cannot read the archive there.";
    return {};
}

} // namespace wm64
