#pragma once

// Compress screen edges (ini keys Spatial*). NR works on a smaller picture: the middle of the frame stays 1:1 and the
// edges are squeezed into the rest of the space, then the answer is stretched back. This file is the part that does
// not need a GPU: the settings, the layout they make for a frame size, the limits the menu and the runtime share, and
// the constants the pack / unpack shaders read.
//
// The mapping itself (external/peripheral_warp/SpatialWarp.h) and the shader that applies it
// (precompile/dlssnr_spatial.hlsl) are adapted from PeripheralWarp 64902dd6 (MIT; see
// external/peripheral_warp/LICENSE), by way of the wilsjo2 fork's v0.8.9. The layout rules below differ from theirs in
// one place: the packed picture is a multiple of 16 on each axis (the model's attention window), and at 100% model
// resolution the middle band is kept exactly 1:1 whatever that rounding does, with the edges taking up the difference.

#include "DlssNr_Guides.h"
#include "../../../external/peripheral_warp/SpatialWarp.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace DlssNr::Spatial {

// Why compression is or is not applied. The runtime publishes this; the menu owns the words.
enum class Status : uint8_t
{
    Off,                // not asked for (or no frame has run since it was asked for)
    Active,             // NR works on the packed picture
    Proxy,              // the driver-proxy backend runs one pass of its own: compression stays off
    BadSettings,        // a value is outside its range (a hand-edited ini)
    TooSmall,           // the packed picture would be under a quarter of the frame on an axis
    NothingToCompress,  // the working size is 100% on both axes
    ThinEdge,           // the layout leaves less than one pixel on one side, or the middle is as wide as the working size
    TurnedOffResources, // the shader or a texture could not be created
    TurnedOffDispatch,  // a compute pass failed
    TurnedOffModel,     // NR rejected the packed picture
};

// The grid the model wants: its attention windows are 16 input pixels wide after the 2x2 pooling. A size that is
// already the native size is left alone, and shrinking never rounds up past native (the same rule the ordinary
// reduced size follows, DlssNr_Dx12.cpp AlignWorkSize, which calls this).
inline uint32_t AlignedExtent(uint32_t size, uint32_t native)
{
    constexpr uint32_t kGrid = 16;
    if (size == native)
        return size;
    uint32_t aligned = (size + kGrid / 2) / kGrid * kGrid;
    if (aligned < kGrid)
        aligned = kGrid;
    if (size < native && aligned > native)
        aligned = native;
    return aligned;
}

struct Settings {
    bool enabled = false;
    float centerX = 80, centerY = 80;
    float workX = 90, workY = 90;
    float offsetX = 0, offsetY = 0;
    float shiftX = 0, shiftY = 0;
    bool operator==(const Settings& o) const {
        const auto same = [](float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); };
        return enabled == o.enabled && same(centerX, o.centerX) && same(centerY, o.centerY) &&
               same(workX, o.workX) && same(workY, o.workY) && same(offsetX, o.offsetX) &&
               same(offsetY, o.offsetY) && same(shiftX, o.shiftX) && same(shiftY, o.shiftY);
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};

template<class ConfigLike> Settings ReadSettings(const ConfigLike& cfg) {
    Settings s;
    s.enabled = cfg.DlssNrSpatialCompression.value_or_default();
    s.centerX = cfg.DlssNrSpatialCenterX.value_or_default();
    s.centerY = cfg.DlssNrSpatialCenterY.value_or_default();
    s.workX = cfg.DlssNrSpatialWorkX.value_or_default();
    s.workY = cfg.DlssNrSpatialWorkY.value_or_default();
    s.offsetX = cfg.DlssNrSpatialOffsetX.value_or_default();
    s.offsetY = cfg.DlssNrSpatialOffsetY.value_or_default();
    s.shiftX = cfg.DlssNrSpatialShiftX.value_or_default();
    s.shiftY = cfg.DlssNrSpatialShiftY.value_or_default();
    return s;
}

// The named layouts the menu offers (centre / working size, both axes, no offset, no shift).
struct NamedLayout { const char* name; float center, work; };
inline constexpr NamedLayout kNamedLayouts[3] = { { "Light", 85.0f, 95.0f },
                                                  { "Balanced", 80.0f, 90.0f },
                                                  { "Strong", 70.0f, 80.0f } };
inline constexpr int kDefaultLayout = 1;

// Which named layout these settings are exactly, or -1 for a custom one.
inline int MatchedLayout(const Settings& s)
{
    for (int i = 0; i != 3; ++i) {
        const NamedLayout& n = kNamedLayouts[i];
        if (s.centerX == n.center && s.centerY == n.center && s.workX == n.work && s.workY == n.work &&
            s.offsetX == 0 && s.offsetY == 0 && s.shiftX == 0 && s.shiftY == 0)
            return i;
    }
    return -1;
}

inline void ApplyNamedLayout(Settings& s, int index)
{
    const NamedLayout& n = kNamedLayouts[std::clamp(index, 0, 2)];
    s.centerX = s.centerY = n.center;
    s.workX = s.workY = n.work;
    s.offsetX = s.offsetY = 0;
    s.shiftX = s.shiftY = 0;
}

inline float MaxCenterOffset(float centerPercent) {
    return std::max(0.0f, (100.0f - centerPercent) * .5f - .5f);
}

// The smallest working size (percent of the frame) that keeps the packed picture at a quarter of the frame, for the
// model resolution NR actually applies. The 16-pixel rounding can still land a little under it; the runtime says so.
inline float MinimumWorkPercent(float globalScale) {
    return std::isfinite(globalScale) && globalScale > 0
        ? std::clamp(25.0f / globalScale, 25.0f, 100.0f) : 100.0f;
}

// How far the edge balance may move, in percent of the frame. Positive moves pixels from the left (top) edge to the
// right (bottom) edge: BuildAxis gives side 0 (left/top) allotted - shift and side 1 (right/bottom) allotted + shift.
inline std::pair<float, float> WorkShiftLimits(float centerPercent, float workPercent, float offsetPercent) {
    const float c = centerPercent * .01f, w = workPercent * .01f;
    const float halfBand = c * .5f, bandCenter = .5f + offsetPercent * .01f;
    const float periphery[2] = {std::max(0.0f, bandCenter - halfBand),
                                std::max(0.0f, 1.0f - bandCenter - halfBand)};
    const float budget = std::max(0.0f, w - c);
    const int narrow = periphery[0] <= periphery[1] ? 0 : 1;
    float base[2] = {};
    base[narrow] = std::min(.5f * budget, periphery[narrow]);
    base[1 - narrow] = std::min(budget - base[narrow], periphery[1 - narrow]);
    return {-100.0f * std::max(0.0f, std::min(base[1], periphery[0] - base[0])),
             100.0f * std::max(0.0f, std::min(base[0], periphery[1] - base[1]))};
}

inline std::pair<float, float> WorkShiftLimits(const Settings& s, bool yAxis) {
    return yAxis ? WorkShiftLimits(s.centerY, s.workY, s.offsetY)
                 : WorkShiftLimits(s.centerX, s.workX, s.offsetX);
}

// Brings every value a user edit can reach inside its range, for the menu and for the model resolution changing under
// it. A hand-edited ini is not repaired: the runtime reports it (Status::BadSettings) and carries on without
// compression.
inline void Constrain(Settings& value, float globalScale)
{
    const float minimumWork = MinimumWorkPercent(globalScale);
    const auto axis = [&](float& center, float& work, float& offset) {
        center = std::clamp(std::isfinite(center) ? center : 80.0f, 1.0f, 99.5f);
        work = std::clamp(std::isfinite(work) ? work : 90.0f, std::max(minimumWork, center + 0.5f), 100.0f);
        const float limit = MaxCenterOffset(center);
        offset = std::clamp(std::isfinite(offset) ? offset : 0.0f, -limit, limit);
    };
    axis(value.centerX, value.workX, value.offsetX);
    axis(value.centerY, value.workY, value.offsetY);
    const auto x = WorkShiftLimits(value, false);
    const auto y = WorkShiftLimits(value, true);
    value.shiftX = std::clamp(std::isfinite(value.shiftX) ? value.shiftX : 0.0f, x.first, x.second);
    value.shiftY = std::clamp(std::isfinite(value.shiftY) ? value.shiftY : 0.0f, y.first, y.second);
}

// Fractions of the native frame (0..1), including offset and shift.
struct Rect { float left = 0, top = 0, right = 1, bottom = 1; };

struct Layout {
    Settings settings{};
    bool requested = false, active = false;
    Status status = Status::Off;
    uint32_t nativeW = 0, nativeH = 0;
    uint32_t ordinaryW = 0, ordinaryH = 0; // what NR works on without compression
    uint32_t modelW = 0, modelH = 0;       // what it works on with it (the same, when inactive)
    float globalScale = 1;
    pw::Layout warp{};
    Rect centerBounds{}, workBounds{};
    // The same geometry, not the same float: the applied scale of Auto model resolution drifts by a hair under dynamic
    // resolution, and a layout that comes out the same sizes is the same layout (nothing to rebuild or reset).
    bool operator==(const Layout& o) const {
        return settings == o.settings && nativeW == o.nativeW && nativeH == o.nativeH && status == o.status &&
               active == o.active && ordinaryW == o.ordinaryW && ordinaryH == o.ordinaryH && modelW == o.modelW &&
               modelH == o.modelH && (!active || (warp.x.rawExtent == o.warp.x.rawExtent &&
                                                  warp.y.rawExtent == o.warp.y.rawExtent));
    }
    bool operator!=(const Layout& o) const { return !(*this == o); }
};

inline uint32_t EvenExtent(uint32_t native, double fraction) {
    if (fraction == 1.0) return native;
    double value = static_cast<double>(native) * fraction;
    const double nearest = std::round(value);
    if (std::abs(value - nearest) <= 1e-6 * std::max(1.0, std::abs(value))) value = nearest;
    uint32_t result = static_cast<uint32_t>(std::ceil(value));
    if (result & 1u) ++result;
    if (fraction < 1.0) result = std::min(result, native & ~1u);
    return std::max(2u, result);
}

// gridOrdinary: whether the ordinary (uncompressed) size is rounded to the 16-pixel grid too, as Direct3D 12's is.
inline Layout Build(const Settings& s, uint32_t nativeW, uint32_t nativeH, float globalScale, bool gridOrdinary = true) {
    Layout result{};
    result.settings = s;
    result.requested = s.enabled;
    result.nativeW = nativeW; result.nativeH = nativeH;
    result.globalScale = globalScale;
    const float ordinaryScale = std::isfinite(globalScale) ? std::clamp(globalScale, .25f, 2.0f) : 1.0f;
    const auto ordinary = [&](uint32_t native) {
        const uint32_t size = std::max(1u, static_cast<uint32_t>(native * ordinaryScale + 0.5f));
        return gridOrdinary ? AlignedExtent(size, native) : size;
    };
    result.ordinaryW = ordinary(nativeW);
    result.ordinaryH = ordinary(nativeH);
    result.modelW = result.ordinaryW; result.modelH = result.ordinaryH;
    if (!s.enabled) return result;
    if (nativeW < 2 || nativeH < 2 || !std::isfinite(globalScale) || globalScale < .25f || globalScale > 2.0f) {
        result.status = Status::BadSettings; return result;
    }
    const auto validAxis = [](float center, float work, float offset, float shift) {
        if (!std::isfinite(center) || !std::isfinite(work) || !std::isfinite(offset) || !std::isfinite(shift) ||
            center <= 0 || work < 25 || work > 100 || center >= work ||
            std::abs(offset) > MaxCenterOffset(center) + 1e-4f) return false;
        const auto limits = WorkShiftLimits(center, work, offset);
        return shift >= limits.first - 1e-4f && shift <= limits.second + 1e-4f;
    };
    if (!validAxis(s.centerX, s.workX, s.offsetX, s.shiftX) ||
        !validAxis(s.centerY, s.workY, s.offsetY, s.shiftY)) {
        result.status = Status::BadSettings; return result;
    }
    if (s.workX == 100 && s.workY == 100) {
        result.status = Status::NothingToCompress; return result;
    }
    const auto packed = [&](uint32_t native, float work) {
        const double size = std::round(static_cast<double>(native) * static_cast<double>(globalScale) * work * .01);
        return AlignedExtent(static_cast<uint32_t>(std::max(1.0, size)), native);
    };
    const uint32_t modelW = packed(nativeW, s.workX);
    const uint32_t modelH = packed(nativeH, s.workY);
    if (modelW < nativeW * .25f || modelH < nativeH * .25f) {
        result.status = Status::TooSmall; return result;
    }
    // At the native size (100% model resolution) the raw working extent is the packed extent itself: the middle band
    // then moves by whole texels and stays 1:1, and the edges take the rounding. Elsewhere the picture is resampled
    // anyway and the raw extent is just the working size.
    const uint32_t rawW = result.ordinaryW == nativeW ? modelW : EvenExtent(nativeW, s.workX * .01);
    const uint32_t rawH = result.ordinaryH == nativeH ? modelH : EvenExtent(nativeH, s.workY * .01);
    if (s.centerX * .01f * static_cast<float>(nativeW) >= static_cast<float>(rawW) ||
        s.centerY * .01f * static_cast<float>(nativeH) >= static_cast<float>(rawH)) {
        result.status = Status::ThinEdge; return result;
    }
    result.warp.x = pw::BuildAxis(nativeW, rawW, modelW, s.centerX * .01f, s.offsetX * .01f, s.shiftX * .01f);
    result.warp.y = pw::BuildAxis(nativeH, rawH, modelH, s.centerY * .01f, s.offsetY * .01f, s.shiftY * .01f);
    const auto& x = result.warp.x; const auto& y = result.warp.y;
    const auto invertible = [](const pw::Axis& axis) {
        // A peripheral side needs at least one raw working texel. At a shift endpoint the reference split may assign
        // zero; snapping can leave a fraction of one.
        return (axis.periphery[0] <= 0 || axis.allotted[0] >= 1.0f) &&
               (axis.periphery[1] <= 0 || axis.allotted[1] >= 1.0f);
    };
    if (!invertible(x) || !invertible(y)) {
        result.status = Status::ThinEdge; return result;
    }
    result.centerBounds = {(x.bandCenter - x.halfBand) / nativeW, (y.bandCenter - y.halfBand) / nativeH,
                           (x.bandCenter + x.halfBand) / nativeW, (y.bandCenter + y.halfBand) / nativeH};
    result.workBounds = {(x.bandCenter - x.halfBand - x.allotted[0]) / nativeW,
                         (y.bandCenter - y.halfBand - y.allotted[0]) / nativeH,
                         (x.bandCenter + x.halfBand + x.allotted[1]) / nativeW,
                         (y.bandCenter + y.halfBand + y.allotted[1]) / nativeH};
    result.modelW = modelW; result.modelH = modelH;
    result.active = true;
    result.status = Status::Active;
    return result;
}

// What the runtime publishes each frame for the menu and the on-screen layout: the status as a code, the sizes NR
// works on (packed, and what it would have been), the model resolution the layout was built for, and the boxes as
// fractions of the frame. The menu owns every word; the overlay draws exactly these boxes.
struct Published {
    Status status = Status::Off;
    bool vulkan = false;
    uint32_t nativeW = 0, nativeH = 0, ordinaryW = 0, ordinaryH = 0, modelW = 0, modelH = 0;
    float scale = 1;
    Rect center{}, work{};
};

inline Published Publish(const Layout& layout, Status status, bool vulkan)
{
    Published p;
    p.status = status;
    p.vulkan = vulkan;
    p.nativeW = layout.nativeW; p.nativeH = layout.nativeH;
    p.ordinaryW = layout.ordinaryW; p.ordinaryH = layout.ordinaryH;
    p.scale = layout.globalScale;
    if (status == Status::Active) {
        p.modelW = layout.modelW; p.modelH = layout.modelH;
        p.center = layout.centerBounds; p.work = layout.workBounds;
    }
    return p;
}

// A word for what the log says about a status (the menu has its own, longer ones).
inline const char* Describe(Status status)
{
    switch (status)
    {
    case Status::Off: return "off";
    case Status::Active: return "on";
    case Status::Proxy: return "not used with the driver-proxy backend";
    case Status::BadSettings: return "a setting is outside its range";
    case Status::TooSmall: return "the packed picture would be under a quarter of the frame";
    case Status::NothingToCompress: return "the working size is 100% on both axes";
    case Status::ThinEdge: return "the layout leaves less than one pixel at an edge";
    case Status::TurnedOffResources: return "its shader or textures could not be created";
    case Status::TurnedOffDispatch: return "a compute pass failed";
    case Status::TurnedOffModel: return "NR rejected the packed picture";
    }
    return "?";
}

// A running hash, for a backend to fold the formats and sizes of what it packs into one number.
inline uint64_t Mix(uint64_t hash, uint64_t value)
{
    return (hash ^ value) * 1099511628211ull + 0x9E3779B97F4A7C15ull;
}

// The frame-to-frame bookkeeping both backends share: this frame's layout, whether compression runs, and the fallback
// that holds it off after something went wrong. A change of layout, or of the formats and sizes that were packed (the
// backend's signature), makes last frame's packed picture a different one: the model's history starts over and a held
// fallback is lifted so the new layout gets its try. Retry lifts it too.
class Tracker
{
  public:
    struct Frame
    {
        Status status = Status::Off;
        bool active = false;       // the picture is packed this frame
        bool resetHistory = false; // the model's history starts over
        bool changed = false;      // the status, or the packed size, differs from the last frame's (for the log)
    };

    // backendOff: this backend does not take compression (the driver-proxy backend runs one pass of its own).
    Frame Begin(const Settings& settings, uint32_t nativeW, uint32_t nativeH, float globalScale, bool gridOrdinary,
                bool backendOff, uint64_t signature)
    {
        const Layout next = Build(settings, nativeW, nativeH, globalScale, gridOrdinary);
        Frame frame;

        if (valid_ && (layout_ != next || signature_ != signature))
        {
            // The model's history is the packed picture's: it starts over only when one was or is being packed.
            frame.resetHistory = layout_.active || next.active;
            fallback_ = Status::Off;
        }

        layout_ = next;
        signature_ = signature;
        valid_ = true;

        frame.status = next.status;

        if (settings.enabled && backendOff)
            frame.status = Status::Proxy;
        else if (next.active && fallback_ != Status::Off)
            frame.status = fallback_;

        frame.active = frame.status == Status::Active;
        const uint32_t w = frame.active ? next.modelW : 0, h = frame.active ? next.modelH : 0;
        frame.changed = frame.status != loggedStatus_ || w != loggedW_ || h != loggedH_;
        loggedStatus_ = frame.status;
        loggedW_ = w;
        loggedH_ = h;
        return frame;
    }

    void TurnOff(Status why) { fallback_ = why; }
    void Retry() { fallback_ = Status::Off; }
    void Reset() { *this = Tracker {}; }

    const Layout& layout() const { return layout_; }
    Status fallback() const { return fallback_; }

  private:
    Layout layout_{};
    Status fallback_ = Status::Off;
    uint64_t signature_ = 0;
    bool valid_ = false;
    Status loggedStatus_ = Status::Off;
    uint32_t loggedW_ = 0, loggedH_ = 0;
};

struct alignas(16) Constants {
    uint32_t mode = 0;
    float unused = 0;
    uint32_t width = 0, height = 0;
    pw::ShaderConstants warp{};
    // padding[0] is the depth-valid flag for Superior's new guide PSOs; original shaders ignore it.
    float depthRect[4] = {}, motionRect[4] = {}, motionScale[2] = {}, padding[6] = {};
};
static_assert(sizeof(Constants) == 256);
static_assert(offsetof(Constants, warp) == 16);

// Modes: 100 packs the encoded colour, 101 packs depth (point-sampled) and motion (by its end points), 102 unpacks the
// packed model input and the model's answer back to the ordinary grid, and 103 does that and adds what the round trip
// took from the model input (a third input: the picture the model would be shown without compression), for the Replace
// curves. mvX / mvY must already convert the game's motion units to this frame's native pixels. Use MakeGuideConstants
// below when starting with the game's DLSS MV scale.
inline Constants MakeConstants(const Layout& layout, uint32_t mode, const GuideRegions& regions, float mvX, float mvY) {
    Constants c{};
    c.mode = mode;
    c.width = mode >= 102 ? layout.ordinaryW : layout.modelW;
    c.height = mode >= 102 ? layout.ordinaryH : layout.modelH;
    c.warp = pw::MakeShaderConstants(layout.warp);
    c.depthRect[0] = static_cast<float>(regions.depth.x);
    c.depthRect[1] = static_cast<float>(regions.depth.y);
    c.depthRect[2] = static_cast<float>(regions.depth.width);
    c.depthRect[3] = static_cast<float>(regions.depth.height);
    c.motionRect[0] = static_cast<float>(regions.motion.x);
    c.motionRect[1] = static_cast<float>(regions.motion.y);
    c.motionRect[2] = static_cast<float>(regions.motion.width);
    c.motionRect[3] = static_cast<float>(regions.motion.height);
    c.motionScale[0] = mvX;
    c.motionScale[1] = mvY;
    return c;
}

// Conservative derivative bound: if false, not even the most compressed edge can
// shrink the source MV lattice. The minification shader checks actual pixel edges.
// Keep this independent of vector units and the source allocation's padding.
inline bool NeedsMinifiedMotion(const Constants& c)
{
    if (c.padding[0] <= .5f || !c.width || !c.height)
        return false;
    if (c.mode == 104u)
        return c.motionRect[2] > float(c.width) * 1.0001f || c.motionRect[3] > float(c.height) * 1.0001f;
    const auto& w = c.warp;
    const float minX = w.workScaleX * std::min(1.f, std::min(w.sideEdgeSlopeNegX, w.sideEdgeSlopePosX));
    const float minY = w.workScaleY * std::min(1.f, std::min(w.sideEdgeSlopeNegY, w.sideEdgeSlopePosY));
    return c.motionRect[2] > w.nativeWidth * minX * 1.0001f || c.motionRect[3] > w.nativeHeight * minY * 1.0001f;
}

struct MinifiedMotionDispatch
{
    uint32_t groupsX = 0, groupsY = 0, pixels = 0;
};

// Compact four disjoint edge bands into one dispatch. The center is not launched
// unless it also shrinks MV. No allocation or texture is needed for this work list.
inline MinifiedMotionDispatch PrepareMinifiedMotion(Constants& c)
{
    if (!NeedsMinifiedMotion(c))
        return {};
    const auto& w = c.warp;
    const auto axis = [&](bool y)
    {
        pw::Axis a {};
        a.nativeExtent = y ? w.nativeHeight : w.nativeWidth;
        a.workExtent = y ? float(c.height) : float(c.width);
        a.bandCenter = y ? w.bandCenterY : w.bandCenterX;
        a.workCenter = y ? w.workCenterY : w.workCenterX;
        a.scale = y ? w.workScaleY : w.workScaleX;
        a.halfSpan[0] = y ? w.halfSpanNegY : w.halfSpanNegX;
        a.halfSpan[1] = y ? w.halfSpanPosY : w.halfSpanPosX;
        a.center[0] = y ? w.sideCenterNegY : w.sideCenterNegX;
        a.center[1] = y ? w.sideCenterPosY : w.sideCenterPosX;
        a.work[0] = y ? w.sideWorkNegY : w.sideWorkNegX;
        a.work[1] = y ? w.sideWorkPosY : w.sideWorkPosX;
        a.compression[0] = y ? w.sideCompressionNegY : w.sideCompressionNegX;
        a.compression[1] = y ? w.sideCompressionPosY : w.sideCompressionPosX;
        a.edgeSlope[0] = y ? w.sideEdgeSlopeNegY : w.sideEdgeSlopeNegX;
        a.edgeSlope[1] = y ? w.sideEdgeSlopePosY : w.sideEdgeSlopePosX;
        return a;
    };
    const auto bands = [&](bool y) -> std::pair<uint32_t, uint32_t>
    {
        const uint32_t size = y ? c.height : c.width;
        const float source = c.motionRect[y ? 3 : 2];
        if (c.mode == 104u)
            return source > float(size) * 1.0001f ? std::make_pair(size, 0u) : std::make_pair(0u, 0u);
        const auto a = axis(y);
        const auto shrinking = [&](uint32_t i)
        {
            const float halfBand = a.center[0] * a.halfSpan[0] * a.scale;
            if (float(i) >= a.workCenter - halfBand && float(i + 1) <= a.workCenter + halfBand)
                return source > a.nativeExtent * a.scale * 1.0001f;
            return (pw::Unpack(float(i + 1), a) - pw::Unpack(float(i), a)) * source / a.nativeExtent > 1.f;
        };
        const uint32_t center = std::min(size - 1, uint32_t(std::max(0.f, std::floor(a.workCenter))));
        if (shrinking(center))
            return { size, 0 };
        uint32_t first = 0, last = 0;
        if (shrinking(0))
        {
            uint32_t lo = 0, hi = center;
            while (lo < hi)
            {
                const uint32_t mid = (lo + hi + 1) / 2;
                if (shrinking(mid))
                    lo = mid;
                else
                    hi = mid - 1;
            }
            first = std::min(size, lo + 3); // guard two texels against CPU/GPU mapping roundoff
        }
        if (shrinking(size - 1))
        {
            uint32_t lo = center, hi = size - 1;
            while (lo < hi)
            {
                const uint32_t mid = (lo + hi) / 2;
                if (shrinking(mid))
                    hi = mid;
                else
                    lo = mid + 1;
            }
            last = std::min(size - first, size - lo + 2);
        }
        return { first, last };
    };
    const auto x = bands(false), y = bands(true);
    const uint32_t pixels = (y.first + y.second) * c.width + (c.height - y.first - y.second) * (x.first + x.second);
    if (!pixels)
        return {};
    const uint32_t groups = (pixels + 63) / 64;
    const uint32_t groupsX = std::min(groups, 65535u), groupsY = (groups + groupsX - 1) / groupsX;
    c.padding[1] = float(groupsX * 64);
    c.padding[2] = float(x.first);
    c.padding[3] = float(x.second);
    c.padding[4] = float(y.first);
    c.padding[5] = float(y.second);
    return { groupsX, groupsY, pixels };
}

inline Constants MakeGuideConstants(const Layout& layout, const GuideRegions& regions, GuideExtent motionReference,
                                    float gameMvX, float gameMvY)
{
    // DLSS's scale describes pixels of its render/output reference, not necessarily
    // the colour frame NR edits. Convert into that frame before warping endpoints.
    // Neither the allocation nor depth's rectangle determines these units.
    const auto referenceW = motionReference.width ? motionReference.width : regions.motion.width;
    const auto referenceH = motionReference.height ? motionReference.height : regions.motion.height;
    const float x = referenceW ? gameMvX * (float(layout.nativeW) / float(referenceW)) : gameMvX;
    const float y = referenceH ? gameMvY * (float(layout.nativeH) / float(referenceH)) : gameMvY;
    return MakeConstants(layout, 101, regions, x, y);
}

// Explicit opt-in: other Enlargement modes retain janblade's original raw-scale constants.
inline Constants MakeEnlargementGuideConstants(bool superior, const Layout& layout, const GuideRegions& regions,
                                               GuideExtent reference, float mvX, float mvY)
{
    return superior ? MakeGuideConstants(layout, regions, reference, mvX, mvY)
                    : MakeConstants(layout, 101, regions, mvX, mvY);
}

} // namespace DlssNr::Spatial
