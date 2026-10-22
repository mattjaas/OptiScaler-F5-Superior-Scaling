# Superior Scaling tests. With no switch: the self-contained CPU tests only (no GPU, no reference files, no Python
# needed), as CI runs them (.github/workflows/superior_checks.yml). Each switch adds a GPU group:
#
#   needs only a D3D12 GPU (tests/nr_gpu harness):  -GpuSpatial -GpuPackedTiming
#   needs the port's own commits (fetch the PR #230 head: git fetch origin pull/230/head) and Python; skipped with a
#   message when they are missing:                  -GpuHost -GpuSpatialTiming -GpuNativeTemporalTiming
#                                                   -GpuHostOptTiming -GpuCandidates -GpuCandidateTiming
#   needs -ReferenceShaderDirectory = the original v59 precompile directory (mattjaas v59 914d8d1f, not in this
#   repository); refused up front with a message when missing:  -GpuQuality -GpuTiming
#
# Outputs go to the ignored artifacts/validation/. -VcVars: path to vcvars64.bat when vswhere cannot find MSVC.
param(
    [string]$ReferenceShaderDirectory,
    [string]$VcVars,
    [switch]$GpuQuality,
    [switch]$GpuTiming,
    [switch]$GpuHost,
    [switch]$GpuSpatial,
    [switch]$GpuSpatialTiming,
    [switch]$GpuNativeTemporalTiming,
    [switch]$GpuPackedTiming,
    [switch]$GpuHostOptTiming,
    [switch]$GpuCandidates,
    [switch]$GpuCandidateTiming,
    [switch]$GpuNativeCenter,
    [switch]$GpuNativeCenterTiming,
    [switch]$GpuDiagnostics,
    [switch]$GpuDiagnosticsTiming,
    [switch]$GpuMotion,
    [switch]$GpuMotionTiming
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$skipped = 3 # prepare_superior_*.py: the port's history is not in this clone
Push-Location $repo
try {
    if ($GpuDiagnosticsTiming) { $GpuDiagnostics = $true }
    if ($GpuMotionTiming) { $GpuMotion = $true }
    if (($GpuQuality -or $GpuTiming) -and
        (!$ReferenceShaderDirectory -or !(Test-Path -LiteralPath $ReferenceShaderDirectory -PathType Container))) {
        throw '-GpuQuality and -GpuTiming compare against the original v59 shaders: pass -ReferenceShaderDirectory ' +
              '<v59 checkout>/OptiScaler/shaders/dlssnr/precompile (mattjaas v59, 914d8d1f). It is not in this repository.'
    }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        & python tests/test_nr_superior_scaling.py
        if ($LASTEXITCODE -ne 0) { throw 'Shader identity/ABI test failed' }
    } else {
        Write-Warning 'Python not found: tests/test_nr_superior_scaling.py (v59 identity/ABI) not run here; CI runs it.'
    }
    if (!$VcVars) {
        $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
        if (Test-Path $vswhere) {
            $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($vs) { $VcVars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat' }
        }
    }
    if (!$VcVars -or !(Test-Path $VcVars)) { throw 'MSVC x64 tools are required (pass -VcVars <path to vcvars64.bat>)' }
    $out = 'artifacts/validation'
    New-Item -ItemType Directory -Force $out | Out-Null

    # Runs a prepare_superior_*.py step; false (with a warning) when the port's history is missing.
    function Prepare([string]$what, [string[]]$arguments) {
        if (!$python) { Write-Warning "$what needs Python; skipped"; return $false }
        & python @arguments | Out-Host
        if ($LASTEXITCODE -eq $skipped) { Write-Warning "$what skipped (see the message above)"; $global:LASTEXITCODE = 0; return $false }
        if ($LASTEXITCODE -ne 0) { throw "$what failed" }
        return $true
    }

    $cpu = @('superior_policy', 'spatial_policy', 'interpass_policy', 'nr_guides_smoke', 'nr_surface_formats_smoke',
             'superior_diagnostics_policy')
    $quality = @('superior_carrier', 'superior_prepare_quality', 'superior_input_quality',
                 'superior_exposure_quality', 'superior_softknee_quality', 'superior_tall_low_quality',
                 'interpass_low_cache_quality')
    if ($GpuSpatialTiming -or $GpuNativeTemporalTiming) {
        if (!(Prepare 'Spatial timing baseline' @('tools/prepare_superior_host_tests.py', '--timing-baseline'))) {
            $GpuSpatialTiming = $false; $GpuNativeTemporalTiming = $false
        }
    }
    if ($GpuHostOptTiming -and !(Prepare 'Host optimization baseline' @('tools/prepare_superior_host_tests.py', '--host-opt-baseline'))) {
        $GpuHostOptTiming = $false
    }
    if ($GpuHost -and !(Prepare 'Host reference shaders' @('tools/prepare_superior_host_tests.py'))) { $GpuHost = $false }
    if ($GpuDiagnostics -and !$GpuHost) {
        if (!(Prepare 'Diagnostic host references' @('tools/prepare_superior_host_tests.py'))) { $GpuDiagnostics = $false }
    }
    if ($GpuNativeCenter -or $GpuNativeCenterTiming -or $GpuDiagnostics) {
        if (!(Prepare 'Native center reference' @('tools/prepare_native_center_tests.py'))) {
            $GpuNativeCenter = $false; $GpuNativeCenterTiming = $false; $GpuDiagnostics = $false
        }
    }
    if (($GpuCandidates -or $GpuCandidateTiming) -and !(Prepare 'Candidate shaders' @('tools/prepare_superior_candidates.py'))) {
        $GpuCandidates = $false; $GpuCandidateTiming = $false
    }

    $names = @($cpu)
    if ($GpuQuality) { $names += $quality }
    if ($GpuTiming) { $names += 'superior_tall_low_timing' }
    if ($GpuSpatial) { $names += @('nr_spatial_dx12_gpu', 'spatial_raw_low_quality', 'packed_prepare_quality') }
    if ($GpuSpatialTiming -or $GpuNativeTemporalTiming) { $names += 'spatial_timing' }
    if ($GpuPackedTiming) { $names += 'packed_stage_timing' }
    if ($GpuHostOptTiming) { $names += 'host_opt_timing' }
    if ($GpuHost) { $names += @('superior_host_quality', 'packed_host_quality') }
    if ($GpuNativeCenter -or $GpuNativeCenterTiming) { $names += @('native_center_mask', 'carrier_bypass_quality') }
    if ($GpuNativeCenterTiming) { $names += 'native_center_timing' }
    if ($GpuDiagnostics) { $names += 'stage_bypass_quality' }
    if ($GpuDiagnosticsTiming) { $names += 'stage_bypass_timing' }
    if ($GpuMotion) { $names += 'continuous_motion_quality' }
    if ($GpuMotionTiming) { $names += 'continuous_motion_timing' }
    if ($GpuCandidates -or $GpuCandidateTiming) {
        $names += @('four_candidates_quality', 'fused_pack_experiment')
        if ($GpuCandidateTiming) { $names += 'four_candidates_timing' }
    }
    foreach ($name in $names) {
        $src = if ($name -like 'nr_*_smoke' -or $name -eq 'nr_spatial_dx12_gpu') { "tests/$name.cpp" } else { "tests/nr_gpu/$name.cpp" }
        cmd /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /EHsc /O2 `"$src`" /Fe:$out/$name.exe /Fo:$out/$name.obj /link d3d12.lib dxgi.lib"
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
    }
    foreach ($name in $cpu) {
        & "$out/$name.exe"
        if ($LASTEXITCODE -ne 0) { throw "Test failed: $name (exit $LASTEXITCODE)" }
    }
    if ($GpuCandidates -or $GpuCandidateTiming) {
        & "$out/four_candidates_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Candidate bit-exact comparison failed' }
        if ($GpuCandidateTiming) {
            foreach ($family in @('fixed', 'cached_fixed', 'lds', 'final')) {
                & "$out/four_candidates_timing.exe" $family
                if ($LASTEXITCODE -ne 0) { throw "Candidate timing/4K quality failed: $family" }
            }
            & "$out/fused_pack_experiment.exe" timing
        } else {
            & "$out/fused_pack_experiment.exe"
        }
        # Fusion is an intentionally retained rejected experiment, never a production path.
        if ($LASTEXITCODE -ne 2) { throw 'Fused experiment result changed: review the recorded rejection' }
        $global:LASTEXITCODE = 0
    }
    if ($GpuSpatial) {
        & "$out/nr_spatial_dx12_gpu.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Spatial GPU validation failed' }
        & "$out/spatial_raw_low_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Spatial raw low GPU comparison failed' }
        & "$out/packed_prepare_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Packed carrier/guide comparison failed' }
    }
    if ($GpuHost) {
        & "$out/superior_host_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Host composition comparison failed' }
        & "$out/packed_host_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Packed host composition comparison failed' }
    }
    if ($GpuNativeCenter -or $GpuNativeCenterTiming) {
        & "$out/native_center_mask.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Native center mask failed' }
        & "$out/carrier_bypass_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'No-DLSS carrier roundtrip failed' }
    }
    if ($GpuNativeCenterTiming) {
        & "$out/native_center_timing.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Native center timing failed' }
    }
    if ($GpuDiagnostics) {
        & "$out/stage_bypass_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Stage bypass GPU test failed' }
    }
    if ($GpuDiagnosticsTiming) {
        & "$out/stage_bypass_timing.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Stage bypass GPU timing failed' }
    }
    if ($GpuMotion) {
        & "$out/continuous_motion_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Continuous Compress motion GPU test failed' }
    }
    if ($GpuMotionTiming) {
        & "$out/continuous_motion_timing.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Continuous Compress motion GPU timing failed' }
    }
    if ($GpuSpatialTiming) {
        & "$out/spatial_timing.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Spatial GPU timing failed' }
    }
    if ($GpuNativeTemporalTiming) {
        & "$out/spatial_timing.exe" --native-temporal
        if ($LASTEXITCODE -ne 0) { throw 'Native temporal stage timing failed' }
    }
    if ($GpuPackedTiming) {
        & "$out/packed_stage_timing.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Packed route GPU timing failed' }
    }
    if ($GpuHostOptTiming) {
        & "$out/host_opt_timing.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Host optimization GPU timing failed' }
    }
    $candidate = 'OptiScaler/shaders/dlssnr/superior/precompile'
    if ($GpuQuality) {
        foreach ($name in $quality) {
            if ($name -eq 'superior_carrier') { & "$out/$name.exe" $candidate }
            else { & "$out/$name.exe" $ReferenceShaderDirectory $candidate }
            if ($LASTEXITCODE -ne 0) { throw "GPU comparison failed: $name" }
        }
    }
    if ($GpuTiming) {
        & "$out/superior_tall_low_timing.exe" $ReferenceShaderDirectory $candidate "$out/port-v59-timing.csv" normal full
        if ($LASTEXITCODE -ne 0) { throw 'GPU timing failed' }
    }
    Write-Host 'Superior tests passed'
}
finally { Pop-Location }
