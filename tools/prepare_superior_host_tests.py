"""Compile legacy and independently extracted v59-estimator reference shaders.

Optional GPU validation helper for tools/test_superior_port.ps1. The baselines come from the Superior port's own
history (commits 24a2d7bf, 67318a5d), which exists only in a clone that has the PR #230 head
(git fetch origin pull/230/head). Without it the script prints why and exits 3, which the PowerShell runner reports
as a skipped mode. Exit 0 = prepared, any other code = failure.

--upstream-parity additionally compiles janblade's composition from 2a5be38d (0.1.33, the port's base) for the
"unmodified janblade resolve" comparison; by default that comparison uses the live source on both sides, so it does
not break whenever the live shader changes after the port.
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
host = root / 'OptiScaler/shaders/dlssnr/precompile'
out = root / 'artifacts/validation/host-shaders'
SKIPPED = 3
args = sys.argv[1:]
upstream_parity = '--upstream-parity' in args
args = [a for a in args if a != '--upstream-parity']


def need(commit, purpose):
    present = subprocess.run(['git', 'cat-file', '-e', commit + '^{commit}'], cwd=root,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if not present:
        print(f'{purpose} needs the Superior port commit {commit}, which is not in this clone '
              f'(git fetch origin pull/230/head); skipped')
        raise SystemExit(SKIPPED)


def show(commit, path):
    return subprocess.check_output(['git', 'show', commit + ':' + path], cwd=root)


out.mkdir(parents=True, exist_ok=True)
if args == ['--timing-baseline']:
    need('24a2d7bf', 'The spatial timing baseline')
    baseline = root / 'artifacts/validation/pre-spatial'
    baseline.mkdir(parents=True, exist_ok=True)
    for variant in ('hdr', 'sdr'):
        name = 'host_resolve_' + variant + '_Shader.cso'
        (baseline / name).write_bytes(show('24a2d7bf', 'OptiScaler/shaders/dlssnr/superior/precompile/' + name))
    raise SystemExit(0)
# Retain the exact shipped pre-optimization PSOs for timing and direct quality checks.
need('67318a5d', 'The host optimization baseline and host references')
baseline = root / 'artifacts/validation/pre-host-opt'
baseline.mkdir(parents=True, exist_ok=True)
for variant in ('hdr', 'sdr', 'raw_hdr', 'raw_sdr', 'packed_hdr', 'packed_sdr'):
    name = 'host_resolve_' + variant + '_Shader.cso'
    (baseline / name).write_bytes(show('67318a5d', 'OptiScaler/shaders/dlssnr/superior/precompile/' + name))
if args == ['--host-opt-baseline']:
    raise SystemExit(0)
dxc = root / 'OptiScaler/shaders/shader_tools/dxc.exe'
# Freeze the pre-optimization host source as well as the independent estimator.
source = show('67318a5d', 'OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl').decode('utf-8')
frozen = (host / '../superior/precompile/dlssnr.hlsl').read_text(encoding='utf-8')
begin = frozen.index('float3 P100GuidedResidualAt(')
end = frozen.index('\n\n#if !defined(VK_MODE)', begin)
reference = frozen[begin:end]
reference = reference.replace('P100GuidedResidualAt(', 'SuperiorGuidedEdit(')
reference = reference.replace('GuidedResidualBilinear(uvq)', 'gModel.SampleLevel(gLinear, saturate(uvq), 0).rgb')
reference = reference.replace('gResidualBlendUnused', '0.015').replace('gResidualScale', '3.0')
reference = reference.replace('gTransfer == 9u', 'true')
reference = reference.replace('return InterPassGuideBlend(bilinearResidual, guided, saturate(gResidualConfidenceUnused));',
    'return gGuideWidth != 0u ? gMvScaleX * guided + (gMvScaleY - gMvScaleX) * gSuperiorLow.SampleLevel(gLinear, uvq, 0).rgb : guided;')
reference = reference.replace('float3 SuperiorGuidedEdit(float2 uvq, float3 nativeGuide)\n{', '''float3 SuperiorGuidedEdit(float2 uvq, float3 nativeGuide)
{
    if (gGuideHeight != 0u)
    {
        const float3 delta = gModel.SampleLevel(gLinear, uvq, 0).rgb - gSource.SampleLevel(gLinear, uvq, 0).rgb;
        return gGuideWidth != 0u ? gMvScaleX * delta + (gMvScaleY-gMvScaleX) * gSuperiorLow.SampleLevel(gLinear, uvq, 0).rgb : delta;
    }''')
# Scalar mapping from the CPU specification; no production mapping HLSL is included.
packed_reference = (root / 'tests/nr_gpu/packed_reference.hlsli').read_text(encoding='utf-8')
reference = reference.replace('    if (gGuideHeight != 0u)', """#if defined(DLSSNR_SUPERIOR_HOST_PACKED)
    uvq = PackedReferenceUv(uvq);
#endif
    if (gGuideHeight != 0u)""")
reference = packed_reference + '\nTexture2D<float4> gSuperiorLow : register(t5);\n' + reference
reference = reference.replace('float3 SuperiorGuidedEdit(float2 uvq, float3 nativeGuide)',
                              'float3 ReferenceFullGuidedEdit(float2 uvq, float3 nativeGuide, bool rawResult)')
reference = reference.replace('return gGuideWidth != 0u ? gMvScaleX * guided',
                              'return rawResult ? guided : gGuideWidth != 0u ? gMvScaleX * guided')
reference += '\n' + (root / 'tests/nr_gpu/native_center_reference.hlsli').read_text(encoding='utf-8')
reference_source = source.replace('#include "../superior/HostResolve.hlsli"', reference)
(out / 'reference.hlsl').write_text(reference_source, encoding='utf-8')


def compile_shader(path, name, defines=()):
    subprocess.run([str(dxc), '-T', 'cs_6_0', '-E', 'CSMain', '-O3', '-Qstrip_debug', '-Qstrip_reflect',
                    '-I', str(host), *[arg for d in defines for arg in ('-D', d)], str(path),
                    '-Fo', str(out / (name + '_Shader.cso'))], check=True)


compile_shader(host / 'dlssnr.hlsl', 'legacy_recompiled')
if upstream_parity:
    # The default composition must be unchanged by the Superior #ifdefs: janblade's source at the port's base.
    need('2a5be38d', 'The upstream composition parity check')
    (out / 'upstream.hlsl').write_bytes(show('2a5be38d', 'OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl'))
    compile_shader(out / 'upstream.hlsl', 'upstream_recompiled')
else:
    compile_shader(host / 'dlssnr.hlsl', 'upstream_recompiled')
# No assertion against a shipped default blob: precompile/DlssNr_Shader.cso is janblade's and changes with every
# default-shader change; it is not part of Superior's validation.
for variant in ('hdr', 'sdr', 'raw_hdr', 'raw_sdr', 'packed_hdr', 'packed_sdr',
                'skip_guided_hdr', 'skip_guided_sdr', 'skip_guided_packed_hdr', 'skip_guided_packed_sdr',
                'full_packed_hdr', 'full_packed_sdr'):
    defines = ['DLSSNR_SUPERIOR_HOST_RESOLVE=1', 'DLSSNR_SUPERIOR_RESOLVE=1', 'DLSSNR_SUPERIOR_NORMAL=1']
    if variant.endswith('sdr'):
        defines += ['DLSSNR_SUPERIOR_SDR=1', 'DLSSNR_SUPERIOR_HOST_SDR=1']
    if variant.startswith('raw_'):
        defines += ['DLSSNR_SUPERIOR_HOST_RAW=1']
    if 'packed_' in variant:
        defines += ['DLSSNR_SUPERIOR_HOST_PACKED=1']
    if variant.startswith('skip_guided_'):
        defines += ['DLSSNR_SUPERIOR_SKIP_GUIDED=1']
    if variant.startswith('full_'):
        defines += ['DLSSNR_SUPERIOR_FORCE_FULL_GUIDED=1']
    compile_shader(out / 'reference.hlsl', 'reference_' + variant, defines)
print('Host reference shaders compiled')
