#pragma once

#include <cstdint>

namespace andor::arp
{

/**
    Assembles raw MIDI bytes (Max's [midiin] outputs one int per byte) into messages, with
    running status; real-time bytes (0xF8..0xFF) pass on their own; SysEx is passed through byte
    by byte (it is never ANDOR ARP's business). Pure C++, no allocation.
*/
class MidiByteParser
{
public:
    struct Message
    {
        std::uint8_t status = 0, data1 = 0, data2 = 0;
        int length = 0;            ///< bytes in this message (1..3)
        bool sysexByte = false;    ///< a byte inside SysEx (pass it on as is)
    };

    /** Feeds one byte; true when `out` holds a complete message (or a byte to pass on). */
    bool feed (int byte, Message& out) noexcept
    {
        const auto b = static_cast<std::uint8_t> (byte & 0xFF);
        if (b >= 0xF8)
        {
            out = { b, 0, 0, 1, false };
            return true;
        }
        if (b == 0xF0 || inSysex)
        {
            inSysex = b != 0xF7;
            out = { b, 0, 0, 1, true };
            return true;
        }
        if (b & 0x80)
        {
            running = b;
            needed = dataBytes (b);
            have = 0;
            if (needed == 0)
            {
                out = { b, 0, 0, 1, false };
                running = b < 0xF0 ? b : 0;
                return true;
            }
            if (b >= 0xF0)
                running = 0;   // system common: no running status afterwards
            pendingStatus = b;
            return false;
        }
        if (pendingStatus == 0)
        {
            if (running == 0)
                return false;   // a stray data byte
            pendingStatus = running;
            needed = dataBytes (running);
            have = 0;
        }
        data[have++] = b;
        if (have < needed)
            return false;
        out = { pendingStatus, data[0], needed > 1 ? data[1] : std::uint8_t { 0 }, 1 + needed, false };
        pendingStatus = 0;
        have = 0;
        return true;
    }

    static int dataBytes (std::uint8_t status) noexcept
    {
        switch (status & 0xF0)
        {
            case 0xC0:
            case 0xD0: return 1;
            case 0xF0:
                return status == 0xF1 || status == 0xF3 ? 1 : status == 0xF2 ? 2 : 0;
            default: return 2;
        }
    }

private:
    std::uint8_t running = 0, pendingStatus = 0;
    std::uint8_t data[2] {};
    int needed = 0, have = 0;
    bool inSysex = false;
};

} // namespace andor::arp
