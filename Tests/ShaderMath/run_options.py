"""Compare actual MTLCompileOptions from the old and current source methods.

No shader compilation or GPU submission. On the shared host, still invoke this
Metal API check through the canonical Debug Tool queue.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def method(text):
    start = text.index('MTLCompileOptions* MVKDevice::getMTLCompileOptions(')
    end = text.index("// Can't use prefilled Metal command buffers", start)
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', required=True, help='Pre-refactor Git revision')
    p.add_argument('--spirv-headers', type=Path, required=True)
    p.add_argument('--vulkan-headers', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    device = 'MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm'
    converter = 'MoltenVKShaderConverter/MoltenVKShaderConverter/SPIRVToMSLConverter.h'
    def git(*items):
        return subprocess.check_output(['git', '-C', str(root), *items], text=True)
    baseline = git('rev-parse', args.baseline).strip()
    old = git('show', baseline + ':' + device)
    new = (root / device).read_text()
    supported = re.search(r'kSPIRVFPFastMathModesSupported\s*=\s*(.*?);',
                          git('show', baseline + ':' + converter), re.S).group(1)
    prefix = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "MVKShaderMathPolicy.h"
#include <iostream>
template<class Tv, class Tm> bool mvkAreAllFlagsEnabled(Tv value, Tm flags) {
    return (value & flags) == flags;
}
namespace mvk { const uint32_t kSPIRVFPFastMathModesSupported = SUPPORTED; }
struct TestConfig { MVKConfigFastMath fastMathEnabled; } config;
TestConfig& getMVKConfig() { return config; }
struct PhysicalDevice {
    struct { MTLLanguageVersion mslVersionEnum = MTLLanguageVersion3_0; } _metalFeatures;
};
class MVKDevice {
    PhysicalDevice physical;
    PhysicalDevice* _physicalDevice = &physical;
public: MTLCompileOptions* getMTLCompileOptions(uint32_t, bool);
};
class LegacyDevice {
    PhysicalDevice physical;
    PhysicalDevice* _physicalDevice = &physical;
public: MTLCompileOptions* getMTLCompileOptions(uint32_t, bool);
};
'''.replace('SUPPORTED', supported)
    main_code = r'''
int main() { @autoreleasepool {
    MVKDevice current; LegacyDevice legacy; unsigned checked = 0;
    const uint32_t bits[] = {spv::FPFastMathModeNSZMask, spv::FPFastMathModeAllowRecipMask,
        spv::FPFastMathModeAllowReassocMask, spv::FPFastMathModeAllowContractMask,
        spv::FPFastMathModeNotNaNMask, spv::FPFastMathModeNotInfMask};
    for (unsigned combination = 0; combination < 65; ++combination) {
        uint32_t flags = 0;
        for (unsigned i = 0; i < 6; ++i)
            if (combination & (1u << i)) flags |= bits[i];
        if (combination == 64) flags = UINT32_MAX;
        for (auto preference : {MVK_CONFIG_FAST_MATH_ON_DEMAND,
                                MVK_CONFIG_FAST_MATH_ALWAYS, MVK_CONFIG_FAST_MATH_NEVER}) {
            config.fastMathEnabled = preference;
            for (bool invariant : {false, true}) {
                auto a = current.getMTLCompileOptions(flags, invariant);
                auto b = legacy.getMTLCompileOptions(flags, invariant);
                bool same = a.languageVersion == b.languageVersion &&
                    a.preserveInvariance == b.preserveInvariance &&
                    a.optimizationLevel == b.optimizationLevel;
#if MVK_XCODE_16
                if ([a respondsToSelector:@selector(mathMode)])
                    same &= a.mathMode == b.mathMode &&
                            a.mathFloatingPointFunctions == b.mathFloatingPointFunctions;
                else
#endif
                    same &= a.fastMathEnabled == b.fastMathEnabled;
                if (!same) {
                    std::cerr << "Mismatch flags=" << flags << " preference=" << preference
                              << " invariant=" << invariant << '\n';
                    return 1;
                }
                ++checked;
            }
        }
    }
    std::cout << "{\"comparisons\":" << checked << ",\"mismatches\":0}\n";
    return 0;
} }
'''
    source = out / 'source_options.mm'
    source.write_text(prefix + method(new) +
                      method(old).replace('MVKDevice::', 'LegacyDevice::') + main_code)
    includes = [root / 'MoltenVK/MoltenVK/GPUObjects', root / 'MoltenVK/MoltenVK/API',
                args.spirv_headers, args.vulkan_headers]
    results = []
    for modern in (0, 1):
        binary = out / ('options-' + str(modern))
        command = ['clang++', '-std=c++17', '-O2', '-Wno-deprecated-declarations',
                   '-DMVK_XCODE_16=' + str(modern), *['-I' + str(i) for i in includes],
                   str(source), '-framework', 'Foundation', '-framework', 'Metal',
                   '-o', str(binary)]
        with (out / ('compile-' + str(modern) + '.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        process = subprocess.run([str(binary)], capture_output=True, text=True)
        (out / ('run-' + str(modern) + '.log')).write_text(process.stdout + process.stderr)
        process.check_returncode()
        results.append(dict(modern=bool(modern), **json.loads(process.stdout)))
    sha = lambda s: hashlib.sha256(s.encode()).hexdigest()
    result = dict(baseline=baseline, current=git('rev-parse', 'HEAD').strip(),
                  sourceOptionsSHA256=sha(source.read_text()),
                  currentMethodSHA256=sha(method(new)), baselineMethodSHA256=sha(method(old)),
                  cases=results, comparisons=sum(r['comparisons'] for r in results),
                  scope='Actual options only; no shader compilation, pixels or game/device claim')
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
