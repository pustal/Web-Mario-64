/**
 * @file SaveBackup.h
 * @brief The saves as one zip file, for the transfer page's backup and restore.
 *
 * A backup holds, relative to Ghostship's app directory (Storage::ShipDir()):
 *   saves/               the save files (the four files, and those of romhacks)
 *   ghostship.cfg.json   settings, enhancements and controls
 * It never holds the ROM, the game's archive or the mods, so it is small and
 * can be shared. A restore only writes these same paths.
 */
#ifndef WM64_SAVE_BACKUP_H
#define WM64_SAVE_BACKUP_H

#include <string>
#include <vector>

namespace wm64
{
namespace SaveBackup
{

/** The save files present in @p shipDir, as paths relative to it. */
std::vector<std::string> CollectFiles(const std::string &shipDir);

/** Whether @p relative (a path inside a backup) is one a restore may write. */
bool IsAllowedPath(const std::string &relative);

/**
 * Writes the saves in @p shipDir to the zip file @p zipPath.
 * @return Empty string on success, otherwise the reason.
 */
std::string Create(const std::string &shipDir, const std::string &zipPath);

/**
 * Checks that @p zipPath is a backup: a zip whose entries are all allowed
 * paths, with at least one of them.
 * @return Empty string if it is, otherwise the reason.
 */
std::string Validate(const std::string &zipPath);

/**
 * Restores the backup @p zipPath into @p shipDir (it overwrites the files
 * it holds and leaves the others).
 * @return Empty string on success, otherwise the reason.
 */
std::string Restore(const std::string &zipPath, const std::string &shipDir);

} // namespace SaveBackup
} // namespace wm64

#endif // WM64_SAVE_BACKUP_H
