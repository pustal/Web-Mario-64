/**
 * @file SaveBackup.cpp
 * @brief Implementation of the save backup zip (see SaveBackup.h), with libzip
 *        (which libultraship already uses for its archives).
 */
#include "SaveBackup.h"

#include <zip.h>

// libzip 1.10 named the "read to the end of the file" length; older releases
// (the desktop test build may use the system's) document -1 for it.
#ifndef ZIP_LENGTH_TO_END
#define ZIP_LENGTH_TO_END (-1)
#endif

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

namespace wm64
{
namespace SaveBackup
{
namespace fs = std::filesystem;

namespace
{

/** Single files a backup holds. */
constexpr std::array<const char *, 1> kFiles = { "ghostship.cfg.json" };
/** Folders a backup holds, with everything in them. */
constexpr std::array<const char *, 1> kFolders = { "saves" };
/** No single save file comes near this; it guards the restore against zip bombs. */
constexpr std::uint64_t kMaxEntryBytes = 16ull * 1024ull * 1024ull;
/** Entries per backup, for the same reason. */
constexpr zip_int64_t kMaxEntries = 4096;

/** Whether every part of @p relative is a plain name (no "", ".", ".." or hidden parts). */
bool HasPlainParts(const std::string &relative)
{
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t end     = relative.find('/', start);
        const std::string segment = relative.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment.front() == '.' || segment.find('\\') != std::string::npos)
            return false;
        if (end == std::string::npos)
            return true;
        start = end + 1;
    }
    return true;
}

/** Closes a libzip archive when it goes out of scope (discarding unsaved changes). */
struct ZipCloser {
    zip_t *zip;
    ~ZipCloser()
    {
        if (zip)
            zip_discard(zip);
    }
};

} // namespace

bool IsAllowedPath(const std::string &relative)
{
    if (relative.empty() || relative.size() > 400 || relative.front() == '/' || !HasPlainParts(relative))
        return false;
    if (std::any_of(kFiles.begin(), kFiles.end(), [&](const char *file) { return relative == file; }))
        return true;
    return std::any_of(kFolders.begin(), kFolders.end(), [&](const char *folder) {
        const std::string prefix = std::string(folder) + "/";
        return relative.size() > prefix.size() && relative.compare(0, prefix.size(), prefix) == 0;
    });
}

std::vector<std::string> CollectFiles(const std::string &shipDir)
{
    std::vector<std::string> files;
    std::error_code ec;
    const fs::path base(shipDir);
    for (const char *file : kFiles) {
        if (fs::is_regular_file(base / file, ec))
            files.emplace_back(file);
    }
    for (const char *folder : kFolders) {
        for (fs::recursive_directory_iterator it(base / folder, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec))
                continue;
            const std::string relative = fs::relative(it->path(), base, ec).generic_string();
            if (!ec && IsAllowedPath(relative))
                files.push_back(relative);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string Create(const std::string &shipDir, const std::string &zipPath)
{
    int status = 0;
    zip_t *zip = zip_open(zipPath.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &status);
    if (!zip)
        return "Could not create the backup file.";

    for (const std::string &relative : CollectFiles(shipDir)) {
        const std::string path = (fs::path(shipDir) / relative).string();
        // The whole file is read when the archive is written (zip_close).
        zip_source_t *source = zip_source_file(zip, path.c_str(), 0, ZIP_LENGTH_TO_END);
        if (!source)
            continue; // removed meanwhile
        if (zip_file_add(zip, relative.c_str(), source, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0) {
            zip_source_free(source);
            zip_discard(zip);
            return "Could not add " + relative + " to the backup.";
        }
    }
    const char *comment = "Web Mario 64 saves (webOS)";
    zip_set_archive_comment(zip, comment, static_cast<zip_uint16_t>(std::char_traits<char>::length(comment)));
    if (zip_close(zip) != 0) {
        zip_discard(zip);
        return "Could not finish the backup file.";
    }
    return {};
}

std::string Validate(const std::string &zipPath)
{
    int status = 0;
    ZipCloser closer{ zip_open(zipPath.c_str(), ZIP_RDONLY | ZIP_CHECKCONS, &status) };
    if (!closer.zip)
        return "That is not a zip file.";
    const zip_int64_t count = zip_get_num_entries(closer.zip, 0);
    if (count > kMaxEntries)
        return "The backup is larger than any saves could be.";
    zip_int64_t files = 0;
    for (zip_int64_t i = 0; i < count; ++i) {
        zip_stat_t info;
        if (zip_stat_index(closer.zip, static_cast<zip_uint64_t>(i), 0, &info) != 0 || !(info.valid & ZIP_STAT_NAME))
            return "The zip file is damaged.";
        const std::string entry(info.name);
        if (!entry.empty() && entry.back() == '/')
            continue; // folder entries some zip tools add
        if (!IsAllowedPath(entry))
            return "\"" + entry + "\" is not part of the saves; only backups made by this page can be restored.";
        if ((info.valid & ZIP_STAT_SIZE) && info.size > kMaxEntryBytes)
            return "The backup is larger than any saves could be.";
        ++files;
    }
    return files == 0 ? "The zip file holds no saves." : std::string();
}

std::string Restore(const std::string &zipPath, const std::string &shipDir)
{
    const std::string invalid = Validate(zipPath);
    if (!invalid.empty())
        return invalid;

    int status = 0;
    ZipCloser closer{ zip_open(zipPath.c_str(), ZIP_RDONLY, &status) };
    if (!closer.zip)
        return "That is not a zip file.";
    std::vector<char> buffer(64 * 1024);
    const zip_int64_t count = zip_get_num_entries(closer.zip, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        zip_stat_t info;
        zip_stat_index(closer.zip, static_cast<zip_uint64_t>(i), 0, &info);
        const std::string entry(info.name);
        if (entry.empty() || entry.back() == '/')
            continue;

        const fs::path destination = fs::path(shipDir) / entry;
        const fs::path partPath    = destination.string() + ".part";
        std::error_code ec;
        fs::create_directories(destination.parent_path(), ec);
        zip_file_t *file = zip_fopen_index(closer.zip, static_cast<zip_uint64_t>(i), 0);
        if (!file)
            return "Could not read " + entry + " from the backup.";
        std::FILE *out = std::fopen(partPath.c_str(), "wb");
        if (!out) {
            zip_fclose(file);
            return "Could not write " + entry + ".";
        }
        std::uint64_t total = 0;
        zip_int64_t length  = 0;
        bool ok             = true;
        while ((length = zip_fread(file, buffer.data(), buffer.size())) > 0) {
            total += static_cast<std::uint64_t>(length);
            if (total > kMaxEntryBytes || std::fwrite(buffer.data(), 1, static_cast<std::size_t>(length), out) != static_cast<std::size_t>(length)) {
                ok = false;
                break;
            }
        }
        // zip_fread reports a bad CRC as an error at the end of the entry.
        ok = ok && length == 0 && std::fflush(out) == 0 && fsync(fileno(out)) == 0;
        std::fclose(out);
        zip_fclose(file);
        if (!ok) {
            fs::remove(partPath, ec);
            return "Could not restore " + entry + " (damaged backup, or the TV is full).";
        }
        fs::rename(partPath, destination, ec);
        if (ec) {
            fs::remove(partPath, ec);
            return "Could not put " + entry + " in place.";
        }
    }
    return {};
}

} // namespace SaveBackup
} // namespace wm64
