# Superior Compress: continuous motion sampling experiment

This records the `b231cdea` experiment. The current UI replaces its point-motion checkbox with
a three-method selector and also supports ordinary scaling; see
[current motion methods and verification](Superior-surface-motion.md).

The game test of `0dffb390` distinguished two failures at Model resolution 100% with Compress:
the converted motion was stable but soft in motion; skipping the additional unit conversion
looked sharper but floated. Original Compress also floated; native NR without Compress did not.
Skipping the conversion is therefore not treated as a fix.

This branch retains the converted motion units and changes **only Superior's motion sampling
before endpoint packing**. Other enlargement modes keep their original shaders and behavior.
It is a candidate remedy for the motion softness, pending a game comparison, not a confirmed
solution based solely on arithmetic tests.

## Motion contract

Render size, output size, valid motion subrect and its independent origin are already read from
the game's NGX parameters. MVLowRes selects render size as the motion reference; otherwise it
selects output size. Missing reference dimensions fall back to the valid motion region.
Superior converts the game's scaled vectors to native color pixels before applying:

`packedMotion = Pack(nativePixel + nativeMotion) - Pack(nativePixel)`

NR receives the packed motion region with scale 1; private DLSS receives the same packed
motion without a second conversion. The installed NR library's host code also divides
MVecScale by its valid motion extent before dispatch; this supports the existing pixel-unit
contract, rather than arbitrarily changing the scale to suppress temporal reuse.

For example, 10 render pixels at 1080p correspond to 20 native pixels at 4K. Uniform 75%
scaling would make that 15 pixels. Peripheral compression retains 20 in its 1:1 center when
both endpoints stay inside the center; the edges use their nonlinear endpoint mapping.
A global 75% factor would incorrectly shrink the center's motion too.

## Changed sampling

The preceding packer point-sampled the motion texture. When low-resolution motion has a smooth
spatial gradient, point replication onto the larger NR grid is a staircase, with errors that
vary as surfaces move relative to that source lattice. The new shader reconstructs that field
using four FP32 source samples, respecting the source's half-texel phase and valid subrect.
Interpolation happens in the original motion field **before** the nonlinear endpoint transform.

Constant source fields pass through unchanged before packing. If any of the four source
vectors is nonfinite, use the valid nearest vector or zero. If any component differs from the
nearest vector by more than one native pixel of displacement, retain nearest sampling rather
than blending across that motion discontinuity. This cutoff is a conservative heuristic, not
proof of surface membership or universal equivalence to the proprietary NR preprocessor.

Depth retains the original point sampling. Color packing, compression layout, center alignment,
carrier, private DLSS, reconstruction and gain formulas are unchanged. Natural Small/B high/medium
remains 0.900 and low remains 0.838 from the preceding test version. The new PSO is lazy and uses
the existing pack dispatch, descriptor table and two output textures; no extra pass, copy,
per-frame resource or synchronization is introduced.

## Compare in game

Use 100% Model resolution, the same Compress preset and the same moving scene. Clear the
diagnostic switches and turn **DLSS residual stabilization (experimental)** off for the first
comparison. In particular, leave **Skip MV unit conversion** off. Then change only:

- **Point-sampled motion (legacy)** off: new interpolated motion (default).
- **Point-sampled motion (legacy)** on: the preceding converted, point-sampled Superior path.

The new checkbox lives in Superior stage bypasses; its `[DlssNr]` key is
`SuperiorPointMotion`, default false. It resets NR/residual history through the diagnostic
mask, now tested in all 512 combinations. Wait several frames after each change. Check center
textures, rotation, moving objects, disocclusions and compressed edges, then repeat with
residual DLSS enabled. The native NR reference remains a separate comparison.

## Verification

```powershell
./tools/compile_nr_interpass.ps1 -Check
./tools/test_superior_port.ps1 -GpuMotion -GpuSpatial
./tools/test_superior_port.ps1 -GpuMotionTiming
```

The new GPU test uses an independently specified affine velocity field and discontinuous
surfaces, not an interpolation reference copied from the implementation. It covers 65/100%,
native/half/third-size motion, odd rasters, offsets in padded allocations, signed normalized
motion, FP16/FP32 sources, border clamping and nonfinite neighbors. Depth output must match
the original shader bit for bit. FP32 endpoint reassociation can change a constant field's
packed output by a few ULPs; the test bounds that below 0.0001 pixel.

RTX 4090 / driver 616.56: 96 cases / 3,465,216 pixels passed. Affine center reprojection RMS
over 242,208 pixels fell from 0.070920473 to 0.001505905 pixel, including source FP16
quantization; maximum error against the analytic motion field was 0.007816315 pixel.
This establishes improved reprojection of the synthetic field, not NR motion quality in game.

The timing harness uses resident 3840x2160 Balanced images, 1920x1080 RG16F source motion,
R32F depth and RG32F packed motion, at 65% and 100%. It compares constant, affine and
discontinuous fields using eight alternating ABBA/BAAB blocks, eight warmup quartets and
forty measured quartets per block, four dispatch repetitions per timestamp, 640 samples per
variant/configuration. The timed region is only guide packing plus both output UAV barriers;
NR/private DLSS inference and the rest of the frame are excluded.

The GPU was unloaded for this run (2% utilization observed afterward). In the 100% Balanced
affine case, guide packing medians were **0.203264 -> 0.210176 ms**, paired median cost
**+0.006528 ms**, paired standard deviation **0.021564 ms**. All eight block median deltas
were positive, between +0.005952 and +0.006976 ms. At 65%, the paired difference was
-0.000256 ms, too small to present as a meaningful performance improvement. This is a motion
quality experiment with a small measured stage cost at 100%, not an optimization claim.
See [raw timestamps](validation/superior-continuous-motion-timing.csv) and
[all medians, variability and block deltas](validation/superior-continuous-motion-timing.json).
No whole-frame game timing or confirmed removal of the reported game blur is claimed.
