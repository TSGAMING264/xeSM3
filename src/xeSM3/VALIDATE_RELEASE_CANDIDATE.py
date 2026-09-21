from pathlib import Path
import re, sys

ROOT = Path(__file__).resolve().parent
failures = []

def check(name, ok):
    print(("PASS" if ok else "FAIL") + ": " + name)
    if not ok:
        failures.append(name)

payload = (ROOT / "XESM3.vcxproj").read_text(errors="ignore")
bootstrap = (ROOT / "XESM3Bootstrap.vcxproj").read_text(errors="ignore")
proxy = (ROOT / "DbgHelpProxy.cpp").read_text(errors="ignore")
config = (ROOT / "Mods" / "mods.config.ini").read_text(errors="ignore")
postfx_ini = (ROOT / "xeSM3.ini").read_text(errors="ignore") if (ROOT / "xeSM3.ini").is_file() else ""
core = (ROOT / "XESM3ResourceRedirector.cpp").read_text(errors="ignore")
dllmain = (ROOT / "dllmain.cpp").read_text(errors="ignore")
postfx = (ROOT / "PostFXResearch.cpp").read_text(errors="ignore") if (ROOT / "PostFXResearch.cpp").is_file() else ""

# Extract Release blocks so Debug diagnostics do not create false failures.
def release_block(xml_text):
    m = re.search(r'<ItemDefinitionGroup Condition="\'\$\(Configuration\)\|\$\(Platform\)\'==\'Release\|Win32\'">(.*?)</ItemDefinitionGroup>', xml_text, re.S)
    return m.group(1) if m else ""

check("payload target is xeSM3", "<TargetName>xeSM3</TargetName>" in payload)
check("bootstrap target is dbghelp", "<TargetName>dbghelp</TargetName>" in bootstrap)
check("payload Release diagnostics compiled out", "XESM3_BOOT_DIAGNOSTIC" not in release_block(payload))
check("bootstrap Release diagnostics compiled out", "XESM3_BOOT_DIAGNOSTIC" not in release_block(bootstrap))
check("Debug diagnostics retained for troubleshooting", payload.count("XESM3_BOOT_DIAGNOSTIC") == 1 and bootstrap.count("XESM3_BOOT_DIAGNOSTIC") == 1)
check("bootstrap loads public payload filename", 'L"xeSM3.dll"' in proxy)
check("SymInitialize forward remains", "Proxy_SymInitialize" in proxy and 'ResolveDbgHelp<Fn>("SymInitialize")' in proxy)

check("config defaults to no enabled mods", "[EnabledMods]" in config and not re.search(r'^\s*[^;#\[\r\n][^=\r\n]*=100\s*$', config, re.M))
check("mod config documents 0/100", "0   = Disabled" in config and "100 = Enabled" in config)
check("mod toggle parser accepts only 0/100", "if (parsed != 0 && parsed != 100)" in core)
check("no generalized numeric priority winner", "candidate.toggleValue > incumbent.toggleValue" not in core and "candidate.toggleValue < incumbent.toggleValue" not in core)
check("public persistent resource log disabled", "void WriteResourceLogLine(const char*)" in core and "public release intentionally produces no persistent resource log file" in core)

check("PostFX source included", '<ClCompile Include="PostFXResearch.cpp" />' in payload and (ROOT / "PostFXResearch.cpp").is_file())
check("PostFX header included", '<ClInclude Include="PostFXResearch.hpp" />' in payload and (ROOT / "PostFXResearch.hpp").is_file())
check("public xeSM3.ini included", '<None Include="xeSM3.ini" />' in payload and bool(postfx_ini))
check("Debug Xbox release route enabled", "PostProcessDebugXbox=100" in postfx_ini and "PostProcessRetailXbox=0" in postfx_ini)
check("PostFX public values document 0/100", "0   = Disabled" in postfx_ini and "100 = Enabled" in postfx_ini)
check("PostFX public parser accepts only value 100 as enabled", "s_postProcessFixEnabled = publicFixValue == 100;" in postfx and "s_postProcessRetailXboxEnabled = publicRetailXboxValue == 100;" in postfx and "s_postProcessDebugXboxEnabled = publicDebugXboxValue == 100;" in postfx)
check("PostFX bridge hooks nglPresent", "XESM3_NGL_PRESENT_ADDRESS = 0x008CD650" in dllmain and "PostFXResearch_OnPresent();" in dllmain)
check("PostFX detours integrated", "AttachPostFXResearchDetours();" in dllmain and "InstallPostFXResearchCallsitePatches();" in dllmain)
check("public PostFX persistent log disabled", "Public xeSM3 keeps the proven PostFX validation" in postfx and "#if defined(XESM3_PUBLIC_BUILD)" in postfx)

check("WRAP magic support present", "XESM3_WRAP_MAGIC = 0x50415257u" in core)
check("WRAP decoder present", "TryUnwrapLooseResource" in core and "ReadLooseResourceFile" in core)
check("WRAP internal pointer normalization present", "WRAP internal pointer" in core and "referenceFlatOffset" in core)
check("WRAP identity strips .wrap before fallback hashing", "Normalize it before explicit-hash parsing and fallback hashing" in core)
check("WRAP loose paths feed native adapters", core.count("ReadLooseResourceFile(looseEntry.path") >= 5)
check("WRAP docs use exWoS-style filename", ".wrap.mesh" in config)
check("self-contained WRAP validator present", (ROOT / "VALIDATE_WRAP_SUPPORT.py").is_file())

check("three catalogs present", all((ROOT / "Mods" / n).is_file() for n in ["filelist.txt", "filelist.apkf.txt", "filelist.apkf.paths.txt"]))
check("no bundled developer mesh test in Mods", not (ROOT / "Mods" / "SM3 Dual Original Mesh Probe Test").exists())
check("Spider-Man MESH examples present", (ROOT / "Examples" / "01 - Spider-Man Character Example" / "CH_SPIDERMAN" / "_O0069.0xCFB154CD.T36.apkf" / "0xAC92103D.ch_spiderman000.mesh").is_file() and (ROOT / "Examples" / "01 - Spider-Man Character Example" / "CH_SPIDERMAN" / "_O0069.0xCFB154CD.T36.apkf" / "0xAC92103E.ch_spiderman001.mesh").is_file())
check("SPIDERMANLOGO TEX example present", (ROOT / "Examples" / "02 - Loading Screen TEX Example" / "SPIDERMANLOGO" / "_O0001.0x348E72F4.T36.apkf" / "0xDEF62318.i_loading_screen_bkg.tex").is_file())
check("public default config text embedded", "Created by TSGAMING264" in core and "PublicVersion=0.1.0" in core)

print()
if failures:
    print(f"VALIDATION FAILED: {len(failures)} check(s)")
    sys.exit(1)
print("VALIDATION PASS")
