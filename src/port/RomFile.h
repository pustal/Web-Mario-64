/**
 * @file RomFile.h
 * @brief Recognising the player's Super Mario 64 ROM.
 *
 * N64 ROMs are dumped in three byte orders, told apart by their first four
 * bytes:
 *   .z64  80 37 12 40   big-endian, the console's own order
 *   .v64  37 80 40 12   each 16-bit word byte-swapped
 *   .n64  40 12 37 80   each 32-bit word reversed
 * File extensions are often wrong, so the content decides. Ghostship's
 * extractor reads only the .z64 order: ToZ64() converts the others. A ROM is
 * accepted when its .z64 form is one of the dumps Ghostship supports (its
 * config.yml lists them by SHA-1): Super Mario 64 (USA) and (Japan).
 */
#ifndef WM64_ROM_FILE_H
#define WM64_ROM_FILE_H

#include <cstdint>
#include <string>
#include <vector>

namespace wm64
{
namespace RomFile
{

/** Size of the supported ROMs: 8 MB. */
constexpr std::uint64_t kRomBytes = 8ull * 1024ull * 1024ull;

/** A ROM's byte order. */
enum class ByteOrder {
    Unknown,
    BigEndian, ///< .z64
    ByteSwap,  ///< .v64
    WordSwap,  ///< .n64
};

/** The byte order of @p data, from its first four bytes. */
ByteOrder DetectByteOrder(const std::vector<std::uint8_t> &data);

/** Converts @p data to the .z64 order in place. @return False if its order is unknown. */
bool ToZ64(std::vector<std::uint8_t> &data);

/** SHA-1 of @p data, as 40 lower-case hex digits. */
std::string Sha1Hex(const std::vector<std::uint8_t> &data);

/**
 * Which supported dump @p z64 (already in the .z64 order) is: "Super Mario 64
 * (USA)" or "Super Mario 64 (Japan)", or empty for anything else.
 */
std::string Identify(const std::vector<std::uint8_t> &z64);

/** Whether @p name has an extension N64 ROMs are dumped with: .z64, .n64 or .v64 (any case). */
bool IsRomFileName(const std::string &name);

/**
 * Reads the file at @p path, converts it to the .z64 order and identifies it.
 * @param z64 Receives the converted ROM when it is a supported one.
 * @return The dump's name, or empty (with @p error saying why).
 */
std::string Load(const std::string &path, std::vector<std::uint8_t> *z64, std::string *error);

/**
 * Converts and checks @p data (a ROM in any byte order), then writes it to
 * @p destination atomically (a .part file, renamed).
 * @return The dump's name, or empty (with @p error saying why).
 */
std::string Install(std::vector<std::uint8_t> data, const std::string &destination, std::string *error);

} // namespace RomFile
} // namespace wm64

#endif // WM64_ROM_FILE_H
