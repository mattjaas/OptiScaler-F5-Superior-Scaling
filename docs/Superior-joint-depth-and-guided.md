# Experimental joint depth/MV reconstruction and guided controls

This extension is limited to Superior Scaling. Private residual DLSS now accepts SR hints
**A (1), D (4), E (5) and K (11)**. The existing A and K selections persist unchanged. This is
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
| `SuperiorScalingPreset` | 1 | Private residual DLSS A=1, D=4, E=5, K=11. |

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

**Controlled performance results are pending.** Idle validation after initial timing attempts
observed 29–32% GPU activity with the test processes stopped. Those attempts must not be used to
claim clean GPU performance. Repeat the timestamp harness after external load has stopped and
save the raw CSV, paired statistics, GPU identity and observed utilization. The experimental
checkbox remains off by default, retaining the old path.
