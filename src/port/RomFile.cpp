/**
 * @file RomFile.cpp
 * @brief Implementation of ROM recognition (see RomFile.h).
 */
#include "RomFile.h"

#include "Storage.h"

#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace wm64
{
namespace RomFile
{
namespace
{

/** The supported dumps, by the SHA-1 of their .z64 form (Ghostship's config.yml). */
struct KnownDump {
    const char *sha1;
    const char *name;
};
constexpr std::array<KnownDump, 2> kKnownDumps = { {
    { "9bef1128717f958171a4afac3ed78ee2bb4e86ce", "Super Mario 64 (USA)" },
    { "8a20a5c83d6ceb0f0506cfc9fa20d8f438cafe51", "Super Mario 64 (Japan)" },
} };

/** SHA-1 (FIPS 180-4), enough for checking 8 MB once. */
class Sha1
{
public:
    void Update(const std::uint8_t *data, std::size_t length)
    {
        for (std::size_t i = 0; i < length; ++i) {
            block_[used_++] = data[i];
            if (used_ == block_.size()) {
                Compress();
                used_ = 0;
            }
        }
        bits_ += static_cast<std::uint64_t>(length) * 8u;
    }

    std::string HexDigest()
    {
        const std::uint64_t bits = bits_;
        const std::uint8_t one   = 0x80;
        Update(&one, 1);
        const std::uint8_t zero = 0;
        while (used_ != 56)
            Update(&zero, 1);
        std::array<std::uint8_t, 8> length{};
        for (int i = 0; i < 8; ++i)
            length[7 - i] = static_cast<std::uint8_t>(bits >> (i * 8));
        Update(length.data(), length.size());

        static const char kHex[] = "0123456789abcdef";
        std::string out;
        for (const std::uint32_t word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4)
                out += kHex[(word >> shift) & 0xf];
        }
        return out;
    }

private:
    static std::uint32_t Rotate(std::uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

    void Compress()
    {
        std::array<std::uint32_t, 80> w{};
        for (int i = 0; i < 16; ++i) {
            w[i] = static_cast<std::uint32_t>(block_[i * 4]) << 24 | static_cast<std::uint32_t>(block_[i * 4 + 1]) << 16
                   | static_cast<std::uint32_t>(block_[i * 4 + 2]) << 8 | block_[i * 4 + 3];
        }
        for (int i = 16; i < 80; ++i)
            w[i] = Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            }
            else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            }
            else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            }
            else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const std::uint32_t next = Rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rotate(b, 30);
            b = a;
            a = next;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    std::array<std::uint32_t, 5> state_ = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    std::array<std::uint8_t, 64> block_{};
    std::size_t used_   = 0;
    std::uint64_t bits_ = 0;
};

} // namespace

ByteOrder DetectByteOrder(const std::vector<std::uint8_t> &data)
{
    if (data.size() < 4)
        return ByteOrder::Unknown;
    const std::array<std::uint8_t, 4> head = { data[0], data[1], data[2], data[3] };
    if (head == std::array<std::uint8_t, 4>{ 0x80, 0x37, 0x12, 0x40 })
        return ByteOrder::BigEndian;
    if (head == std::array<std::uint8_t, 4>{ 0x37, 0x80, 0x40, 0x12 })
        return ByteOrder::ByteSwap;
    if (head == std::array<std::uint8_t, 4>{ 0x40, 0x12, 0x37, 0x80 })
        return ByteOrder::WordSwap;
    return ByteOrder::Unknown;
}

bool ToZ64(std::vector<std::uint8_t> &data)
{
    switch (DetectByteOrder(data)) {
        case ByteOrder::BigEndian: return true;
        case ByteOrder::ByteSwap:
            for (std::size_t i = 0; i + 1 < data.size(); i += 2)
                std::swap(data[i], data[i + 1]);
            return true;
        case ByteOrder::WordSwap:
            for (std::size_t i = 0; i + 3 < data.size(); i += 4) {
                std::swap(data[i], data[i + 3]);
                std::swap(data[i + 1], data[i + 2]);
            }
            return true;
        default: return false;
    }
}

std::string Sha1Hex(const std::vector<std::uint8_t> &data)
{
    Sha1 hash;
    hash.Update(data.data(), data.size());
    return hash.HexDigest();
}

std::string Identify(const std::vector<std::uint8_t> &z64)
{
    if (z64.size() != kRomBytes)
        return {};
    const std::string digest = Sha1Hex(z64);
    for (const KnownDump &dump : kKnownDumps) {
        if (digest == dump.sha1)
            return dump.name;
    }
    return {};
}

bool IsRomFileName(const std::string &name)
{
    return EndsWithNoCase(name, ".z64") || EndsWithNoCase(name, ".n64") || EndsWithNoCase(name, ".v64");
}

std::string Load(const std::string &path, std::vector<std::uint8_t> *z64, std::string *error)
{
    if (Storage::FileSize(path) != kRomBytes) {
        *error = "Not an 8 MB ROM.";
        return {};
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(kRomBytes));
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()))) {
        *error = "The ROM could not be read.";
        return {};
    }
    if (!ToZ64(data)) {
        *error = "Not an N64 ROM.";
        return {};
    }
    const std::string name = Identify(data);
    if (name.empty()) {
        *error = "Not a supported Super Mario 64 ROM (USA or Japan).";
        return {};
    }
    *z64 = std::move(data);
    return name;
}

std::string Install(std::vector<std::uint8_t> data, const std::string &destination, std::string *error)
{
    if (data.size() != kRomBytes) {
        *error = "Not an 8 MB ROM.";
        return {};
    }
    if (!ToZ64(data)) {
        *error = "Not an N64 ROM.";
        return {};
    }
    const std::string name = Identify(data);
    if (name.empty()) {
        *error = "Not a supported Super Mario 64 ROM (USA or Japan).";
        return {};
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(destination).parent_path(), ec);
    const std::string part = destination + ".part";
    std::FILE *out         = std::fopen(part.c_str(), "wb");
    bool written           = out && std::fwrite(data.data(), 1, data.size(), out) == data.size();
    if (out)
        written = std::fflush(out) == 0 && fsync(fileno(out)) == 0 && std::fclose(out) == 0 && written;
    if (written)
        fs::rename(part, destination, ec);
    if (!written || ec) {
        fs::remove(part, ec);
        *error = "The ROM could not be saved (is the TV's storage full?).";
        return {};
    }
    return name;
}

} // namespace RomFile
} // namespace wm64
