#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include "DlssNr_GuidedShaping.h"
#include "DlssNr_InterPassSchedule.h"

namespace DlssNr::SuperiorScaling
{
// Runtime state, defined with the D3D12 pipeline (DlssNr_Dx12.cpp). Status: the last thing worth telling the user.
std::string Status();
// Why Superior is not running this session (empty while it runs); the ordinary path uses Matched residual meanwhile.
// Selecting Superior again retries. SrUnavailable: the NGX DLSS SR runtime itself could not be used here.
std::string Unavailable();
bool SrUnavailable();
// The game's NGX D3D12 runtime is shutting down: every NGX handle is about to be destroyed by NGX itself.
void NgxDx12Shutdown();
constexpr uint32_t Transfer = 11u;
// Where Superior cannot run (native Vulkan, no NGX DLSS SR, no depth), the ordinary path runs Matched residual.
constexpr uint32_t FallbackTransfer = 1u;
constexpr uint32_t ShaderFlag = 65536u;
constexpr uint32_t LowGroupHeight = 16u;
inline bool Enabled(uint32_t transfer) { return transfer == Transfer; }
template <class C> inline bool Enabled(const C& cfg) { return Enabled(cfg.DlssNrTransfer.value_or_default()); }
inline unsigned Preset(unsigned preset) { return preset == 4u || preset == 5u || preset == 11u ? preset : 1u; }
inline float GuidedRange(float value) { return std::isfinite(value) ? std::clamp(value, .00001f, 1.f) : .015f; }
inline float GuidedSpatial(float value) { return std::isfinite(value) ? std::clamp(value, .0001f, 64.f) : 3.f; }
inline bool CustomGuided(float range, float spatial)
{
    return GuidedRange(range) != .015f || GuidedSpatial(spatial) != 3.f;
}
// Resolve-only reuse of meter dimensions, absent from Superior's composition path.
// Separate tunable PSOs read the IEEE754 bits; defaults keep their original constant-folded PSOs.
template <class P> inline void SetGuided(P& p, float range, float spatial)
{
    range = GuidedRange(range);
    spatial = GuidedSpatial(spatial);
    range = 1.f / (range * range);
    spatial = 1.f / (spatial * spatial);
    std::memcpy(&p.ExposureSourceWidth, &range, sizeof(range));
    std::memcpy(&p.ExposureSourceHeight, &spatial, sizeof(spatial));
}
// -1 keeps the generic PSO. Specialization changes no user setting or colour calculation.
inline int NormalResolveVariant(bool rawAnswer, bool packed, unsigned passthrough, unsigned mapping, unsigned debugView,
                                unsigned compareMode, unsigned applyModel)
{
    if (rawAnswer || debugView || compareMode || !applyModel)
        return -1;
    const int variant = passthrough     ? (mapping == 0u ? 0 : -1)
                        : mapping == 0u ? 1
                        : mapping == 2u ? 2
                        : mapping == 4u ? 3
                        : mapping == 6u ? 4
                                        : -1;
    return variant < 0 ? -1 : variant + (packed ? 5 : 0);
}
// 310.8 has only Main. On 310.9, only A (1) selects Main; Default (0), B (2)
// and the legacy saved value 3 select Small. This is independent of SR's A/D/E/K.
inline bool MainOnlyRuntime(unsigned major, unsigned minor) { return major == 310u && minor < 9u; }
inline unsigned EffectiveNrPreset(unsigned requested, bool mainOnly) { return mainOnly || requested == 1u ? 1u : 2u; }
// Calibrated IDs: styles Standard/Natural/Cinematic; presets Default/A/B and legacy Small alias 3.
constexpr float UncalibratedHigh = .950f, UncalibratedLow = .850f;
inline bool Calibrated(unsigned style, unsigned nrPreset) { return style < 3u && nrPreset <= 3u; }
// The preset and style the model actually runs with: the same clamps as PassPreset/PassStyle (PassProfiles.h), so a
// hand-edited ini value above the range gets the calibrated gains of the model that really runs.
template <class C> inline unsigned GainPresetSelection(const C& cfg, unsigned pass)
{
    if (pass == 1 && cfg.DlssNrPass2Preset.has_value())
        return std::min(cfg.DlssNrPass2Preset.value(), 3u);
    if (pass == 2 && cfg.DlssNrPass3Preset.has_value())
        return std::min(cfg.DlssNrPass3Preset.value(), 3u);
    return std::min(cfg.DlssNrPreset.value_or_default(), 3u);
}
template <class C> inline unsigned GainStyleSelection(const C& cfg, unsigned pass)
{
    if (pass == 1 && cfg.DlssNrPass2Style.has_value())
        return std::min(cfg.DlssNrPass2Style.value(), 2u);
    if (pass == 2 && cfg.DlssNrPass3Style.has_value())
        return std::min(cfg.DlssNrPass3Style.value(), 2u);
    if (pass >= 3 && pass < 30 && cfg.DlssNrExtraPasses[pass - 3].style.has_value())
        return std::min(cfg.DlssNrExtraPasses[pass - 3].style.value(), 2u);
    return std::min(cfg.DlssNrStyle.value_or_default(), 2u);
}
inline float High(unsigned style, unsigned nrPreset = 1u, bool mainOnly = false)
{
    if (!Calibrated(style, nrPreset))
        return UncalibratedHigh;
    constexpr float main[] { .8f, .75f, .95f }, smallModel[] { .950f, .900f, .955f };
    return (EffectiveNrPreset(nrPreset, mainOnly) == 2u ? smallModel : main)[style];
}
inline float Low(unsigned style, unsigned nrPreset = 1u, bool mainOnly = false)
{
    if (!Calibrated(style, nrPreset))
        return UncalibratedLow;
    constexpr float main[] { .661f, .787f, .784f }, smallModel[] { .861f, .838f, .861f };
    return (EffectiveNrPreset(nrPreset, mainOnly) == 2u ? smallModel : main)[style];
}

inline float EffectiveBand(float p50, float scale, float structure, float intensity)
{
    const float exponent = std::log2(1.0f / std::clamp(scale, .25f, 1.0f));
    const float value = std::pow(std::max(0.0f, p50), exponent);
    return Profiles::ModelAdjustedGain(std::isfinite(value) ? value : 1.0f, structure, intensity);
}
// v59's exact requested raster; janblade's optional 16-pixel NR grid must not alter Superior.
inline unsigned WorkingExtent(unsigned native, float scale)
{
    return std::max(1u, static_cast<unsigned>(native * scale + .5f));
}
// Hysteresis for a continuously moving scale (Auto model resolution, dynamic resolution). Superior keeps its exact
// raster, but a new raster rebuilds the NR feature and the private DLSS generation, so the scale only moves when the
// ordinary path's own 16-pixel grid cell (the aligned extents, `bucket`) or the frame size changes.
struct ScaleHold
{
    unsigned nativeW = 0, nativeH = 0, bucketW = 0, bucketH = 0;
    float scale = 0.0f;
    float Hold(unsigned w, unsigned h, unsigned alignedW, unsigned alignedH, float requested)
    {
        if (scale <= 0.0f || w != nativeW || h != nativeH || alignedW != bucketW || alignedH != bucketH)
        {
            nativeW = w;
            nativeH = h;
            bucketW = alignedW;
            bucketH = alignedH;
            scale = requested;
        }
        return scale;
    }
};
// Isotropic gain tables use one global linear scale, including actual raster rounding.
inline float ResolutionScale(unsigned nativeW, unsigned nativeH, unsigned modelW, unsigned modelH)
{
    if (!nativeW || !nativeH || !modelW || !modelH)
        return 1.0f;
    return float(std::sqrt((double(modelW) * modelH) / (double(nativeW) * nativeH)));
}
struct FrequencyScales
{
    float high = 1.0f, low = 1.0f;
};
// Compress screen edges changes only low's scale. High/mid retain the ordinary Model resolution raster.
inline FrequencyScales MakeFrequencyScales(unsigned nativeW, unsigned nativeH, unsigned ordinaryW, unsigned ordinaryH,
                                           unsigned modelW, unsigned modelH)
{
    return { ResolutionScale(nativeW, nativeH, ordinaryW, ordinaryH),
             ResolutionScale(nativeW, nativeH, modelW, modelH) };
}
inline InterPass::ScheduleGains BandGains(unsigned style, unsigned preset, bool mainOnly, FrequencyScales scales,
                                          float structure, float intensity)
{
    return { EffectiveBand(High(style, preset, mainOnly), scales.high, structure, intensity),
             EffectiveBand(Low(style, preset, mainOnly), scales.low, structure, intensity) };
}
inline bool NeedsTemporal(unsigned nativeW, unsigned nativeH, unsigned ordinaryW, unsigned ordinaryH, bool compressed,
                          unsigned modelW, unsigned modelH)
{
    if (!nativeW || !nativeH || !ordinaryW || !ordinaryH || !modelW || !modelH)
        return false;
    return ordinaryW < nativeW || ordinaryH < nativeH ||
           (compressed && ordinaryW == nativeW && ordinaryH == nativeH && (modelW < nativeW || modelH < nativeH));
}
// Missing render/output metadata falls back to the original valid motion region, before any resample.
inline float MotionScale(float gameScale, unsigned motionExtent, unsigned referenceExtent,
                         unsigned validSourceExtent = 0)
{
    referenceExtent = referenceExtent ? referenceExtent : validSourceExtent;
    return referenceExtent ? gameScale * (float(motionExtent) / float(referenceExtent)) : gameScale;
}

template <class C> inline bool InputEncode(const C& c, uint64_t sourceWidth, uint32_t sourceHeight)
{
    return c.Mode == 0u && c.DirectResolveFlags == ShaderFlag && c.Passthrough <= 1u && c.ReversibleMode <= 4u &&
           c.Width != 0u && c.Height != 0u && sourceWidth == c.Width && sourceHeight == c.Height;
}

// Fixed 64x64 meter and 1x1 white reduction, only for Superior's automatic HDR exposure.
template <class C> inline uint32_t ExposureStage(const C& c, uint64_t sourceWidth, uint32_t sourceHeight)
{
    if (c.DirectResolveFlags != ShaderFlag || c.ExposureSourceWidth == 0u || c.ExposureSourceHeight == 0u)
        return 0u;
    if (c.Mode == 3u && c.Width == 64u && c.Height == 64u && sourceWidth == c.ExposureSourceWidth &&
        sourceHeight == c.ExposureSourceHeight)
        return 1u;
    if (c.Mode == 11u && c.Width == 1u && c.Height == 1u && sourceWidth == 64u && sourceHeight == 64u)
        return 2u;
    return 0u;
}

// Area at the model input only, with the materialized native proxy retained.
template <class C> inline bool AreaDownsample(const C& c, uint64_t sourceWidth, uint32_t sourceHeight)
{
    return c.Mode == 2u && c.Transfer == 0u && c.DirectResolveFlags == ShaderFlag && c.Width != 0u && c.Height != 0u &&
           sourceWidth >= c.Width && sourceHeight >= c.Height && (sourceWidth != c.Width || sourceHeight != c.Height);
}

// Final low field of a decoded signed residual, at the existing ceil(work/8) dimensions.
template <class C> inline bool LowResidual(const C& c, uint64_t sourceWidth, uint32_t sourceHeight)
{
    return c.Mode == 18u && c.Transfer == 1u && c.DirectResolveFlags == ShaderFlag && sourceWidth != 0u &&
           sourceHeight != 0u && c.Width == (sourceWidth - 1u) / 8u + 1u && c.Height == (sourceHeight - 1u) / 8u + 1u;
}

// Reuse only the identical current-frame R32 point-sampled depth, already in SRV state.
template <class R>
inline bool ReuseDepth(bool sameSource, bool r32, uint64_t width, uint32_t height, uint32_t workWidth,
                       uint32_t workHeight, const R& nr, const R& dlaa)
{
    return sameSource && r32 && workWidth != 0u && workHeight != 0u && width == workWidth && height == workHeight &&
           nr.valid() && dlaa.valid() && nr.x == dlaa.x && nr.y == dlaa.y && nr.width == dlaa.width &&
           nr.height == dlaa.height;
}

// Mode 1 / signed Transfer 9 only. The optimized PSO fixes inspector switches and actual reduction.
template <class C> inline bool NormalResolve(const C& c, uint64_t sourceWidth, uint32_t sourceHeight)
{
    return c.Mode == 1u && c.ResidualHistoryValid == 1u && c.Transfer == 9u && c.DebugView == 0u &&
           c.CompareMode == 0u && c.ShowSkinMask == 0u && c.ApplyModel != 0u && c.DirectResolveFlags == ShaderFlag &&
           sourceWidth != 0u && sourceHeight != 0u && sourceWidth <= c.Width && sourceHeight <= c.Height &&
           (sourceWidth != c.Width || sourceHeight != c.Height);
}

// Called after NormalResolve eligibility; only HDR soft knee has this literal PSO.
template <class C> inline bool SoftKneeResolve(const C& c) { return c.Passthrough == 0u && c.ReversibleMode == 0u; }

// Called after NormalResolve eligibility. Zero retains the general normal PSO.
template <class C> inline uint32_t NormalColorProfile(const C& c)
{
    // Retain v50 rounding in the optional replace-mode native-detail restoration.
    if ((c.ReversibleMode == 2u || c.ReversibleMode == 4u) && c.ModelWorkScale > 0.0f && c.ModelWorkScale < .999f &&
        c.ReplaceDetailStrength > 0.0f)
        return 0u;
    if (c.ReversibleMode > 4u)
        return 0u;
    if (c.Passthrough == 1u)
        return 1u; // SDR; its mapping selector remains dynamic.
    if (c.Passthrough != 0u)
        return 0u;
    // HDR soft knee is tried separately, with this general PSO retained as fallback.
    return c.ReversibleMode == 2u ? 2u : c.ReversibleMode == 4u ? 3u : 0u;
}
} // namespace DlssNr::SuperiorScaling
