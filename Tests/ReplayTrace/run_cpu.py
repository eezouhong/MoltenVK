from pathlib import Path
import argparse,json,subprocess,sys
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);args=p.parse_args()
root=Path(__file__).resolve().parent;headers=root.parents[1]/'MoltenVK/MoltenVK/GPUObjects'
args.output.mkdir(parents=True,exist_ok=True);results=[]
for name in ['binding_trace_test','descriptor_trace_test','frame_trace_test','gpu_stage_data_test','draw_work_test','disabled_trace_test']:
    binary=args.output/name;flag='0' if name=='disabled_trace_test' else '1'
    with (args.output/(name+'.log')).open('w') as log:
        c=subprocess.run(['clang++','-std=c++17','-O2','-pthread','-DMVK_REPLAY_TRACE='+flag,'-I'+str(headers),str(root/(name+'.cpp')),'-o',str(binary)],stdout=log,stderr=subprocess.STDOUT)
        r=subprocess.run([str(binary)],stdout=log,stderr=subprocess.STDOUT) if c.returncode==0 else None
    results.append({'test':name,'compile':c.returncode,'run':r.returncode if r else None})
    if c.returncode or (r and r.returncode):break
(args.output/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results))
sys.exit(0 if len(results)==6 and all(r['compile']==0 and r['run']==0 for r in results) else 1)
