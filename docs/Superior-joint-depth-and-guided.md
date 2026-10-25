# Experimental joint depth/MV reconstruction and guided controls

This extension is limited to Superior Scaling. Private residual DLSS now accepts SR hints
**A (1), C (3), D (4), E (5), F (6) and K (11)**. The existing A and K selections persist unchanged. This is
independent of the NR Main/Small preset, style and frequency-gain tables. The requested hint is
applied to DLAA and the other quality hints before feature creation; a change recreates the private
feature and resets NR/residual history. IsHDR, AutoExposure, MVLowRes, unit exposure and carrier
encoding remain unchanged. The runtime can reject/ignore unavailable CNN hints; the existing
explicit-creation-failure fallback to runtime Default remains, with a log warning.

## UI and persistence

Only the Superior menu shows the added controls:

| Key in `[DlssNr]` | Default | Meaning |
| --- | ---: | --- |
| `SuperiorReconstructDepth` | false | Joint depth/MV reconstruction, active only with Depth-aware motion. Disable for the previous point-depth path. |
| `SuperiorGuidedRangeSigma` | 0.015 | Final guided residual reconstruction colour tolerance. |
| `SuperiorGuidedSpatialSigma` | 3.0 | Final radius-one filter spatial falloff. |
| `SuperiorScalingPreset` | 1 | Private residual DLSS A=1, C=3, D=4, E=5, F=6, K=11. |

Both sigma controls have individual Reset buttons and support Ctrl+click numeric entry. CPU
sanitization accepts range sigma 0.00001..1 and spatial sigma 0.0001..64; nonfinite values restore
the corresponding default. Changing these sigmas does not change inter-pass filtering, radius,
the number of reconstruction samples or frequency gains. They affect only pixels where guided
reconstruction actually runs; the native uncompressed-center bypass and diagnostic skip remain.

At the exact default sigmas, the original resolve PSOs run unchanged. Non-default values select
separate tunable PSOs, including cached/direct warp and native-center variants. Inverse variances
are computed once on CPU and passed as float bits in the resolve-only, otherwise unused meter-
dimension fields of the existing 256-byte buffer. No exposure calculation or meter pass is
changed. There is no per-pixel sigma division, extra texture or dispatch for these controls.

## Joint surface reconstruction

The experimental checkbox is off by default, so updating the DLL retains the previous GPU path.
The experimental checkbox is independent of the residual DLSS checkbox. Both NR and private
residual DLSS consume the resulting depth and compatible MV. The original point-depth PSOs and
sparse MV-only correction remain available when the new checkbox is off. Point and Bilinear motion
methods retain their original behavior. Native 100% without Compress introduces no guide resample.
Checkbox/motion changes participate in the generation key and reset NR/residual history.

The base joint PSO fits limited one-sided device-depth slopes on continuous geometry, reconstructs
depth at the continuous query and bounds the result by neighboring source values. It does not
average foreground and background depth. Motion matching evaluates its depth plane at the same
fractional query, keeping independent valid rectangles/origins and the existing game-to-working
motion units. Depth stays in the game's device-depth encoding and R32F working storage.

Where **either depth or MV** is reduced, a separate sparse correction dispatch processes the
union of minified edge bands, or the entire working raster for uniform reduction. The center is
not launched unless it also minifies a guide. Both guide UAVs are ordered by one UAV barrier;
no new textures, per-frame allocations, descriptor tables or history buffers are added.

The correction maps the pixel edges through the exact uniform/nonlinear warp and considers
foreground coverage inside that source footprint. Candidate depths are transported along the
local plane before comparison, so an inclined plane is not automatically mistaken for an occluder.
A proposed foreground depth must have a nearby finite MV confirmed by its supporting source-depth
texels; the chosen depth and that vector are committed as a pair. Ambiguous cross-edge MV support
is rejected. Depth inversion is taken from the frame's existing declaration. Packed motion still
transforms endpoints as `Pack(p+v)-Pack(p)` at the output query, rather than globally scaling edges.

Searches cover at most 8x8 samples, with stratification spanning larger footprints. This is a bounded
approximation and can miss subpixel surfaces. Foreground selection may expand thin silhouettes;
it is an experimental visual tradeoff, not a universal quality guarantee. Missing/invalid depth
uses the existing fallback. Motion absent from the source cannot be reconstructed reliably.

## Reproduction

```powershell
./tools/compile_nr_interpass.ps1 -Check
./tools/test_superior_port.ps1 -GpuJointDepth -GpuSpatial -GpuSurfaceMotion
./tools/test_superior_port.ps1 -GpuJointDepthTiming
python tools/summarize_superior_candidates.py artifacts/validation/joint-depth-timing.csv
```

Quality and timestamp results are reported below. Timestamp tests exclude proprietary NR/private
DLSS inference; they measure guide preparation, sparse correction when needed, fused carrier/MV
preparation and all related barriers/transitions. Private preset performance is not inferred from
these shader timings.

## Validation results

RTX 4090 / driver 616.56. All 58 previously shipped Superior blobs are byte-identical, including
point-depth guides, sparse MV-only correction, fixed-default Guided, carrier, inter-pass and HDR
composition variants. Four new joint PSOs and ten tunable resolve PSOs bring the total to 72.
The 256-byte constant ABI and original v59 shader manifest still pass. CPU tests cover A/D/E/K
selection, invalid/custom sigma values, INI motion selection precedence, 1,536 diagnostic/motion/
joint-checkbox combinations, history-key flags and old/new minification coverage up to 4K.

The joint GPU suite covers **1,008 cases / 9,907,968 pixels**: uniform/packed 25/65/100/125%,
FP16/FP32 source MV, 0.5x/1x independent depth and MV densities, normal/reversed depth, independent
padded origins, constants, sloped planes, thin/diagonal layers and missing/invalid guides. Another
44 correction dispatches deliberately cap X to three groups to exercise 2D work-list indexing.

- Plane device-depth RMS: **0.000073801336 -> 0.000000023841** over 1,097,376 interior queries.
- Working-pixel MV RMS on that plane: 0.000653148 -> 0.000653150 (FP rounding scale).
- Depth/MV layer mismatches: **828 -> 828** of 4,246,272 layer queries; the original mixed-grid
  ambiguity is not solved when an input vector does not identify the query surface.
- 2,830,848 same-grid pairs pass the layer-consistency checks. Foreground coverage is retained in
  4,840 additional working pixels; this is a geometric coverage result, not proof of better game
  image quality or absence of silhouette dilation.
- Tunable Guided: **160 cases / 8,008,320 components**, SDR/HDR with direct/cached/native paths,
  game/automatic exposure, PQ input and FP16/FP32 output. Supplying default sigmas to the tunable
  variants is bit-identical in these tests (maximum error zero). Custom values affect 2,010,622
  components, confirming both controls reach reconstruction. Actual default settings still use
  the old constant-specialized PSOs.
- Original spatial, raw-low, carrier/shared-MV and 576-case motion tests pass unchanged, as do the
  2,880-case minified-MV test and its 140 2D dispatches.

[GPU quality output](validation/superior-joint-depth-quality.txt).

## GPU timestamps

The supplied `-GpuJointDepthTiming` harness compares the ed3e60e3 point-depth Depth-aware path
(including its sparse MV correction) with joint reconstruction enabled. Both sides include guide
preparation, fused carrier/private-motion preparation and their correction barriers/transitions.
Uploads, readbacks and proprietary NR/DLSS inference are excluded. Sigma controls remain at defaults.

It covers 3840x2160 output, 1920x1080 RG16F source MV, 1920x1080 and 3840x2160 R32F source depth,
ordinary 25/50/65% and Balanced Compress 65/100%, with affine, single-edge and dense-island fields.
Thirty configurations use eight alternating ABBA/BAAB blocks, eight warmup and forty measured
quartets per block, four repetitions per timestamp and 640 samples per variant/configuration.

The final controlled repeat began after three idle checks of **2%, 2%, 3%** GPU utilization;
a settled post-run sample was **2%**. GPU: RTX 4090 / driver 616.56. All samples and paired-block
statistics are retained. Initial attempts with an observed unrelated 29–32% load are excluded
from the published results; this table and raw data contain only the final repeat.

| Configuration | Baseline median ms | Joint median ms | Paired delta ms | Paired stddev ms |
| --- | ---: | ---: | ---: | ---: |
| packed-affine-depth1080, 65% | 0.100608 | 0.117248 | +0.016640 | 0.028931 |
| packed-affine-depth1080, 100% | 0.283392 | 0.315648 | +0.032448 | 0.030187 |
| packed-affine-depth2160, 65% | 0.114944 | 0.209408 | +0.094272 | 0.022924 |
| packed-affine-depth2160, 100% | 0.297216 | 0.338944 | +0.042112 | 0.036929 |
| packed-edge-depth1080, 65% | 0.077312 | 0.092416 | +0.014912 | 0.020393 |
| packed-edge-depth1080, 100% | 0.221696 | 0.239872 | +0.018688 | 0.030157 |
| packed-edge-depth2160, 65% | 0.085504 | 0.179456 | +0.093696 | 0.059860 |
| packed-edge-depth2160, 100% | 0.224512 | 0.264704 | +0.039808 | 0.036946 |
| packed-islands-depth1080, 65% | 0.103680 | 0.124672 | +0.020864 | 0.036176 |
| packed-islands-depth1080, 100% | 0.276096 | 0.307200 | +0.032000 | 0.031323 |
| packed-islands-depth2160, 65% | 0.120832 | 0.263168 | +0.142784 | 0.055350 |
| packed-islands-depth2160, 100% | 0.292096 | 0.356608 | +0.064064 | 0.034492 |
| uniform-affine-depth1080, 25% | 0.034048 | 0.054016 | +0.020224 | 0.027357 |
| uniform-affine-depth1080, 50% | 0.059904 | 0.066048 | +0.006272 | 0.016599 |
| uniform-affine-depth1080, 65% | 0.116736 | 0.125696 | +0.009472 | 0.032142 |
| uniform-affine-depth2160, 25% | 0.041216 | 0.065792 | +0.024576 | 0.036684 |
| uniform-affine-depth2160, 50% | 0.068864 | 0.119552 | +0.050688 | 0.048225 |
| uniform-affine-depth2160, 65% | 0.128256 | 0.230144 | +0.102528 | 0.059159 |
| uniform-edge-depth1080, 25% | 0.017920 | 0.024064 | +0.006528 | 0.017072 |
| uniform-edge-depth1080, 50% | 0.042496 | 0.045568 | +0.003200 | 0.040277 |
| uniform-edge-depth1080, 65% | 0.080000 | 0.085760 | +0.006016 | 0.021613 |
| uniform-edge-depth2160, 25% | 0.019200 | 0.059136 | +0.039680 | 0.050215 |
| uniform-edge-depth2160, 50% | 0.046592 | 0.093696 | +0.046464 | 0.045180 |
| uniform-edge-depth2160, 65% | 0.088576 | 0.183552 | +0.094336 | 0.081745 |
| uniform-islands-depth1080, 25% | 0.048128 | 0.076032 | +0.027520 | 0.024159 |
| uniform-islands-depth1080, 50% | 0.058112 | 0.064000 | +0.006016 | 0.034043 |
| uniform-islands-depth1080, 65% | 0.107264 | 0.117504 | +0.009984 | 0.025472 |
| uniform-islands-depth2160, 25% | 0.031488 | 0.071936 | +0.041600 | 0.060057 |
| uniform-islands-depth2160, 50% | 0.061696 | 0.109056 | +0.047360 | 0.025728 |
| uniform-islands-depth2160, 65% | 0.118016 | 0.297472 | +0.178944 | 0.100632 |

With 1080p source depth, ordinary 65% costs +0.006016..0.009984 ms and 100% + Balanced costs
+0.018688..0.032448 ms in these fixtures. With 4K source depth, ordinary 65% costs
+0.094336..0.178944 ms, and 100% + Balanced costs +0.039808..0.064064 ms. Some small differences
are below per-pair variability; they must not be presented as exact whole-game costs. Source
**depth** resolution matters independently of motion and NR color resolution.

The experimental checkbox is off by default, retaining the previous path. The quality gain is
not free: compare real game motion and thin silhouettes before choosing it. These timestamps
exclude proprietary NR/private DLSS inference and do not establish a whole-frame FPS change.
The source DLL was built from `974ed30e`; this report adds no rendering-code change.

[Raw timestamps](validation/superior-joint-depth-timing.csv),
[medians, variability and per-block deltas](validation/superior-joint-depth-timing.json), and
[GPU quality output](validation/superior-joint-depth-quality.txt).

The optional native-100% render-resolution compressed guide lattice is described in
[Superior-render-resolution-guides.md](Superior-render-resolution-guides.md).
