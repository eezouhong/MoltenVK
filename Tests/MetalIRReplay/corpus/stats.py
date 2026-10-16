#!/usr/bin/env python3
"""Count static instruction sites in extracted modules; never infer draw hotness."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import struct
from extract_cache import instructions, digest


def literal_string(words):
    return struct.pack('<' + 'I' * len(words), *words).split(b'\0', 1)[0].decode('utf-8')


def count_module(code, grammar, glsl):
    names = {x['opcode']: x['opname'] for x in grammar['instructions']}
    enumerants = {x['kind']: {int(str(e['value']), 0): e['enumerant'] for e in x.get('enumerants', [])}
                  for x in grammar['operand_kinds'] if x['category'] == 'ValueEnum'}
    glsl_names = {x['opcode']: x['opname'] for x in glsl['instructions']}
    stream = list(instructions(code))
    imports = {args[0]: literal_string(args[1:]) for op, args in stream if names.get(op) == 'OpExtInstImport'}
    result = {name: Counter() for name in ('opcodes', 'glsl450', 'otherExtended', 'decorations', 'executionModes')}
    for op, args in stream:
        name = names.get(op, f'UnknownOpcode({op})')
        result['opcodes'][name] += 1
        if name in ('OpExtInst', 'OpExtInstWithForwardRefsKHR'):
            pack = imports.get(args[2], f'UnknownImport({args[2]})')
            if pack == 'GLSL.std.450':
                result['glsl450'][glsl_names.get(args[3], f'UnknownGlsl({args[3]})')] += 1
            else:
                result['otherExtended'][f'{pack}:{args[3]}'] += 1
        elif name in ('OpDecorate', 'OpDecorateId', 'OpDecorateString', 'OpMemberDecorate', 'OpMemberDecorateString'):
            index = 2 if 'Member' in name else 1
            value = args[index]
            result['decorations'][enumerants['Decoration'].get(value, f'UnknownDecoration({value})')] += 1
        elif name in ('OpExecutionMode', 'OpExecutionModeId'):
            value = args[1]
            mode = enumerants['ExecutionMode'].get(value, f'UnknownMode({value})')
            # Preserve bit-width/arguments in the private report as well as the mode name.
            result['executionModes'][mode + ':' + ','.join(map(str, args[2:]))] += 1
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--grammar', type=Path, required=True)
    parser.add_argument('--glsl-grammar', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    if manifest['schema'] != 'melonx-ir-corpus-v1':
        raise ValueError('unsupported corpus schema')
    grammar, glsl = json.loads(args.grammar.read_text()), json.loads(args.glsl_grammar.read_text())
    totals, module_counts = defaultdict(Counter), defaultdict(Counter)
    stages = Counter()
    for module in manifest['modules']:
        path = args.manifest.parent / module['file']
        code = path.read_bytes()
        if hashlib.sha256(code).hexdigest() != module['sha256']:
            raise ValueError('module identity changed')
        counts = count_module(code, grammar, glsl)
        stages[module['stage']] += 1
        for kind, values in counts.items():
            totals[kind].update(values)
            module_counts[kind].update(values.keys())
    report = {'schema': 'melonx-ir-corpus-stats-v1', 'manifestSHA256': digest(args.manifest),
              'grammarSHA256': digest(args.grammar), 'glslGrammarSHA256': digest(args.glsl_grammar),
              'moduleCount': manifest['moduleCount'], 'validationFailures': manifest['validationFailures'],
              'extractionErrors': len(manifest['errors']), 'sourceLabel': manifest['sourceLabel'],
              'stages': dict(stages), 'unit': 'static sites in unique modules; not runtime frequency',
              'counts': {kind: [{'name': name, 'sites': count, 'modules': module_counts[kind][name]}
                                for name, count in values.most_common()] for kind, values in totals.items()}}
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'stats.json').write_text(json.dumps(report, indent=2) + '\n')
    lines = ['# Private SPIR-V corpus statistics', '', report['unit'], '',
             f"Modules: {report['moduleCount']}; validation failures: {report['validationFailures']}.", '']
    for kind, entries in report['counts'].items():
        lines += ['## ' + kind, '', '| Name | Static sites | Modules |', '| --- | ---: | ---: |']
        lines += [f"| {e['name']} | {e['sites']} | {e['modules']} |" for e in entries]
        lines.append('')
    (args.output / 'stats.md').write_text('\n'.join(lines))
    print(json.dumps({k: report[k] for k in ('moduleCount', 'stages', 'validationFailures', 'extractionErrors')}))


if __name__ == '__main__':
    main()
