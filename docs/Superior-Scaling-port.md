# Superior Scaling port

**Experimental branch:** see [native-center reconstruction and the residual-DLSS checkbox](Superior-native-residual-experiment.md).
At 100% + Compress this branch restores the center bypass described there; the full-frame-guided
description below documents the merged baseline, not that experimental difference.

Upstream: janblade `main`, `2a5be38d` (0.1.33). Superior reconstruction reference: mattjaas v59, `914d8d1f50f2d52b11f9bacd7b6bf8fd7966d9e2`.

## Scope

**Superior Scaling is an Enlargement option, not a replacement for janblade's NR pipeline.**
It changes the downscale to the fixed custom Area, inserts cached Low36/Cross5 inter-passes according to the 0.15 worsening-gap rule,
and stabilizes the working residual with private DLSS SR in DLAA mode. The final v59 radius-one estimator and low/high shaping
are folded into janblade's final composition, without an extra full-resolution reconstruction texture/pass.

The ordinary janblade exposure system runs unchanged: Auto, scanned/game/manual sources, metering, adaptation, trim, anchors and Tune.
Superior does not remap exposure choices or change their defaults. The input encoding, HDR curves, LUT, input styles, scene cut,
model/detail reuse, edit cache, diagnostics and output composition remain in the shared host path and keep their UI controls.
Only Bilinear/SGSR1 selection is replaced when Superior is active. Extra final-colour recovery and exposure controls from the initial port were removed.
The private DLSS feature still forces HDR + Auto Exposure and uses unit exposure, independently of janblade's NR input exposure.

The UI adds **DLSS to temporally stabilize NR**: A (higher performance) and K (slower, more stable in motion).
NR model A/B is independent. Model resolution and pass settings stay configurable.
Frequency gains follow the NR runtime's effective model: on 310.9, A uses Main and Default/B (including legacy saved 3)
use Small. On 310.8, all presets use Main because that runtime has no Small model. The selected runtime's version is read
once at feature creation, not per frame. If version metadata is absent, the 310.9 mapping is used with a warning in the log.
Passes 2/3 use their explicit preset when set, including Default; otherwise they inherit pass 1. Passes 4+ inherit pass 1.
The same mapping feeds both inter-pass scheduling/shaping and final block shaping. Informational Area/success banners are hidden.
At 100% without compression, janblade's original multipass boundaries remain in use. At 100% with active edge compression that actually reduces NR, private DLSS and guided reconstruction also run. Above 100% their previous behavior is retained. Native guide units are corrected from their actual valid/reference dimensions.
Below 100%, depth and MV are matched to the working grid with independent regions/offsets; the private DLSS path reuses current depth.
After frames skipped by the edit cache, private DLSS resets its history on the next refresh.
With edge compression at or below 100%, private DLSS now works on the packed NR grid and reuses its current depth and endpoint-transformed motion. Guided reconstruction unwarps the signed residual directly into the native composition. Inter-pass correction stays on the packed model grid.
The fixed Area/inter-pass/carrier/low shaders retain v59 bytecode and their lazy PSOs. The host composition PSOs use janblade's existing 256-byte ABI.

| NR model | Style | High/medium | Low |
| --- | --- | ---: | ---: |
| A main | Standard | 0.800 | 0.661 |
| A main | Natural | 0.750 | 0.787 |
| A main | Cinematic | 0.950 | 0.784 |
| B small | Standard | 0.950 | 0.861 |
| B small | Natural | 0.925 | 0.838 |
| B small | Cinematic | 0.955 | 0.861 |

Gain selection clamps the preset and style exactly like the NR model does (PassPreset: min(preset, 3); PassStyle:
min(style, 2)), so a hand-edited ini value above the range gets the calibrated gains of the model that actually runs.
The uncalibrated pair, high/medium **0.950** and low **0.850**, remains in the table functions for identifiers outside
the calibrated range, which only a future NR model/style range could reach (it would also need its PassPreset/PassStyle
clamp raised). A future change to the NR runtime's meaning of an existing ID (such as Default) still needs an explicit
model-mapping update.

Gains use the v59 resolution correction and Structure * Intensity. High/medium multiply within a block; low is the block's arithmetic mean.
An inter-pass runs before the next pass only if the absolute high/low gap would both increase and exceed 0.15.
Final shaping uses only the tail after the last successful inter-pass. Carrier K=1, fixed linear 0.5 encoding, guided radius 1, range sigma .015, spatial sigma 3, strength 1.
No K/headroom analysis is introduced. Native Vulkan is outside the original v59 implementation; DirectX 12 and supported D3D12 bridges are required.

### When Superior cannot run

Superior never leaves NR off. Native Vulkan with Superior selected runs Matched residual. On D3D12, if the temporal
path cannot run -- no depth/motion guides, NVIDIA DLSS SR through NGX unavailable (non-NVIDIA GPU, the vendor-neutral
NR port, missing nvngx_dlss), its surfaces cannot be created, or the private DLSS creation is never submitted within
240 frames -- Superior gives up for the session with one warning in the log, skips that one frame, and the ordinary path
runs with **Matched residual** from the next frame. The menu says why; selecting Superior again retries. When NGX itself
was unavailable (or on Vulkan) the Superior entry is greyed out. If the requested private DLSS preset (A or K) cannot be
created by the installed runtime, it is created once more with the runtime's default preset (logged once).

Every non-Superior resolve while Superior is selected (100% without compression, supersampling, or after giving up)
explicitly runs Matched residual. The private DLSS's first evaluate waits for a later submission epoch, exactly like the
NR feature. Its NGX handles are dropped (never released or evaluated) when the game shuts NGX down, and rebuilt under
the next runtime. With Auto model resolution the exact raster only moves when the ordinary 16-pixel cell moves, so
dynamic resolution does not rebuild NR and the private DLSS every frame. Superior's own dispatches use a separate
descriptor ring, created the first time Superior runs; users who never select it allocate nothing extra.

### Frozen v59 shader baseline

`OptiScaler/shaders/dlssnr/superior/precompile/dlssnr.hlsl` is a **second, frozen copy of the NR shader** (v59,
`914d8d1f`, plus a documentation header). It keeps the Superior passes on v59's exact bytecode and does **not** receive
composition, exposure, colour-encoding or other changes made to the live `precompile/dlssnr.hlsl`. It builds
`DlssNr_Shader` (no defines), the `dlssnr_interpass_*` and `dlssnr_superior_*` blobs and, through `RawLow.hlsl`,
`host_low_raw`. Its 328 KB `DlssNr_Shader.cso` is the fallback PSO for every Superior pass without a specialised PSO
(`superior/Shader.inl`, `DispatchSuperiorPass`): the unit exposure (mode 7), the clamp between NR passes (mode 8)
and the private DLSS guide matching (mode 10) always run on it; the Area downsample (2), final low field (18) and the
inter-pass passes (34, 36) use it only if their own PSO cannot be created. The final composition is **not** frozen: it
is the live shader compiled with `superior/HostResolve.hlsli` (`host_resolve_*`, `host_normal_*`,
`host_cached_*`), so composition and exposure fixes do reach Superior's final image.

### Carrier encoding limits

The private DLSS stabilises the signed residual E = NR - proxy, carried as `clamp(0.5 + E, -1, 2)` in an RGBA16F
texture (modes 37/39) and decoded as `DLSS(carrier) - 0.5` (mode 38). This is unchanged from v59; the limits, not
measured in game:

- Range: |E| above 1.5 is clipped by the encode. Around the 0.5 bias FP16 has a step of 2^-11 (about 4.9e-4) for
  carrier values in [0.5, 1) and 2^-10 above 1, so residuals are quantised to about 2.4e-4 (4.9e-4 above E = 0.5)
  before DLSS sees them; smaller detail survives only through DLSS's own accumulation.
- Negative carrier: E below -0.5 gives a carrier below zero. The private feature is created with IsHDR; if the DLSS
  runtime clamps negative input colour, those residuals come back as -0.5 at most (darkening limited to 0.5 in the
  encoded domain). Whether a given runtime clamps is not verified.
- The decode sanitises non-finite DLSS output to the bias (no change).

### Calibrating new models and styles

The [full-frame Low Frequency calibration guide](DLSS_NR_Low_Frequency_Full_Frame_Calibration.md)
describes matched RAW P50/P100 captures, true NR100 residual references, production guided/Low64
reconstruction and a joint two-band least-squares fit over every RGB pixel. Accumulate and solve
in FP64; use its Low coefficient as the calibration baseline across representative scenes.
The fitted High/Medium coefficient is only an intermediate regression parameter. No automatic
criterion has been found that reliably reproduces the preferred visual High/Medium result;
those defaults are tuned by eye in-game, including motion, and the final pair must be validated
together. These are offline calibration steps, with no per-frame fitting in the runtime.

## Compress screen edges

Depth and MV use independent active rectangles and offsets. Before the nonlinear endpoint transform, game MV units are
converted into the current NR colour frame's pixels using its native extent divided by the declared render/output MV
reference (selected by MVLowRes). Missing reference dimensions fall back to the valid motion region, never its allocation
or the depth dimensions. **This additional unit conversion is enabled only with Superior Scaling, on D3D12/bridges.**
Without Superior, D3D12 and native Vulkan keep janblade's original raw MV scale and original packer. The packer's existing
endpoint transform remains part of original Compress behavior; it has not been disabled. Superior frequency gains and
inter-pass shaping are likewise confined to Superior. The native Vulkan off-Superior evaluation body matches upstream.
Packed motion is already in packed pixels; NR receives scale 1. Depth stays point sampled on the matching warped grid.

Superior's ordinary raster remains exact, while the actual packed NR raster is aligned to 16 pixels. Normal Superior
rendering at or below 100% bypasses the ordinary raster after packing. Legacy enlargement, above-native processing and
on-demand exposure calibration still use the original unpacker with the exact ordinary size.

Frequency gains remain global across the image, with **separate resolution scales for high/medium and low**:
high/medium use `sqrt((ordinaryWidth/nativeWidth) * (ordinaryHeight/nativeHeight))`, from Model resolution before edge
compression; low uses `sqrt((modelWidth/nativeWidth) * (modelHeight/nativeHeight))`, from the actual final packed NR
dimensions. Both include raster rounding. Compress screen edges therefore modifies only low's resolution correction;
at Model resolution 100%, high/medium remain neutral 1 while low still accounts for compression. Center and edges share
the same gains. This split feeds lookahead scheduling, every inter-pass block and final tail shaping. Without compression,
both scales are identical and the previous gains are preserved. Model tables, Structure * Intensity and the high product /
low mean rules are unchanged. Historical GPU measurements below and in the four-optimization report predate this policy
change; this coefficient/schedule change has no newly claimed performance result and changes no shader bytecode.
At or below 100% with active compression, the sequence is:

| Stage, 3840x2160 output | 65% + Balanced | 100% + Balanced |
| --- | --- | --- |
| Original Compress colour/depth/motion pack | 2240x1264 | 3456x1952 |
| NR and inter-passes | 2240x1264 | 3456x1952 |
| Private DLSS residual stabilization (DLAA A/K) | 2240x1264 | 3456x1952 |
| Low64 frequency field | Packed grid / 8, rounded up | Packed grid / 8, rounded up |
| Guided unwarp + native composition, one dispatch | 3840x2160 | 3840x2160 |

Guided residual reconstruction covers the **entire output frame**, including the uncompressed
center at Model resolution 100%. The experimental native-center bypass and its transition were
reverted after in-game motion blur was reported. This restores the runtime tree at `1ff8c811`
exactly; the earlier low-only compression correction remains. Any future center bypass needs
separate temporal validation, not just static image or kernel timing checks.

**The colour packer and the packed NR guide shaders are unchanged, including their compiled bytecode.** There is no
intermediate bilinear unpack of proxy/answer in normal Superior rendering. The original full-resolution encoded colour
guides the final 3x3 residual reconstruction: each output query is mapped with the original nonlinear Pack transform,
then the guided taps and Low64 lookup use packed coordinates. Only the stabilized, shaped residual is carried back;
the original image remains the composition base. Center/edge density affects sample mapping, never the global gain scale.
Packed vectors already contain `Pack(p+v)-Pack(p)` in packed pixels; carrier preparation uses scale 1, sanitizes nonfinite
pairs and writes zero motion on Hold. The private feature receives the current packed depth directly, preserving inversion.
Its input/output dimensions are the actual packed model dimensions. Layout/offset/shift, packed/ordinary mode and size
changes recreate/reset the feature through the existing deferred GPU lifetime mechanism.

The packed HDR/SDR resolve uses two separate PSOs and the otherwise unused D3D12 motion slot t3 for immutable 11-float4
warp metadata. It uploads once per feature generation, with no per-frame metadata copy/dispatch, no new root signature
and no full-resolution mapping texture. Exposure remains at t4. Ordinary guide matching, the separate unpack dispatch
and its two intermediate colour allocations are omitted. Exposure calibration alone can request its original unpacked
proxy on demand; this does not move rendering or private DLSS off the packed grid. Other enlargement modes retain their
original resamplers. Above-100% behavior is unchanged.
If compression is disabled, invalid or does not reduce NR, native 100% retains ordinary NR without private DLSS.
A raw-answer PSO remains for above-native ordinary grids where compression reduces the actual NR pixel count below native.
Its raw low field uses the cooperative 8x16 Low64 cache, with exactly the reference N-P arithmetic.
Separate raw/temporal/packed PSOs avoid a per-pixel mode branch. At the port these host PSOs were byte-identical to 389e8d96 and 24a2d7bf. In this repository the host estimator also sanitises non-finite taps, which changes their bytecode but no finite result (bit-exact GPU comparison against the previous blobs, see validation/README.md); the raw SDR and HDR builds were identical and are one blob, host_resolve_raw.

Validation on RTX 4090 / driver 616.56: 48 CPU metadata/size/gain cases; D3D12 pack/unpack with native, pre/post-SR,
independent/clipped/offset depth and MV, low/high-resolution vectors and exact P59/P65 grids; Vulkan spatial smoke;
2304 host-composition comparisons / 5,974,272 words (including native signed-residual guided resolve); 216 raw cached-low comparisons / 59,616 words. GPU comparisons passed
bit-exact where applicable. Proprietary NR/DLSS inference and full-game temporal behavior still require game validation.

The packed-guided path additionally passed **3072 comparisons / 666,267,648 components**, bit-exact against an
independent scalar warp plus the extracted v59 estimator in janblade composition: P50/P65/P100, Light/Balanced/Strong,
asymmetric offsets/shifts, odd dimensions, HDR curves, SDR, split/zoom, debug views, input encoding, exposure sources and
FP16/FP32 source/target storage. Packed carrier/motion preparation passed 48 comparisons / 4,202,496 words, bit-exact
for unit motion scale, Hold, nonfinite pairs and FP16/FP32. These validate the requested new pipeline's arithmetic,
**not temporal equivalence to the previous unpack-before-DLSS pipeline**. NGX inference and moving footage require game validation.

### Current packed-route stage timing

RTX 4090 / 616.56, synthetic 3840x2160, Balanced compression, NR Small/Standard shaping coefficients,
Structure=Intensity=1. Resident GPU timestamps, 8 ABBA/BAAB blocks with separate warm-up, 640 samples per variant.
FP16 production colour/intermediate/output formats, R32/RG32 guides, FP32 original frame.

| Profile | NR slider | Previous route ms | Packed route ms | Paired delta ms | Previous / packed stddev ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| SDR | 65% | 0.697088 | 0.398848 | -0.299904 | 0.133596 / 0.094521 |
| SDR | 100% | 1.130496 | 0.567296 | -0.609984 | 0.074536 / 0.056834 |
| HDR 0 | 65% | 0.673536 | 0.368896 | -0.309696 | 0.077042 / 0.059670 |
| HDR 0 | 100% | 1.096320 | 0.534272 | -0.601984 | 0.079325 / 0.059789 |

Baseline: 389e8d96, ordinary-grid stabilization after bilinear unpack. Measured stages include ordinary unpack/guide
matching when present, fused carrier/motion preparation, identity carrier decode, Low64 and guided composition.
**NR and private DLSS inference are excluded**; both variants use identity carrier output in place of private inference.
Common colour/guide packing is prepared outside timestamps. The new feature has fewer private-DLSS pixels, but the actual
inference saving is unmeasured. These figures are component results and do not establish total in-game performance or
visual parity. Raw samples are summarised in [validation/README.md](validation/README.md).

### Superior isolation, residual flags and lossless host optimization

The model-resolution tooltip now explains that a few percentage points higher or lower can give a better match to native NR.
The residual DLSS feature explicitly requests **IsHDR + AutoExposure + MVLowRes** with unit exposure and A/K DLAA; these
flags are independent of the game's exposure settings and are passed to the private parameter object through the direct
NGX proxy entry point. No game/NR exposure flags or options are changed.

The motion audit covers raw versus matched motion subrects, pre/post-SR, low/high-resolution vectors, independent/clipped
guide rectangles, missing reference dimensions, packed endpoint transforms, Hold and changes of layout. NR uses units
of the motion subrect it actually reads (the matched grid below native; otherwise the game's valid motion region).
Private DLSS receives work-grid pixel vectors, or unit-scale packed vectors after the nonlinear transform. If render/output
reference metadata is absent, Superior now consistently falls back to the original valid motion region. Other enlargement
modes retain their original guide conversion, model grid, gains-free composition and resamplers.

Packed host resolve now samples/decode the initial proxy/model pair only for diagnostics. The normal path rebuilds these
values later from the native guide and guided residual, so those initial reads were unused. All guided taps, weights,
Low64, FP16/FP32 storage and frequency coefficients are retained. Debug views still execute their original samples.
Only packed HDR/SDR PSOs use the optimization: applying it to ordinary grids regressed the synthetic fixture and was
rejected. The four ordinary/raw host PSOs and the default janblade, colour-packing and guide shader binaries remain unchanged.

Validation: 2304 ordinary/raw host comparisons plus 3072 packed comparisons, bit-exact against the frozen 67318a5d host
source with an independent warp/estimator reference. Coverage includes HDR/SDR debug views and clean output. CPU checks
verify the off-Superior guide constants are byte-identical to original constants; GPU tests additionally cover legacy
low/high-resolution vectors at native/reduced scale. D3D12 spatial/raw-low/carrier tests and native Vulkan smoke pass.
These fixtures validate arithmetic and resource layouts; they do not run proprietary NGX inference or establish full-game parity.

RTX 4090 / 616.56, synthetic 3840x2160, Balanced compression, Small/Standard shaping, Structure=Intensity=1, debug off.
Resident final-composition timestamps, 8 ABBA/BAAB blocks, separate warm-up, 640 samples per variant/profile/scale:

| Profile | NR slider | Before ms | After ms | Paired delta ms | Before / after stddev ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Packed SDR | 65% | 0.342400 | 0.335616 | -0.005760 | 0.045816 / 0.045176 |
| Packed SDR | 100% | 0.363648 | 0.346880 | -0.013952 | 0.043253 / 0.040590 |
| Packed HDR 0 | 65% | 0.320768 | 0.298240 | -0.021504 | 0.045300 / 0.038454 |
| Packed HDR 0 | 100% | 0.339456 | 0.317184 | -0.021824 | 0.091630 / 0.088421 |
| Ordinary SDR control | 65% | 0.299904 | 0.299904 | -0.000064 | 0.040056 / 0.039698 |
| Ordinary HDR 0 control | 65% | 0.268544 | 0.270336 | -0.000576 | 0.074417 / 0.078824 |

All eight paired block medians are negative for each packed profile. Ordinary controls use identical bytecode and show
only sampling variability. These are **final-shader-only** timings, excluding NR/DLSS inference and other pipeline stages.
Raw samples: see [validation/README.md](validation/README.md). Full in-game savings remain unmeasured.

Audit of the other Superior stages: fixed Area and colour packing, cached inter-pass Low36/Cross5, carrier preparation,
Low64 and private DLSS retain their prior algorithms. Removing the carrier decode store or approximating filtered samples
could change FP16 rounding/interpolation and was not adopted as a quality-preserving optimization. No extra per-frame
diagnostics, GPU readbacks, textures, dispatches or synchronization are introduced by this change.

### Cost of the spatial correction before native temporal stabilization

RTX 4090 / 616.56, synthetic 3840x2160, Balanced compression, NR Small/Standard gain settings.
Resident GPU timestamps, 8 alternating ABBA/BAAB blocks, separate warm-up, 640 samples per variant.
Final-stage inputs use FP16 proxy/answer/residual and output; the original frame is FP32.

| Stage | NR slider | Before ms | After ms | Paired delta ms |
| --- | ---: | ---: | ---: | ---: |
| guide_pack | 65% | 0.098048 | 0.098048 | +0.000000 |
| guide_pack | 100% | 0.142080 | 0.142336 | -0.000128 |
| final_sdr | 65% | 0.306944 | 0.309632 | +0.000640 |
| final_sdr | 100% | 0.247552 | 0.322816 | +0.075392 |
| final_hdr0 | 65% | 0.297344 | 0.297472 | +0.000192 |
| final_hdr0 | 100% | 0.256256 | 0.328192 | +0.072064 |

Historical comparison up to 606638ef; it does not measure the subsequently enabled private DLSS/guided native path.
At 100% with compression, the previous final pass omitted frequency shaping; the corrected path does real additional
Low64 work. This is a correctness cost, not an optimization gain. At reduced scales the temporal resolve shader is
byte-identical to the baseline; the gain coefficients change. Raw samples: see [validation/README.md](validation/README.md).
These component timings exclude NR/DLSS inference, changes in inter-pass count, colour pack/unpack and the complete game.

### Native temporal final-stage cost

RTX 4090 / 616.56, synthetic 3840x2160, 100% NR slider with Balanced compression, Small/Standard,
Structure=Intensity=1. The same timestamp method and FP16 production texture formats as above compare
606638ef's raw-answer Low64/composition against signed-residual Low64/guided composition at native size.

| Profile | Raw baseline ms | Guided native ms | Paired delta ms | Baseline / candidate stddev ms |
| --- | ---: | ---: | ---: | ---: |
| SDR | 0.323840 | 0.337664 | +0.013056 | 0.059459 / 0.100831 |
| HDR 0 | 0.328960 | 0.306432 | -0.022272 | 0.039076 / 0.032653 |

These small, noisy differences describe only the final stage and do not establish a total performance gain.
**Private DLSS inference, carrier encode/decode and guide preparation are excluded**; enabling them adds work
whose complete cost must be measured in game. The candidate uses a prepared decoded residual outside the timing window.
Raw samples: see [validation/README.md](validation/README.md).

## Building

Shaders are embedded in the DLL; no loose HLSL/CSO files are needed. After any change to `precompile/dlssnr.hlsl`,
`precompile/dlssnr_spatial_warp.hlsli` or `superior/*.hlsl*`, rebuild the Superior blobs with
`tools/compile_nr_interpass.ps1` (PowerShell and the repository's dxc only) and commit them. CI
(`.github/workflows/superior_checks.yml`) rebuilds them with `-Check` and fails on any difference, and builds and
runs the CPU policy tests. The private temporal stabilisation needs NVIDIA's DLSS SR runtime (`nvngx_dlss.dll`).

## Validation

The integration was narrowed after commit `21fb2960`: that earlier build replaced unrelated janblade input/exposure/composition and is superseded. Check the build matching the current commit.
CPU contracts cover fixed values, A/K, A/B gains (including mixed passes), exact raster rounding, native/reduced MV units,
strict scheduling, decreasing gaps, low means, failed boundaries, PSO guards, independent guide regions and surface formats.
Recompiling the Superior shaders reproduced the v59 bytecode hashes. The default janblade shader bytecode is unchanged; new Superior-only PSOs compile its shared composition with the v59 residual estimator.

The corrected host integration passed **768 GPU comparisons / 1,991,424 words**, bit-exact against
same-compiler upstream composition and an independently extracted v59 estimator inside that composition.
Coverage: eight HDR curves, SDR, manual/game/automatic exposure, exposure anchors, gamma 2.2/PQ encoding,
clean output, skin protection, split comparison, odd dimensions and FP16/FP32 targets.
The shipped legacy shader blob also remains byte-identical to upstream. Its original compiler settings produce
some rounding differences from a fresh compile, so it is preserved rather than regenerated.

The older tests below validate frozen v59 components, **not the corrected integration as a whole**.
They ran on **NVIDIA GeForce RTX 4090, driver 616.56**:

| Test | Coverage | Result |
| --- | --- | --- |
| Carrier/decode/shaping | 52 cases | Bit-exact |
| Carrier + guide preparation | 240 cases, 10,957,920 words | Bit-exact |
| Encode/keep/Area | 3,840 cases, 66,528,000 words | Bit-exact |
| Exposure chain | 2,880 cases, 217,294,560 words | Zero measured numerical difference |
| HDR soft-knee resolve | 7,776 cases, 43,830,720 words | Bit-exact |
| Final low | 198 cases, 54,648 words | Bit-exact |
| Cached Low36/Cross5 | 480 comparisons | Zero measured numerical difference |

These fixtures cover FP16/FP32 textures, signed HDR, non-finite values, borders, odd dimensions and cropped guides where applicable.
Some compare the frozen generic/reference path against its optimized specialization; others compare original and port blobs directly.
They do not execute proprietary NR/DLSS inference or the complete game's command stream.

### GPU timing scope

Historical timing before the integration correction; this does not measure the new host composition.

Resident **Low64 + guided resolve only**, synthetic 3840x2160 image, P50/P59/P65, SDR and HDR 0/2/4.
Eight blocks alternate ABBA/BAAB with separate warm-up, 40 quartets per block and four GPU repetitions per timestamp:
640 samples per variant/profile/scale. Both use the same v59 resolve bytecode. No NR or DLSS inference is included.

| Profile | P50 original / port ms | P59 original / port ms | P65 original / port ms |
| --- | --- | --- | --- |
| SDR | 0.340736 / 0.340736 | 0.359040 / 0.360192 | 0.361472 / 0.361472 |
| HDR 0 | 0.300672 / 0.300288 | 0.295680 / 0.295168 | 0.300800 / 0.300160 |
| HDR 2 | 0.262912 / 0.262656 | 0.279040 / 0.278784 | 0.293632 / 0.293632 |
| HDR 4 | 0.261376 / 0.261376 | 0.278784 / 0.278784 | 0.292608 / 0.292608 |

Paired median differences are -0.000128 to +0.000384 ms, much smaller than observed variability
(per-variant standard deviation about 0.038-0.056 ms). There is no resolved timing difference in this fixture.
Raw samples: see [validation/README.md](validation/README.md).
This does **not** prove equal total in-game performance or complete temporal image equivalence.

### Reproduce and complete game validation

The subsequent four-candidate optimization study, production changes, rejected experiments and exact reproduction
commands are documented in [Superior-four-optimizations.md](Superior-four-optimizations.md).

From Windows with MSVC x64 tools (Python only for the modes that need the port's history; see
[validation/README.md](validation/README.md)):

```powershell
./tools/test_superior_port.ps1
./tools/test_superior_port.ps1 -GpuHost -GpuSpatial
./tools/test_superior_port.ps1 -GpuSpatialTiming
./tools/test_superior_port.ps1 -GpuNativeTemporalTiming
./tools/test_superior_port.ps1 -GpuPackedTiming
./tools/test_superior_port.ps1 -GpuHostOptTiming
./tools/test_superior_port.ps1 -GpuQuality -ReferenceShaderDirectory <v59-checkout>/OptiScaler/shaders/dlssnr/precompile
./tools/test_superior_port.ps1 -GpuTiming -ReferenceShaderDirectory <v59-checkout>/OptiScaler/shaders/dlssnr/precompile
```

GPU timing requires an otherwise idle GPU. For full validation, compare the original v59 DLL against the port in the same game,
scene/camera, output resolution, DLSS/NR runtimes and explicit settings (including exposure trim and model precision).
Warm each independently and alternate ABBA. Start with the user's 2-pass Cinematic at 65%, then P50/P59 and native 100%,
A/K SR, A/B/mixed NR, HDR/SDR, motion/disocclusion, resolution and preset switches, hold and crop offsets.
Record GPU milliseconds, medians and spread, not only FPS. Compare captured output and moving footage.
No full-game result or native Vulkan implementation is claimed by the headless tests.
The corrected complete path has not been timed against v59; equal total performance is not established.
