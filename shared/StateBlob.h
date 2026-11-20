#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace andor::arp
{

/**
    A device's saved state (a Receiver's lane, the Master's performance: JSON text) as a list of
    small integers, the form Max for Live can keep in a Live Set: a [pattr] with parameter mode on
    ("Stored Only", type blob) stores a list of numbers with the Set (docs/M4L_IMPLEMENTATION.md
    §Persistence; to be confirmed in Live, as the spec requires).

    Why integers: Max symbols are interned for the life of the process (a symbol per edit would
    grow memory without bound) and a number may pass through a 32-bit float somewhere on the way to
    disk, so each value carries 3 bytes (24 bits: exact even as a float).

    Layout (version 1): [magic, version, byte length, CRC-32 high 16 bits, CRC-32 low 16 bits,
    payload words...]. A wrong magic, a newer version, a short list or a checksum mismatch is
    refused: the device then keeps its defaults and says so, never plays a corrupted state.
*/
namespace blob
{
    inline constexpr std::int32_t magic = 0x414E44;   // "AND"
    inline constexpr std::int32_t version = 1;
    inline constexpr std::size_t maxBytes = 1u << 20;  // 1 MB: far above a lane (a few KB)

    std::vector<std::int32_t> encode (const std::string& bytes);
    std::optional<std::string> decode (const std::vector<std::int32_t>& words, std::string* error = nullptr);
    std::uint32_t crc32 (const std::string& bytes) noexcept;
}

} // namespace andor::arp
