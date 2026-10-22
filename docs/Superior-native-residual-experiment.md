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
