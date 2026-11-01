"""Build canonical desktop USB sources, reusing the original QEMU dependency."""
from pathlib import Path
import json,os,subprocess,hashlib
R=Path(__file__).resolve().parent
recipe=json.loads((R/'engine/build-recipe.json').read_text(encoding='utf-8'))
env=dict(os.environ);env['PATH']=str(Path(recipe['link'][0]).parent)+os.pathsep+env.get('PATH','');env['TMP']=env['TEMP']=str(R/'engine')
rows=[]
for row in recipe['compile']+[dict(name='link',argv=recipe['link'],cwd=recipe['cwd'])]:
 p=subprocess.run(row['argv'],cwd=row['cwd'],env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=180)
 (R/'logs'/('build-'+row['name']+'.log')).write_text(p.stdout+p.stderr,encoding='utf-8');print(row['name'],p.returncode,flush=True);rows.append(dict(row,exit=p.returncode))
 if p.returncode:raise SystemExit(p.stderr[-3000:])
exe=R/'engine/qemu-system-arm-classic7.exe'
(R/'ENGINE-FREEZE.json').write_text(json.dumps(dict(exe=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),commands=rows,formal_FCa0c9_modified=False,seed_modified=False),indent=2),encoding='utf-8')
