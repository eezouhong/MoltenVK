"""Generate public sparse interface fixtures, independent of game data."""
import argparse
from pathlib import Path
import subprocess

BASE = ('#version 460\n'
 '{decl}\n'
 'void main() {{\n'
 ' vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));\n'
 ' gl_Position=vec4(p[gl_VertexIndex],0,1);\n'
 ' {assign}\n'
 '}}\n')
FRAGMENT = ('#version 460\n'
 '{decl}\n'
 'layout(set=0,binding=0,std430) buffer Out {{ uvec4 rows[]; }} data;\n'
 'layout(location=0) out vec4 color;\n'
 'void main() {{\n'
 ' uvec2 p=uvec2(gl_FragCoord.xy); vec4 value={value};\n'
 ' data.rows[p.y*16u+p.x]=uvec4(round(value*{scale})); color=value;\n'
 '}}\n')
VARIANTS = {'sparse': ('layout(location=0) out vec4 unusedValue;\nlayout(location=2) out vec4 selected;',
            'unusedValue=vec4(1); selected=vec4(0.25,0.5,0.75,1);',
            'layout(location=2) in vec4 selected;',
            'selected'),
 'reordered': ('layout(location=7) out vec4 selected;\nlayout(location=0) out vec4 unusedValue;',
               'unusedValue=vec4(1); selected=vec4(0.25,0.5,0.75,1);',
               'layout(location=7) in vec4 selected;\nlayout(location=0) in vec4 unusedValue;',
               'selected+unusedValue*0.000001'),
 'high': ('layout(location=0) out vec4 unusedValue;\nlayout(location=31) out vec4 selected;',
          'unusedValue=vec4(1); selected=vec4(0.25,0.5,0.75,1);',
          'layout(location=31) in vec4 selected;',
          'selected'),
 'array': ('layout(location=0) out vec4 unusedValue;\nlayout(location=3) out vec4 selected[2];',
           'unusedValue=vec4(1); selected[0]=vec4(0.25,0.5,0.75,1); selected[1]=vec4(0);',
           'layout(location=3) in vec4 selected[2];',
           'selected[0]+selected[1]'),
 'components': ('layout(location=0) out vec4 unusedValue;\n'
                'layout(location=5,component=0) out vec2 first;\n'
                'layout(location=5,component=2) out vec2 second;',
                'unusedValue=vec4(1);first=vec2(0.25,0.5);second=vec2(0.75,1);',
                'layout(location=5,component=2) in vec2 second;\n'
                'layout(location=5,component=0) in vec2 first;',
                'vec4(first,second)'),
 'flat': ('layout(location=0) out vec4 unusedValue;\nlayout(location=6) flat out vec4 selected;',
          'unusedValue=vec4(1);selected=vec4(0.25,0.5,0.75,1);',
          'layout(location=6) flat in vec4 selected;',
          'selected'),
 'gradient': ('layout(location=0) out vec4 unusedValue;\n'
              'layout(location=4) noperspective out vec4 selected;',
              'unusedValue=vec4(1);selected=vec4(p[gl_VertexIndex]*0.5+0.5,0.5,1);',
              'layout(location=4) noperspective in vec4 selected;',
              'selected')}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--glslang',required=True)
    parser.add_argument('--validator',required=True)
    args=parser.parse_args(); args.output.mkdir(parents=True,exist_ok=True)
    for name,(vd,assignment,fd,value) in VARIANTS.items():
        for stage,text in [('vert',BASE.format(decl=vd,assign=assignment)),('frag',FRAGMENT.format(decl=fd,value=value,scale=32 if name=='gradient' else 255))]:
            source=args.output/('varying-'+name+'.'+stage);source.write_text(text)
            binary=Path(str(source)+'.spv')
            subprocess.run([args.glslang,'-V','--target-env','vulkan1.1',str(source),'-o',str(binary)],check=True)
            subprocess.run([args.validator,'--target-env','vulkan1.1',str(binary)],check=True)

if __name__=='__main__': main()
