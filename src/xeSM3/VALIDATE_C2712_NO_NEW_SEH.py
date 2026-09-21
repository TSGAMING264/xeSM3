from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parent
src = (root / 'XESM3ResourceRedirector.cpp').read_text(encoding='utf-8', errors='replace')
actual_try = len(re.findall(r'^\s*__try\s*$', src, re.M))
expected_baseline_try = 71
checks = [
    (actual_try == expected_baseline_try,
     f'XESM3ResourceRedirector.cpp real __try count matches proven baseline ({expected_baseline_try})'),
    ('ResolveResourcePointerSehPrimitive' not in src,
     'WoS external-MAT path adds no private SEH resolver helper'),
    ('void* resolved = resolver(&targetName, resourceType);' in src,
     'WoS external-MAT path uses existing direct stock resolver call pattern'),
    ('WOS-HASH-REFERENCE' in src,
     'WoS external-MAT feature remains present'),
]
failed = False
for ok, label in checks:
    print(('PASS: ' if ok else 'FAIL: ') + label)
    failed |= not ok
if failed:
    sys.exit(1)
print('\nC2712 NO-NEW-SEH VALIDATION PASS')
