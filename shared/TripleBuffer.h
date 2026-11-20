#pragma once

#include <array>
#include <atomic>

namespace andor::arp
{

/**
    Hands a value from one writer thread to one reader thread without locks or allocation: the
    writer never waits, the reader never sees a half-written value and gets the latest one.
    (The same scheme as OSP's CustomPatternExchange, for any trivially copyable T.) Used by the
    Max external between Max's main thread (edits, link messages, JSON) and its scheduler thread
    (which, in Live, is the audio thread).
*/
template <typename T>
class TripleBuffer
{
public:
    /** Writer thread. */
    void write (const T& value) noexcept
    {
        buffers[static_cast<std::size_t> (writeIndex)] = value;
        writeIndex = middle.exchange (writeIndex | dirty, std::memory_order_acq_rel) & indexMask;
    }

    /** Reader thread: true (and `out` set) when a value was written since the last read. */
    bool read (T& out) noexcept
    {
        if ((middle.load (std::memory_order_acquire) & dirty) == 0)
            return false;
        readIndex = middle.exchange (readIndex, std::memory_order_acq_rel) & indexMask;
        out = buffers[static_cast<std::size_t> (readIndex)];
        return true;
    }

private:
    static constexpr int dirty = 4, indexMask = 3;
    std::array<T, 3> buffers {};
    std::atomic<int> middle { 1 };
    int writeIndex = 0, readIndex = 2;
};

/** A fixed single-producer single-consumer queue (no allocation, no lock). */
template <typename T, int Capacity>
class SpscQueue
{
public:
    bool push (const T& value) noexcept
    {
        const int h = head.load (std::memory_order_relaxed);
        const int next = (h + 1) % Capacity;
        if (next == tail.load (std::memory_order_acquire))
            return false;   // full: the caller counts it
        items[static_cast<std::size_t> (h)] = value;
        head.store (next, std::memory_order_release);
        return true;
    }

    bool pop (T& out) noexcept
    {
        const int t = tail.load (std::memory_order_relaxed);
        if (t == head.load (std::memory_order_acquire))
            return false;
        out = items[static_cast<std::size_t> (t)];
        tail.store ((t + 1) % Capacity, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> items {};
    std::atomic<int> head { 0 }, tail { 0 };
};

} // namespace andor::arp
