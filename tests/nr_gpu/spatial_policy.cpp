#include "pch.h"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Spatial.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"

void require(bool value, int line)
{
    if (!value)
        throw std::runtime_error("Spatial metadata/gain policy failed at line " + std::to_string(line));
}
#define require(value) require(value, __LINE__)
int main()
{
    try
    {
        namespace Sp = DlssNr::Spatial;
        namespace Ss = DlssNr::SuperiorScaling;
        Sp::Settings settings;
        settings.enabled = true;
        unsigned checks = 0;
        for (const auto size : { DlssNr::GuideExtent { 3840, 2160 }, { 1919, 1079 } })
            for (float scale : { .5f, .59f, .65f, .75f, 1.0f, 1.5f })
            {
                const auto layout = Sp::Build(settings, size.width, size.height, scale, false);
                require(layout.active);
                require(layout.ordinaryW == Ss::WorkingExtent(size.width, scale));
                require(layout.ordinaryH == Ss::WorkingExtent(size.height, scale));
                const auto gainScale = Ss::ResolutionScale(size.width, size.height, layout.modelW, layout.modelH);
                const double expectedPixels =
                    double(layout.modelW) * layout.modelH / (double(size.width) * size.height);
                require(std::abs(double(gainScale) * gainScale - expectedPixels) < 1e-6);
                const auto guides = DlssNr::ResolveGuideRegions({ 2048, 1200 }, { 4096, 2304 }, { 1920, 1080 },
                                                                { 3840, 2160 }, true, 16, 8, 64, 32);
                for (auto reference : { DlssNr::GuideExtent { 1920, 1080 }, { 3840, 2160 }, { 0, 0 } })
                {
                    const auto constants =
                        Sp::MakeEnlargementGuideConstants(true, layout, guides, reference, 1920, -1080);
                    const auto legacy =
                        Sp::MakeEnlargementGuideConstants(false, layout, guides, reference, 1920, -1080);
                    const auto original = Sp::MakeConstants(layout, 101, guides, 1920, -1080);
                    require(std::memcmp(&legacy, &original, sizeof(legacy)) == 0);
                    require(legacy.motionScale[0] == 1920 && legacy.motionScale[1] == -1080);
                    const auto rw = reference.width ? reference.width : guides.motion.width;
                    const auto rh = reference.height ? reference.height : guides.motion.height;
                    require(std::abs(constants.motionScale[0] - 1920.f * size.width / rw) < .001f);
                    require(std::abs(constants.motionScale[1] + 1080.f * size.height / rh) < .001f);
                    require(constants.depthRect[0] == 16 && constants.depthRect[1] == 8);
                    require(constants.motionRect[0] == 64 && constants.motionRect[1] == 32);
                    ++checks;
                }
                ++checks;
            }
        // Declaration controls vector units even when the available subrect is clipped.
        const auto native = Sp::Build(settings, 3840, 2160, 1.f, false);
        require(Ss::NeedsTemporal(3840, 2160, 3840, 2160, true, native.modelW, native.modelH));
        require(!Ss::NeedsTemporal(3840, 2160, 3840, 2160, false, native.modelW, native.modelH));
        require(!Ss::NeedsTemporal(3840, 2160, 3840, 2160, true, 3840, 2160));
        require(Ss::NeedsTemporal(3840, 2160, 3840, 2160, true, 3840, 1936)); // one compressed axis
        require(Ss::NeedsTemporal(3840, 2160, 2496, 1404, false, 2496, 1404));
        require(Ss::NeedsTemporal(3840, 2160, 2496, 1404, true, 2240, 1264));
        require(!Ss::NeedsTemporal(3840, 2160, 5760, 3240, true, 5184, 2928)); // supersampling is unchanged
        require(!Ss::NeedsTemporal(0, 2160, 3840, 2160, true, 3456, 1952));
        const DlssNr::GuideRegions clipped { { 20, 7, 1234, 700 }, { 3, 5, 1888, 1060 } };
        const auto c = Sp::MakeGuideConstants(native, clipped, { 1920, 1080 }, 1920, -1080);
        require(c.motionScale[0] == 3840 && c.motionScale[1] == -2160);
        const auto pre = Sp::Build(settings, 1920, 1080, 1.f, false);
        const auto preC = Sp::MakeGuideConstants(pre, clipped, { 3840, 2160 }, 3840, -2160);
        require(preC.motionScale[0] == 1920 && preC.motionScale[1] == -1080);
        const auto p65 = Sp::Build(settings, 3840, 2160, .65f, false);
        require(p65.ordinaryW == 2496 && p65.ordinaryH == 1404);
        require(Ss::ResolutionScale(3840, 2160, p65.modelW, p65.modelH) < .65f);
        require(Ss::ResolutionScale(3840, 2160, native.modelW, native.modelH) < 1.f);
        Sp::Settings asymmetric = settings;
        asymmetric.workX = 85;
        asymmetric.centerX = 70;
        asymmetric.workY = 95;
        const auto both = Sp::Build(asymmetric, 3840, 2160, .65f, false);
        require(both.active);
        const auto equivalent = Ss::ResolutionScale(3840, 2160, both.modelW, both.modelH);
        require(std::abs(equivalent - std::sqrt((both.modelW / 3840.f) * (both.modelH / 2160.f))) < 1e-6f);
        require(Ss::EffectiveBand(.8f, equivalent, 1, 1) < Ss::EffectiveBand(.8f, .65f, 1, 1));
        unsigned bandChecks = 0;
        for (const auto size : { DlssNr::GuideExtent { 3840, 2160 }, { 1919, 1079 } })
            for (float scale : { .5f, .65f, 1.f, 1.5f })
                for (int preset = 0; preset < 4; ++preset)
                {
                    auto settingsForBands = asymmetric;
                    if (preset < 3)
                        Sp::ApplyNamedLayout(settingsForBands, preset);
                    const auto l = Sp::Build(settingsForBands, size.width, size.height, scale, false);
                    const auto scales =
                        Ss::MakeFrequencyScales(size.width, size.height, l.ordinaryW, l.ordinaryH, l.modelW, l.modelH);
                    const auto plain = Ss::MakeFrequencyScales(size.width, size.height, l.ordinaryW, l.ordinaryH,
                                                               l.ordinaryW, l.ordinaryH);
                    require(scales.high == plain.high && scales.low < scales.high);
                    require(scales.low == Ss::ResolutionScale(size.width, size.height, l.modelW, l.modelH));
                    for (unsigned style : { 0u, 1u, 2u, 99u })
                        for (unsigned model : { 0u, 1u, 2u, 3u, 99u })
                            for (bool mainOnly : { false, true })
                                for (float structure : { 0.f, .75f, 1.f })
                                    for (float intensity : { 0.f, .5f, 1.f })
                                    {
                                        const auto g =
                                            Ss::BandGains(style, model, mainOnly, scales, structure, intensity);
                                        const auto uncompressed =
                                            Ss::BandGains(style, model, mainOnly, plain, structure, intensity);
                                        require(g.high == uncompressed.high);
                                        require(g.low == Ss::EffectiveBand(Ss::Low(style, model, mainOnly), scales.low,
                                                                           structure, intensity));
                                        require(uncompressed.high == Ss::EffectiveBand(Ss::High(style, model, mainOnly),
                                                                                       plain.high, structure,
                                                                                       intensity));
                                        require(uncompressed.low == Ss::EffectiveBand(Ss::Low(style, model, mainOnly),
                                                                                      plain.high, structure,
                                                                                      intensity));
                                        if (scale == 1.f || structure == 0.f || intensity == 0.f)
                                            require(g.high == 1.f);
                                        ++bandChecks;
                                    }
                    // Mixed models/styles: the scheduler, interpass block and final tail use the same split scales.
                    const auto plan = DlssNr::InterPass::MakeGainSchedule(
                        6, true, true, false,
                        [&](unsigned p) { return Ss::BandGains(p % 3, p % 2 + 1, false, scales, .75f, .5f); });
                    for (unsigned first = 0; first < 6; ++first)
                        for (unsigned end = first + 1; end <= 6; ++end)
                        {
                            double high = 1, low = 0;
                            for (unsigned p = first; p < end; ++p)
                            {
                                high *= Ss::EffectiveBand(Ss::High(p % 3, p % 2 + 1), plain.high, .75f, .5f);
                                low += Ss::EffectiveBand(Ss::Low(p % 3, p % 2 + 1), scales.low, .75f, .5f);
                            }
                            const auto block = plan.Block(first, end);
                            require(block.high == float(high) && block.low == float(low / (end - first)));
                        }
                }
        unsigned minifyChecks = 0;
        for (auto dim : { DlssNr::GuideExtent { 320, 192 }, { 321, 193 }, { 3840, 2160 } })
            for (float scale : { .25f, .4f, .5f, .65f, 1.f, 1.25f })
                for (bool packed : { false, true })
                    for (float density : { .25f, .5f, 1.f, 2.f })
                    {
                        auto layout = Sp::Build(settings, dim.width, dim.height, scale, false);
                        if (packed && !layout.active)
                            continue;
                        if (!packed)
                        {
                            layout.modelW = layout.ordinaryW;
                            layout.modelH = layout.ordinaryH;
                            layout.warp.x = pw::BuildAxis(dim.width, dim.width, layout.modelW, 1, 0, 0);
                            layout.warp.y = pw::BuildAxis(dim.height, dim.height, layout.modelH, 1, 0, 0);
                        }
                        const DlssNr::GuideRegions regions { { 3, 7, dim.width, dim.height },
                                                             { 5, 11, uint32_t(dim.width * density),
                                                               uint32_t(dim.height * density) } };
                        auto c = Sp::MakeGuideConstants(layout, regions, { dim.width, dim.height }, -2, 3);
                        c.mode = packed ? 101u : 104u;
                        auto disabled = c;
                        disabled.padding[0] = 0;
                        require(!Sp::PrepareMinifiedMotion(disabled).pixels);
                        c.padding[0] = 1;
                        const auto plan = Sp::PrepareMinifiedMotion(c);
                        require(plan.pixels <= c.width * c.height);
                        const uint32_t left = uint32_t(c.padding[2]), right = uint32_t(c.padding[3]);
                        const uint32_t top = uint32_t(c.padding[4]), bottom = uint32_t(c.padding[5]);
                        require(left + right <= c.width && top + bottom <= c.height);
                        if (plan.pixels)
                        {
                            require(plan.groupsX <= 65535 && plan.groupsY <= 65535);
                            require(uint64_t(plan.groupsX) * plan.groupsY * 64 >= plan.pixels);
                        }
                        for (uint32_t x = 0; x < c.width; ++x)
                        {
                            const auto& a = layout.warp.x;
                            const bool inCenter = packed && float(x) >= a.workCenter - a.halfBand * a.scale &&
                                                  float(x + 1) <= a.workCenter + a.halfBand * a.scale;
                            const float span = inCenter ? 1.f / a.scale
                                               : packed ? pw::Unpack(float(x + 1), a) - pw::Unpack(float(x), a)
                                                        : float(dim.width) / c.width;
                            if (span * regions.motion.width / dim.width > 1.0001f)
                                require(plan.pixels && (x < left || x >= c.width - right));
                        }
                        for (uint32_t y = 0; y < c.height; ++y)
                        {
                            const auto& a = layout.warp.y;
                            const bool inCenter = packed && float(y) >= a.workCenter - a.halfBand * a.scale &&
                                                  float(y + 1) <= a.workCenter + a.halfBand * a.scale;
                            const float span = inCenter ? 1.f / a.scale
                                               : packed ? pw::Unpack(float(y + 1), a) - pw::Unpack(float(y), a)
                                                        : float(dim.height) / c.height;
                            if (span * regions.motion.height / dim.height > 1.0001f)
                                require(plan.pixels && (y < top || y >= c.height - bottom));
                        }
                        ++minifyChecks;
                    }
        std::cout << "Minified MV dispatch coverage PASS: " << minifyChecks
                  << " layouts; no-depth, 25/40/50/65/100/125%, offset MV rectangles and 4K\n";
        std::cout << "Spatial metadata and final-resolution gain policy PASS: " << checks << " cases; " << bandChecks
                  << " split high/low gain cases, mixed blocks and final tails; "
                  << "native/pre/post-SR, clipped/offset guides, exact Superior grid and both packed axes\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
