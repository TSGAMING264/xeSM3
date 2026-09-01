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


def read_text(relative_path):
    return (ROOT / relative_path).read_text(encoding="utf-8-sig", errors="ignore")


def sha256(relative_path):
    return hashlib.sha256((ROOT / relative_path).read_bytes()).hexdigest()


def release_block(xml_text):
    match = re.search(
        r'<ItemDefinitionGroup Condition="\'\$\(Configuration\)\|\$\(Platform\)\'==\'Release\|Win32\'">(.*?)</ItemDefinitionGroup>',
        xml_text,
        re.S,
    )
    return match.group(1) if match else ""


def frozen_projection_hash(core_text):
    normalized = core_text.replace("\r\n", "\n")
    start = normalized.find("    void EnsureExWosStyleModsConfig")
    end = normalized.find("    bool SameArchiveIdentity", start)
    if start < 0 or end < 0:
        return ""
    projected = normalized[:start] + "    <INI_HOTFIX_REGION>\n\n" + normalized[end:]
    return hashlib.sha256(projected.encode("utf-8")).hexdigest()


def sample_enabled_mods(text):
    enabled = {}
    in_enabled = False
    for raw_line in text.lstrip("\ufeff").splitlines():
        line = raw_line.strip()
        if not line or line[0] in "#;":
            continue
        if line.startswith("[") and line.endswith("]"):
            in_enabled = line[1:-1].strip().casefold() == "enabledmods"
            continue
        if not in_enabled or "=" not in line:
            continue
        name, value = line.split("=", 1)
        enabled[name.strip().casefold()] = value.split(";", 1)[0].strip()
    return enabled


payload = read_text("XESM3.vcxproj")
bootstrap = read_text("XESM3Bootstrap.vcxproj")
proxy = read_text("DbgHelpProxy.cpp")
config = read_text("Mods/mods.config.ini")
core = read_text("XESM3ResourceRedirector.cpp")
package_script = read_text("MAKE_RELEASE_PACKAGE.cmd")

check("payload target is xeSM3", "<TargetName>xeSM3</TargetName>" in payload)
check("bootstrap target is dbghelp", "<TargetName>dbghelp</TargetName>" in bootstrap)
check("bootstrap loads xeSM3.dll", 'L"xeSM3.dll"' in proxy)
check("payload Release diagnostics compiled out", "XESM3_BOOT_DIAGNOSTIC" not in release_block(payload))
check("bootstrap Release diagnostics compiled out", "XESM3_BOOT_DIAGNOSTIC" not in release_block(bootstrap))
check(
    "Debug diagnostics retained for troubleshooting",
    payload.count("XESM3_BOOT_DIAGNOSTIC") == 1
    and bootstrap.count("XESM3_BOOT_DIAGNOSTIC") == 1,
)

active_default_mods = sample_enabled_mods(config)
check("Mods/mods.config.ini is the only documented config", "ONLY this file is read" in config and "<Game.exe>\\Mods\\mods.config.ini" in config)
check("default config enables no mods", active_default_mods == {})
check("default config documents 0/100", "0   = Disabled" in config and "100 = Enabled" in config)
check("default config documents last assignment wins", "LAST assignment wins" in config)
check("default config says examples are not auto-enabled", "No example/test mod is enabled by default" in config)
check("public parser accepts only 0/100", "if (parsed != 0 && parsed != 100)" in core)

check(
    "legacy root-INI migration removed",
    "legacyConfigPath" not in core
    and "CONFIG-MIGRATED" not in core
    and "Never migrate/copy a legacy root-level mods.config.ini" in core,
)
check(
    "only Mods/mods.config.ini is authoritative",
    "GetModsConfigPath(modsDirectory)" in core
    and "Authoritative config is ONLY <Game.exe>\\Mods\\mods.config.ini" in core,
)
check(
    "duplicate entries are last-assignment-wins",
    "std::find_if" in core
    and "_stricmp(candidate.name.c_str(), name.c_str()) == 0" in core
    and "*existing = info;" in core
    and sample_enabled_mods("\ufeff[EnabledMods]\nExample Mod=100\nExample Mod=0\n").get("example mod") == "0",
)
check(
    "UTF-8 BOM-safe INI parsing exists",
    "static_cast<unsigned char>(line[0]) == 0xEF" in core
    and "static_cast<unsigned char>(line[1]) == 0xBB" in core
    and "static_cast<unsigned char>(line[2]) == 0xBF" in core
    and "line.erase(0, 3);" in core,
)

allowed_mod_files = {
    "mods.config.ini",
    "filelist.txt",
    "filelist.apkf.txt",
    "filelist.apkf.paths.txt",
}
actual_mod_files = {
    path.relative_to(ROOT / "Mods").as_posix()
    for path in (ROOT / "Mods").rglob("*")
    if path.is_file()
}
check("Mods contains only config and three catalogs", actual_mod_files == allowed_mod_files)
check("no Mods/TESTMOD directory", not (ROOT / "Mods" / "TESTMOD").exists())
forbidden_test_toggle = "TESTMOD" + "=100"
check("no enabled TESTMOD default or package entry", forbidden_test_toggle not in config and forbidden_test_toggle not in package_script)
check("three master catalogs present", all((ROOT / "Mods" / name).is_file() for name in allowed_mod_files if name != "mods.config.ini"))

route_markers = {
    "MESH": "[NativeAPKF] MESH-HANDOFF",
    "MAT": "[NativeAPKF] MAT-HANDOFF",
    "TEX": "[NativeAPKF] TEX-HANDOFF",
    "ANIM": "[NativeAPKF] ANIM-HANDOFF",
    "SKEL": "[SKELAudit] POSTFIXUP",
    "ASKL": "[NativeAPKF] ASKL-HANDOFF-PREPARE",
}
for route, marker in route_markers.items():
    check(f"{route} loader route remains present", marker in core)
check("frozen route declaration remains", "frozenRoutes={MESH,MAT,ANIM,TEX,SKEL,ASKL}" in core)
check(
    "frozen core outside INI hotfix is unchanged",
    frozen_projection_hash(core) == "c82eaa8eefa8b21e7038487e520c9555a9c7cd91d7fe0757affd428f381a2239",
)

frozen_file_hashes = {
    "DbgHelpProxy.cpp": "f7b4fd2c4a999bda174feebd95b205d8a6b5b2f6000aa0214380d00ff23da3bc",
    "dllmain.cpp": "9ccee080b687272aa906e2f0b6717b271749897e95a88323c3dbb7d36b53b9f3",
    "XESM3ResourceRedirector.hpp": "30935378a133d5fe96a665b77b1a2be99f02d462aab174c5d558540b8658f4dc",
    "XESM3RuntimeOptions.cpp": "58d1b2a4a237f566794607ce4d115bc30dc7c45d555a6fc785d5598c93ed888b",
    "XESM3RuntimeOptions.hpp": "c376b210eaae4b926fcab043bf4c76b3a25e96e6d249963f500fd9cd9ead581a",
    "XESM3X86Only.hpp": "9cb3521461be1c8de28b15e2a25a49e2eae613a2b315dc478a496dfe6a2b7cb4",
    "DBGHELP.def": "7a277d031d524ff6085dec5df0edc99766696041427df50aeaeadefbecb81d79",
    "XESM3.def": "d7a98e0b4229fff327a60645977765dc77c55b111bca08ef112dffe5570bd27c",
    "XESM3.vcxproj": "e4684786d94fcd83599e874730c8b95b6a27f8a3d6b1124eb13e0d667008d581",
    "XESM3Bootstrap.vcxproj": "82c858268553247cd0dc792224568a10bb4b77b6130266493ab9e61b207fb818",
}
check("frozen companion source/project hashes unchanged", all(sha256(name) == expected for name, expected in frozen_file_hashes.items()))

check("release packager targets v0.1.1 ZIP", 'xeSM3 v0.1.1.zip' in package_script)
check("release ZIP is built from drop-in root contents", "Compress-Archive -LiteralPath" in package_script and "%OUT%\\dbghelp.dll" in package_script and "%OUT%\\xeSM3.dll" in package_script and "%OUT%\\Mods" in package_script)
check("release packager copies only safe Mods files", 'xcopy /E /I /Y "Mods\\*"' not in package_script and all(f'Mods\\{name}' in package_script for name in allowed_mod_files))
check("release packager does not include TESTMOD", "TESTMOD" not in package_script.upper())

check(
    "Spider-Man MESH examples remain reference-only",
    (ROOT / "Examples" / "01 - Spider-Man Character Example" / "CH_SPIDERMAN" / "_O0069.0xCFB154CD.T36.apkf" / "0xAC92103D.ch_spiderman000.mesh").is_file()
    and (ROOT / "Examples" / "01 - Spider-Man Character Example" / "CH_SPIDERMAN" / "_O0069.0xCFB154CD.T36.apkf" / "0xAC92103E.ch_spiderman001.mesh").is_file(),
)
check(
    "loading-screen TEX example remains reference-only",
    (ROOT / "Examples" / "02 - Loading Screen TEX Example" / "SPIDERMANLOGO" / "_O0001.0x348E72F4.T36.apkf" / "0xDEF62318.i_loading_screen_bkg.tex").is_file(),
)

print()
if failures:
    print(f"VALIDATION FAILED: {len(failures)} check(s)")
    sys.exit(1)
print("VALIDATION PASS")
