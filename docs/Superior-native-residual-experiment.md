# Experimental native-center reconstruction and optional residual DLSS

This separate test branch starts from janblade `feat/plugins-page`, `cb02f81f`.
It does not replace the merged Superior implementation or reopen PR #230.

## Behavior

With Superior selected, active Compress screen edges and Model resolution **100%**, the
uncompressed center uses the NR residual directly instead of the radius-one guided estimator.
NR remains active there. Frequency shaping, including the global Low correction for the packed
resolution, and janblade's exposure/color composition remain active throughout the frame.
This is not a clean-image bypass and compressed NR does not have the same context as native NR.

The guided filter reaches full strength when a pixel footprint first touches compression.
Its smooth transition starts **four native pixels inside the uncompressed region**. The
separable smooth union follows the actual shifted/asymmetric layout and works with one-axis
compression. This transition cannot guarantee an invisible boundary or temporal equivalence.
The previous experiment was reverted after motion blur was reported; game motion tests are
the purpose of this branch, not an already established success claim.

Below 100%, guided reconstruction still covers the full output frame. At 100% without active
compression, ordinary NR behavior is retained. Other Enlargement modes are unchanged.
High/Medium still use Model resolution before compression; **only Low** receives the additional
scale correction for the actual packed dimensions. Low remains a block mean, High/Medium a
block product, with the same Structure * Intensity adjustment and worsening-gap schedule.

## Residual DLSS checkbox

In Superior's output options, **DLSS residual stabilization (experimental)** defaults to on.
Its INI key is `[DlssNr] SuperiorResidualDlss=true/false` (`auto` means true).

- **On:** the normal private A/K DLSS feature stabilizes the carrier. IsHDR and AutoExposure
  remain forced for this feature, independently of NR exposure.
- **Off:** private DLSS is neither created nor evaluated. The signed residual still goes through
  the **same encoder, FP16 carrier storage and decoder**:
  `C = clamp(0.5 + (NR - proxy), -1, 2)`, `E = C - 0.5`.
  The range and rounding are preserved for comparison. The fixed encode/decode shaders are reused;
  there is no extra residual shader or changed K/headroom analysis. Guided reconstruction and
  frequency shaping consume the decoded residual exactly as on the normal path.

The disabled mode does not allocate private DLSS motion/output history or call NGX for SR. It
retains two carrier textures for encode/decode. NR depth/motion matching and the nonlinear
Compress endpoint transform stay active. Uncompressed matching is also preserved when depth
is available; NR's normal depth-optional behavior is retained otherwise. Changing the checkbox
retires the previous generation through the existing GPU lifetime tracker, clears failed private
SR state and resets history. Re-enabling DLSS creates fresh history. A/K selection is disabled
in the UI while the checkbox is off.

## Performance constraints

Separate lazy native PSOs contain the estimator change. Reduced-resolution and ordinary host
PSOs retain their previous bytecode. The existing warp lookup's spare channels hold axis mask
weights and direct texel coordinates; its original UV channel stays exact. No per-frame mask
dispatch, upload or full-resolution intermediate is added. The no-DLSS path builds warp data
once per generation too, even though it never creates the private feature.

Disabling DLSS may increase flicker or motion instability. A synthetic final-resolve timing
does not measure NR/private inference or prove the complete game's speedup/quality.

## Verification

```powershell
./tools/compile_nr_interpass.ps1 -Check
./tools/test_superior_port.ps1 -GpuHost -GpuSpatial -GpuNativeCenter
./tools/test_superior_port.ps1 -GpuNativeCenterTiming
```

The native mask GPU probe compares the production mask/mapping with the independent CPU warp
specification, including odd rasters, custom offsets, 50/65/100/150% and one-axis layouts.
Every compressed footprint must have guided weight 1. The original lookup's UV channel is
compared bit for bit against `cb02f81f`. The host test compares generic and cached native PSOs
with an independent scalar mask plus the frozen guided estimator. It allows bounded rounding
differences only for the intentional native estimator change; reduced paths require exact equality.
The carrier bypass test executes real encode -> texture transition -> decode and compares the
complete output with an independent clipping/FP16-roundtrip reference, including nonfinite/HDR
inputs and borders. CPU tests retain preset selection and 11,520 split-band scale/gain cases.

In-game A/B: use the original `cb02f81f` DLL as full-guided reference, then this branch with the
checkbox on and off. Compare 100% with Light/Balanced/Strong during camera movement and
disocclusion, plus 65% controls, HDR/SDR and NR Main/Small/mixed passes. Check the transition,
thin geometry and fine textures. Use GPU timestamps with warmup and ABBA for whole-path timings.

## Results for code commit 30774526

Release x64 [build passed](https://github.com/mattjaas/OptiScaler-F5-Superior-Scaling/actions/runs/39861211814).
All 47 Superior shaders/headers rebuild byte for byte; CPU gain/guide/scheduling tests passed.
Local GPU validation on RTX 4090 / driver 616.56 passed:

| Test | Result |
| --- | --- |
| Ordinary host composition | 2,304 cases / 5,974,272 words, bit-exact |
| Packed host independent estimator | 4,824 cases / 1,046,248,416 components; native FP32 max error 1.52e-6, FP16 at most 2 ULPs |
| Native center mask | 2,602,608 pixels; all 208,251 compressed footprints fully guided |
| Cached original UV channel | 58,176 coordinates, bit-exact |
| No-DLSS carrier encode/decode | 24 cases / 601,224 words, bit-exact |
| Packed carrier/MV preparation | 48 cases / 4,202,496 words, bit-exact |
| Raw cached-low field | 216 cases / 59,616 words, bit-exact |

The native estimator intentionally differs from the full-guided baseline. The above small error
bounds describe agreement with its independent new specification, not equality to the old image.
Unchanged reduced-resolution paths require exact equality in these comparisons.

GPU timestamps compare the original full-guided cached normal PSOs from **cb02f81f** with this
branch's native cached PSOs, on resident synthetic 3840x2160 textured/edge images. Inputs are
FP16 proxy/residual/Low64/output and FP32 original guide/warp metadata. Both sides receive the
same carrier encode/decode result (no proprietary DLSS inference in this harness), NR Small
Standard gains, Structure=Intensity=1. Profiles: Light/Balanced/Strong, 65%/100%, SDR and HDR
mappings 0/2/4/6. Eight alternating ABBA/BAAB blocks each have eight warmup quartets and forty
measured quartets, with four GPU repetitions per timestamp: **640 samples per variant/profile**.
No samples are removed. The measured region includes only final resolve and its output UAV barrier.

All 15 native configurations improve all eight block median deltas. Paired median savings range
from **0.022144 to 0.061312 ms**. The 15 P65 controls have paired median deltas within 0.000064 ms
of zero and pass bit-exact output comparison before timing. Example native HDR mapping 0:

| Compress | Baseline median ms | Candidate median ms | Paired median delta ms | Baseline / candidate stddev ms |
| --- | ---: | ---: | ---: | ---: |
| Light | 0.278784 | 0.243712 | -0.031488 | 0.110300 / 0.099220 |
| Balanced | 0.286464 | 0.246016 | -0.035520 | 0.048488 / 0.038041 |
| Strong | 0.270720 | 0.244224 | -0.024128 | 0.052003 / 0.043523 |

[Raw timestamp samples](validation/superior-native-residual-experiment.csv) and
[all medians, variability and paired block deltas](validation/superior-native-residual-experiment.json).
Recreate the JSON with `python tools/summarize_superior_candidates.py <csv>`.
No full-game speedup or measured private-DLSS-on/off inference saving is claimed. The game still
needs motion/transition validation with the checkbox on and off.
