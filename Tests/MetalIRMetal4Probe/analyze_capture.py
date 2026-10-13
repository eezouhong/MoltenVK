"""Conservative Vulkan descriptor grouping, explicitly not the native Metal base key."""
import argparse
from collections import Counter, defaultdict
import copy
import hashlib
import json
from pathlib import Path


def key(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def render_pass(info, subpass):
    value = copy.deepcopy(info)
    if not value:
        return None
    passes = value.get('pSubpasses') or []
    if subpass >= len(passes):
        return None
    colors = passes[subpass].get('pColorAttachments') or []
    attachments = value.get('pAttachments') or []
    for color in colors:
        index = color['attachment']
        if isinstance(index, int) and index < len(attachments):
            attachments[index]['format'] = 'color-format-unspecialized'
    return value


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--records', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    passes = {}
    pairs = Counter()
    groups = defaultdict(set)
    stages = Counter()
    projections = defaultdict(set)
    missing = 0
    count = 0
    for line in a.records.open():
        record = json.loads(line)
        call = record.get('function', {})
        args = call.get('args', {})
        if call.get('return') != 'VK_SUCCESS':
            continue
        if call.get('name') in ('vkCreateRenderPass', 'vkCreateRenderPass2', 'vkCreateRenderPass2KHR'):
            passes[args['pRenderPass']] = args['pCreateInfo']
        if call.get('name') != 'vkCreateGraphicsPipelines':
            continue
        for info in args['pCreateInfos']:
            count += 1
            stage_set = tuple(stage['stage'] for stage in info['pStages'])
            stages[str(stage_set)] += 1
            pair = [(stage['stage'], stage['module'], stage['pName'], stage.get('pSpecializationInfo'))
                    for stage in info['pStages']]
            pairs[key(pair)] += 1
            normalized = copy.deepcopy(info)
            state = normalized.get('pColorBlendState')
            if state:
                # Keep attachment count, logic operations and blend constants;
                # only the fields specialized by Metal 4 are removed.
                for attachment in state.get('pAttachments') or []:
                    attachment.clear()
                    attachment['state'] = 'blend-and-write-mask-unspecialized'
            rp = render_pass(passes.get(info['renderPass']), info['subpass'])
            if rp is None:
                missing += 1
                continue
            normalized['renderPass'] = rp
            groups[key(normalized)].add(key(info))
            topology=info['pInputAssemblyState']['topology']
            topology_class='point' if 'POINT' in topology else 'line' if 'LINE' in topology else 'triangle'
            ms=info['pMultisampleState']
            projection={'stages':pair, 'layoutIdentity':info['layout'],
                        'vertexInput':info['pVertexInputState'],
                        'sampleCount':ms['rasterizationSamples'],
                        'alphaToCoverage':ms['alphaToCoverageEnable'],
                        'alphaToOne':ms['alphaToOneEnable'],
                        'rasterization':not info['pRasterizationState']['rasterizerDiscardEnable'],
                        'topologyClass':topology_class,
                        'renderPassExtensions':rp.get('pNext')}
            projections[key(projection)].add(key(info))
    unique = sum(len(values) for values in groups.values())
    repeated = sum(len(values)-1 for values in groups.values())
    result = {'graphicsCreateInfoCount': count, 'missingRenderPassCount': missing,
              'stageSets': dict(stages), 'moduleIdentityPairCount': len(pairs),
              'modulePairRequestHistogram': dict(Counter(pairs.values())),
              'uniqueFullDescriptors': unique, 'conservativeColorVariantGroups': len(groups),
              'groupSizeHistogram': dict(Counter(len(values) for values in groups.values())),
              'reusableColorVariantDescriptors': repeated,
              'reusableColorVariantFraction': repeated/unique if unique else None,
              'psoPerConservativeGroup': unique/len(groups) if groups else None,
              'nativeLikeProjectionGroups':len(projections),
              'nativeLikeProjectionSizeHistogram':dict(Counter(len(values) for values in projections.values())),
              'nativeLikeProjectionReusableFraction':(unique-len(projections))/unique if unique else None,
              'nativeBaseKeyMeasured': False,
              'limitations': ['Capture omits shader bytecode; shader-module identity is used, not IR artifact SHA.',
                              'All other Vulkan state/layout identity is kept; unused-state differences may over-split.',
                              'The native-like projection keeps full Vulkan vertex input and layout identity; actual IR function keys, used Metal attributes, per-pipeline ICB/amplification settings are unavailable.',
                              'This is one historical captured frame, not the entire cold-run workload.',
                              'A projected grouping does not replace E0-3 instrumentation using makeMetal4PipelineBaseKey.',
                              'Counts are not a time-weighted savings bound or a correctness oracle.']}
    with a.output.open('x') as out:
        json.dump(result, out, indent=2)
        out.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
