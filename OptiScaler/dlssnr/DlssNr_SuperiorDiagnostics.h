#pragma once
#include "DlssNr_SuperiorScaling.h"
#include <optional>

namespace DlssNr::SuperiorScaling::Diagnostics
{
constexpr uint32_t MvUnits = 1u, InterPass = 2u, Carrier = 4u, Guided = 8u, FinalShaping = 16u, CompressionLow = 32u,
                   WarpCache = 64u, FullGuided = 128u, PointMotion = 256u, SurfaceMotion = 512u, JointDepth = 1024u;
inline std::optional<uint32_t> LoadMotionMethod(std::optional<uint32_t> method, std::optional<bool> legacyPoint)
{
    if (method && *method <= 2u)
        return method;
    if (legacyPoint)
        return *legacyPoint ? 0u : 1u;
    return {};
}
template <class C> inline uint32_t MotionMethod(const C& c)
{
    const auto method = c.DlssNrSuperiorMotionMethod.value_or_default();
    return method <= 2u ? method : 2u;
}
inline uint32_t MotionMethodFromMask(uint32_t mask) { return mask & PointMotion ? 0u : mask & SurfaceMotion ? 2u : 1u; }
template <class C> inline uint32_t Mask(const C& c)
{
    return (c.DlssNrSuperiorSkipMvUnits.value_or_default() ? MvUnits : 0u) |
           (c.DlssNrSuperiorSkipInterPass.value_or_default() ? InterPass : 0u) |
           (c.DlssNrSuperiorSkipCarrier.value_or_default() ? Carrier : 0u) |
           (c.DlssNrSuperiorSkipGuided.value_or_default() ? Guided : 0u) |
           (c.DlssNrSuperiorSkipFinalShaping.value_or_default() ? FinalShaping : 0u) |
           (c.DlssNrSuperiorSkipCompressionLow.value_or_default() ? CompressionLow : 0u) |
           (c.DlssNrSuperiorSkipWarpCache.value_or_default() ? WarpCache : 0u) |
           (c.DlssNrSuperiorFullGuided.value_or_default() ? FullGuided : 0u) |
           (MotionMethod(c) == 2u && c.DlssNrSuperiorReconstructDepth.value_or_default() ? JointDepth : 0u) |
           (MotionMethod(c) == 0u   ? PointMotion
            : MotionMethod(c) == 2u ? SurfaceMotion
                                    : 0u);
}
inline bool PrivateDlss(bool requested, uint32_t mask) { return requested && !(mask & Carrier); }
inline FrequencyScales Scales(FrequencyScales scales, uint32_t mask)
{
    if (mask & CompressionLow)
        scales.low = scales.high;
    return scales;
}
inline InterPass::ScheduleGains FinalGains(InterPass::ScheduleGains gains, uint32_t mask)
{
    return mask & FinalShaping ? InterPass::ScheduleGains {} : gains;
}
inline bool CachedWarp(uint32_t mask) { return !(mask & (WarpCache | Guided)); }
inline bool NativeCenter(uint32_t mask) { return !(mask & (FullGuided | Guided)); }
inline bool ContinuousMotion(uint32_t mask) { return !(mask & PointMotion); }
} // namespace DlssNr::SuperiorScaling::Diagnostics
