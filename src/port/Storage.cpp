/**
 * @file Storage.cpp
 * @brief Implementation of the data folder helpers (see Storage.h).
 */
#include "Storage.h"

#include "Log.h"
#include "RomFile.h"
#include "SaveBackup.h"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace wm64
{
namespace fs = std::filesystem;

namespace
{

/** Used when no persistent folder can be created (nothing then survives a restart). */
constexpr const char *kVolatileFallback = "/tmp/web-mario-64";
/** The text file Ghostship drops into a new mods folder. */
constexpr const char *kModsPlaceholder = "custom_mod_files_go_here.txt";

/** Creates @p dir (and its parents) and checks the app can write there. */
bool EnsureWritableDirectory(const fs::path &dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec))
        return false;
    return access(dir.c_str(), W_OK) == 0;
}

/** Modification time of @p path in seconds since the Unix epoch, or 0. */
std::int64_t ModificationTime(const fs::path &path)
{
    struct stat info {};
    if (stat(path.c_str(), &info) != 0)
        return 0;
    return static_cast<std::int64_t>(info.st_mtime);
}

} // namespace

bool EndsWithNoCase(const std::string &name, const std::string &suffix)
{
    if (name.size() < suffix.size())
        return false;
    return std::equal(suffix.rbegin(), suffix.rend(), name.rbegin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string HumanBytes(std::uint64_t bytes)
{
    constexpr const char *kUnits[] = { "B", "KB", "MB", "GB", "TB" };
    double value     = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
    return buffer;
}

Storage Storage::Resolve()
{
    if (const char *overrideDir = std::getenv("WM64_DATA_DIR")) {
        if (*overrideDir && EnsureWritableDirectory(overrideDir))
            return Storage(overrideDir, true);
    }

    if (const char *home = std::getenv("HOME")) {
        // On webOS HOME is the app's private, persistent data folder. On a
        // desktop it is the user's home, so use a dedicated sub-folder.
#if defined(WM64_TARGET_WEBOS)
        const fs::path dir = fs::path(home) / "data";
#else
        const fs::path dir = fs::path(home) / ".local" / "share" / "WebMario64";
#endif
        if (*home && EnsureWritableDirectory(dir))
            return Storage(dir.string(), true);
    }

    EnsureWritableDirectory(kVolatileFallback);
    return Storage(kVolatileFallback, false);
}

Storage::Storage(std::string root, bool persistent) : root_(std::move(root)), persistent_(persistent)
{
    while (root_.size() > 1 && root_.back() == '/')
        root_.pop_back();
}

std::string Storage::PathFor(const std::string &relative) const { return root_ + "/" + relative; }

void Storage::CreateFolders() const
{
    std::error_code ec;
    fs::create_directories(ModsDir(), ec);
    fs::create_directories(SavesDir(), ec);
    fs::create_directories(ShipDir() + "/logs", ec);
    fs::create_directories(PathFor("rom"), ec);
    fs::create_directories(IncomingDir(), ec);
}

bool Storage::HasRom() const { return FileSize(RomPath()) == RomFile::kRomBytes; }

std::uint64_t Storage::FreeBytes(const std::string &path) const
{
    struct statvfs info {};
    if (statvfs((path.empty() ? root_ : path).c_str(), &info) != 0)
        return 0;
    return static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
}

std::uint64_t Storage::FileSize(const std::string &path)
{
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return 0;
    return static_cast<std::uint64_t>(info.st_size);
}

bool Storage::IsPlainFile(const std::string &path)
{
    struct stat info {};
    return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool Storage::IsModArchiveName(const std::string &name)
{
    return EndsWithNoCase(name, ".o2r") || EndsWithNoCase(name, ".otr");
}

bool Storage::IsAllowedModName(const std::string &name)
{
    if (name.size() <= 4 || name.size() > 200 || name.front() == '.' || name.front() == '~' || !IsModArchiveName(name))
        return false;
    // No folders, no control characters, nothing FAT/exFAT drives reject.
    return std::none_of(name.begin(), name.end(), [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7F || std::strchr("/\\:*?\"<>|", c) != nullptr;
    });
}

std::vector<ModEntry> Storage::ListMods(const std::string &dir)
{
    std::vector<ModEntry> mods;
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        // Hidden files, Ghostship's own folders (~romhacks), our USB folders and its placeholder.
        if (name.empty() || name.front() == '.' || name.front() == '~' || name.rfind("usb-", 0) == 0 || name == kModsPlaceholder)
            continue;
        if (entry.is_directory(ec))
            mods.push_back({ name, 0, true });
        else if (IsModArchiveName(name))
            mods.push_back({ name, FileSize(entry.path().string()), false });
    }
    std::sort(mods.begin(), mods.end(), [](const ModEntry &a, const ModEntry &b) { return a.name < b.name; });
    return mods;
}

std::vector<SaveFile> Storage::ListSaves() const
{
    std::vector<SaveFile> saves;
    const fs::path base(ShipDir());
    for (const std::string &relative : SaveBackup::CollectFiles(ShipDir())) {
        const fs::path path = base / relative;
        saves.push_back({ relative, FileSize(path.string()), ModificationTime(path) });
    }
    return saves;
}

bool Storage::ApplyPendingBackup() const
{
    const std::string pending = PendingBackupPath();
    if (FileSize(pending) == 0)
        return false;
    const std::string error = SaveBackup::Restore(pending, ShipDir());
    std::error_code ec;
    fs::remove(pending, ec);
    Log::Printf("pending save backup: %s", error.empty() ? "restored" : error.c_str());
    return error.empty();
}

} // namespace wm64
