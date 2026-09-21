from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parent
CORE = (ROOT / "XESM3ResourceRedirector.cpp").read_text(errors="ignore")

failures = []

def check(name, ok):
    print(("PASS" if ok else "FAIL") + ": " + name)
    if not ok:
        failures.append(name)


def align(value, boundary=16):
    return (value + boundary - 1) & ~(boundary - 1)


def rel32(buf, field, target):
    struct.pack_into("<i", buf, field, target - field)


def make_synthetic_wrap():
    # Mirrors WebOfShadowsTools Wrapper.wrapResourceFile layout closely enough
    # to validate xeSM3's release decoder contract. Two components are used so
    # the synthetic internal pointer crosses the WRAP-only PHYS separator.
    # One external and one global patch are also included; their serialized
    # target tokens must remain untouched by the release decoder.
    component0 = bytearray(16)
    component1 = bytes(b"ABCDEFGH")

    component_table = 0x14
    patch_table = 0x30
    external_table = 0x50
    internal_table = 0x60
    global_table = 0x70
    wrapper_end = 0x80
    component0_off = wrapper_end
    phys_off = component0_off + len(component0)
    component1_off = phys_off + 4
    total = component1_off + len(component1)

    buf = bytearray(total)
    buf[0:4] = b"WRAP"
    struct.pack_into("<I", buf, 4, 0xEE185533)
    rel32(buf, 8, patch_table)
    struct.pack_into("<I", buf, 12, 2)
    rel32(buf, 16, component_table)

    # Component table.
    struct.pack_into("<I", buf, component_table + 0, len(component0))
    rel32(buf, component_table + 4, component0_off)
    struct.pack_into("<I", buf, component_table + 8, len(component1))
    rel32(buf, component_table + 12, component1_off)

    # Patch table: ext=1, internal=1, global=1.
    struct.pack_into("<I", buf, patch_table + 0x00, 1)
    rel32(buf, patch_table + 0x04, external_table)
    struct.pack_into("<I", buf, patch_table + 0x08, 1)
    rel32(buf, patch_table + 0x0C, internal_table)
    struct.pack_into("<I", buf, patch_table + 0x10, 1)
    rel32(buf, patch_table + 0x14, global_table)

    # External patch entry: NAME/hash/expected-index/pTarget. The target token is
    # deliberately left serialized in component0+8.
    buf[external_table:external_table + 4] = b"MAT\x00"
    struct.pack_into("<I", buf, external_table + 4, 0x2BB8A7BB)
    struct.pack_into("<i", buf, external_table + 8, -1)
    rel32(buf, external_table + 12, component0_off + 8)

    # Internal patch entry points to component0+4. The pointer stored there
    # references component1+3 through the WRAP-relative representation.
    target = component0_off + 4
    reference = component1_off + 3
    rel32(buf, internal_table, target)
    struct.pack_into("<i", component0, 4, reference - target)

    # Global patch entry with its own pTarget. The value at component0+12 is an
    # opaque global token and must survive flattening unchanged.
    buf[global_table:global_table + 4] = b"TEX\x00"
    struct.pack_into("<I", buf, global_table + 4, 0x12345678)
    buf[global_table + 8:global_table + 12] = b"GLBL"
    rel32(buf, global_table + 12, component0_off + 12)

    struct.pack_into("<I", component0, 8, 0x04001234)
    struct.pack_into("<I", component0, 12, 0xDEADBEEF)

    buf[component0_off:component0_off + len(component0)] = component0
    buf[phys_off:phys_off + 4] = b"PHYS"
    buf[component1_off:component1_off + len(component1)] = component1
    return bytes(buf)


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def s32(data, off):
    return struct.unpack_from("<i", data, off)[0]


def resolve_rel(data, off):
    target = off + s32(data, off)
    if target < 0 or target > len(data):
        raise ValueError("relative pointer outside wrapper")
    return target


def unwrap_like_release(data):
    if data[:4] != b"WRAP":
        raise ValueError("not WRAP")
    patch_table = resolve_rel(data, 8)
    component_count = u32(data, 12)
    component_table = resolve_rel(data, 16)
    if not 1 <= component_count <= 16:
        raise ValueError("component count")

    comps = []
    flat_size = 0
    for i in range(component_count):
        entry = component_table + i * 8
        size = u32(data, entry)
        off = resolve_rel(data, entry + 4)
        if size == 0 or off + size > len(data):
            raise ValueError("component range")
        comps.append((off, size, flat_size))
        flat_size += size

    flat = bytearray(flat_size)
    for off, size, flat_off in comps:
        flat[flat_off:flat_off + size] = data[off:off + size]

    def map_flat(wrapper_off, needed):
        for off, size, flat_off in comps:
            if wrapper_off >= off and wrapper_off + needed <= off + size:
                return flat_off + (wrapper_off - off)
        raise ValueError("offset outside components")

    if patch_table + 0x18 > len(data):
        raise ValueError("patch table")
    ext_count = u32(data, patch_table + 0x00)
    ext_table = resolve_rel(data, patch_table + 0x04)
    int_count = u32(data, patch_table + 0x08)
    int_table = resolve_rel(data, patch_table + 0x0C)
    global_count = u32(data, patch_table + 0x10)
    global_table = resolve_rel(data, patch_table + 0x14)

    def valid_table(off, count, stride):
        return count == 0 or (off <= len(data) and off + count * stride <= len(data))

    if not valid_table(ext_table, ext_count, 16):
        raise ValueError("external patch table")
    if not valid_table(int_table, int_count, 4):
        raise ValueError("internal patch table")
    if not valid_table(global_table, global_count, 16):
        raise ValueError("global patch table")

    external = []
    for i in range(ext_count):
        entry = ext_table + i * 16
        resource_type = u32(data, entry + 0)
        resource_hash = u32(data, entry + 4)
        expected_index = u32(data, entry + 8)
        target = resolve_rel(data, entry + 12)
        target_flat = map_flat(target, 4)
        external.append((resource_type, resource_hash, expected_index, target_flat))

    for i in range(int_count):
        patch_entry = int_table + i * 4
        target = resolve_rel(data, patch_entry)
        target_flat = map_flat(target, 4)
        reference = target + s32(data, target)
        reference_flat = map_flat(reference, 1)
        struct.pack_into("<I", flat, target_flat, reference_flat)

    return bytes(flat), (ext_count, int_count, global_count), external


# Source-level checks for the exact release implementation.
check("WRAP magic detector exists", "XESM3_WRAP_MAGIC = 0x50415257u" in CORE)
check("WRAP decoder is fail-closed", "WRAP decode failed:" in CORE and "TryUnwrapLooseResource" in CORE)
check("internal pointers normalized to flat offsets", "const uint32_t localOffset = static_cast<uint32_t>(referenceFlatOffset);" in CORE)
check("external/global tables are validated", "WRAP patch array is outside file" in CORE)
check("external patches normalize TYPE+HASH+flat target metadata", "XESM3WrapExternalPatch" in CORE and "targetFlatOffset" in CORE)
check("WoS-style external MAT hashes reach NativeMESH", "EXT-MAT-APPLY" in CORE and "WOS-HASH-REFERENCE" in CORE)
check("external MAT resolution uses master catalog names", "s_MaterialNameByHash" in CORE and "TryGetCatalogResourceName" in CORE)
check("native loose reads route through WRAP-aware reader", CORE.count("ReadLooseResourceFile(") >= 7)
check(".wrap is removed before resource hashing", "Normalize it before explicit-hash parsing and fallback hashing" in CORE)

# Functional synthetic WRAP test.
synthetic = make_synthetic_wrap()
flat, counts, external = unwrap_like_release(synthetic)
check("synthetic WRAP magic/layout parses", synthetic[:4] == b"WRAP" and counts == (1, 1, 1))
check("synthetic external MAT patch captures hash and flat target", external == [(0x0054414D, 0x2BB8A7BB, 0xFFFFFFFF, 8)])
check("PHYS separator is not copied into flattened native resource", len(flat) == 24 and flat[16:24] == b"ABCDEFGH")
check("cross-component internal pointer becomes flat local offset", u32(flat, 4) == 19)
check("external serialized target token is preserved", u32(flat, 8) == 0x04001234)
check("global serialized target token is preserved", u32(flat, 12) == 0xDEADBEEF)

# Malformed/truncated WRAP must fail instead of reaching a native adapter.
try:
    unwrap_like_release(synthetic[:-3])
except ValueError:
    truncated_rejected = True
else:
    truncated_rejected = False
check("truncated WRAP is rejected", truncated_rejected)

print()
if failures:
    print(f"WRAP VALIDATION FAILED: {len(failures)} check(s)")
    sys.exit(1)
print("WRAP VALIDATION PASS")
