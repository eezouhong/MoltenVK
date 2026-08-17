#!/usr/bin/env python3
from pathlib import Path

queue = Path("MoltenVK/MoltenVK/GPUObjects/MVKQueue.mm")
text = queue.read_text()
old = '''\tif (!getPhysicalDevice()->getMTLDeviceCapabilities().supportsMetal4 ||
\t\t!mvkOSVersionIsAtLeast(26.0)) {
\t\t_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
\t\t\t\t\t\t  "Metal 4 command backend requested but unavailable on this OS or GPU.");
\t\treturn;
\t}
'''
new = '''\tbool supportsMetal4Family = getPhysicalDevice()->getMTLDeviceCapabilities().supportsMetal4;
\tbool validationOverride =
\t\tmvkGetEnvVarNumber("MVK_CONFIG_METAL4_COMMAND_VALIDATION", 0.0) != 0.0;
\tif ((!supportsMetal4Family && !validationOverride) || !mvkOSVersionIsAtLeast(26.0)) {
\t\t_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
\t\t\t\t\t\t  "Metal 4 command backend requested but unavailable on this OS or GPU.");
\t\treturn;
\t}
\tif (!supportsMetal4Family) {
\t\t_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
\t\t\t\t\t\t  "Metal 4 command validation override bypassed only the GPU-family advertisement; public factories and the bounded commit probe must still pass.");
\t}
'''
if text.count(old) != 1:
    raise SystemExit(f"MVKQueue.mm capability gate found {text.count(old)} times")
queue.write_text(text.replace(old, new, 1))

runner = Path("Tests/Metal4CommandBackend/run-macos.sh")
text = runner.read_text()
old = '''MVK_CONFIG_METAL4_COMMAND_BACKEND=1 \\
  "${BUILD_DIR}/metal4-transfer-e2e" \\
'''
new = '''MVK_CONFIG_METAL4_COMMAND_BACKEND=1 \\
MVK_CONFIG_METAL4_COMMAND_VALIDATION=1 \\
  "${BUILD_DIR}/metal4-transfer-e2e" \\
'''
if text.count(old) != 1:
    raise SystemExit(f"run-macos.sh Metal 4 invocation found {text.count(old)} times")
runner.write_text(text.replace(old, new, 1))
