# Render-resolution compressed guides (experimental)

Superior's checkbox **Render-resolution depth/MV with Compress at 100% (experimental)** persists as
`[DlssNr] SuperiorRenderGuides=false`. It is off by default. It only affects Superior, active Compress
screen edges and exactly 100% Model resolution. Other enlargement modes, ordinary scaling and native
100% without Compress keep their previous paths. Missing render dimensions leave the previous path active.

Both NR guide textures use the game's declared render subrect, reduced independently on each axis by
the actual packed-colour/native-colour ratio, rounded to the nearest integer with a minimum of one pixel.
The dimensions are not aligned to the NR model's 16-pixel colour grid: these are auxiliary subrects.

For 3840x2160 colour, 1920x1080 render and Balanced compression:

| Input to NR | Previous/default | New checkbox enabled |
| --- | --- | --- |
| Colour | 3456x1952 | 3456x1952 |
| Depth | 3456x1952 | 1728x976 |
| Motion | 3456x1952 | 1728x976 |

The render size comes from frame metadata, never from a padded guide allocation. Source depth and
motion still use their independently resolved valid rectangles and origins. Source MV units still
follow MVLowRes and the game's MV scale. The warp retains the colour layout's native coordinate system,
side curves, offsets, shifts and centre rounding; only its output coordinate scale changes. Rebuilding
an independently rounded compression layout at the render size would misalign the two geometries.

Motion remains `Pack(p+v)-Pack(p)` at the query position, expressed in pixels of the new packed guide
subrect. NR receives its actual depth/MV dimensions and MV scale 1. It can map these guides to its
different colour raster. No global compression multiplier replaces the nonlinear endpoint transform.
Point, guarded bilinear, Depth-aware and optional joint depth/MV all remain selectable on this grid;
their existing sparse minification correction follows the guide lattice's actual footprint.

With residual DLSS disabled, no private guide reconstruction is dispatched. With it enabled, private
DLAA needs depth on the carrier's colour raster: one uniform guide reconstruction converts the packed
NR pair to that raster, without applying the nonlinear warp again. The selected motion method and
joint-depth setting apply. The fused carrier/MV pass then converts smaller-lattice motion units to
carrier pixels once. Held frames still pass zero temporal motion. Private IsHDR, AutoExposure, carrier
encoding, NR gain tables and high/medium-versus-low resolution rules remain unchanged.

The smaller NR guide pair can alter temporal behaviour and lose fine guide detail. This is an
experiment, not a guaranteed visual improvement or a promise that NR accepts every independent grid.
Changing the checkbox resets NR/private history; changing render dimensions also changes the compression
signature and resets history. Resource dimensions are cached, with no per-frame allocations. The log
reports the smaller packed guide dimensions alongside NR colour/native sizes when resources change.

Private DLSS SR hints now include **A=1, C=3, D=4, E=5, F=6, K=11**. Unsupported hints may be ignored by
the installed runtime or fall back to its default after an explicit creation failure. These hints do
not select NR Main/Small models or change their frequency gains.

## Verification

`tools/test_superior_port.ps1 -GpuRenderGuides -GpuJointDepth` covers 480 CPU warp/endpoint checks,
3072 diagnostic combinations and the existing gain/scheduling policies. GPU checks cover 559,104 NR
pixels and 6,488,064 private motion components across all three motion methods, optional joint depth,
odd render sizes, source subrects, custom compression shifts, reversed Z, private unit conversion and
held frames. The joint suite additionally exercises 84 render-resolution configurations with layered,
sloped and invalid inputs (1092 total cases / 10,284,288 pixels). Existing SDR/HDR tunable-guided checks
also pass. No HLSL or precompiled shader changes are needed: this option reuses existing PSOs.

The timestamp harness `-GpuRenderGuidesTiming` compares colour-sized guide preparation against the
smaller NR grid, with and without private guide preparation. It includes guide dispatches, sparse
corrections, carrier/MV preparation and associated barriers; it excludes model inference and the rest
of the frame. It uses 8 alternating ABBA/BAAB blocks, separate warmup and 640 samples per variant/config.
Full-game NR/DLSS acceptance and motion appearance must still be compared in the game.

## GPU timestamp results (2026-10-25)

RTX 4090, driver 616.56; 3840x2160 output, Model resolution 100%, Balanced Compress,
sloped R32F depth and affine RG32F motion at the declared render resolution. Baseline and
new option use the same source data and motion method. `surface` is the existing Depth-aware
method with point depth; `joint` enables the experimental joint depth/MV option.

Before the isolated harness: 5% utilization, 27.37 W and 210 MHz. After settling: 2%,
28.17 W and 210 MHz. A separate earlier 35% utilization reading had no recorded process
attribution; it cannot establish that a game or another user application was running.

Medians below time the guide preparation, sparse correction and barriers. The private-guides
rows additionally include uniform private guide reconstruction and fused carrier/MV preparation,
**without proprietary NR or DLSS inference**. No full-frame or game performance gain is claimed.
ABBA/BAAB used 8 blocks, 8 warmup quartets per block, 40 measured quartets and four GPU repetitions
per timestamp: 640 samples per variant/configuration. Delta is the median of paired-quartet
candidate-minus-baseline means; variability is the standard deviation of those paired deltas.

| Render | Guide method | Timed route | Baseline ms | New ms | Paired delta ms | Delta stddev ms |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| 1080p | Depth-aware | NR guides only | 0.131584 | 0.037632 | -0.094208 | 0.071364 |
| 1080p | Depth-aware | NR + private preparation | 0.284800 | 0.298752 | +0.014272 | 0.030528 |
| 1080p | Joint | NR guides only | 0.163584 | 0.049152 | -0.114432 | 0.081137 |
| 1080p | Joint | NR + private preparation | 0.320768 | 0.331264 | +0.010624 | 0.033314 |
| 1440p | Depth-aware | NR guides only | 0.141056 | 0.086016 | -0.055040 | 0.039974 |
| 1440p | Depth-aware | NR + private preparation | 0.297344 | 0.361728 | +0.064384 | 0.035502 |
| 1440p | Joint | NR guides only | 0.175104 | 0.136448 | -0.038656 | 0.027412 |
| 1440p | Joint | NR + private preparation | 0.334080 | 0.429312 | +0.095616 | 0.037522 |

The smaller NR grid reduced guide preparation cost in these fixtures, but private guide
reconstruction consumed the savings and added overhead, especially at 1440p. Small 1080p
private-preparation deltas are below their variability and should not be treated as precise
predictions. The option stays an opt-in image/motion experiment, not an automatic performance
policy. Raw samples and summary are in `validation/superior-render-guides-timing.csv` and `.json`.
