# Run from a Developer PowerShell, or let this script locate vcvars64 automatically.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$validation = Join-Path $repo 'artifacts/validation/finished-fg'
New-Item -ItemType Directory -Force -Path $validation | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC x64 tools are required.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
# Test the production wrappers: remove only their application includes and inject the test seams.
# No hook implementation is duplicated in the test.
$production = Join-Path $repo 'OptiScaler/dlssnr/DlssNr_StreamlinePicture.cpp'
$body = (Get-Content -LiteralPath $production | Where-Object { $_ -notmatch '^#include' }) -join "`n"
[IO.File]::WriteAllText((Join-Path $validation 'StreamlinePicture.cpp'), $body, [Text.UTF8Encoding]::new($false))
$cases = @(
    @{ Source = 'tests/nr_gpu/nr_streamline_hooks.cpp'; Libs = 'd3d12.lib dxgi.lib' },
    @{ Source = 'tests/nr_gpu/nr_streamline_picture_smoke.cpp'; Libs = 'd3d12.lib dxgi.lib' },
    @{ Source = 'tests/nr_finished_queue_smoke.cpp'; Libs = 'd3d12.lib dxgi.lib' },
    @{ Source = 'tests/nr_finished_color_smoke.cpp'; Libs = 'd3d11.lib d3dcompiler.lib dxgi.lib' }
)
Push-Location $repo
try {
    foreach ($case in $cases) {
        $name = [IO.Path]::GetFileNameWithoutExtension($case.Source)
        $exe = Join-Path $validation "$name.exe"
        $obj = Join-Path $validation "$name.obj"
        & cmd /d /c "`"$vcvars`" >nul && cl /nologo /std:c++17 /EHsc /O2 /I`"$validation`" $($case.Source) /Fe:`"$exe`" /Fo:`"$obj`" /link $($case.Libs)"
        if ($LASTEXITCODE -ne 0) { throw "Compile failed: $name" }
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "Test failed: $name" }
    }
} finally { Pop-Location }
