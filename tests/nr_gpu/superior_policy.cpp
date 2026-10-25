#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"
#include "../../OptiScaler/dlssnr/DlssNr_InterPassSchedule.h"
#include "../../OptiScaler/shaders/dlssnr/superior/Constants.h"
using namespace DlssNr::Superior;
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Guides.h"
#include <optional>

template<class T> struct Setting { T value; T value_or_default() const { return value; } };
struct GainSelections
{
    Setting<unsigned> DlssNrPreset { 0 }, DlssNrStyle { 0 };
    std::optional<unsigned> DlssNrPass2Preset, DlssNrPass3Preset, DlssNrPass2Style, DlssNrPass3Style;
    struct Extra
    {
        std::optional<unsigned> style;
    };
    std::array<Extra, 27> DlssNrExtraPasses {};
};
struct SavedSettings {
    Setting<uint32_t> DlssNrTransfer {11};
};
void require(bool condition) { if(!condition)throw std::runtime_error("Superior policy check failed"); }
int main() {
    try {
        using namespace DlssNr::SuperiorScaling;
        SavedSettings cfg;
        require(Enabled(cfg));
        for (bool packed : { false, true })
            for (unsigned mapping = 0; mapping < 12; ++mapping)
                for (unsigned pass : { 0u, 1u })
                {
                    const int normal = NormalResolveVariant(false, packed, pass, mapping, 0, 0, 1);
                    const bool supported =
                        pass ? mapping == 0 : mapping == 0 || mapping == 2 || mapping == 4 || mapping == 6;
                    require((normal >= 0) == supported);
                    if (supported)
                    {
                        require(normal == (pass ? 0 : int(mapping / 2 + 1)) + (packed ? 5 : 0));
                        require(NormalResolveVariant(true, packed, pass, mapping, 0, 0, 1) == -1);
                        require(NormalResolveVariant(false, packed, pass, mapping, 1, 0, 1) == -1);
                        require(NormalResolveVariant(false, packed, pass, mapping, 0, 1, 1) == -1);
                        require(NormalResolveVariant(false, packed, pass, mapping, 0, 0, 0) == -1);
                    }
                }
        require(High(0)==.8f && Low(0)==.661f && High(1)==.75f && Low(1)==.787f);
        require(High(2) == .95f && Low(2) == .784f && High(99) == .950f && Low(99) == .850f);
        require(High(0,2)==.950f && Low(0,2)==.861f);
        require(High(1, 2) == .900f && Low(1, 2) == .838f);
        require(High(2,2)==.955f && Low(2,2)==.861f);
        require(MainOnlyRuntime(310, 8) && !MainOnlyRuntime(310, 9));
        require(!MainOnlyRuntime(310, 10) && !MainOnlyRuntime(311, 0) && !MainOnlyRuntime(0, 0));
        const float mainHigh[] { .8f, .75f, .95f }, mainLow[] { .661f, .787f, .784f };
        const float smallHigh[] { .950f, .900f, .955f }, smallLow[] { .861f, .838f, .861f };
        for (unsigned requested : { 0u, 1u, 2u, 3u })
            for (unsigned style = 0; style < 3; ++style)
            {
                const bool useSmallModel = requested != 1u;
                require(EffectiveNrPreset(requested, false) == (useSmallModel ? 2u : 1u));
                require(High(style, requested) == (useSmallModel ? smallHigh[style] : mainHigh[style]));
                require(Low(style, requested) == (useSmallModel ? smallLow[style] : mainLow[style]));
                // 310.8 ignores every requested preset and always runs Main.
                require(EffectiveNrPreset(requested, true) == 1u);
                require(High(style, requested, true) == mainHigh[style]);
                require(Low(style, requested, true) == mainLow[style]);
                for (float scale : { .5f, .65f, 1.0f })
                {
                    require(EffectiveBand(High(style, 0), scale, .75f, .6f) ==
                            EffectiveBand(High(style, 2), scale, .75f, .6f));
                    require(EffectiveBand(Low(style, 0), scale, .75f, .6f) ==
                            EffectiveBand(Low(style, 2), scale, .75f, .6f));
                }
            }
        for (unsigned newStyle : { 3u, 4u, 99u, UINT32_MAX })
            for (unsigned preset : { 0u, 1u, 2u, 3u, 4u, 99u, UINT32_MAX })
                for (bool mainOnly : { false, true })
                    require(High(newStyle, preset, mainOnly) == .950f && Low(newStyle, preset, mainOnly) == .850f);
        for (unsigned newPreset : { 4u, 5u, 99u, UINT32_MAX })
            for (unsigned style : { 0u, 1u, 2u })
                for (bool mainOnly : { false, true })
                    require(High(style, newPreset, mainOnly) == .950f && Low(style, newPreset, mainOnly) == .850f);
        GainSelections future;
        future.DlssNrPreset.value = 7;
        future.DlssNrStyle.value = 4;
        future.DlssNrPass2Preset = 0;
        future.DlssNrPass2Style = 1;
        future.DlssNrPass3Preset = 1;
        future.DlssNrPass3Style = 9;
        future.DlssNrExtraPasses[0].style = 2;
        future.DlssNrExtraPasses[26].style = 8;
        // Out-of-range ini values are clamped exactly like PassPreset/PassStyle, so the gains follow the running model.
        require(GainPresetSelection(future, 0) == 3 && GainStyleSelection(future, 0) == 2);
        require(GainPresetSelection(future, 1) == 0 && GainStyleSelection(future, 1) == 1);
        require(High(GainStyleSelection(future, 1), GainPresetSelection(future, 1)) == .900f);
        require(GainPresetSelection(future, 2) == 1 && GainStyleSelection(future, 2) == 2);
        require(GainStyleSelection(future, 3) == 2 && GainPresetSelection(future, 3) == 3);
        require(GainStyleSelection(future, 29) == 2 && GainStyleSelection(future, 30) == 2);
        const auto futureSchedule =
            DlssNr::InterPass::MakeGainSchedule(3, true, true, false,
                                                [&](unsigned pass) -> DlssNr::InterPass::ScheduleGains
                                                {
                                                    const auto style = GainStyleSelection(future, pass),
                                                               preset = GainPresetSelection(future, pass);
                                                    return { High(style, preset), Low(style, preset) };
                                                });
        require(std::abs(futureSchedule.Block(0, 3).high - .955f * .900f * .95f) < 1e-6f);
        require(std::abs(futureSchedule.Block(0, 3).low - (.861f + .838f + .784f) / 3) < 1e-6f);
        require(EffectiveBand(.8f,.5f,1,1)==.8f);
        require(EffectiveBand(.8f,1,1,1)==1 && EffectiveBand(.8f,.5f,.75f,0)==1);
        require(std::abs(EffectiveBand(.8f,.5f,.5f,.5f)-.95f)<1e-6f);
        // Mixed NR models: each pass uses its own table, then block high is a product
        // and low an arithmetic mean. SR A/K never participates in this calculation.
        const auto mixed = DlssNr::InterPass::MakeGainSchedule(3, true, true, false,
            [](unsigned pass) -> DlssNr::InterPass::ScheduleGains {
                const unsigned style = pass == 2 ? 2u : 1u;
                const unsigned model = pass == 1 ? 2u : 1u;
                return { EffectiveBand(High(style,model),.5f,1,1),
                         EffectiveBand(Low(style,model),.5f,1,1) };
            });
        require(std::abs(mixed.Block(0, 2).high - .75f * .900f) < 1e-6f);
        require(std::abs(mixed.Block(0,2).low - (.787f+.838f)/2)<1e-6f);
        require(mixed.expectedMask == 2u); // .900 Small Natural crosses .15 before the third mixed-model pass
        const auto mainNatural = DlssNr::InterPass::MakeGainSchedule(3, true, true, false,
            [](unsigned) -> DlssNr::InterPass::ScheduleGains { return { High(1),Low(1) }; });
        require(mainNatural.expectedMask == 3u); // .787 - .75*.75 > .15 before either next pass
        require(mainNatural.Block(2,3).low == Low(1)); // final low covers only the tail
        const auto defaultNatural =
            DlssNr::InterPass::MakeGainSchedule(3, true, true, false,
                                                [](unsigned) -> DlssNr::InterPass::ScheduleGains {
                                                    return { High(1, 0), Low(1, 0) };
                                                });
        require(defaultNatural.expectedMask == 0u); // 310.9 Default runs Small, not Main
        require(std::abs(defaultNatural.Block(0, 3).high - .900f * .900f * .900f) < 1e-6f);
        require(std::abs(defaultNatural.Block(0, 3).low - .838f) < 1e-6f);
        const auto oldDefaultNatural =
            DlssNr::InterPass::MakeGainSchedule(3, true, true, false,
                                                [](unsigned) -> DlssNr::InterPass::ScheduleGains {
                                                    return { High(1, 0, true), Low(1, 0, true) };
                                                });
        require(oldDefaultNatural.expectedMask == mainNatural.expectedMask);
        require(WorkingExtent(3840,.65f)==2496 && WorkingExtent(2160,.65f)==1404);
        require(WorkingExtent(3840,.59f)==2266 && WorkingExtent(2160,.59f)==1274);
        require(WorkingExtent(1919,1.0f)==1919 && WorkingExtent(1079,1.0f)==1079);
        require(WorkingExtent(1919,.65f)==1247 && WorkingExtent(1079,.65f)==701);
        {
            // Auto scale hysteresis: the exact raster only moves with the ordinary 16-pixel cell or the frame size.
            ScaleHold hold;
            require(hold.Hold(3840, 2160, 2496, 1408, .650f) == .650f);
            require(hold.Hold(3840, 2160, 2496, 1408, .652f) == .650f); // same cells: no new raster
            require(hold.Hold(3840, 2160, 2512, 1408, .654f) == .654f); // width cell moved
            require(hold.Hold(3840, 2160, 2512, 1424, .656f) == .656f); // height cell moved
            require(hold.Hold(2560, 1440, 2512, 1424, .657f) == .657f); // frame size changed
            require(WorkingExtent(3840, .650f) != WorkingExtent(3840, .652f)); // what the hold saves a rebuild for
        }
        require(MotionScale(1920,1920,1920)==1920); // native NR, game uses render-size vectors
        require(MotionScale(1920,2496,1920)==2496); // P65 of 3840, matched guides
        require(MotionScale(3840,2496,3840)==2496); // high-resolution motion texture
        require(MotionScale(-1,1274,1440)==-1*(1274.f/1440.f));
        require(MotionScale(1, 1274, 0, 1440) ==
                1274.f / 1440.f); // missing declared reference uses valid source, not allocation/depth
        require(MotionScale(1920, 2496, 0, 1920) == 2496);
        require(MotionScale(1,1274,0)==1); // unspecified game reference
        for (unsigned i = 0; i < 30; ++i)
            require(Preset(i) == (i == 3u || i == 4u || i == 5u || i == 6u || i == 11u ? i : 1u));
        require(Preset(UINT32_MAX)==1u);
        require(!CustomGuided(.015f, 3.f) && CustomGuided(.02f, 3.f) && CustomGuided(.015f, 2.f));
        require(GuidedRange(NAN) == .015f && GuidedSpatial(INFINITY) == 3.f);
        require(GuidedRange(0) == .00001f && GuidedSpatial(-2) == .0001f);
        require(GuidedRange(2) == 1.f && GuidedSpatial(100) == 64.f);
        struct TunableFields
        {
            uint32_t ExposureSourceWidth = 0, ExposureSourceHeight = 0;
        } tunable;
        SetGuided(tunable, .02f, 1.5f);
        float range, spatial;
        std::memcpy(&range, &tunable.ExposureSourceWidth, 4);
        std::memcpy(&spatial, &tunable.ExposureSourceHeight, 4);
        require(range == 1.f / (.02f * .02f) && spatial == 1.f / (1.5f * 1.5f));

        // Native-scale fallback is tested by the host integration contract.
        DlssNrConstants input{};input.Mode=0;input.Width=1920;input.Height=1080;input.DirectResolveFlags=ShaderFlag;
        for(uint32_t pass : {0u,1u})for(uint32_t mode=0;mode<5;++mode) {
            input.Passthrough=pass;input.ReversibleMode=mode;require(InputEncode(input,1920,1080));
        }
        input.Mode=1;require(!InputEncode(input,1920,1080));input.Mode=2;require(!InputEncode(input,1920,1080));input.Mode=0;
        input.ReversibleMode=5;require(!InputEncode(input,1920,1080));input.ReversibleMode=0;
        input.Passthrough=2;require(!InputEncode(input,1920,1080));input.Passthrough=0;
        input.DirectResolveFlags=0;require(!InputEncode(input,1920,1080));input.DirectResolveFlags=ShaderFlag|4;
        require(!InputEncode(input,1920,1080));input.DirectResolveFlags=ShaderFlag;
        require(!InputEncode(input,1921,1080) && !InputEncode(input,1920,1081));
        require(!InputEncode(input,1919,1080) && !InputEncode(input,1920,1079));
        input.Width=0;require(!InputEncode(input,0,1080));input.Width=1920;
        input.Height=0;require(!InputEncode(input,1920,0));input.Width=input.Height=1;require(InputEncode(input,1,1));
        DlssNrConstants exposure{};exposure.Mode=3;exposure.Width=exposure.Height=64;
        exposure.ExposureSourceWidth=3840;exposure.ExposureSourceHeight=2160;exposure.DirectResolveFlags=ShaderFlag;
        require(ExposureStage(exposure,3840,2160)==1);
        require(ExposureStage(exposure,3841,2160)==0 && ExposureStage(exposure,3840,2161)==0);
        exposure.Width=63;require(ExposureStage(exposure,3840,2160)==0);exposure.Width=64;
        exposure.Mode=11;exposure.Width=exposure.Height=1;require(ExposureStage(exposure,64,64)==2);
        require(ExposureStage(exposure,63,64)==0 && ExposureStage(exposure,64,63)==0);
        exposure.Width=2;require(ExposureStage(exposure,64,64)==0);exposure.Width=1;
        exposure.Mode=0;require(ExposureStage(exposure,64,64)==0);exposure.Mode=11;
        exposure.ExposureSourceWidth=0;require(ExposureStage(exposure,64,64)==0);exposure.ExposureSourceWidth=3840;
        exposure.ExposureSourceHeight=0;require(ExposureStage(exposure,64,64)==0);exposure.ExposureSourceHeight=2160;
        exposure.DirectResolveFlags=0;require(ExposureStage(exposure,64,64)==0);exposure.DirectResolveFlags=ShaderFlag|4;
        require(ExposureStage(exposure,64,64)==0);
        DlssNrConstants area{};area.Mode=2;area.Width=2496;area.Height=1404;area.DirectResolveFlags=ShaderFlag;
        require(AreaDownsample(area,3840,2160) && AreaDownsample(area,2496,2160) && AreaDownsample(area,3840,1404));
        require(!AreaDownsample(area,2496,1404) && !AreaDownsample(area,1920,1080));
        require(!AreaDownsample(area,0,2160) && !AreaDownsample(area,3840,0));
        area.Transfer=1;require(!AreaDownsample(area,3840,2160));area.Transfer=0;
        area.Mode=32;require(!AreaDownsample(area,3840,2160));area.Mode=2;
        area.DirectResolveFlags=0;require(!AreaDownsample(area,3840,2160));area.DirectResolveFlags=ShaderFlag|4;
        require(!AreaDownsample(area,3840,2160));area.DirectResolveFlags=ShaderFlag;
        area.Width=0;require(!AreaDownsample(area,3840,2160));area.Width=1;area.Height=0;
        require(!AreaDownsample(area,3840,2160));area.Height=1;require(AreaDownsample(area,1,2));
        DlssNrConstants low{};low.Mode=18;low.Transfer=1;low.Width=312;low.Height=176;low.DirectResolveFlags=ShaderFlag;
        require(LowResidual(low,2496,1404));
        low.Mode=34;require(!LowResidual(low,2496,1404));low.Mode=18;
        low.Transfer=0;require(!LowResidual(low,2496,1404));low.Transfer=1;
        low.Transfer=9;require(!LowResidual(low,2496,1404));low.Transfer=1;
        low.DirectResolveFlags=0;require(!LowResidual(low,2496,1404));low.DirectResolveFlags=ShaderFlag|4;
        require(!LowResidual(low,2496,1404));low.DirectResolveFlags=ShaderFlag;
        require(!LowResidual(low,0,1404) && !LowResidual(low,2496,0));
        low.Width=313;require(LowResidual(low,2497,1404) && !LowResidual(low,2496,1404));
        low.Width=312;require(LowResidual(low,2496,1404)); // 2496 is exactly divisible by eight.
        low.Height=175;require(!LowResidual(low,2496,1404));low.Height=176;
        low.Width=311;require(!LowResidual(low,2496,1404));low.Width=0;require(!LowResidual(low,2496,1404));
        low.Width=low.Height=1;require(LowResidual(low,1,1) && LowResidual(low,8,8) && !LowResidual(low,9,8));
        DlssNr::GuideRegion region {3,5,2560,1440};
        require(ReuseDepth(true,true,1920,1080,1920,1080,region,region));
        require(!ReuseDepth(false,true,1920,1080,1920,1080,region,region));
        require(!ReuseDepth(true,false,1920,1080,1920,1080,region,region));
        require(!ReuseDepth(true,true,1921,1080,1920,1080,region,region));
        require(!ReuseDepth(true,true,1920,1081,1920,1080,region,region));
        require(!ReuseDepth(true,true,0,1080,0,1080,region,region));
        require(!ReuseDepth(true,true,1920,0,1920,0,region,region));
        for(auto field : {&DlssNr::GuideRegion::x,&DlssNr::GuideRegion::y,
                          &DlssNr::GuideRegion::width,&DlssNr::GuideRegion::height}) {
            auto changed=region;++(changed.*field);
            require(!ReuseDepth(true,true,1920,1080,1920,1080,changed,region));
        }
        region.width=0;require(!ReuseDepth(true,true,1920,1080,1920,1080,region,region));
        region.width=2560;region.height=0;
        require(!ReuseDepth(true,true,1920,1080,1920,1080,region,region));
        DlssNrConstants resolve{};resolve.Mode=1;resolve.Transfer=9;resolve.Width=3840;resolve.Height=2160;
        resolve.ResidualHistoryValid=1;resolve.ApplyModel=1;resolve.DirectResolveFlags=ShaderFlag;
        require(NormalResolve(resolve,1920,1080) && NormalResolve(resolve,3840,1080) && NormalResolve(resolve,1920,2160));
        require(!NormalResolve(resolve,3840,2160) && !NormalResolve(resolve,7680,4320));
        require(!NormalResolve(resolve,0,1080) && !NormalResolve(resolve,1920,0) && !NormalResolve(resolve,7680,1080));
        for(auto field : {&DlssNrConstants::DebugView,&DlssNrConstants::CompareMode,&DlssNrConstants::ShowSkinMask}) {
            resolve.*field=1;require(!NormalResolve(resolve,1920,1080));resolve.*field=0;
        }
        resolve.ApplyModel=0;require(!NormalResolve(resolve,1920,1080));resolve.ApplyModel=1;
        resolve.Transfer=8;require(!NormalResolve(resolve,1920,1080));resolve.Transfer=9;
        resolve.Mode=18;require(!NormalResolve(resolve,1920,1080));resolve.Mode=1;
        resolve.ResidualHistoryValid=2;require(!NormalResolve(resolve,1920,1080));resolve.ResidualHistoryValid=1;
        resolve.DirectResolveFlags|=4;require(!NormalResolve(resolve,1920,1080));resolve.DirectResolveFlags=ShaderFlag;
        for(uint32_t mode=0;mode<12;++mode) {
            resolve.ReversibleMode=mode;
            for(uint32_t pass : {0u,1u,2u}) {
                resolve.Passthrough=pass;require(SoftKneeResolve(resolve)==(pass==0 && mode==0));
            }
            resolve.ReversibleMode=mode;resolve.Passthrough=1;require(NormalColorProfile(resolve)==(mode<=4?1:0));
            resolve.Passthrough=0;require(NormalColorProfile(resolve)==(mode==2?2:mode==4?3:0));
            resolve.Passthrough=2;require(NormalColorProfile(resolve)==0);
        }
        for(uint32_t mode : {2u,4u}) {
            resolve.ReversibleMode=mode;resolve.Passthrough=0;resolve.ReplaceDetailStrength=.5f;
            resolve.ModelWorkScale=.5f;require(NormalColorProfile(resolve)==0);
            resolve.Passthrough=1;require(NormalColorProfile(resolve)==0);
            resolve.ModelWorkScale=.999f;require(NormalColorProfile(resolve)==1);
            resolve.ModelWorkScale=1;require(NormalColorProfile(resolve)==1);
            resolve.ModelWorkScale=0;require(NormalColorProfile(resolve)==1);
            resolve.ModelWorkScale=.5f;resolve.ReplaceDetailStrength=0;require(NormalColorProfile(resolve)==1);
        }
        resolve.Width=resolve.Height=1;require(!NormalResolve(resolve,1,1)); // A rounded 1x1 is not reduced.
        for(uint32_t mode=0;mode<=10;++mode) {
            cfg.DlssNrTransfer.value=mode;
            require(!Enabled(cfg));
        }
        std::cout << "Superior policy PASS: Main/Small/Default/legacy presets on 310.8 and 310.9, mixed gain blocks "
                     "and schedules, fixed settings, A/C/D/E/F/K, tunable guided, guides and PSO guards\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
