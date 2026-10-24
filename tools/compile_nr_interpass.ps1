# Rebuilds every Superior Scaling shader blob (OptiScaler/shaders/dlssnr/superior/precompile/*_Shader.cso + .h) with the
# repository's own dxc (OptiScaler/shaders/shader_tools/dxc.exe). PowerShell only, no Python.
#
#   Frozen v59 port baseline  superior/precompile/dlssnr.hlsl  -> DlssNr, dlssnr_interpass_*, dlssnr_superior_*
#                             superior/RawLow.hlsl (includes the frozen file) -> host_low_raw
#   Live NR shader            precompile/dlssnr.hlsl + superior/HostResolve.hlsli -> host_resolve_*, host_normal_*,
#                             host_cached_*;  superior/WarpCache.hlsl (WarpMap.hlsli + precompile/dlssnr_spatial_warp.hlsli)
#                             -> host_warp_cache
#
# Any change to precompile/dlssnr.hlsl, dlssnr_spatial_warp.hlsli or the superior/*.hlsl* sources must be followed by
# this script, and the result committed: CI (.github/workflows/superior_checks.yml) runs it and fails on any difference.
# The default janblade/Vulkan blobs in precompile/ are never written here.
#
# -Check: build into a temporary copy and compare instead of writing (exit 1 on any difference).
param([switch]$Check)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$shaders = Join-Path $root "OptiScaler/shaders/dlssnr"
$dxc = Join-Path $root "OptiScaler/shaders/shader_tools/dxc.exe"
$out = Join-Path $shaders "superior/precompile"
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("superior-blobs-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $scratch | Out-Null
$built = New-Object System.Collections.Generic.List[string]

# Byte-for-byte what shader_tools/create_header.py writes, with trailing spaces trimmed and CRLF lines.
function Write-Header([string]$cso, [string]$header, [string]$name) {
    $bytes = [IO.File]::ReadAllBytes($cso)
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append("#pragma once`n`ninline static const unsigned char $name[] = {`n    ")
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        [void]$sb.Append(('0x{0:x2}' -f $bytes[$i]))
        if ($i -lt $bytes.Length - 1) { [void]$sb.Append(', ') }
        if ((($i + 1) % 12) -eq 0) { [void]$sb.Append("`n    ") }
    }
    [void]$sb.Append("`n};`n")
    $lines = $sb.ToString().TrimEnd("`n").Split("`n") | ForEach-Object { $_.TrimEnd() }
    [IO.File]::WriteAllLines($header, [string[]]$lines, [Text.UTF8Encoding]::new($false))
}

# Compile one blob into the scratch directory, then make its header; $asm keeps the disassembly for the LDS checks.
function Build([string]$source, [string]$entry, [string[]]$defines, [string]$stem, [switch]$asm) {
    $cso = Join-Path $scratch "${stem}_Shader.cso"
    $dxcArgs = @('-T', 'cs_6_0', '-E', $entry, '-O3', '-Qstrip_debug', '-Qstrip_reflect') + $defines + @((Split-Path $source -Leaf), '-Fo', $cso)
    if ($asm) { $dxcArgs += @('-Fc', (Join-Path $scratch "${stem}.asm")) }
    Push-Location (Split-Path $source -Parent)
    try { & $dxc @dxcArgs | Out-Null } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { throw "DXC failed: $stem" }
    Write-Header $cso (Join-Path $scratch "${stem}_Shader.h") "${stem}_cso"
    $built.Add($stem)
}
function Lds([string]$stem) { @(Select-String (Join-Path $scratch "${stem}.asm") -Pattern 'addrspace\(3\) global') }
function Geometry([string]$stem, [string]$threads) {
    [bool](Select-String -LiteralPath (Join-Path $scratch "${stem}.asm") -SimpleMatch -Pattern "; NumThreads=$threads")
}

try {
    $frozen = Join-Path $out "dlssnr.hlsl"
    foreach ($pitch in @(0, -1, -2, -4, -5)) {
        $stem = switch ($pitch) {
            0 { "DlssNr" }
            -1 { "dlssnr_interpass_fast" }
            -2 { "dlssnr_interpass_low36" }
            -4 { "dlssnr_interpass_cross5" }
            -5 { "dlssnr_interpass_low36_cached" }
        }
        $defines = switch ($pitch) {
            0 { @() }
            -1 { @("-D", "DLSSNR_INTERPASS_FAST=1") }
            -2 { @("-D", "DLSSNR_INTERPASS_LOW36=1") }
            -4 { @("-D", "DLSSNR_INTERPASS_CROSS5=1") }
            -5 { @("-D", "DLSSNR_INTERPASS_LOW36=1", "-D", "DLSSNR_LOW36_CACHE=1") }
        }
        Build $frozen "CSMain" $defines $stem -asm
        $lds = Lds $stem
        if ($pitch -eq -5) {
            if ($lds.Count -ne 1 -or $lds[0].Line -notmatch "\[3468 x float\]") { throw "Unexpected Low36 LDS" }
        } elseif ($pitch -lt 0) {
            if ($lds.Count -ne 0) { throw "Fast PSO unexpectedly allocates LDS" }
        } # Standard shader keeps its existing exposure-meter LDS, without inter-pass cache.
    }
    foreach ($variant in @("carrier", "resolve", "cross5", "normal", "sdr", "hdr2", "hdr4", "encode", "low", "area", "meter", "exposure", "hdr0")) {
        $stem = "dlssnr_superior_$variant"
        $entry = if ($variant -eq "carrier") { "SuperiorCarrierMain" } else { "CSMain" }
        $defines = @("-D", "DLSSNR_SUPERIOR_SCALING=1")
        $normal = $variant -in @("normal", "sdr", "hdr2", "hdr4", "hdr0")
        if ($variant -eq "resolve" -or $normal) { $defines += @("-D", "DLSSNR_SUPERIOR_RESOLVE=1") }
        if ($normal) { $defines += @("-D", "DLSSNR_SUPERIOR_NORMAL=1") }
        if ($variant -eq "sdr") { $defines += @("-D", "DLSSNR_SUPERIOR_SDR=1") }
        if ($variant -like "hdr*") { $defines += @("-D", "DLSSNR_SUPERIOR_HDR=$($variant.Substring(3))") }
        if ($variant -eq "cross5") { $defines += @("-D", "DLSSNR_INTERPASS_CROSS5=1") }
        if ($variant -eq "encode") { $defines += @("-D", "DLSSNR_SUPERIOR_ENCODE=1") }
        if ($variant -eq "low") { $defines += @("-D", "DLSSNR_SUPERIOR_LOW=1") }
        if ($variant -eq "area") { $defines += @("-D", "DLSSNR_SUPERIOR_AREA=1") }
        if ($variant -eq "meter") { $defines += @("-D", "DLSSNR_SUPERIOR_METER=1") }
        if ($variant -eq "exposure") { $defines += @("-D", "DLSSNR_SUPERIOR_EXPOSURE=1") }
        Build $frozen $entry $defines $stem -asm
        if (($variant -in @("carrier", "resolve", "encode", "area") -or $normal) -and (Lds $stem).Count) { throw "Superior $variant unexpectedly uses LDS" }
        if ($variant -eq "low") {
            if (-not (Geometry $stem "(8,16,1)")) { throw "Unexpected low group geometry" }
            $lds = Lds $stem
            if ($lds.Count -ne 1 -or $lds[0].Line -notmatch '\[7344 x float\]') { throw "Unexpected Superior low cache LDS" }
        }
        if ($variant -in @("meter", "exposure")) {
            if (-not (Geometry $stem "(8,8,1)")) { throw "Unexpected exposure group geometry: $stem" }
            $lds = Lds $stem
            if ($lds.Count -ne 1 -or $lds[0].Line -notmatch '\[256 x float\]') { throw "Unexpected exposure LDS: $stem" }
        }
    }
    Build (Join-Path $shaders "superior/RawLow.hlsl") "RawLowMain" @() "host_low_raw"
    Build (Join-Path $shaders "superior/RawResidual.hlsl") "RawResidualMain" @() "host_residual_raw"
    Build (Join-Path $shaders "superior/SpatialGuides.hlsl") "SuperiorSpatialGuidesMain" @() "host_spatial_guides"
    Build (Join-Path $shaders "superior/SpatialGuides.hlsl") "SuperiorSurfaceGuidesMain" @() "host_surface_guides"
    Build (Join-Path $shaders "superior/SpatialGuides.hlsl") "SuperiorUniformGuidesMain" @() "host_uniform_guides"
    Build (Join-Path $shaders "superior/SpatialGuides.hlsl") "SuperiorUniformSurfaceGuidesMain" @() "host_uniform_surface_guides"
    Build (Join-Path $shaders "superior/MinifiedMotion.hlsl") "SuperiorMinifiedGuidesMain" @() "host_minified_surface_guides"
    Build (Join-Path $shaders "superior/MinifiedMotion.hlsl") "SuperiorUniformMinifiedGuidesMain" @() "host_uniform_minified_surface_guides"

    # The host resolve preserves janblade's exposure and composition and substitutes only the residual estimator.
    # raw: the non-temporal resolve; its SDR and HDR builds are identical bytecode, so there is one blob for both.
    $live = Join-Path $shaders "precompile/dlssnr.hlsl"
    $hostVariants = @("hdr", "sdr", "raw", "packed_hdr", "packed_sdr")
    $hostVariants += @("normal_sdr", "normal_hdr0", "normal_hdr2", "normal_hdr4", "normal_hdr6",
                       "normal_packed_sdr", "normal_packed_hdr0", "normal_packed_hdr2", "normal_packed_hdr4", "normal_packed_hdr6")
    $hostVariants += @('cached_packed_sdr', 'cached_packed_hdr0', 'cached_packed_hdr2', 'cached_packed_hdr4', 'cached_packed_hdr6')
    $hostVariants += @('native_packed_hdr', 'native_packed_sdr', 'native_cached_packed_sdr',
                       'native_cached_packed_hdr0', 'native_cached_packed_hdr2', 'native_cached_packed_hdr4', 'native_cached_packed_hdr6')
    $hostVariants += @('skip_guided_hdr', 'skip_guided_sdr', 'skip_guided_packed_hdr', 'skip_guided_packed_sdr')
    foreach ($variant in $hostVariants) {
        $native = $variant.StartsWith('native_')
        $cached = $variant.Contains('cached_')
        $specialized = $variant.StartsWith('normal_') -or $cached
        $stem = if ($specialized) { "host_$variant" } else { "host_resolve_$variant" }
        if ($native) { $stem = "host_$variant" }
        if ($variant.StartsWith('skip_guided_')) { $stem = "host_$variant" }
        $defines = @("-D", "DLSSNR_SUPERIOR_HOST_RESOLVE=1")
        if ($native) { $defines += @('-D', 'DLSSNR_SUPERIOR_NATIVE_CENTER=1') }
        if ($variant.StartsWith('skip_guided_')) { $defines += @('-D', 'DLSSNR_SUPERIOR_SKIP_GUIDED=1') }
        if ($variant.EndsWith("sdr")) { $defines += @("-D", "DLSSNR_SUPERIOR_HOST_SDR=1") }
        if ($variant -eq "raw") { $defines += @("-D", "DLSSNR_SUPERIOR_HOST_RAW=1") }
        if ($variant.Contains("packed_")) { $defines += @("-D", "DLSSNR_SUPERIOR_HOST_PACKED=1") }
        if ($specialized) {
            $pass = if ($variant.EndsWith('sdr')) { 1 } else { 0 }
            $mapping = if ($pass) { 0 } else { [int]$variant.Substring($variant.Length - 1) }
            $defines += @('-D', 'DLSSNR_SUPERIOR_HOST_NORMAL=1', '-D', "DLSSNR_SUPERIOR_HOST_PASSTHROUGH=$pass",
                          '-D', "DLSSNR_SUPERIOR_HOST_MAPPING=$mapping")
        }
        if ($cached) { $defines += @('-D', 'DLSSNR_SUPERIOR_HOST_CACHED=1') }
        Build $live "CSMain" $defines $stem
    }
    Build (Join-Path $shaders "superior/WarpCache.hlsl") "CSMain" @() "host_warp_cache"

    # Every shipped blob must come from this script, and nothing else may sit beside them.
    $shipped = @(Get-ChildItem $out -Filter "*_Shader.cso" | ForEach-Object { $_.Name -replace '_Shader\.cso$', '' })
    $orphans = @($shipped | Where-Object { $built -notcontains $_ })
    $different = New-Object System.Collections.Generic.List[string]
    foreach ($stem in $built) {
        foreach ($ext in @("_Shader.cso", "_Shader.h")) {
            $new = Join-Path $scratch "$stem$ext"
            $old = Join-Path $out "$stem$ext"
            if ($Check) {
                $same = (Test-Path $old) -and
                    ($ext -eq "_Shader.cso" -and (Get-FileHash $new).Hash -eq (Get-FileHash $old).Hash -or
                     $ext -eq "_Shader.h" -and ([IO.File]::ReadAllText($new).Replace("`r`n", "`n") -eq [IO.File]::ReadAllText($old).Replace("`r`n", "`n")))
                if (-not $same) { $different.Add("$stem$ext") }
            } else {
                Copy-Item $new $old -Force
            }
        }
        Write-Host "$stem SHA256=$((Get-FileHash (Join-Path $scratch "${stem}_Shader.cso")).Hash)"
    }
    Write-Host "$($built.Count) blobs built with $dxc"
    if ($orphans.Count) { Write-Host "Not built by this script: $($orphans -join ', ')" }
    if ($Check -and $different.Count) { Write-Host "Differs from the committed blob: $($different -join ', ')" }
    if ($Check -and ($orphans.Count -or $different.Count)) { exit 1 }
    if ($Check) { Write-Host "All $($built.Count) blobs and headers match byte for byte" }
} finally {
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
