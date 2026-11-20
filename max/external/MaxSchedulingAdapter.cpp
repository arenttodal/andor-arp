#include "MaxSchedulingAdapter.h"

#include <algorithm>
#include <cmath>

namespace andor::arp
{

void MaxSchedulingAdapter::configure (const Config& config) noexcept
{
    cfg = config;
    cfg.sampleRate = cfg.sampleRate > 1000.0 ? cfg.sampleRate : 48000.0;
    cfg.latencyMs = std::max (0.0, cfg.latencyMs);
    cfg.envelopeMs = std::max (0.0, cfg.envelopeMs);
    reset();
}

void MaxSchedulingAdapter::reset() noexcept
{
    started = wasPlaying = false;
    renderedSamples = 0;
    ppqAtEnd = 0.0;
    lastError = 0.0;
    jumpCount = 0;
    historyCount = historyNext = 0;
    anchoredAtMs = -1.0e18;
}

MaxSchedulingAdapter::Block MaxSchedulingAdapter::service (double nowMs, const TransportReading& t) noexcept
{
    Block b;
    if (! started)
    {
        started = true;
        originMs = nowMs;
        renderedSamples = 0;
    }

    // Render up to now on the virtual clock (Live's timeline); the block plays `latency` later.
    const double msPerSample = 1000.0 / cfg.sampleRate;
    const auto target = static_cast<std::int64_t> (std::floor ((nowMs - originMs) / msPerSample));
    const auto length = target - renderedSamples;
    const double blockTimelineMs = originMs + static_cast<double> (renderedSamples) * msPerSample;
    b.wallStartMs = blockTimelineMs + cfg.latencyMs;
    if (length <= 0)
        return b;   // a service sooner than a sample: nothing to do
    // A long stall (the scheduler was blocked): never render more than a second in one go; the
    // notes of a stall that long are late whatever happens, and the clock catches up over the
    // following services.
    b.numSamples = static_cast<int> (std::min<std::int64_t> (length, static_cast<std::int64_t> (cfg.sampleRate)));

    const bool tempoKnown = t.valid && std::isfinite (t.bpm) && t.bpm > 0.0;
    const double bpm = tempoKnown ? std::clamp (t.bpm, 5.0, 1000.0) : lastBpm;
    lastBpm = bpm;
    const bool playing = tempoKnown && t.playing && std::isfinite (t.ppq);

    b.timing.valid = t.valid;
    b.timing.playing = playing;
    b.timing.numerator = t.numerator;
    b.timing.denominator = t.denominator;
    b.timing.bpm = bpm;

    if (playing)
    {
        // Where the transport was at the block's first sample, from this reading.
        const double projected = t.ppq + (blockTimelineMs - nowMs) / 60000.0 * bpm;
        const double error = projected - ppqAtEnd;
        const double jumpLimit = std::max (cfg.jumpQuarters, cfg.jumpMs / 60000.0 * bpm);
        if (! wasPlaying || std::abs (error) > jumpLimit)
        {
            ppqAtEnd = projected;
            b.jumped = true;
            ++jumpCount;
            lastError = 0.0;
            historyCount = historyNext = 0;
            anchoredAtMs = nowMs;
            correctedQuarters = 0.0;
        }
        else
        {
            // Readings are never ahead of Live, only behind (stale): the most advanced recent one
            // is the closest to the truth.
            // (Stored with the correction made so far, so older readings are compared as they
            // would be now.)
            history[historyNext] = { nowMs, error + correctedQuarters };
            historyNext = (historyNext + 1) % historySize;
            historyCount = std::min (historyCount + 1, historySize);
            double best = error;
            for (int i = 0; i < historyCount; ++i)
                if (nowMs - history[i].ms <= cfg.envelopeMs)
                    best = std::max (best, history[i].error - correctedQuarters);
            lastError = best;
            const double limit = nowMs - anchoredAtMs < cfg.acquireSeconds * 1000.0 ? cfg.acquireTempoCorrection : cfg.maxTempoCorrection;
            // Remove the error over correctionSeconds by running slightly faster or slower.
            const double correction = std::clamp (best / (cfg.correctionSeconds * bpm / 60.0), -limit, limit);
            b.timing.bpm = bpm * (1.0 + correction);
        }
        b.timing.ppq = ppqAtEnd;
        const double advance = static_cast<double> (b.numSamples) / (cfg.sampleRate * 60.0 / b.timing.bpm);
        correctedQuarters += advance - static_cast<double> (b.numSamples) / (cfg.sampleRate * 60.0 / bpm);
        ppqAtEnd += advance;
    }
    else
    {
        b.timing.ppq = std::isfinite (t.ppq) ? t.ppq : 0.0;
        lastError = 0.0;
    }
    wasPlaying = playing;
    renderedSamples += b.numSamples;
    return b;
}

} // namespace andor::arp
