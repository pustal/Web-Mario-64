/**
 * @file UsbAccess.cpp
 * @brief Implementation of root access, USB drive discovery, the ROM search
 *        and the drives' mods (see UsbAccess.h).
 */
#include "UsbAccess.h"

#include "Log.h"
#include "RomFile.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace wm64
{
namespace fs = std::filesystem;

namespace
{

/** Where webOS (and some other Linux setups) mount removable drives. */
constexpr std::array<const char *, 4> kUsbMountPrefixes = { "/tmp/usb/", "/media/usb", "/mnt/usb", "/run/media/" };
/** The Homebrew Channel's root command endpoint. */
constexpr const char *kHbChannelExec = "luna://org.webosbrew.hbchannel.service/exec";
/** luna-send-pub is the bus client apps may use; luna-send needs root. */
constexpr std::array<const char *, 2> kLunaClients = { "/usr/bin/luna-send-pub", "/usr/bin/luna-send" };
/** A drive's mods appear in the mods folder under this prefix plus the drive's label (ModLinkName). */
constexpr const char *kModLinkPrefix = "usb-";
/** The RetroArch system folders searched below "roms" (compared in lower case). */
constexpr std::array<const char *, 2> kRetroArchSystemFolders = { "n64", "nintendo64" };
/** Folder levels searched below a system folder (roms/n64/Mario/... is level 1). */
constexpr int kRetroArchDepth = 2;
/** Most entries visited in one system folder (big collections stay quick to scan). */
constexpr std::size_t kRetroArchScanLimit = 20000;
/** Most ROM-sized files read per drive while looking for a supported dump. */
constexpr std::size_t kMaxRomCandidates = 16;

/** Whether @p name (lower case) is one the game's ROM can be filed under: it has "mario" in it. */
bool LooksLikeMario(const std::string &name) { return name.find("mario") != std::string::npos; }

/** Runs @p command with /bin/sh and captures stdout. @return The exit status, or -1. */
int RunShell(const std::string &command, std::string *output)
{
    std::FILE *pipe = popen(command.c_str(), "r");
    if (!pipe)
        return -1;
    std::string captured;
    std::array<char, 4096> buffer{};
    std::size_t length = 0;
    while ((length = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0)
        captured.append(buffer.data(), length);
    const int status = pclose(pipe);
    if (output)
        *output = std::move(captured);
    return status;
}

/** Escapes @p text as a JSON string body. */
std::string JsonEscape(const std::string &text)
{
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                    out += hex;
                }
                else {
                    out += c;
                }
        }
    }
    return out;
}

/**
 * Extracts the string value of "@p key" from a flat JSON reply (luna-send
 * output). Handles the escapes JSON strings use for shell output.
 * @return False if the key is missing.
 */
bool JsonStringField(const std::string &json, const std::string &key, std::string *value)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t at           = json.find(needle);
    if (at == std::string::npos)
        return false;
    at = json.find('"', json.find(':', at + needle.size()));
    if (at == std::string::npos)
        return false;

    std::string out;
    for (std::size_t i = at + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"')
            break;
        if (c != '\\' || i + 1 >= json.size()) {
            out += c;
            continue;
        }
        const char escaped = json[++i];
        switch (escaped) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'u':
                if (i + 4 < json.size()) {
                    out += static_cast<char>(std::strtol(json.substr(i + 1, 4).c_str(), nullptr, 16));
                    i += 4;
                }
                break;
            default: out += escaped; break;
        }
    }
    *value = std::move(out);
    return true;
}

/** Decodes the octal escapes /proc/mounts uses in paths ("\040" = space). */
std::string DecodeMountPath(const std::string &raw)
{
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 3 < raw.size()) {
            const std::string digits = raw.substr(i + 1, 3);
            if (std::all_of(digits.begin(), digits.end(), [](char d) { return d >= '0' && d <= '7'; })) {
                out += static_cast<char>(std::strtol(digits.c_str(), nullptr, 8));
                i += 3;
                continue;
            }
        }
        out += raw[i];
    }
    return out;
}

bool IsUsbMountPoint(const std::string &path)
{
    return std::any_of(kUsbMountPrefixes.begin(), kUsbMountPrefixes.end(),
                       [&](const char *prefix) { return path.rfind(prefix, 0) == 0; });
}

/** Mount points (field 2) of the /proc/mounts text @p mounts that look like USB drives. */
std::vector<std::string> ParseUsbMounts(const std::string &mounts)
{
    std::vector<std::string> found;
    std::istringstream lines(mounts);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string device, mountPoint;
        if (!(fields >> device >> mountPoint))
            continue;
        const std::string decoded = DecodeMountPath(mountPoint);
        if (IsUsbMountPoint(decoded) && std::find(found.begin(), found.end(), decoded) == found.end())
            found.push_back(decoded);
    }
    return found;
}

/** Whether the app can create (and remove) a file in @p dir. */
bool CanWrite(const std::string &dir)
{
    const std::string probe = dir + "/.webmario64_write_test";
    const int fd            = open(probe.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    close(fd);
    unlink(probe.c_str());
    return true;
}

/** Whether the app can list @p dir. */
bool CanRead(const std::string &dir)
{
    DIR *handle = opendir(dir.c_str());
    if (!handle)
        return false;
    closedir(handle);
    return true;
}

/** True if @p path is a mount point in /proc/mounts (used to spot our own bind mounts). */
bool IsMountedAt(const std::string &path)
{
    std::ifstream mounts("/proc/mounts");
    std::string device, mountPoint, rest;
    while (mounts >> device >> mountPoint && std::getline(mounts, rest)) {
        if (DecodeMountPath(mountPoint) == path)
            return true;
    }
    return false;
}

} // namespace

std::string UsbAccess::ModLinkName(const std::string &label)
{
    // Only characters that are safe in a folder name ("sda1").
    std::string safe;
    for (const char c : label)
        safe += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
    return kModLinkPrefix + (safe.empty() ? std::string("drive") : safe);
}

std::string UsbAccess::ShellQuote(const std::string &text)
{
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

void UsbAccess::ProbeRoot()
{
    RootStatus status;
    status.runningAsRoot = geteuid() == 0;
    if (status.runningAsRoot) {
        status.detail = "The app runs as root.";
    }
    else {
        // Ask the Homebrew Channel to run "id -u" as root.
        std::string luna;
        for (const char *client : kLunaClients) {
            if (access(client, X_OK) == 0) {
                luna = client;
                break;
            }
        }
        if (luna.empty()) {
            status.detail = "No luna-send client available to this app.";
        }
        else {
            const std::string payload = "{\"command\":\"id -u\"}";
            std::string reply;
            RunShell(luna + " -n 1 -w 5000 " + kHbChannelExec + " " + ShellQuote(payload) + " 2>&1", &reply);
            std::string stdoutText;
            if (JsonStringField(reply, "stdoutString", &stdoutText) && stdoutText.rfind("0", 0) == 0) {
                status.homebrewChannel = true;
                status.detail          = "Root commands run through the Homebrew Channel.";
            }
            else {
                std::string error;
                if (!JsonStringField(reply, "errorText", &error))
                    error = reply.substr(0, 160);
                while (!error.empty() && (error.back() == '\n' || error.back() == ' '))
                    error.pop_back();
                status.detail = "Homebrew Channel root commands unavailable" + (error.empty() ? std::string(".") : ": " + error);
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    root_ = status;
    Log::Printf("root: %s", status.detail.c_str());
}

RootStatus UsbAccess::Root() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_;
}

bool UsbAccess::RunAsRoot(const std::string &command, std::string *output)
{
    const RootStatus status = Root();
    if (status.runningAsRoot)
        return RunShell("sh -c " + ShellQuote(command), output) == 0;
    if (!status.homebrewChannel)
        return false;

    std::string luna;
    for (const char *client : kLunaClients) {
        if (access(client, X_OK) == 0) {
            luna = client;
            break;
        }
    }
    // The exec method reports success through returnValue; append a marker to
    // learn the command's own exit status as well.
    const std::string wrapped = "(" + command + ") && echo __WM64_OK__";
    const std::string payload = "{\"command\":\"" + JsonEscape(wrapped) + "\"}";
    std::string reply;
    RunShell(luna + " -n 1 -w 60000 " + kHbChannelExec + " " + ShellQuote(payload) + " 2>&1", &reply);

    std::string stdoutText;
    if (!JsonStringField(reply, "stdoutString", &stdoutText))
        return false;
    const std::size_t marker = stdoutText.rfind("__WM64_OK__");
    if (output)
        *output = stdoutText.substr(0, marker == std::string::npos ? stdoutText.size() : marker);
    return marker != std::string::npos;
}

std::vector<std::string> UsbAccess::UsbMountPoints()
{
    // Stand-ins for drives, for tests on a desktop (no /tmp/usb there): each folder is one drive.
    if (const char *standIn = std::getenv("WM64_USB_ROOT")) {
        std::vector<std::string> found;
        std::error_code ec;
        for (const fs::directory_entry &entry : fs::directory_iterator(standIn, ec)) {
            if (entry.is_directory(ec))
                found.push_back(entry.path().string());
        }
        std::sort(found.begin(), found.end());
        return found;
    }

    std::string mounts;
    {
        std::ifstream file("/proc/mounts");
        std::ostringstream text;
        text << file.rdbuf();
        mounts = text.str();
    }
    std::vector<std::string> found = ParseUsbMounts(mounts);

    // Inside the jail /proc/mounts may not list the drives; root sees the real table.
    if (found.empty() && Root().Available()) {
        std::string rootMounts;
        if (RunAsRoot("cat /proc/mounts", &rootMounts))
            found = ParseUsbMounts(rootMounts);
    }
    return found;
}

void UsbAccess::InspectDrive(UsbDrive *drive)
{
    const bool root = Root().Available();
    struct statvfs info {};
    if (statvfs(drive->mountPoint.c_str(), &info) == 0 && info.f_blocks > 0) {
        drive->freeBytes  = static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
        drive->totalBytes = static_cast<std::uint64_t>(info.f_blocks) * info.f_frsize;
        drive->spaceKnown = true;
    }
    else if (root) {
        // df -k prints: Filesystem 1K-blocks Used Available Use% Mounted-on
        std::string df;
        if (RunAsRoot("df -k " + ShellQuote(drive->mountPoint) + " | tail -n 1", &df)) {
            std::istringstream fields(df);
            std::string filesystem;
            unsigned long long blocks = 0, used = 0, available = 0;
            if (fields >> filesystem >> blocks >> used >> available) {
                drive->totalBytes = blocks * 1024ull;
                drive->freeBytes  = available * 1024ull;
                drive->spaceKnown = true;
            }
        }
    }

    // The game's folder: the ROM (the one the app put there first, else the first
    // ROM-sized file with an N64 ROM extension) and the archive.
    const std::string folder = drive->mountPoint + "/" + kDriveFolder;
    for (const ModEntry &entry : ListDriveFolder(folder, drive->readable)) {
        if (entry.folder)
            continue;
        if (entry.size == RomFile::kRomBytes && RomFile::IsRomFileName(entry.name)
            && (drive->romFile.empty() || entry.name == kDriveRomName))
            drive->romFile = entry.name;
        else if (entry.name == kDriveArchiveName)
            drive->archiveBytes = entry.size;
    }

    const std::string modsDir = drive->mountPoint + "/" + kDriveModsFolder;
    if (drive->readable) {
        std::error_code ec;
        drive->modsFolder = fs::is_directory(modsDir, ec);
    }
    else {
        drive->modsFolder = root && RunAsRoot("test -d " + ShellQuote(modsDir));
    }
    if (drive->modsFolder) {
        for (const ModEntry &entry : ListDriveFolder(modsDir, drive->readable)) {
            if (entry.folder || Storage::IsModArchiveName(entry.name))
                drive->mods.push_back(entry);
        }
    }
}

std::vector<ModEntry> UsbAccess::ListDriveFolder(const std::string &path, bool readable)
{
    std::vector<ModEntry> entries;
    if (readable) {
        std::error_code ec;
        for (const fs::directory_entry &entry : fs::directory_iterator(path, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.empty() || name.front() == '.')
                continue;
            const bool folder = entry.is_directory(ec);
            entries.push_back({ name, folder ? 0 : Storage::FileSize(entry.path().string()), folder });
        }
    }
    else if (Root().Available()) {
        // One line per entry: "<size>|<type>|<name>", as busybox stat prints it.
        std::string listing;
        if (RunAsRoot("cd " + ShellQuote(path) + " && for f in *; do [ -e \"$f\" ] && stat -L -c '%s|%F|%n' \"$f\"; done", &listing)) {
            std::istringstream lines(listing);
            std::string line;
            while (std::getline(lines, line)) {
                const std::size_t first  = line.find('|');
                const std::size_t second = line.find('|', first + 1);
                if (first == std::string::npos || second == std::string::npos)
                    continue;
                const std::string name = line.substr(second + 1);
                const bool folder      = line.compare(first + 1, second - first - 1, "directory") == 0;
                if (!name.empty() && name.front() != '.')
                    entries.push_back({ name, folder ? 0 : std::strtoull(line.c_str(), nullptr, 10), folder });
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const ModEntry &a, const ModEntry &b) { return a.name < b.name; });
    return entries;
}

bool UsbAccess::ReadDriveFile(const std::string &path, bool readable, std::uint64_t maxBytes, const std::string &tempDir,
                              std::vector<std::uint8_t> *out)
{
    // A drive the app cannot read: root copies the file into the data folder first.
    std::string source = path;
    std::error_code ec;
    if (!readable) {
        if (!Root().Available())
            return false;
        fs::create_directories(tempDir, ec);
        source = tempDir + "/rom-read.tmp";
        if (!RunAsRoot("cp -f " + ShellQuote(path) + " " + ShellQuote(source) + " && chmod 644 " + ShellQuote(source))) {
            RemoveFile(source);
            return false;
        }
    }
    const std::uint64_t size = Storage::FileSize(source);
    bool ok                  = size > 0 && size <= maxBytes;
    if (ok) {
        out->resize(static_cast<std::size_t>(size));
        std::ifstream in(source, std::ios::binary);
        ok = static_cast<bool>(in.read(reinterpret_cast<char *>(out->data()), static_cast<std::streamsize>(out->size())));
    }
    if (!readable)
        RemoveFile(source);
    return ok;
}

std::vector<std::string> UsbAccess::RetroArchCandidates(const UsbDrive &drive)
{
    std::vector<std::string> files;
    if (drive.readable) {
        std::error_code ec;
        for (fs::directory_iterator top(drive.mountPoint, fs::directory_options::skip_permission_denied, ec), end;
             !ec && top != end; top.increment(ec)) {
            if (!top->is_directory(ec) || Lower(top->path().filename().string()) != "roms")
                continue;
            std::error_code systemError;
            for (fs::directory_iterator system(top->path(), fs::directory_options::skip_permission_denied, systemError), systemEnd;
                 !systemError && system != systemEnd; system.increment(systemError)) {
                const std::string systemName = Lower(system->path().filename().string());
                if (!system->is_directory(systemError)
                    || std::none_of(kRetroArchSystemFolders.begin(), kRetroArchSystemFolders.end(),
                                    [&](const char *folder) { return systemName == folder; }))
                    continue;
                std::size_t visited = 0;
                std::error_code walkError;
                for (fs::recursive_directory_iterator it(system->path(), fs::directory_options::skip_permission_denied, walkError), stop;
                     !walkError && it != stop && visited < kRetroArchScanLimit && !stopSearch_.load(); it.increment(walkError), ++visited) {
                    if (it.depth() >= kRetroArchDepth)
                        it.disable_recursion_pending();
                    const std::string name = it->path().filename().string();
                    if (!RomFile::IsRomFileName(name) || !LooksLikeMario(Lower(name)) || !it->is_regular_file(walkError))
                        continue;
                    // Only files of the ROM's size are worth reading.
                    if (it->file_size(walkError) == RomFile::kRomBytes && files.size() < kMaxRomCandidates)
                        files.push_back(it->path().string());
                }
            }
        }
    }
    else if (Root().Available()) {
        // The app cannot list the drive: ask root (busybox find has no -iname on every TV).
        std::string listing;
        const std::string command = "for r in " + ShellQuote(drive.mountPoint) + "/*; do "
                                    "[ -d \"$r\" ] && [ \"$(basename \"$r\" | tr 'A-Z' 'a-z')\" = roms ] || continue; "
                                    "for d in \"$r\"/*; do "
                                    "case \"$(basename \"$d\" | tr 'A-Z' 'a-z')\" in n64|nintendo64) ;; *) continue ;; esac; "
                                    "[ -d \"$d\" ] && find \"$d\" -maxdepth " + std::to_string(kRetroArchDepth + 1) +
                                    // Only files of the ROM's size (busybox find counts -size in kB with k).
                                    " -type f -size 8192k; done; done"
                                    " | grep -i -E '\\.(z64|n64|v64)$' | grep -i mario | head -n " + std::to_string(kMaxRomCandidates);
        if (RunAsRoot(command, &listing)) {
            std::istringstream lines(listing);
            std::string line;
            while (std::getline(lines, line)) {
                if (!line.empty())
                    files.push_back(line);
            }
        }
    }
    return files;
}

bool UsbAccess::ImportRom(const Storage &storage, const std::vector<UsbDrive> &drives)
{
    // A drive's game folder holds a ROM: that one is used, nothing is copied.
    if (std::any_of(drives.begin(), drives.end(), [](const UsbDrive &drive) { return !drive.romFile.empty(); }))
        return false;
    const std::string tempDir = storage.IncomingDir();
    for (const UsbDrive &drive : drives) {
        if (stopSearch_.load())
            return false;

        // Its RetroArch folders (a ROM in a drive's "Super Mario 64" folder is used where it is).
        std::vector<std::string> candidates;
        Log::Printf("looking for the ROM in roms/n64 and roms/nintendo64 on %s", drive.mountPoint.c_str());
        for (const std::string &path : RetroArchCandidates(drive))
            candidates.push_back(path);
        Log::Printf("%zu ROM-sized files to check on %s", candidates.size(), drive.label.c_str());

        for (const std::string &path : candidates) {
            if (stopSearch_.load())
                return false;
            std::vector<std::uint8_t> rom;
            if (!ReadDriveFile(path, drive.readable, RomFile::kRomBytes, tempDir, &rom))
                continue;
            // This runs alongside the setup screen and the transfer page: a ROM placed meanwhile wins.
            std::lock_guard<std::recursive_mutex> placing(placeMutex_);
            if (storage.HasRom())
                return false;
            std::string error;
            const std::string name = RomFile::Install(std::move(rom), storage.RomPath(), &error);
            if (name.empty()) {
                Log::Printf("%s: %s", path.c_str(), error.c_str());
                continue;
            }
            std::error_code ec;
            const std::string where = "USB " + drive.label + ": " + fs::relative(path, drive.mountPoint, ec).string();
            Log::Printf("%s copied from %s", name.c_str(), where.c_str());
            std::lock_guard<std::mutex> lock(mutex_);
            romImportedFrom_ = where;
            return true;
        }
    }
    return false;
}

UsbAccess::~UsbAccess()
{
    stopSearch_.store(true);
    if (searchThread_.joinable())
        searchThread_.join();
}

void UsbAccess::StartRomSearch(const Storage &storage)
{
    std::lock_guard<std::mutex> starting(searchMutex_);
    if (searching_.load() || storage.HasRom())
        return;
    if (searchThread_.joinable())
        searchThread_.join(); // the last search, finished
    searching_.store(true);
    const Storage target = storage;
    searchThread_ = std::thread([this, target] {
        ImportRom(target, ListDrives());
        searching_.store(false);
    });
}

bool UsbAccess::RomSearchRunning() const { return searching_.load(); }

std::vector<UsbDrive> UsbAccess::ListDrives()
{
    std::vector<UsbDrive> drives;
    for (const std::string &mountPoint : UsbMountPoints()) {
        UsbDrive drive;
        drive.mountPoint = mountPoint;
        drive.label      = fs::path(mountPoint).filename().string();
        drive.readable   = CanRead(mountPoint);
        drive.writable   = drive.readable && CanWrite(mountPoint);
        InspectDrive(&drive);
        drives.push_back(std::move(drive));
    }
    return drives;
}

bool UsbAccess::FindDrive(const std::string &mountPoint, UsbDrive *drive)
{
    if (!IsUsbMount(mountPoint))
        return false;
    *drive            = UsbDrive{};
    drive->mountPoint = mountPoint;
    drive->label      = fs::path(mountPoint).filename().string();
    drive->readable   = CanRead(mountPoint);
    drive->writable   = drive->readable && CanWrite(mountPoint);
    InspectDrive(drive);
    return true;
}

bool UsbAccess::IsUsbMount(const std::string &mountPoint)
{
    if (mountPoint.empty())
        return false;
    const std::vector<std::string> mounts = UsbMountPoints();
    return std::find(mounts.begin(), mounts.end(), mountPoint) != mounts.end();
}

std::string UsbAccess::RomImportedFrom() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return romImportedFrom_;
}

std::string UsbAccess::MountSignature()
{
    std::string signature;
    for (const std::string &mountPoint : UsbMountPoints())
        signature += mountPoint + "\n";
    return signature;
}

void UsbAccess::ReleaseModLinks(const Storage &storage)
{
    std::error_code ec;
    std::vector<fs::path> links;
    for (const fs::directory_entry &entry : fs::directory_iterator(storage.ModsDir(), ec)) {
        if (entry.path().filename().string().rfind(kModLinkPrefix, 0) == 0)
            links.push_back(entry.path());
    }
    for (const fs::path &link : links) {
        const std::string path = link.string();
        if (IsMountedAt(path)) {
            // Lazy: detaches it even while the game reads through it.
            RunAsRoot("umount -l " + ShellQuote(path));
            fs::remove(link, ec); // the empty folder it covered
        }
        else if (fs::is_symlink(link, ec)) {
            fs::remove(link, ec);
        }
        else if (fs::is_directory(link, ec)) {
            // A folder of links (never real files: those stay on the drive).
            bool onlyLinks = true;
            for (const fs::directory_entry &inside : fs::directory_iterator(link, ec))
                onlyLinks = onlyLinks && inside.is_symlink(ec);
            if (onlyLinks)
                fs::remove_all(link, ec);
        }
    }
}

void UsbAccess::PlaceContent(const Storage &storage)
{
    std::lock_guard<std::recursive_mutex> placing(placeMutex_);
    const std::vector<UsbDrive> drives = ListDrives();
    std::error_code ec;

    // Each drive's mods appear as mods/usb-<label>/.
    ReleaseModLinks(storage);
    std::vector<std::string> linked;
    for (const UsbDrive &drive : drives) {
        if (!drive.modsFolder || drive.mods.empty())
            continue;
        const std::string source = drive.mountPoint + "/" + kDriveModsFolder;
        const std::string local  = storage.ModsDir() + "/" + ModLinkName(drive.label);
        bool done                = false;
        if (drive.readable) {
            // A real folder of links: the game descends into folders of mods/, not into folder links.
            fs::create_directories(local, ec);
            for (const ModEntry &mod : drive.mods) {
                std::error_code linkError;
                if (mod.folder)
                    fs::create_directory_symlink(source + "/" + mod.name, local + "/" + mod.name, linkError);
                else
                    fs::create_symlink(source + "/" + mod.name, local + "/" + mod.name, linkError);
                done = done || !linkError;
            }
        }
        else if (Root().Available()) {
            fs::create_directories(local, ec);
            done = RunAsRoot("mount --bind " + ShellQuote(source) + " " + ShellQuote(local)) && CanRead(local);
            if (!done)
                RunAsRoot("umount " + ShellQuote(local));
        }
        if (done) {
            linked.push_back(drive.mountPoint);
            Log::Printf("mods: %s linked (%zu)", source.c_str(), drive.mods.size());
        }
        else {
            fs::remove_all(local, ec);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    modDrives_ = std::move(linked);
}

std::vector<std::string> UsbAccess::LinkedModDrives() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return modDrives_;
}

std::string UsbAccess::CreateDriveFolder(const std::string &mountPoint, const std::string &relative)
{
    const std::string folder = mountPoint + "/" + relative;
    std::error_code ec;
    if (CanWrite(mountPoint)) {
        fs::create_directories(folder, ec);
        return ec ? "Could not create the folder: " + ec.message() : std::string();
    }
    if (Root().Available())
        return RunAsRoot("mkdir -p " + ShellQuote(folder)) ? std::string() : "Root could not create the folder on the drive.";
    return "The app cannot write to this drive and has no root access.";
}

std::string UsbAccess::MoveWithRoot(const std::string &sourcePath, const std::string &destination)
{
    const std::string dir = fs::path(destination).parent_path().string();
    // mv across filesystems copies, then removes the source.
    if (RunAsRoot("mkdir -p " + ShellQuote(dir) + " && mv -f " + ShellQuote(sourcePath) + " " + ShellQuote(destination)))
        return {};
    return "Root could not move the file onto the drive.";
}

std::string UsbAccess::CopyFileTo(const std::string &source, const std::string &destination,
                                  const std::function<void(std::uint64_t)> &onProgress)
{
    const std::string folder = fs::path(destination).parent_path().string();
    std::FILE *in            = access(folder.c_str(), W_OK) == 0 ? std::fopen(source.c_str(), "rb") : nullptr;
    if (in) {
        std::FILE *out = std::fopen(destination.c_str(), "wb");
        if (!out) {
            std::fclose(in);
            return "Could not create " + destination + ".";
        }
        std::vector<char> buffer(1u << 20);
        std::uint64_t copied = 0;
        bool ok              = true;
        std::size_t length   = 0;
        while ((length = std::fread(buffer.data(), 1, buffer.size(), in)) > 0) {
            if (std::fwrite(buffer.data(), 1, length, out) != length) {
                ok = false; // disk full or I/O error
                break;
            }
            copied += length;
            if (onProgress)
                onProgress(copied);
        }
        ok = ok && !std::ferror(in) && std::fflush(out) == 0 && fsync(fileno(out)) == 0;
        std::fclose(in);
        std::fclose(out);
        return ok ? std::string() : "The copy could not be written (is the destination full?).";
    }
    if (!Root().Available())
        return "The app cannot read the file or write there, and has no root access.";
    // Readable by the app afterwards (the TV's own copies are read by the game).
    if (RunAsRoot("cp -f " + ShellQuote(source) + " " + ShellQuote(destination) + " && chmod 644 " + ShellQuote(destination)
                  + " && sync"))
        return {};
    return "Root could not copy the file there (is the destination full?).";
}

bool UsbAccess::IsMountPoint(const std::string &path) { return IsMountedAt(path); }

bool UsbAccess::RemoveFile(const std::string &path)
{
    std::error_code ec;
    if (fs::remove(path, ec) || (!ec && !fs::exists(fs::symlink_status(path, ec))))
        return true;
    return Root().Available() && RunAsRoot("rm -f " + ShellQuote(path));
}

bool UsbAccess::RemoveFolder(const std::string &path)
{
    std::error_code ec;
    fs::remove_all(path, ec);
    if (!ec && !fs::exists(fs::symlink_status(path, ec)))
        return true;
    return Root().Available() && RunAsRoot("rm -rf " + ShellQuote(path));
}

bool UsbAccess::RenameFile(const std::string &from, const std::string &to)
{
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec)
        return true;
    return Root().Available() && RunAsRoot("mv -f " + ShellQuote(from) + " " + ShellQuote(to));
}

} // namespace wm64
