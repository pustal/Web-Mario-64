/**
 * @file TransferServer.h
 * @brief A small HTTP server for the browser transfer page: the game files
 *        (the ROM and the archive the TV makes from it, on the TV or a USB
 *        drive, and moving them between the two), mods (likewise), save
 *        backups, and the logs.
 *
 * Design notes for a TV:
 *  - Uploads are streamed straight to disk, never held in memory, and free
 *    space is checked against the announced size before anything is written.
 *  - Only clients on private networks are answered, and every API call must
 *    carry the PIN shown on the TV.
 *  - A few worker threads; the game's main thread is never blocked.
 */
#ifndef WM64_TRANSFER_SERVER_H
#define WM64_TRANSFER_SERVER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Extractor.h"

namespace httplib
{
class Server;
}

namespace wm64
{

class GameFiles;
class Storage;
class UsbAccess;

/** Snapshot of what the server is doing, for the TV screens to display. */
struct TransferStatus {
    bool busy                   = false; ///< An upload or move of a large file is running.
    std::string busyWhat;                ///< What it is ("ROM", a mod's name).
    std::uint64_t doneBytes     = 0;     ///< Bytes of it written so far.
    std::uint64_t totalBytes    = 0;     ///< Its size.
    bool restartPending         = false; ///< Files arrived that the game only reads at start-up.
    std::uint32_t eventSerial   = 0;     ///< Increments whenever lastEvent changes.
    std::string lastEvent;               ///< Short description of the latest event.
};

class TransferServer
{
public:
    /**
     * Port tried first, clear of the other homebrew web servers of these ports
     * (SonicWebMania uses 47801, Animal Crossing 47811), common dev ports and
     * webOS' own (3000/3001/9922/9998). If it is busy, the next nine are tried.
     */
    static constexpr std::uint16_t kPreferredPort = 47821;

    TransferServer(Storage &storage, UsbAccess &usb, GameFiles &gameFiles, std::string pin);
    ~TransferServer();

    TransferServer(const TransferServer &)            = delete;
    TransferServer &operator=(const TransferServer &) = delete;

    /** Binds the first free port in [kPreferredPort, kPreferredPort + 9] and serves on a background thread. */
    bool Start();

    /** Stops the server and joins its thread. Safe to call more than once. */
    void Stop();

    /**
     * Setup mode: the game has not started (no archive yet), so whatever
     * arrives is used straight away. In game mode, a new ROM or a save backup
     * is used from the next start, and the page offers a restart.
     */
    void SetSetupMode(bool setupMode) { setupMode_.store(setupMode); }

    /** The archive being made from the ROM (the setup screen reports it), for the page. */
    void SetExtraction(const ExtractionStatus &status);

    /** Whether the game ended unexpectedly at the last launch (the page then points at its logs). */
    void SetCrashedLastTime(bool crashed) { crashedLastTime_.store(crashed); }

    /** The bound port, or 0 when not running. */
    std::uint16_t Port() const { return port_.load(); }

    /** The PIN clients must send. */
    const std::string &Pin() const { return pin_; }

    /** Thread-safe copy of the current status. */
    TransferStatus Snapshot() const;

private:
    void RegisterRoutes();
    void RegisterRomRoutes();
    void RegisterModRoutes();
    void RegisterSaveRoutes();
    void PublishEvent(const std::string &message);
    void MarkRestartPending();
    void SetProgress(bool busy, const std::string &what, std::uint64_t done, std::uint64_t total);

    Storage &storage_;
    UsbAccess &usb_;
    GameFiles &gameFiles_;
    const std::string pin_;

    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    std::atomic<std::uint16_t> port_{ 0 };
    std::atomic<bool> setupMode_{ true };
    std::atomic<bool> crashedLastTime_{ false };
    std::atomic<bool> busy_{ false }; ///< A large upload or move is running (one at a time).

    mutable std::mutex statusMutex_;
    TransferStatus status_;
    ExtractionStatus extraction_; ///< Guarded by statusMutex_.
};

/** Returns the PIN stored in the data folder, creating a random 4-digit one the first time. */
std::string LoadOrCreatePin(const Storage &storage);

} // namespace wm64

#endif // WM64_TRANSFER_SERVER_H
