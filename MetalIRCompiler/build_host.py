#!/usr/bin/env python3
"""Build the pinned macOS compiler from source into a new external directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
EXPORTS = ("MeloNXCompileMetalIR", "MeloNXReleaseMetalIR",
           "MeloNXMetalIRABIVersion", "MeloNXMetalIRDependencyIdentity")


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def output(args):
    return subprocess.check_output([str(a) for a in args], text=True).strip()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--msc-dir", type=Path, required=True)
    parser.add_argument("--llvm-config", type=Path, required=True)
    parser.add_argument("--zstd-library", type=Path)
    parser.add_argument("--mesa-repository", default="https://gitlab.freedesktop.org/has207/mesa.git")
    parser.add_argument("--meson", default="meson")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    out = args.output.resolve()
    checkout = ROOT.parent
    if out == checkout or checkout in out.parents or out in checkout.parents:
        parser.error("output must be outside the source checkout")
    if out.exists() and any(out.iterdir()):
        parser.error("output must be empty; use a new directory for reproducibility")
    if args.jobs < 1:
        parser.error("jobs must be positive")
    args.msc_dir = args.msc_dir.resolve()
    args.llvm_config = args.llvm_config.resolve()
    pin = json.loads((ROOT / "msc/VERSION").read_text())
    msc = args.msc_dir / pin.get("hostLibrary", "libmetalirconverter.dylib")
    if not msc.is_file() or sha(msc) != pin["librarySHA256"]:
        parser.error("MSC library does not match msc/VERSION")
    for header in pin["headers"]:
        file = args.msc_dir / header["path"]
        if not file.is_file() or sha(file) != header["sha256"]:
            parser.error("MSC header mismatch: " + header["path"])
    if output([args.llvm_config, "--version"]) != "17.0.6":
        parser.error("LLVM 17.0.6 is required")
    if not output(["xcrun", "xcodebuild", "-version"]).startswith("Xcode 27"):
        parser.error("select Xcode 27 with DEVELOPER_DIR")
    sdk = output(["xcrun", "--sdk", "macosx", "--show-sdk-path"])
    cc = output(["xcrun", "--sdk", "macosx", "--find", "clang"])
    cxx = output(["xcrun", "--sdk", "macosx", "--find", "clang++"])
    llvm_include = output([args.llvm_config, "--includedir"])
    llvm_libdir = Path(output([args.llvm_config, "--libdir"]))
    components = ["--link-static", "core", "bitreader", "bitwriter"]
    llvm_libraries = shlex.split(output([args.llvm_config, "--libnames", *components]))
    system = shlex.split(output([args.llvm_config, "--system-libs", *components]))
    if "-lzstd" in system:
        if not args.zstd_library or not args.zstd_library.is_file():
            parser.error("this LLVM SDK requires --zstd-library pointing to a macOS static archive")
        system = [str(args.zstd_library.resolve()) if a == "-lzstd" else a for a in system]
    out.mkdir(parents=True, exist_ok=True)
    source = out / "mesa-source"
    build = out / "mesa-build"
    lib = out / "lib"
    obj = out / "objects"
    lib.mkdir()
    obj.mkdir()
    revision = (ROOT / "mesa/MESA_COMMIT").read_text().strip()
    run(["git", "init", "-q", source])
    run(["git", "-C", source, "fetch", "--depth=1", args.mesa_repository, revision])
    run(["git", "-C", source, "checkout", "--detach", "-q", "FETCH_HEAD"])
    for patch in sorted((ROOT / "mesa/patches").glob("*.patch")):
        run(["git", "-C", source, "apply", "--check", patch])
        run(["git", "-C", source, "apply", patch])
    meson = shutil.which(args.meson)
    ninja = shutil.which(args.ninja)
    if not meson or not ninja:
        parser.error("Meson and Ninja must be available")
    tool_path = os.pathsep.join([str(Path(meson).parent), str(Path(ninja).parent), os.environ["PATH"]])
    env = dict(os.environ, CC=cc, CXX=cxx, SDKROOT=sdk, PATH=tool_path,
               MACOSX_DEPLOYMENT_TARGET="26.0",
               CFLAGS=shlex.join(["-isysroot", sdk, "-arch", "arm64"]),
               CXXFLAGS=shlex.join(["-isysroot", sdk, "-arch", "arm64"]),
               LDFLAGS=shlex.join(["-isysroot", sdk, "-arch", "arm64"]))
    options = ["-D" + line for line in (ROOT / "mesa/meson-options.txt").read_text().splitlines()
               if line and not line.startswith("#")]
    run([meson, "setup", build, source, *options], env=env)
    run([ninja, "-C", build, "-j", args.jobs,
         "src/microsoft/spirv_to_dxil/libspirv_to_dxil.dylib"], env=env)
    mesa = lib / "libspirv_to_dxil.dylib"
    shutil.copyfile(build / "src/microsoft/spirv_to_dxil/libspirv_to_dxil.dylib", mesa)
    shutil.copyfile(msc, lib / msc.name)
    flags = ["-std=c++17", "-O2", "-fno-rtti", "-fvisibility=hidden", "-DNDEBUG",
             "-isysroot", sdk, "-arch", "arm64", "-mmacosx-version-min=26.0",
             "-I" + str(ROOT / "src"), "-I" + str(ROOT / "src/air"), "-I" + llvm_include]
    objects = []
    for file in (ROOT / "src/air/BitcodeWriter.cpp", ROOT / "src/native_raster_adapter.cpp", ROOT / "src/air_math_adapter.cpp"):
        target = obj / (file.stem + ".o")
        run([cxx, *flags, "-c", file, "-o", target], env=env)
        objects.append(target)
    identity = hashlib.sha256(bytes.fromhex(sha(mesa)) + bytes.fromhex(sha(msc))).hexdigest()
    plugin = lib / "libMeloNXMetalIR.dylib"
    bridge = ROOT.parent / "MoltenVK/MoltenVK/GPUObjects"
    run([cxx, "-dynamiclib", *flags, "-I" + str(bridge),
         "-I" + str(source / "src/microsoft/spirv_to_dxil"),
         "-I" + str(source / "src/microsoft/compiler"), "-I" + str(args.msc_dir / "include"),
         '-DMELONX_METAL_IR_DEPENDENCY_IDENTITY="' + identity + '"',
         ROOT / "src/compiler_plugin.mm", *objects,
         *[llvm_libdir / name for name in llvm_libraries], *system,
         "-L" + str(lib), "-lspirv_to_dxil", "-lmetalirconverter",
         "-Wl,-rpath,@loader_path", "-Wl,-install_name,@rpath/libMeloNXMetalIR.dylib",
         "-Wl,-dead_strip", *["-Wl,-exported_symbol,_" + name for name in EXPORTS],
         "-o", plugin], env=env)
    manifest = {"mesaRevision": revision, "mscVersion": pin["version"], "llvmVersion": "17.0.6",
                "dependencyIdentity": identity, "sdk": sdk,
                "binaries": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size}
                             for p in (plugin, mesa, lib / msc.name)},
                "patches": {p.name: sha(p) for p in sorted((ROOT / "mesa/patches").glob("*.patch"))},
                "llvmArchives": {name: sha(llvm_libdir / name) for name in llvm_libraries},
                "sourceCommit": output(["git", "-C", ROOT, "rev-parse", "HEAD"])}
    (out / "identity.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
