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
