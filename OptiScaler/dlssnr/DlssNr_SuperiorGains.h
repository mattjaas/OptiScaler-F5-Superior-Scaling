#pragma once
#include "DlssNr_SuperiorScaling.h"
#include "DlssNr_SuperiorDiagnostics.h"
#include "DlssNr_GuidedShaping.h"
#include "DlssNr_InterPassPolicy.h"
#include "PassProfiles.h"

namespace DlssNr::SuperiorScaling
{
// Same per-band correction and slider product as v59, using independent high/low scales.
// The table is selected per NR pass,
// independently of the private DLSS SR A/K preset.
inline InterPass::ScheduleGains PassGains(const Config& cfg, unsigned pass, FrequencyScales scales, bool mainOnly)
{
    const auto tuning = Profiles::PassTuning(cfg, pass);
    const auto style = GainStyleSelection(cfg, pass);
    const auto preset = GainPresetSelection(cfg, pass);
    return BandGains(style, preset, mainOnly, scales, tuning.structure, tuning.intensity);
}
inline InterPass::ScheduleGains BlockGains(const Config& cfg, unsigned first, unsigned completed,
                                           FrequencyScales scales, bool mainOnly)
{
    return { Profiles::ProductGuidedHigh(first, completed, true,
                                         [&](unsigned p) { return PassGains(cfg, p, scales, mainOnly).high; }),
             Profiles::MeanGuidedLow(first, completed,
                                     [&](unsigned p) { return PassGains(cfg, p, scales, mainOnly).low; }) };
}
inline InterPass::GainSchedule Schedule(const Config& cfg, unsigned passes, FrequencyScales scales, bool mainOnly)
{
    return InterPass::MakeGainSchedule(passes, InterPass::Eligible(8u, scales.low, passes, false), true, false,
                                       [&](unsigned p) { return PassGains(cfg, p, scales, mainOnly); });
}
} // namespace DlssNr::SuperiorScaling
