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
Hold still emits zero private motion. PSOs are lazy; there are no new per-frame allocations,
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
