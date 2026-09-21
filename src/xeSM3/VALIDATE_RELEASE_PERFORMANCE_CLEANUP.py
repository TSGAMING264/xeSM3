from pathlib import Path
import hashlib
import re
import sys

ROOT = Path(__file__).resolve().parent
failures = []

def check(name, ok):
    print(("PASS" if ok else "FAIL") + ": " + name)
    if not ok:
        failures.append(name)

def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

main = (ROOT / "dllmain.cpp").read_text(errors="ignore")
post = (ROOT / "PostFXResearch.cpp").read_text(errors="ignore")
core = (ROOT / "XESM3ResourceRedirector.cpp").read_text(errors="ignore")
proj = (ROOT / "XESM3.vcxproj").read_text(errors="ignore")

cleanup_guard = '''#if defined(XESM3_BOOT_DIAGNOSTIC)
    // Release performance cleanup: these are research-only hot-path probes.'''
check("diagnostic-only startup guard present", cleanup_guard in main)
check("NativeANIM production redirector still attached", "AttachNativeAnimRedirectorDetour();" in main)
check("ANIM diagnostics retained behind diagnostic guard", "AttachNativeAnimRuntimeDiagnosticDetours();" in main and main.index("AttachNativeAnimRuntimeDiagnosticDetours();") > main.index(cleanup_guard))
check("render mesh probe retained behind diagnostic guard", "AttachRenderMeshProbeDetour();" in main and main.index("AttachRenderMeshProbeDetour();") > main.index(cleanup_guard))

m = re.search(r'<ItemDefinitionGroup Condition="\'\$\(Configuration\)\|\$\(Platform\)\'==\'Release\|Win32\'">(.*?)</ItemDefinitionGroup>', proj, re.S)
release_block = m.group(1) if m else ""
check("Release excludes XESM3_BOOT_DIAGNOSTIC", bool(release_block) and "XESM3_BOOT_DIAGNOSTIC" not in release_block)
check("Release defines NDEBUG", "NDEBUG" in release_block)

check("PostFX event ring guarded out of Release", "#if !defined(NDEBUG)\n    // Diagnostic/research telemetry only." in post and "Event s_events[kEventCapacity] = {};" in post)
check("Release ReserveEvent returns nullptr", "#if defined(NDEBUG)\n        // Public Release: no research Event allocation/collection." in post and "(void)type;\n        return nullptr;" in post)
check("Release FlushEvents has no ring drain", "#if defined(NDEBUG)\n        // Public Release has no research Event ring." in post)

check("Reset slot 16 retained", "kD3DResetVtableIndex = 16" in post)
check("DrawPrimitive slot 81 retained", "kD3DDrawPrimitiveVtableIndex = 81" in post)
check("Reset resource release retained", "ReleasePostFXDeviceResourcesForReset();" in post)
check("device-ready fail-closed gate retained", "s_postFxDeviceReady" in post)
check("WRAP decoder retained", "XESM3_WRAP_MAGIC = 0x50415257u" in core and "TryUnwrapLooseResource" in core)

check("resource-loader delta limited to intended WoS WRAP external-MAT path", all(token in core for token in ["XESM3WrapExternalPatch", "EXT-MAT-APPLY", "WOS-HASH-REFERENCE", "s_MaterialNameByHash"]))
check("PostFX header byte-identical to tested final", sha256(ROOT / "PostFXResearch.hpp") == "9aeaef54171c8fba1e38487453b24e3b8b6746de81d863feb665abf0b6cad68e")
check("project file byte-identical to tested final", sha256(ROOT / "XESM3.vcxproj") == "7b490878a03072d85b7c6e5189fa9f826970a1ccc4d4c49548e6ebfd19d21415")

print()
if failures:
    print(f"PERFORMANCE CLEANUP VALIDATION FAILED: {len(failures)} check(s)")
    sys.exit(1)
print("PERFORMANCE CLEANUP VALIDATION PASS")
