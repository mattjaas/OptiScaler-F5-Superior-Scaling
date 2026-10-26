#pragma once
#include <cstddef>
#include <cstdint>

namespace DlssNr
{
// Whether a finished-picture pass may use guides the producer queue signals at `required`.
// A producer's queued signal can itself sit behind a wait on this presentation (the game's DLSS-G
// does this), so the present queue must never wait on another queue. Same-queue submission order is
// enough; input from another queue must already be complete. UINT64_MAX means the device was removed.
inline bool FinishedInputReady(bool sameQueue, uint64_t completed, uint64_t required)
{
    return completed != UINT64_MAX && (sameQueue || completed >= required);
}

struct FinishedInput
{
    bool eligible = false; // submitted, pending, inside the epoch window, matching size and mode
    uint64_t serial = 0;
    bool sameQueue = false;
    uint64_t completed = 0, required = 0;
};

enum class FinishedPick
{
    None,     // nothing eligible
    Ready,    // `index` is the newest eligible input and may be composed now
    NotReady, // the newest eligible input is still rendering (or the device was removed)
};

// Only the newest eligible input may be composed. An older one that already finished holds the
// previous frame's guides, which do not line up with this picture.
inline FinishedPick PickFinishedInput(const FinishedInput* inputs, size_t count, size_t& index)
{
    const FinishedInput* newest = nullptr;
    for (size_t i = 0; i < count; ++i)
        if (inputs[i].eligible && (!newest || inputs[i].serial > newest->serial))
        {
            newest = &inputs[i];
            index = i;
        }
    if (!newest)
        return FinishedPick::None;
    return FinishedInputReady(newest->sameQueue, newest->completed, newest->required) ? FinishedPick::Ready
                                                                                       : FinishedPick::NotReady;
}

// Experimental wilsjo2 handoff: choose the newest already ordered/completed capture. Native FG
// inflates presentation epochs, so that route deliberately uses capture serials instead. Keep the
// janblade policy above separate for the unchanged A/B path.
inline FinishedPick PickReadyFinishedInput(const FinishedInput* inputs, size_t count, size_t& index)
{
    const FinishedInput* newest = nullptr;
    bool eligible = false;
    for (size_t i = 0; i < count; ++i)
    {
        eligible |= inputs[i].eligible;
        if (inputs[i].eligible && FinishedInputReady(inputs[i].sameQueue, inputs[i].completed, inputs[i].required) &&
            (!newest || inputs[i].serial > newest->serial))
        {
            newest = &inputs[i];
            index = i;
        }
    }
    return newest ? FinishedPick::Ready : eligible ? FinishedPick::NotReady : FinishedPick::None;
}

// Presents from `then` to `now`; UINT64_MAX when `then` never happened (UINT64_MAX). A `then` ahead of `now`
// (the two are read without a common lock) counts as just now, so a race errs towards pausing.
inline uint64_t PresentsSince(uint64_t now, uint64_t then)
{
    return then == UINT64_MAX ? UINT64_MAX : then > now ? 0 : now - then;
}

// A DLSS-G evaluate this many presents ago still counts as running: each real frame is followed by at most
// a handful of generated presents.
inline constexpr uint64_t DlssgEvaluateWindow = 16;

// The game's own Streamline DLSS-G. The swapchain NR composes on sits below Streamline, so its presents
// arrive after interpolation and most are generated frames: there is no single finished picture.
// `fgInputDlssg` / `fgOutputDlssg`: OptiScaler's FG takes DLSS-G's inputs / outputs DLSS-G itself; either
// keeps the game's DLSS-G from loading (hkslInit). `gameModeOn`: the last slDLSSGSetOptions left DLSS-G on.
// `presentsSinceEvaluate`: presents since the last DLSS-G evaluate seen (a stale count must not pause NR).
inline bool GameFrameGenerationOn(bool optiFgActive, bool fgInputDlssg, bool fgOutputDlssg, bool gameModeOn,
                                  uint64_t presentsSinceEvaluate)
{
    return !optiFgActive && !fgInputDlssg && !fgOutputDlssg &&
           (gameModeOn || presentsSinceEvaluate <= DlssgEvaluateWindow);
}

// Whether a slot's last GPU work touched the back buffer. Arm leaves done == ready (guide copy, signalled
// on the producer queue); a compose signals ready + 1 on the present queue; a discarded recording ready - 1.
inline bool TouchedBackBuffer(uint64_t ready, uint64_t done) { return done > ready; }
} // namespace DlssNr
