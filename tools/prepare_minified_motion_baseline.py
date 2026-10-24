"""Save the published three-method depth-aware baseline for paired GPU comparisons.

Only the ignored artifacts directory is written. Needs the baseline in git history;
no dependency on this history is introduced into normal quality tests or CI.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
revision = '2b2eb39fc6857260f6c5d6b06564f24fb36dfbf3'
if subprocess.run(['git', 'cat-file', '-e', revision + '^{commit}'], cwd=root,
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
    print('Minified MV timing needs baseline 2b2eb39f in git history; skipped.')
    raise SystemExit(3)
out = root / 'artifacts/validation/motion-minify-base'
out.mkdir(parents=True, exist_ok=True)
for name in ('host_surface_guides', 'host_uniform_surface_guides'):
    file = name + '_Shader.cso'
    data = subprocess.check_output(['git', 'show', revision +
        ':OptiScaler/shaders/dlssnr/superior/precompile/' + file], cwd=root)
    (out / file).write_bytes(data)
print('Minified MV timing baseline: ' + revision)
