# Three selectable Superior motion reconstruction methods

The Superior UI now exposes **Motion reconstruction (experimental)** for both ordinary Model
resolution scaling and scaling combined with Compress screen edges:

| Value | Method | Behavior |
| --- | --- | --- |
| 0 | Point sampled | Previous nearest-texel sampling. Existing Compress point shader and ordinary Mode 10 remain unchanged. |
| 1 | Bilinear + motion guard | Previous four-tap interpolation with the one-native-pixel motion discontinuity guard. |
| 2 | Depth-aware limited reconstruction | New depth-guided, slope-limited spatial reconstruction described below. |

The `[DlssNr] SuperiorMotionMethod` key persists the choice. Default `auto` is 2. An explicitly
saved legacy `SuperiorPointMotion=true/false` migrates to 0/1 only when the new key is missing,
auto or invalid; a valid new selection takes precedence. The old checkbox is replaced by the
selector. The eight independent stage bypasses remain. Resetting bypasses preserves the chosen
method. Method changes reset NR/private history and safely retire the old generation.

All methods retain separate X/Y conversion from the game's render/output MV reference and its
valid region/origin. Sampling and motion-unit conversion are separate controls. For normal
comparison leave **Skip MV unit conversion** off. No extra resampling is introduced at native
100% without Compress, where the original NR path remains the reference. Non-Superior modes,
color packing, depth point sampling, frequency gains and other NR settings are retained.
The NR MV texture still uses the working raster; reduced-density MV allocation is not part of
this change. Natural Small/B remains high/medium 0.900, low 0.838.

## New spatial estimator

The new estimator reads the nearest source MV and its four axial neighbors. It reconstructs
an affine field from limited one-sided slopes, anchored on the nearest vector. A common limiter
for both components prevents selecting a different slope branch for X and Y. Unlike the prior
fixed one-pixel cutoff, this permits steep but consistent affine motion; a step with a flat
same-surface side limits the slope to zero even when the jump is smaller than one pixel.

When valid depth is available, source-depth samples at the MV texel centers fit a local plane
using limited depth slopes. The query depth and neighboring samples must agree with that plane
within a small FP32 device-depth tolerance. Incompatible neighbors do not contribute a slope.
If query depth disagrees with the anchor's plane, select an existing finite neighbor vector
with the closest query depth instead of averaging layers. No linear-depth/projection-matrix
guess is made; device depth and reversed Z are handled consistently. Invalid/missing depth
uses the motion-only limiter. Nonfinite MV results are sanitized; constant fields take a fast path.

This is our own implementation. The general use of depth when choosing motion is also described
in AMD's [FSR2 Reconstruct and dilate documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-temporal/).
We do not copy its dilation algorithm or expand the nearest foreground across a 3x3 neighborhood.

This is a spatial estimator with **no additional frame history**. It cannot recover a surface's
motion when no input vector describes that surface, distinguish every coplanar motion boundary,
or determine previous-frame visibility without previous depth. NR/private DLSS retain responsibility
for temporal disocclusion rejection. Synthetic accuracy is not proof that the reported TLOU blur
has been eliminated. Conservative limiting can be slightly less accurate than bilinear sampling
on smooth nonlinear fields; all three choices remain available for this reason.

## Shared implementation and GPU work

Compressed reconstruction replaces the existing guide-pack PSO, retaining the same two outputs,
dispatch and barriers. The original four-tap packed shader remains byte-identical. Ordinary scaling
uses the existing matched depth/MV scratch surfaces with two new uniform-grid PSOs. They store
MV in the game's raw encoding; NR applies its existing working-grid unit scale. For the new
ordinary methods, private SR's fused carrier/MV pass consumes that same matched field and converts
it once into working pixels. It no longer independently point-samples the game's source vectors.
Hold still emits zero private motion. The minification extension below adds a sparse correction
pass only when needed. PSOs are lazy; there are no new per-frame allocations,
history buffers, depth-pyramid dispatches or readbacks in the rendering path.

## Verification

```powershell
./tools/compile_nr_interpass.ps1 -Check
./tools/test_superior_port.ps1 -GpuSurfaceMotion -GpuMotion -GpuSpatial
./tools/test_superior_port.ps1 -GpuSurfaceMotionTiming
```

CPU policy checks cover all 768 method/bypass combinations and old/new INI selection precedence.
The GPU comparison evaluates 576 cases / 32,515,536 pixels: point, guarded bilinear and limited
reconstruction; uniform/packed rasters at 65/100/125%; FP16/FP32 source MV; reversed/ordinary
device depth; padded, offset and independent regions; no depth, nonfinite vectors, steep/gentle
affine fields, curved motion, small same-depth steps, thin layers, diagonal edges and a moving
foreground exposing background. References specify the source scene and its motion analytically,
not by copying the shader's limiter. Results on RTX 4090 / driver 616.56, RMS in working pixels:

| Scene group | Point | Bilinear + guard | Depth-aware limited |
| --- | ---: | ---: | ---: |
| Steep affine field | 0.652617889 | 0.652617875 | 0.016978410 |
| Depth boundaries and moving layers | 0.456782497 | 0.461863007 | 0.110294961 |
| Constants and small motion steps | 0.000477050 | 0.011086958 | 0.000477054 |
| Gentle affine field | 0.061717200 | 0.001621269 | 0.001704886 |
| Curved smooth field | 0.049704007 | 0.001423443 | 0.002332556 |

The old bilinear guard produced 30,424 vectors more than 0.02 pixel away from either expected
small-step layer; the new estimator produced zero such mixed vectors in this fixture. On 32,736
newly exposed-background queries, RMS was 0.870852764 / 0.870853126 / 0.000001047 pixel. This
checks current-frame layer selection in the sequence, not temporal rejection inside NR/DLSS.
The fused private-SR preparation additionally checks 70,686,528 components for single unit
conversion of the shared selected field and zero motion on Hold. Original spatial/guide,
carrier and cached-low tests pass. All 53 prior Superior blobs and the original nine host/spatial
blobs remain unchanged; three new blobs bring the Superior total to 56.

## GPU timestamps

The resident synthetic 4K harness measures **guide preparation + fused carrier/private-motion
preparation + their transitions/barriers**, excluding proprietary NR/DLSS inference and the
remaining render path. It uses 1920x1080 RG16F source motion, 3840x2160 R32F depth, the existing
working-grid surfaces and Balanced Compress where enabled. Affine and depth-edge fields are
tested at 65% with/without compression and at 100% with compression. Native 100% without
compression performs no guide resample and is not an artificial timed configuration.

Each comparison has eight alternating ABBA/BAAB blocks, eight warmup quartets then forty
measured quartets per block, four GPU repetitions per timestamp: 640 samples per variant per
configuration. All samples are retained. Background GPU use after the run was 5%.

| Configuration | Baseline median ms | Limited median ms | Paired delta ms | Paired stddev ms |
| --- | ---: | ---: | ---: | ---: |
| 100% Balanced, affine, vs point | 0.248576 | 0.281344 | +0.034880 | 0.057878 |
| 100% Balanced, affine, vs bilinear | 0.256896 | 0.284928 | +0.029120 | 0.032414 |
| 100% Balanced, depth edge, vs point | 0.206592 | 0.218624 | +0.011648 | 0.015804 |
| 65% ordinary, affine, vs point | 0.145408 | 0.124160 | -0.023680 | 0.023447 |
| 65% ordinary, affine, vs bilinear | 0.127488 | 0.129536 | +0.002048 | 0.019993 |

The ordinary point baseline resamples the original motion again for private SR; the two new
methods reuse the matched NR field. The recorded saving includes this reuse. With private SR
disabled, that work is absent, so the saving must not be applied to that mode. No total-frame
game speedup is claimed. Compare the three choices in the actual scene, then repeat with
private SR off/on, compression off/Light/Balanced/Strong and different NR model/style selections.

[Raw timestamps](validation/superior-surface-motion-timing.csv) and
[all medians, variability and block deltas](validation/superior-surface-motion-timing.json).

## Minification-aware surface selection

Depth-aware reconstruction now distinguishes local MV minification from magnification. This
is based on the **source MV valid rectangle versus the actual working pixel**, not Model
resolution alone. A 65% NR raster can magnify 1080p game MV while shrinking full-resolution MV;
with Compress, the center and periphery can need different treatment in the same dispatch.

When the nearest vector's local depth plane disagrees with the point-sampled depth sent to NR,
the minification path maps both edges of the working pixel back to the source MV lattice.
Uniform scaling uses the exact X/Y extent ratios. Compress uses `PwUnpack` at the pixel edges,
so the search covers the nonlinear, potentially asymmetric footprint rather than approximating
it by a global compression percentage or a center derivative. The existing five axial samples
alone could miss a small surface represented only by a diagonal texel in a shrinking 2x2 footprint.

Within that footprint, select the nearest finite source vector whose depth, transported along
the existing limited device-depth plane, matches the query surface. Device-depth comparison
remains reversed-Z symmetric. The output depth is unchanged; this does **not** dilate foreground
or average the motion of overlapping objects and background. Smooth fields retain their existing
limited reconstruction. Constant fields retain their original fast return. Missing or invalid depth keeps the existing
motion-only fallback, rather than guessing a foreground layer.

Coverage is exhaustive up to eight source texels per axis (64 locations). Larger footprints use
at most 8x8 stratified samples spanning the entire footprint; this is a bounded approximation,
not proof of recovering every subpixel surface under arbitrary reduction. If no matching finite
sample is found, the previous five-sample fallback remains. The existing Depth-aware PSOs remain byte-identical. When no part of the image shrinks the MV
lattice, exactly the previous GPU path runs. When minification is required, an additional sparse
correction dispatch follows it, with one MV UAV barrier. Four disjoint edge bands are compacted
into that one dispatch; the center is not launched unless it also shrinks MV. Pure uniform
minification covers the full working raster. Binary searches use the separable warp, with two
extra boundary texels to guard CPU/GPU rounding; the shader still checks exact local footprints.
The center's constant derivative is evaluated analytically to avoid false minification from
FP32 subtraction of large pixel coordinates. A 2D dispatch handles work lists larger than 65,535
groups. Both passes share the same constants and descriptors.

There are no new textures, per-frame allocations or history. Motion units, NR/private-SR sharing,
color processing and gains are unchanged. Point sampled and Bilinear + motion guard remain available.
All 56 previously shipped Superior blobs are byte-identical; two new, lazily created correction
PSOs bring the total to 58.

### Minification validation

On RTX 4090 / driver 616.56, the added GPU test covers **2,880 cases / 21,008,340 pixels** at
25/40/50/100/125% Model resolution, with source MV density 0.5x/1x/2x on each axis, including
mixed minification/magnification. It covers exact source-to-target 2:1 ratios, fractional ratios,
nonlinear/asymmetric Compress, odd dimensions, independent valid origins and padded allocations,
FP16/FP32 source motion, normal/reversed depth, invalid MV/depth, missing depth, smooth affine
motion, subpixel same-depth steps, diagonal stripes and small moving depth islands. CPU coverage
checks include 136 layouts up to 4K. Another 140 GPU dispatches use a deliberately small X group
cap to exercise 2D work-list indexing without allocating giant textures.

Against published baseline `2b2eb39f`:

- 8,607,572 non-minified or depth-disabled queries are bit-identical, including output depth.
- All 1,514,252 recoverable flat-layer queries within exhaustive footprint coverage choose the
  expected query-depth layer; 2,008 previously incorrect choices are corrected, with zero
  previously correct layer choices regressing in that fixture.
- No mixed foreground/background vectors are generated. Minified depth-island RMS decreases
  from 0.414471601 to 0.387100630 working pixels over 1,550,096 queries. Some surfaces have no
  representative source vector; searching a larger footprint cannot invent the missing motion.
- Maximum interior FP32 affine-field error is 0.000026703 working pixels.
- The original 576-case surface suite retains its reported metrics, including the revealed-
  background sequence and single NR/private-SR unit conversion. Original spatial, continuous-
  motion, carrier and CPU policy checks also pass.

These are synthetic motion/depth checks, not proof of image-quality improvement in every game.
Compare in-game motion at the same Model resolution/Compress settings with **Skip MV unit
conversion off**. The method remains experimental.

Reproduce:

```powershell
./tools/compile_nr_interpass.ps1 -Check
# Optional timestamp baseline; quality tests also compare against the shipped original PSOs.
python tools/prepare_minified_motion_baseline.py
./tools/test_superior_port.ps1 -GpuSurfaceMotion -GpuMotion -GpuSpatial
./tools/test_superior_port.ps1 -GpuMinifiedMotionTiming
```

The optional timestamp baseline requires `2b2eb39f` in local history. It is saved only under
ignored `artifacts/validation/motion-minify-base`, never substituted for production shaders.

### Timestamp cost of the minification extension

Resident synthetic 3840x2160, RTX 4090 / driver 616.56, 1920x1080 RG16F source MV and 3840x2160
R32F depth. Compare **published Depth-aware 2b2eb39f versus the minification extension**, not
Point or Bilinear. Both variants include guide preparation, fused carrier/private-motion preparation
and their barriers/transitions. The candidate additionally includes the sparse correction dispatch
and its MV UAV barrier where required. Proprietary NR/DLSS inference and the rest of the frame
are excluded. There are no CPU uploads/readbacks inside timestamp intervals.

Each of 21 configurations uses eight alternating ABBA/BAAB blocks, eight warmup quartets and
forty measured quartets per block, four repetitions per timestamp, 640 samples per variant.
Background GPU use was 2% after the run. Fields are gentle affine motion with a sloped depth plane,
a single depth/motion edge, and many small diagonal depth islands (a stress fixture, not a game).
Balanced Compress is active in packed configurations; 28% is used because 25% + Balanced is
below the production minimum packed extent. Ordinary 25% tests exact source-MV-to-work 2:1
reduction. Ordinary 65% magnifies source MV and launches no correction; all three paired medians
are zero. Small differences relative to the listed variability must not be presented as a
confirmed whole-game cost or speedup.

| Configuration | Baseline median ms | Extended median ms | Paired delta ms | Paired stddev ms |
| --- | ---: | ---: | ---: | ---: |
| Balanced-affine, 28% | 0.054784 | 0.093440 | +0.038400 | 0.023518 |
| Balanced-affine, 40% | 0.032256 | 0.050432 | +0.018432 | 0.027063 |
| Balanced-affine, 65% | 0.103680 | 0.115456 | +0.011776 | 0.037572 |
| Balanced-affine, 100% | 0.287232 | 0.297728 | +0.010496 | 0.035804 |
| Balanced-edge, 28% | 0.036608 | 0.052224 | +0.015488 | 0.011499 |
| Balanced-edge, 40% | 0.053248 | 0.075776 | +0.022528 | 0.021706 |
| Balanced-edge, 65% | 0.076800 | 0.084480 | +0.007552 | 0.030151 |
| Balanced-edge, 100% | 0.216576 | 0.222720 | +0.006272 | 0.038051 |
| Balanced-islands, 28% | 0.020480 | 0.040448 | +0.020224 | 0.028593 |
| Balanced-islands, 40% | 0.031744 | 0.052480 | +0.020864 | 0.041648 |
| Balanced-islands, 65% | 0.108288 | 0.120320 | +0.012032 | 0.051992 |
| Balanced-islands, 100% | 0.280576 | 0.290816 | +0.009984 | 0.027332 |
| Ordinary-affine, 25% | 0.022784 | 0.041472 | +0.018624 | 0.024660 |
| Ordinary-affine, 40% | 0.036608 | 0.060160 | +0.023552 | 0.042230 |
| Ordinary-affine, 65% | 0.128768 | 0.128512 | +0.000000 | 0.023145 |
| Ordinary-edge, 25% | 0.031744 | 0.045824 | +0.014208 | 0.013320 |
| Ordinary-edge, 40% | 0.026368 | 0.038400 | +0.011904 | 0.024735 |
| Ordinary-edge, 65% | 0.088576 | 0.088064 | +0.000000 | 0.014327 |
| Ordinary-islands, 25% | 0.017920 | 0.036864 | +0.018944 | 0.021733 |
| Ordinary-islands, 40% | 0.035328 | 0.051968 | +0.016512 | 0.032630 |
| Ordinary-islands, 65% | 0.118016 | 0.117760 | +0.000000 | 0.021234 |

At 100% + Balanced, the recorded paired overhead is 0.006272–0.010496 ms; at 65% + Balanced,
0.007552–0.012032 ms; actual ordinary reduction at 25/40% costs 0.011904–0.023552 ms in these
fixtures. More aggressive 28% + Balanced costs 0.015488–0.038400 ms. Pure magnification retains
the exact previous GPU path. Results on other cards and full-game NR/DLSS timings still need
in-game verification.

[Raw timestamps](validation/superior-minified-motion-timing.csv),
[medians, variability and individual block deltas](validation/superior-minified-motion-timing.json),
and [GPU quality output](validation/superior-minified-motion-quality.txt).
