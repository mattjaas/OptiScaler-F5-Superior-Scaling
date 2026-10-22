"""Frozen pre-centre PSOs and a GPU probe of the production identity-region mask."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/validation/native-center'
out.mkdir(parents=True, exist_ok=True)
prefix = 'OptiScaler/shaders/dlssnr/superior/precompile/'
for stem in ('host_warp_cache', 'host_resolve_packed_hdr', 'host_resolve_packed_sdr',
             *('host_cached_packed_' + s for s in ('sdr','hdr0','hdr2','hdr4','hdr6'))):
    data = subprocess.check_output(['git','show','cb02f81f:' + prefix + stem + '_Shader.cso'], cwd=root)
    (out / (stem + '_Shader.cso')).write_bytes(data)
host = root / 'OptiScaler/shaders/dlssnr/precompile'
source = (host / 'dlssnr.hlsl').read_text(encoding='utf-8')
source = source.replace('void CSMain(', 'void UnusedHostMain(')
source += '''
[numthreads(8,8,1)]
void NativeMaskProbe(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=gWidth || id.y>=gHeight)return;
    float2 uv=(float2(id.xy)+0.5)/float2(gWidth,gHeight);
    float2 pixel=uv*float2(gWidth,gHeight);
    float w=1.0;
    if(gModelWorkScale==1.0 && all(gWorkScale.xy==1.0))w=SuperiorNativeGuidedWeight(pixel);
    const int2 shift=int2(round(gBandCenter.zw-gBandCenter.xy));
    gTarget[id.xy]=float4(w,float2(int2(pixel)+shift),1.0);
}
'''
(out / 'probe.hlsl').write_text(source, encoding='utf-8')
dxc = root / 'OptiScaler/shaders/shader_tools/dxc.exe'
subprocess.run([str(dxc),'-T','cs_6_0','-E','NativeMaskProbe','-O3','-Qstrip_debug','-Qstrip_reflect',
                '-D','DLSSNR_SUPERIOR_HOST_RESOLVE=1','-D','DLSSNR_SUPERIOR_HOST_PACKED=1',
                '-D','DLSSNR_SUPERIOR_NATIVE_CENTER=1',
                '-I',str(host),str(out/'probe.hlsl'),'-Fo',str(out/'native_mask_Shader.cso')],check=True)
subprocess.run([str(dxc),'-T','cs_6_0','-E','CSMain','-O3','-Qstrip_debug','-Qstrip_reflect',
                str(root/'tests/nr_gpu/carrier_bypass_reference.hlsl'),
                '-Fo',str(out/'carrier_bypass_reference_Shader.cso')],check=True)
