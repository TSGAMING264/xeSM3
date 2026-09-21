from pathlib import Path
import re
p=Path(__file__).with_name("XESM3ResourceRedirector.cpp")
s=p.read_text(encoding="utf-8")

def extract(name):
    m=re.search(r"(?m)^    [^/\n]*\b"+re.escape(name)+r"\s*\(", s)
    if not m: raise SystemExit(f"FAIL: missing {name}")
    start=s.rfind("\n",0,m.start())+1
    brace=s.find("{",m.end())
    if brace<0: raise SystemExit(f"FAIL: missing body {name}")
    depth=0
    for i in range(brace,len(s)):
        if s[i]=='{': depth+=1
        elif s[i]=='}':
            depth-=1
            if depth==0: return s[start:i+1]
    raise SystemExit(f"FAIL: unterminated {name}")

fallback=extract("ResolveChSpidermanPlayerMaterialPointer")
helper=extract("ResolveWrapExternalMaterialForMeshSection")
resolver=extract("ResolveWrapExternalResourcePointer")

if "__try" not in fallback:
    raise SystemExit("FAIL: baseline fallback SEH unexpectedly missing")
# pointer/reference mentions of std::vector in the signature are harmless; disallow RAII locals.
for pat,label in [
    (r"\bstd::string\s+[A-Za-z_]", "std::string local"),
    (r"\bstd::vector\s*<[^>]+>\s+[A-Za-z_]", "std::vector local"),
    (r"\bstd::lock_guard\s*<", "lock_guard local"),
    (r"\bstd::unique_ptr\s*<", "unique_ptr local"),
    (r"\bstd::shared_ptr\s*<", "shared_ptr local"),
]:
    if re.search(pat,fallback):
        raise SystemExit(f"FAIL: {label} remains inside SEH fallback function")
print("PASS: SEH fallback has no new RAII locals")

helper_code=re.sub(r"//.*", "", helper)
if "__try" in helper_code or "__except" in helper_code:
    raise SystemExit("FAIL: external-MAT string helper contains SEH")
if "std::string resourceName" not in helper or "std::string resolveError" not in helper:
    raise SystemExit("FAIL: MAT strings were not moved into no-SEH helper")
print("PASS: external-MAT strings live in no-SEH helper")

resolver_code=re.sub(r"//.*", "", resolver)
if "__try" in resolver_code or "__except" in resolver_code:
    raise SystemExit("FAIL: stock resolver wrapper contains SEH")
print("PASS: stock MAT resolver wrapper contains no SEH")

print("\nC2712 ACTUAL-FIX VALIDATION PASS")
