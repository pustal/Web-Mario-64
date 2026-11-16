/**
 * @file TransferServer.cpp
 * @brief Implementation of the transfer server (see TransferServer.h).
 *
 * HTTP API (all /api routes need the PIN, as an "X-Pin" header or ?pin=).
 * "drive" is "internal" (the TV, the default) or a USB drive's mount point.
 *   GET    /                               the transfer page (web/index.html, embedded at build time)
 *   GET    /api/status                     JSON: ROM, archive, extraction, mods, drives, root, space, progress
 *   PUT    /api/rom?drive=                 raw body = a Super Mario 64 ROM (.z64, .n64 or .v64), kept on the TV
 *                                          or in a drive's game folder (the archive is then made there)
 *   DELETE /api/rom                        removes the ROM in use (the archive stays, so the game still runs)
 *   POST   /api/archive/rebuild            makes the archive again from the ROM at the next start
 *   POST   /api/gamefiles/move?drive=      moves the ROM and the archive to the TV or a drive
 *   PUT    /api/mods/:name?drive=          raw body = a mod (.o2r or .otr archive)
 *   POST   /api/mods/:name/move?from=&drive=   moves a mod between the TV and a drive
 *   DELETE /api/mods/:name?drive=          removes a mod
 *   GET    /api/saves                      JSON list of the save files
 *   GET    /api/saves/backup               the saves and settings as a zip file
 *   PUT    /api/saves/backup               raw body = a backup zip to restore
 *   GET    /api/log                        the game's log file
 *   GET    /api/portlog[?previous=1]       this port's log (port-log.txt; the last launch's with previous=1)
 *   POST   /api/restart                    quits the game and starts the app again
 */
#include "TransferServer.h"

#include "GameFiles.h"
#include "Log.h"
#include "MenuOverlay.h"
#include "NetInfo.h"
#include "RomFile.h"
#include "SaveBackup.h"
#include "Storage.h"
#include "UsbAccess.h"
#include "WebPage.generated.h"

// Four workers: the page polls the status while an upload runs.
#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include <httplib.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>

namespace wm64
{
namespace fs = std::filesystem;

namespace
{

constexpr const char *kJsonType  = "application/json";
constexpr const char *kTextType  = "text/plain; charset=utf-8";
constexpr const char *kPinHeader = "X-Pin";
constexpr const char *kPinFile   = "transfer_pin.txt";
constexpr const char *kInternal  = "internal";
constexpr int kPortAttempts      = 10;
/** The log can grow large after long sessions; the page gets its end. */
constexpr std::uint64_t kMaxLogBytes = 2ull * 1024ull * 1024ull;

/** Escapes @p text for use inside a JSON string literal. */
std::string JsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else {
                    out += c;
                }
        }
    }
    return out;
}

const char *JsonBool(bool value) { return value ? "true" : "false"; }

/** Sends a plain-text error. */
void Fail(httplib::Response &res, int status, const std::string &message)
{
    res.status = status;
    res.set_content(message, kTextType);
}

/** Reads the announced body size. @return False if the header is missing or malformed. */
bool ContentLength(const httplib::Request &req, std::uint64_t *out)
{
    const std::string value = req.get_header_value("Content-Length");
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
        return false;
    try {
        *out = static_cast<std::uint64_t>(std::stoull(value));
        return true;
    }
    catch (const std::exception &) {
        return false;
    }
}

/**
 * Streams the request body into @p path, refusing more than @p expected
 * bytes, and flushes it to storage.
 * @return Bytes written, or -1 on error.
 */
std::int64_t StreamBodyToFile(const httplib::ContentReader &reader, const std::string &path, std::uint64_t expected,
                              const std::function<void(std::uint64_t)> &onProgress)
{
    std::FILE *out = std::fopen(path.c_str(), "wb");
    if (!out)
        return -1;

    std::uint64_t written = 0;
    const bool completed  = reader([&](const char *data, std::size_t length) {
        if (written + length > expected)
            return false; // more than announced
        if (std::fwrite(data, 1, length, out) != length)
            return false; // disk full or I/O error
        written += length;
        onProgress(written);
        return true;
    });

    const bool flushed = std::fflush(out) == 0 && fsync(fileno(out)) == 0;
    std::fclose(out);
    if (!completed || !flushed)
        return -1;
    return static_cast<std::int64_t>(written);
}

/** Clears a "busy" flag when the request handler returns, however it returns. */
class BusyGuard
{
public:
    explicit BusyGuard(std::atomic<bool> &flag) : flag_(flag) {}
    ~BusyGuard() { flag_.store(false); }
    BusyGuard(const BusyGuard &)            = delete;
    BusyGuard &operator=(const BusyGuard &) = delete;

private:
    std::atomic<bool> &flag_;
};

/** Where an upload or move goes: the TV, or a USB drive. */
struct Target {
    bool usb = false;
    UsbDrive drive;          ///< The drive, for a USB target.
    std::string folder;      ///< Absolute folder the file goes in.
    std::uint64_t free = 0;  ///< Free bytes there.
    bool direct = true;      ///< The app writes there itself (else root moves it there).
};

/** The mods folder on a drive. */
std::string DriveModsFolder(const std::string &mountPoint) { return mountPoint + "/" + UsbAccess::kDriveModsFolder; }

/**
 * Reads the "drive" value @p drive (a USB mount point, or "internal") into
 * @p target, the mods folder there. Answers @p res and returns false when it is not usable.
 */
bool ParseTarget(const std::string &drive, Storage &storage, UsbAccess &usb, Target *target, httplib::Response &res)
{
    if (drive.empty() || drive == kInternal) {
        target->folder = storage.ModsDir();
        target->free   = storage.FreeBytes();
        return true;
    }
    if (!usb.FindDrive(drive, &target->drive)) {
        Fail(res, 400, "That USB drive is not connected.");
        return false;
    }
    target->usb    = true;
    target->folder = DriveModsFolder(drive);
    target->direct = target->drive.writable;
    target->free   = target->drive.spaceKnown ? target->drive.freeBytes : 0;
    if (!target->drive.writable && !usb.Root().Available()) {
        Fail(res, 403, "The app cannot write to this USB drive and has no root access.");
        return false;
    }
    if (!target->drive.spaceKnown) {
        Fail(res, 507, "The free space on this USB drive could not be read.");
        return false;
    }
    return true;
}


/** Whether the archive at @p path starts like one Ghostship reads: a zip (.o2r) or an MPQ (.otr). */
bool LooksLikeModArchive(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    if (!in.read(magic, sizeof(magic)))
        return false;
    return std::memcmp(magic, "PK\x03\x04", 4) == 0 || std::memcmp(magic, "MPQ\x1a", 4) == 0;
}

/** The JSON array of @p mods. */
std::string ModsJson(const std::vector<ModEntry> &mods)
{
    std::ostringstream json;
    json << "[";
    bool first = true;
    for (const ModEntry &mod : mods) {
        json << (first ? "" : ",") << "{\"name\":\"" << JsonEscape(mod.name) << "\",\"size\":" << mod.size
             << ",\"folder\":" << JsonBool(mod.folder) << "}";
        first = false;
    }
    json << "]";
    return json.str();
}

/** The dump name of the kept ROM ("Super Mario 64 (USA)"), or empty. Read once per change of the file. */
std::string KeptRomName(const std::string &path)
{
    static std::mutex cacheMutex;
    static std::string cachedKey, cachedName;
    struct stat info {};
    if (stat(path.c_str(), &info) != 0)
        return {};
    const std::string key = std::to_string(info.st_size) + ":" + std::to_string(info.st_mtime);
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (key != cachedKey) {
        std::vector<std::uint8_t> rom;
        std::string error;
        cachedName = RomFile::Load(path, &rom, &error);
        cachedKey  = key;
    }
    return cachedName;
}

} // namespace

std::string LoadOrCreatePin(const Storage &storage)
{
    const std::string path = storage.PathFor(kPinFile);
    std::ifstream in(path);
    std::string pin;
    if (in >> pin && pin.size() == 4 && std::all_of(pin.begin(), pin.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
        return pin;

    std::random_device device;
    std::uniform_int_distribution<int> digits(0, 9999);
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%04d", digits(device));
    std::ofstream(path) << buffer << "\n";
    return buffer;
}

TransferServer::TransferServer(Storage &storage, UsbAccess &usb, GameFiles &gameFiles, std::string pin)
    : storage_(storage), usb_(usb), gameFiles_(gameFiles), pin_(std::move(pin))
{
}

TransferServer::~TransferServer() { Stop(); }

bool TransferServer::Start()
{
    if (server_)
        return port_.load() != 0;

    server_ = std::make_unique<httplib::Server>();
    server_->new_task_queue = [] { return new httplib::ThreadPool(CPPHTTPLIB_THREAD_POOL_COUNT); };
    server_->set_payload_max_length(std::numeric_limits<std::size_t>::max());
    server_->set_read_timeout(30, 0);
    server_->set_write_timeout(30, 0);
    RegisterRoutes();

    for (int attempt = 0; attempt < kPortAttempts; ++attempt) {
        const int port = kPreferredPort + attempt;
        if (server_->bind_to_port("0.0.0.0", port)) {
            port_.store(static_cast<std::uint16_t>(port));
            thread_ = std::thread([this] { server_->listen_after_bind(); });
            return true;
        }
    }
    server_.reset();
    return false;
}

void TransferServer::Stop()
{
    if (!server_)
        return;
    server_->stop();
    if (thread_.joinable())
        thread_.join();
    server_.reset();
    port_.store(0);
}

TransferStatus TransferServer::Snapshot() const
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void TransferServer::SetExtraction(const ExtractionStatus &status)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    extraction_ = status;
}

void TransferServer::PublishEvent(const std::string &message)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.lastEvent = message;
    ++status_.eventSerial;
    Log::Printf("transfer: %s", message.c_str());
}

void TransferServer::MarkRestartPending()
{
    if (setupMode_.load())
        return;
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.restartPending = true;
}

void TransferServer::SetProgress(bool busy, const std::string &what, std::uint64_t done, std::uint64_t total)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.busy       = busy;
    status_.busyWhat   = what;
    status_.doneBytes  = done;
    status_.totalBytes = total;
}

void TransferServer::RegisterRoutes()
{
    httplib::Server &server = *server_;

    // Every request: private networks only, and /api needs the PIN shown on the TV.
    server.set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
        if (!IsPrivateNetworkAddress(req.remote_addr)) {
            Fail(res, 403, "Only devices on the local network may connect.");
            return httplib::Server::HandlerResponse::Handled;
        }
        const std::string pin = req.has_header(kPinHeader) ? req.get_header_value(kPinHeader) : req.get_param_value("pin");
        if (req.path.rfind("/api/", 0) == 0 && pin != pin_) {
            Fail(res, 401, "Wrong or missing PIN. Use the PIN shown on the TV.");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server.Get("/", [](const httplib::Request &, httplib::Response &res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(kWebPageHtml, "text/html; charset=utf-8");
    });

    server.Get("/api/status", [this](const httplib::Request &, httplib::Response &res) {
        const TransferStatus status        = Snapshot();
        ExtractionStatus extraction;
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            extraction = extraction_;
        }
        const RootStatus root              = usb_.Root();
        const std::vector<UsbDrive> drives = usb_.ListDrives();
        const GameFilesState files         = gameFiles_.StateFor(drives);
        const std::string romName = files.rom.present && files.rom.readable ? KeptRomName(files.rom.path) : std::string();
        const auto place          = [](const GameFilePlace &file) {
            return ",\"place\":\"" + JsonEscape(file.drive.empty() ? std::string(kInternal) : file.drive) + "\",\"placeLabel\":\""
                   + JsonEscape(file.drive.empty() ? std::string("TV") : "USB " + file.driveLabel) + "\"";
        };

        std::ostringstream json;
        json << "{\"version\":\"" << WM64_VERSION << "\""
             << ",\"mode\":\"" << (setupMode_.load() ? "setup" : "game") << "\""
             << ",\"persistent\":" << JsonBool(storage_.Persistent())
             << ",\"freeBytes\":" << storage_.FreeBytes() << ",\"safetyMarginBytes\":" << Storage::kSafetyMarginBytes
             << ",\"driveFolder\":\"" << UsbAccess::kDriveFolder << "\""
             << ",\"restartPending\":" << JsonBool(status.restartPending)
             << ",\"crashedLastTime\":" << JsonBool(crashedLastTime_.load())
             << ",\"event\":{\"serial\":" << status.eventSerial << ",\"text\":\"" << JsonEscape(status.lastEvent) << "\"}"
             << ",\"progress\":{\"busy\":" << JsonBool(status.busy) << ",\"what\":\"" << JsonEscape(status.busyWhat)
             << "\",\"done\":" << status.doneBytes << ",\"total\":" << status.totalBytes << "}"
             << ",\"root\":{\"process\":" << JsonBool(root.runningAsRoot) << ",\"homebrewChannel\":"
             << JsonBool(root.homebrewChannel) << ",\"detail\":\"" << JsonEscape(root.detail) << "\"}"
             << ",\"rom\":{\"present\":" << JsonBool(files.rom.present) << ",\"name\":\"" << JsonEscape(romName) << "\""
             << ",\"importedFrom\":\"" << JsonEscape(usb_.RomImportedFrom()) << "\"" << place(files.rom) << "}"
             << ",\"archive\":{\"present\":" << JsonBool(files.archive.present) << ",\"linked\":" << JsonBool(storage_.HasArchive())
             << ",\"size\":" << files.archive.bytes << place(files.archive)
             << ",\"rebuildPending\":" << JsonBool(Storage::FileSize(storage_.RebuildMarkerPath()) > 0
                                                   || std::filesystem::exists(storage_.RebuildMarkerPath())) << "}"
             << ",\"extraction\":{\"running\":" << JsonBool(extraction.running) << ",\"finished\":" << JsonBool(extraction.finished)
             << ",\"succeeded\":" << JsonBool(extraction.succeeded) << ",\"done\":" << extraction.done
             << ",\"total\":" << extraction.total << ",\"error\":\"" << JsonEscape(extraction.error) << "\"}";

        json << ",\"mods\":" << ModsJson(Storage::ListMods(storage_.ModsDir()));
        json << ",\"drives\":[";
        bool first = true;
        for (const UsbDrive &drive : drives) {
            json << (first ? "" : ",") << "{\"mount\":\"" << JsonEscape(drive.mountPoint) << "\",\"label\":\""
                 << JsonEscape(drive.label) << "\",\"spaceKnown\":" << JsonBool(drive.spaceKnown)
                 << ",\"freeBytes\":" << drive.freeBytes << ",\"totalBytes\":" << drive.totalBytes
                 << ",\"readable\":" << JsonBool(drive.readable) << ",\"writable\":" << JsonBool(drive.writable)
                 << ",\"romFile\":\"" << JsonEscape(drive.romFile) << "\",\"archiveBytes\":" << drive.archiveBytes
                 << ",\"modsFolder\":" << JsonBool(drive.modsFolder) << ",\"mods\":" << ModsJson(drive.mods) << "}";
            first = false;
        }
        json << "]}";
        res.set_content(json.str(), kJsonType);
    });

    server.Get("/api/log", [this](const httplib::Request &, httplib::Response &res) {
        const std::string path   = storage_.GameLogPath();
        const std::uint64_t size = Storage::FileSize(path);
        if (size == 0)
            return Fail(res, 404, "There is no game log yet.");
        std::ifstream in(path, std::ios::binary);
        const std::uint64_t skip = size > kMaxLogBytes ? size - kMaxLogBytes : 0;
        in.seekg(static_cast<std::streamoff>(skip));
        std::string text(static_cast<std::size_t>(size - skip), '\0');
        in.read(&text[0], static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(in.gcount()));
        res.set_header("Cache-Control", "no-store");
        res.set_content(std::move(text), kTextType);
    });

    server.Get("/api/portlog", [this](const httplib::Request &req, httplib::Response &res) {
        const std::string path = storage_.PortLogPath(req.get_param_value("previous") == "1");
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return Fail(res, 404, "There is no such log.");
        std::ostringstream text;
        text << in.rdbuf();
        res.set_header("Cache-Control", "no-store");
        res.set_content(text.str(), kTextType);
    });

    server.Post("/api/restart", [this](const httplib::Request &, httplib::Response &res) {
        if (setupMode_.load())
            return Fail(res, 409, "The game has not started yet: it starts on its own once the ROM is installed.");
        PublishEvent("Restarting the game...");
        MenuOverlay::RequestRestart();
        res.set_content("restarting", kTextType);
    });

    RegisterRomRoutes();
    RegisterModRoutes();
    RegisterSaveRoutes();
}

void TransferServer::RegisterRomRoutes()
{
    httplib::Server &server = *server_;

    server.Put("/api/rom", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected == 0)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (expected != RomFile::kRomBytes) {
            return Fail(res, 415, "That is not a Super Mario 64 ROM: it must be exactly 8 MB ("
                                      + std::to_string(RomFile::kRomBytes) + " bytes), this file has " + std::to_string(expected) + ".");
        }
        // Where it is kept: the TV, or a drive's game folder (the archive is then made there too).
        const std::string driveParam = req.get_param_value("drive");
        const std::string drive      = driveParam == kInternal ? std::string() : driveParam;
        const std::uint64_t needed   = expected + Storage::kArchiveSpaceBytes + Storage::kSafetyMarginBytes;
        if (drive.empty() && needed > storage_.FreeBytes()) {
            return Fail(res, 507, "Not enough space on the TV: the ROM and the game archive made from it need "
                                      + HumanBytes(needed) + " (including a safety margin), " + HumanBytes(storage_.FreeBytes())
                                      + " is free. Choose a USB drive instead.");
        }
        if (!drive.empty()) {
            UsbDrive target;
            if (!usb_.FindDrive(drive, &target))
                return Fail(res, 400, "That USB drive is not connected.");
            if (!target.writable && !usb_.Root().Available())
                return Fail(res, 403, "The app cannot write to this USB drive and has no root access.");
            if (!target.spaceKnown || needed > target.freeBytes) {
                return Fail(res, 507, "Not enough space on USB " + target.label + ": the ROM and the game archive made from it need "
                                          + HumanBytes(needed) + " (including a safety margin).");
            }
            // The ROM is checked on the TV before it goes to the drive.
            if (expected + Storage::kSafetyMarginBytes > storage_.FreeBytes())
                return Fail(res, 507, "Not enough space on the TV to check the ROM before it goes to the drive.");
        }
        if (busy_.exchange(true))
            return Fail(res, 409, "Another upload or move is already running.");
        BusyGuard busyGuard(busy_);

        // The ROM is small: received in memory, then converted, checked and written in one go.
        SetProgress(true, "ROM", 0, expected);
        PublishEvent("Receiving the ROM...");
        std::vector<std::uint8_t> data;
        data.reserve(static_cast<std::size_t>(expected));
        const bool completed = reader([&](const char *chunk, std::size_t length) {
            if (data.size() + length > expected)
                return false;
            data.insert(data.end(), chunk, chunk + length);
            SetProgress(true, "ROM", data.size(), expected);
            return true;
        });
        SetProgress(false, "ROM", data.size(), expected);
        if (!completed || data.size() != expected) {
            PublishEvent("Upload interrupted. Please try again.");
            return Fail(res, 400, "The upload was interrupted.");
        }

        std::string name;
        const std::string error = gameFiles_.StoreRom(std::move(data), drive, &name);
        if (!error.empty()) {
            PublishEvent(name.empty() ? "That file is not a supported Super Mario 64 ROM." : error);
            return Fail(res, name.empty() ? 415 : 500, error);
        }
        const std::string where = drive.empty() ? std::string("the TV") : "USB " + std::filesystem::path(drive).filename().string();
        if (!setupMode_.load()) {
            // The running game holds its archive open: it is made again at the next start.
            std::ofstream(storage_.RebuildMarkerPath()) << "new ROM\n";
            MarkRestartPending();
            PublishEvent(name + " received on " + where + ". The game's archive is made from it when the game starts again.");
            return res.set_content("staged", kTextType);
        }
        PublishEvent(name + " received on " + where + ". The TV is preparing the game...");
        res.set_content("installed", kTextType);
    });

    server.Delete("/api/rom", [this](const httplib::Request &, httplib::Response &res) {
        const std::string error = gameFiles_.RemoveRom();
        if (!error.empty())
            return Fail(res, 500, error);
        std::error_code ec;
        std::filesystem::remove(storage_.RebuildMarkerPath(), ec);
        PublishEvent(storage_.HasArchive() ? "ROM removed. The game keeps running from its archive."
                                           : "ROM removed.");
        res.set_content("removed", kTextType);
    });

    server.Post("/api/archive/rebuild", [this](const httplib::Request &, httplib::Response &res) {
        if (!gameFiles_.HasRom())
            return Fail(res, 409, "There is no ROM to make the archive from. Upload it first.");
        if (setupMode_.load())
            return Fail(res, 409, "The archive is being made now.");
        std::ofstream(storage_.RebuildMarkerPath()) << "requested\n";
        MarkRestartPending();
        PublishEvent("The game's archive is made again from the ROM when the game starts again.");
        res.set_content("staged", kTextType);
    });

    server.Post("/api/gamefiles/move", [this](const httplib::Request &req, httplib::Response &res) {
        const std::string driveParam = req.get_param_value("drive");
        const std::string drive      = driveParam == kInternal ? std::string() : driveParam;
        if (busy_.exchange(true))
            return Fail(res, 409, "Another upload or move is already running.");
        BusyGuard busyGuard(busy_);
        const std::string where = drive.empty() ? std::string("the TV") : "USB " + std::filesystem::path(drive).filename().string();
        PublishEvent("Moving the game files to " + where + "...");
        const std::string error = gameFiles_.MoveTo(drive, [this](std::uint64_t done, std::uint64_t total) {
            SetProgress(true, "Game files", done, total);
        });
        SetProgress(false, "Game files", 0, 0);
        if (!error.empty()) {
            PublishEvent(error);
            return Fail(res, 500, error);
        }
        PublishEvent("The game files are on " + where + " now.");
        res.set_content("moved", kTextType);
    });
}

void TransferServer::RegisterModRoutes()
{
    httplib::Server &server = *server_;

    server.Put("/api/mods/:name", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        const std::string name = req.path_params.at("name");
        if (!Storage::IsAllowedModName(name))
            return Fail(res, 400, "Mods are .o2r or .otr files with plain names.");

        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected == 0)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (expected > Storage::kMaxModBytes)
            return Fail(res, 413, "That file is " + HumanBytes(expected) + "; mods up to 4 GB can be uploaded (the largest file a FAT32 USB drive holds).");

        Target target;
        if (!ParseTarget(req.get_param_value("drive"), storage_, usb_, &target, res))
            return;
        const std::uint64_t needed = expected + Storage::kSafetyMarginBytes;
        if (needed > target.free)
            return Fail(res, 507, "Not enough space: this mod needs " + HumanBytes(needed) + ", " + HumanBytes(target.free) + " is free.");
        const bool staged = target.usb && !target.direct;
        if (staged && needed > storage_.FreeBytes())
            return Fail(res, 507, "The upload goes to the TV first (root moves it to the drive), and the TV is too full.");

        if (busy_.exchange(true))
            return Fail(res, 409, "Another upload or move is already running.");
        BusyGuard busyGuard(busy_);

        std::error_code ec;
        if (target.usb) {
            const std::string folderError = usb_.CreateDriveFolder(target.drive.mountPoint, UsbAccess::kDriveModsFolder);
            if (!folderError.empty())
                return Fail(res, 500, folderError);
        }
        fs::create_directories(staged ? storage_.IncomingDir() : target.folder, ec);

        SetProgress(true, name, 0, expected);
        const std::string partPath = (staged ? storage_.IncomingDir() : target.folder) + "/" + name + ".part";
        const std::int64_t written = StreamBodyToFile(reader, partPath, expected, [this, &name, expected](std::uint64_t total) {
            SetProgress(true, name, total, expected);
        });
        SetProgress(false, name, written < 0 ? 0 : static_cast<std::uint64_t>(written), expected);
        if (written < 0 || static_cast<std::uint64_t>(written) != expected) {
            usb_.RemoveFile(partPath);
            return Fail(res, 400, "The upload was interrupted or could not be written.");
        }
        if (!LooksLikeModArchive(partPath)) {
            usb_.RemoveFile(partPath);
            return Fail(res, 415, name + " is not an .o2r or .otr archive.");
        }

        const std::string destination = target.folder + "/" + name;
        const bool placed = staged ? usb_.MoveWithRoot(partPath, destination).empty() : usb_.RenameFile(partPath, destination);
        if (!placed) {
            usb_.RemoveFile(partPath);
            return Fail(res, 500, "Could not move the mod into place.");
        }
        if (target.usb)
            usb_.PlaceContent(storage_);
        MarkRestartPending();
        PublishEvent(name + " added" + (target.usb ? " to the USB drive" : "") + ". The game loads it at its next start.");
        res.set_content(setupMode_.load() ? "installed" : "staged", kTextType);
    });

    server.Delete("/api/mods/:name", [this](const httplib::Request &req, httplib::Response &res) {
        const std::string name  = req.path_params.at("name");
        const std::string drive = req.get_param_value("drive");
        if (name.empty() || name.front() == '.' || name.front() == '~' || name.find('/') != std::string::npos
            || name.find('\\') != std::string::npos || name.rfind("usb-", 0) == 0)
            return Fail(res, 400, "That mod name is not allowed.");

        std::string folder;
        if (drive.empty() || drive == kInternal) {
            folder = storage_.ModsDir();
        }
        else {
            if (!usb_.IsUsbMount(drive))
                return Fail(res, 400, "That USB drive is not connected.");
            folder = DriveModsFolder(drive);
        }
        const std::string path = folder + "/" + name;
        // Archives are files; anything else in a mods folder is a folder of loose files.
        const bool removed = Storage::IsModArchiveName(name) ? usb_.RemoveFile(path) : usb_.RemoveFolder(path);
        if (!removed)
            return Fail(res, 500, "Could not remove " + name + ".");
        if (!(drive.empty() || drive == kInternal))
            usb_.PlaceContent(storage_);
        MarkRestartPending();
        PublishEvent(name + " removed.");
        res.set_content("removed", kTextType);
    });

    server.Post("/api/mods/:name/move", [this](const httplib::Request &req, httplib::Response &res) {
        const std::string name = req.path_params.at("name");
        if (!Storage::IsAllowedModName(name))
            return Fail(res, 400, "Only .o2r and .otr mods can be moved.");
        const std::string from = req.get_param_value("from");
        std::string sourcePath;
        if (from.empty() || from == kInternal) {
            sourcePath = storage_.ModsDir() + "/" + name;
        }
        else {
            if (!usb_.IsUsbMount(from))
                return Fail(res, 400, "That USB drive is not connected.");
            sourcePath = DriveModsFolder(from) + "/" + name;
        }
        UsbDrive sourceDrive;
        const bool sourceReadable = from.empty() || from == kInternal || (usb_.FindDrive(from, &sourceDrive) && sourceDrive.readable);
        std::uint64_t size = Storage::FileSize(sourcePath);
        if (size == 0 && !sourceReadable) {
            for (const ModEntry &mod : sourceDrive.mods) {
                if (mod.name == name)
                    size = mod.size;
            }
        }
        if (size == 0)
            return Fail(res, 404, name + " is not there.");

        Target target;
        if (!ParseTarget(req.get_param_value("drive"), storage_, usb_, &target, res))
            return;
        if (fs::path(target.folder) == fs::path(sourcePath).parent_path())
            return Fail(res, 400, "It is already there.");
        if (size + Storage::kSafetyMarginBytes > target.free)
            return Fail(res, 507, "Not enough space there for " + name + ".");

        if (busy_.exchange(true))
            return Fail(res, 409, "Another upload or move is already running.");
        BusyGuard busyGuard(busy_);

        if (target.usb) {
            const std::string folderError = usb_.CreateDriveFolder(target.drive.mountPoint, UsbAccess::kDriveModsFolder);
            if (!folderError.empty())
                return Fail(res, 500, folderError);
        }
        std::error_code ec;
        fs::create_directories(target.folder, ec);
        const std::string destination = target.folder + "/" + name;
        SetProgress(true, name, 0, size);
        const std::string copyError = usb_.CopyFileTo(sourcePath, destination + ".part", [this, &name, size](std::uint64_t total) {
            SetProgress(true, name, total, size);
        });
        SetProgress(false, name, size, size);
        if (!copyError.empty() || !usb_.RenameFile(destination + ".part", destination)) {
            usb_.RemoveFile(destination + ".part");
            return Fail(res, 500, (copyError.empty() ? std::string("Could not move the copy into place.") : copyError)
                                      + " The original is untouched.");
        }
        usb_.RemoveFile(sourcePath);
        usb_.PlaceContent(storage_);
        MarkRestartPending();
        PublishEvent(name + " moved to " + (target.usb ? "the USB drive" : "the TV") + ".");
        res.set_content("moved", kTextType);
    });
}

void TransferServer::RegisterSaveRoutes()
{
    httplib::Server &server = *server_;

    server.Get("/api/saves", [this](const httplib::Request &, httplib::Response &res) {
        std::ostringstream json;
        json << "{\"saves\":[";
        bool first = true;
        for (const SaveFile &save : storage_.ListSaves()) {
            json << (first ? "" : ",") << "{\"path\":\"" << JsonEscape(save.path) << "\",\"size\":" << save.size
                 << ",\"mtime\":" << save.mtime << "}";
            first = false;
        }
        json << "],\"pendingRestore\":" << JsonBool(Storage::FileSize(storage_.PendingBackupPath()) > 0) << "}";
        res.set_content(json.str(), kJsonType);
    });

    server.Get("/api/saves/backup", [this](const httplib::Request &, httplib::Response &res) {
        // Each download gets its own file, removed once it has been sent.
        static std::atomic<unsigned> counter{ 0 };
        const std::string path   = storage_.PathFor("backup-download-" + std::to_string(++counter) + ".zip");
        const std::string error  = SaveBackup::Create(storage_.ShipDir(), path);
        const std::uint64_t size = Storage::FileSize(path);
        if (!error.empty() || size == 0) {
            std::error_code ec;
            fs::remove(path, ec);
            return Fail(res, 500, error.empty() ? std::string("There are no saves yet.") : error);
        }
        auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
        res.set_header("Content-Disposition", "attachment; filename=\"web-mario-64-saves.zip\"");
        res.set_header("Cache-Control", "no-store");
        res.set_content_provider(
            static_cast<std::size_t>(size), "application/zip",
            [in](std::size_t offset, std::size_t length, httplib::DataSink &sink) {
                std::vector<char> buffer(std::min<std::size_t>(length, 64 * 1024));
                in->seekg(static_cast<std::streamoff>(offset));
                in->read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize got = in->gcount();
                return got > 0 && sink.write(buffer.data(), static_cast<std::size_t>(got));
            },
            [in, path](bool) {
                in->close();
                std::error_code ec;
                fs::remove(path, ec);
            });
    });

    server.Put("/api/saves/backup", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected == 0)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (expected > Storage::kMaxBackupBytes)
            return Fail(res, 413, "That is larger than any save backup; refusing " + HumanBytes(expected) + ".");
        if (expected + Storage::kSafetyMarginBytes > storage_.FreeBytes())
            return Fail(res, 507, "Not enough space on the TV.");

        std::error_code ec;
        fs::create_directories(storage_.IncomingDir(), ec);
        const std::string partPath = storage_.IncomingDir() + "/backup-upload.part";
        const std::int64_t written = StreamBodyToFile(reader, partPath, expected, [](std::uint64_t) {});
        if (written < 0 || static_cast<std::uint64_t>(written) != expected) {
            fs::remove(partPath, ec);
            return Fail(res, 400, "The upload was interrupted.");
        }
        const std::string invalid = SaveBackup::Validate(partPath);
        if (!invalid.empty()) {
            fs::remove(partPath, ec);
            return Fail(res, 415, invalid);
        }

        if (setupMode_.load()) {
            // The game has not started: restore straight away.
            const std::string error = SaveBackup::Restore(partPath, storage_.ShipDir());
            fs::remove(partPath, ec);
            if (!error.empty())
                return Fail(res, 500, error);
            PublishEvent("Saves restored.");
            return res.set_content("installed", kTextType);
        }
        // While playing, the game owns its saves (and would write over them): restore at the next start.
        fs::rename(partPath, storage_.PendingBackupPath(), ec);
        if (ec) {
            fs::remove(partPath, ec);
            return Fail(res, 500, "Could not keep the backup for the next start.");
        }
        MarkRestartPending();
        PublishEvent("Save backup received. It is restored when the game starts again.");
        res.set_content("staged", kTextType);
    });
}

} // namespace wm64
