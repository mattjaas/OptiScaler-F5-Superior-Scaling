# Superior Scaling stage diagnostics

This documents the original diagnostic build. The current UI replaces the point-motion checkbox
with a three-method selector, usable with or without Compress; see
[current motion methods and verification](Superior-surface-motion.md). The other eight stage bypasses remain.

This experimental branch adds controls for investigating motion blur with Superior Scaling,
Model resolution 100% and Compress screen edges. These are diagnostic variants, not proposed
production quality/performance defaults. A headless shader comparison cannot establish which
motion convention the proprietary NR runtime expects in a particular game.

In the Superior menu, expand **Superior stage bypasses (diagnostic)**. All nine switches
default to false. They affect Superior only; other enlargement algorithms retain their behavior.
Changing a switch resets NR history and retires the previous residual generation safely.
**Reset stage bypasses** clears these nine switches but preserves the separate residual-DLSS
choice. Configuration keys are under `[DlssNr]`.

| Checkbox | INI key | Effect |
| --- | --- | --- |
| Skip MV unit conversion | SuperiorSkipMvUnits | Restore original motion units before Compress endpoint mapping; without Compress, retain the game's scale for matched NR vectors. Depth resampling and the nonlinear endpoint transform stay active. Packed motion is shared by NR and private DLSS. |
| Skip inter-pass correction | SuperiorSkipInterPass | Run the NR passes without corrective inter-passes. Final block shaping remains active. |
| Skip carrier encode/decode | SuperiorSkipCarrier | Extract signed NR minus proxy directly into FP16, avoiding the 0.5 offset and carrier clipping to [-1,2]. Temporarily disable private residual DLSS without overwriting its saved checkbox choice. |
| Skip guided reconstruction | SuperiorSkipGuided | Use bilinear signed-residual reconstruction, retaining Compress unwarping, final shaping and composition. Use arithmetic warp mapping for this diagnostic. |
| Skip final frequency shaping | SuperiorSkipFinalShaping | Set final high/medium and low gains to 1 and omit the final low field. Inter-pass shaping has its own switch. |
| Skip Compress correction for low | SuperiorSkipCompressionLow | Use the pre-compression Model resolution scale for low as well as high/medium. This affects the correction schedule and final gains. |
| Skip cached warp lookup | SuperiorSkipWarpCache | Compute Compress mapping directly instead of using its cached axis lookup. |
| Use full-frame guided at 100% | SuperiorFullGuided | Restore guided reconstruction throughout the frame at 100% with Compress, bypassing this branch's native-center experiment. Has no effect while guided reconstruction is skipped. |
| Point-sampled motion (legacy) | SuperiorPointMotion | Restore the preceding point-sampled motion pack instead of the continuous-field experiment. Keep Skip MV unit conversion off for this comparison. |

See the [motion sampling experiment](Superior-Compress-motion-sampling.md) for the current default.
Start with residual DLSS off and all nine switches clear. Reproduce the same moving-camera
scene and test **Skip MV unit conversion** alone first. Clear it, then test **Skip carrier
encode/decode**, final shaping, the low compression correction, guided reconstruction and
full-frame guided individually. Leave a few frames after every change for history to settle.
Compare the center as well as the compressed edges. Only combine switches after recording
their individual effects. Some switches naturally do nothing when their corresponding stage
is inactive, for example inter-passes in a configuration that needs no correction.

With carrier bypass active, the residual-DLSS checkbox displays off and is disabled. Clearing
carrier bypass restores the saved DLSS choice. Without carrier bypass, disabling DLSS still
retains carrier encode/decode, as in the preceding experimental DLL.

## Natural Small/B gain experiment

The only calibration change is **Natural, NR Small/Preset B high/medium: 0.925 -> 0.900**.
Low remains **0.838**. NR Default and legacy Small selections use the Small table on 310.9
and newer supported runtimes; the 310.8 Main-only runtime retains the Main table. Main/A,
other styles and unknown-preset/style fallback values remain unchanged. Private SR A/K
selection does not select the NR gain table. Existing resolution, Structure and Intensity
scaling applies to the new value, including automatic correction scheduling.

## Verification

```powershell
./tools/compile_nr_interpass.ps1 -Check
./tools/test_superior_port.ps1 -GpuDiagnostics -GpuSpatial
./tools/test_superior_port.ps1 -GpuDiagnosticsTiming
```

CPU tests exercise all 512 diagnostic masks, their dependencies, effective model selection
and gain/schedule behavior. GPU comparisons check the new bilinear and full-guided choices
against independent references, with packed/unpacked and HDR/SDR inputs, odd dimensions,
FP16/FP32 outputs, and raw residual extraction including nonfinite values. Existing shaders
retain their bytecode when no new diagnostic specialization is selected.

The timing harness compares resident synthetic 4K Balanced configurations at 65%/100% on
SDR/HDR inputs. It measures final resolve plus its output UAV barrier, or carrier roundtrip
versus direct signed extraction including texture transitions and barriers. It does not
execute proprietary NR/private DLSS or measure the whole game's performance. Diagnostic
bypasses intentionally alter the image; their timing is not evidence of an equal-quality
optimization. In-game motion comparison is still required to identify the blur's cause.

Local verification passed on RTX 4090, driver 616.56: all six CPU tests, all 120 new GPU
cases / 754,056 words (bit-exact against references), packed guide/preparation and low-cache
tests, and byte-for-byte rebuild of all 52 shader/header pairs. The 47 pre-existing Superior
shader binaries are unchanged.

The timestamp run used eight alternating ABBA/BAAB blocks, eight warmup quartets per block
and forty measured quartets, four dispatch repetitions per timestamp: 640 samples per variant
per configuration. At 100% Balanced HDR0 the carrier stage medians were 0.144384 -> 0.092672 ms
(paired median delta -0.051456 ms, paired standard deviation 0.030953 ms); final resolve was
0.238592 -> 0.231936 ms (paired delta -0.009984 ms, paired standard deviation 0.062971 ms).
There was substantial variability and 38% background GPU utilization immediately after the
run. These are noisy diagnostic timings, not confirmation of a controlled performance gain.
All samples, including outliers, are retained in the [raw CSV](validation/superior-stage-bypass-timing.csv)
and [statistics](validation/superior-stage-bypass-timing.json). No whole-path game timing is claimed.
