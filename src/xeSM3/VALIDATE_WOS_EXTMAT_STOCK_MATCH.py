from pathlib import Path
import sys

p = Path(__file__).with_name('XESM3ResourceRedirector.cpp')
s = p.read_text(encoding='utf-8', errors='replace')
checks = [
    ('stock MAT hash reader exists', 'TryGetStockMeshSectionMaterialHash(' in s),
    ('stock MAT hash scanner exists', 'FindStockRuntimeMaterialByHash(' in s),
    ('MAT pointer read uses MeshInfo+0x20', 'section + 0x20' in s),
    ('MAT hash read documented at +0x04', 'runtime MAT +0x04' in s),
    ('stock hash match mode logged', 'mode=STOCK-MESH-HASH-MATCH' in s),
    ('global resolver remains fallback', 'ResolveWrapExternalResourcePointer(' in s),
]
failed = False
for label, ok in checks:
    print(('PASS' if ok else 'FAIL') + ': ' + label)
    failed |= not ok

# Ensure the preferred stock-MAT path appears before the resolver fallback in the helper.
fn = s.find('void* ResolveWrapExternalMaterialForMeshSection(')
end = s.find('void* ResolveChSpidermanPlayerMaterialPointer(', fn)
body = s[fn:end] if fn >= 0 and end > fn else ''
preferred = body.find('FindStockRuntimeMaterialByHash(')
fallback = body.find('ResolveWrapExternalResourcePointer(')
order_ok = preferred >= 0 and fallback > preferred
print(('PASS' if order_ok else 'FAIL') + ': stock-runtime MAT hash match precedes global resolver fallback')
failed |= not order_ok

# New feature must not add SEH in these helpers.
for name in ('TryGetStockMeshSectionMaterialHash', 'FindStockRuntimeMaterialByHash', 'ResolveWrapExternalMaterialForMeshSection'):
    start = s.find(name + '(')
    next_candidates = [x for x in [s.find('\n    bool ', start + 1), s.find('\n    void* ', start + 1)] if x != -1]
    stop = min(next_candidates) if next_candidates else len(s)
    chunk = s[start:stop]
    ok = '__try' not in chunk and '__except' not in chunk
    print(('PASS' if ok else 'FAIL') + f': {name} adds no SEH')
    failed |= not ok

if failed:
    sys.exit(1)
print('\nWOS EXTERNAL MAT STOCK-MATCH VALIDATION PASS')
