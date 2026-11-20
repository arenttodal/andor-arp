#include "StateBlob.h"

#include <array>

namespace andor::arp::blob
{

std::uint32_t crc32 (const std::string& bytes) noexcept
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> t {};
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (const char ch : bytes)
        c = table[(c ^ static_cast<std::uint8_t> (ch)) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::vector<std::int32_t> encode (const std::string& bytes)
{
    const auto crc = crc32 (bytes);
    std::vector<std::int32_t> words { magic, version, static_cast<std::int32_t> (bytes.size()), static_cast<std::int32_t> (crc >> 16),
                                      static_cast<std::int32_t> (crc & 0xFFFFu) };
    words.reserve (words.size() + bytes.size() / 3 + 1);
    for (std::size_t i = 0; i < bytes.size(); i += 3)
    {
        std::int32_t w = 0;
        for (std::size_t k = 0; k < 3; ++k)
            w = (w << 8) | (i + k < bytes.size() ? static_cast<std::uint8_t> (bytes[i + k]) : 0);
        words.push_back (w);
    }
    return words;
}

std::optional<std::string> decode (const std::vector<std::int32_t>& words, std::string* error)
{
    auto fail = [error] (const char* what) -> std::optional<std::string> {
        if (error != nullptr)
            *error = what;
        return std::nullopt;
    };
    if (words.size() < 5 || words[0] != magic)
        return fail ("no saved ANDOR ARP state");
    if (words[1] > version || words[1] < 1)
        return fail ("state saved by a newer version");
    if (words[2] < 0 || static_cast<std::size_t> (words[2]) > maxBytes)
        return fail ("state length out of range");
    const auto length = static_cast<std::size_t> (words[2]);
    if (words.size() != 5 + (length + 2) / 3)
        return fail ("state truncated");
    std::string bytes;
    bytes.reserve (length);
    for (std::size_t i = 5; i < words.size(); ++i)
    {
        const auto w = static_cast<std::uint32_t> (words[i]);
        if (w > 0xFFFFFFu)
            return fail ("state damaged");
        for (int k = 2; k >= 0; --k)
        {
            const auto byte = static_cast<char> ((w >> (8 * k)) & 0xFFu);
            if (bytes.size() < length)
                bytes += byte;
            else if (byte != 0)
                return fail ("state damaged (padding)");
        }
    }
    const auto crc = (static_cast<std::uint32_t> (words[3]) << 16) | static_cast<std::uint32_t> (words[4]);
    if (words[3] < 0 || words[3] > 0xFFFF || words[4] < 0 || words[4] > 0xFFFF || crc32 (bytes) != crc)
        return fail ("state damaged (checksum)");
    return bytes;
}

} // namespace andor::arp::blob
