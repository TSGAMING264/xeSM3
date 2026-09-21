from pathlib import Path
import re, sys
R=Path(__file__).resolve().parent
fail=[]
def chk(name, ok):
    print(('PASS' if ok else 'FAIL')+': '+name)
    if not ok: fail.append(name)

make=(R/'MAKE_RELEASE_PACKAGE.cmd').read_text(errors='ignore')
build=(R/'BUILD_FINAL_RELEASE_X86.cmd').read_text(errors='ignore')
readme=(R/'README.md').read_text(errors='ignore')
ini=(R/'xeSM3.ini').read_text(errors='ignore')
mods=(R/'Mods/mods.config.ini').read_text(errors='ignore')
post=(R/'PostFXResearch.cpp').read_text(errors='ignore')
core=(R/'XESM3ResourceRedirector.cpp').read_text(errors='ignore')

# Old-style public layout + only intended new config file.
for token in ['dbghelp.dll','xeSM3.dll','xeSM3.ini','Mods\\mods.config.ini','Mods\\filelist.txt','Mods\\filelist.apkf.txt','Mods\\filelist.apkf.paths.txt','Examples\\*','INSTALL.txt']:
    chk('package references '+token, token in make)
chk('runtime package does not copy source cpp', '.cpp" "%OUT%' not in make)
chk('runtime package does not copy headers', '.hpp" "%OUT%' not in make)
chk('runtime package does not copy PDB', '.pdb' not in make.lower())
chk('runtime package does not copy research logs', 'PostFXResearchLog' not in make and 'ResourceLog' not in make)

# Public 0/100 behavior.
chk('mod INI documents 0/100', '0   = Disabled' in mods and '100 = Enabled' in mods)
chk('mod parser strict 0/100', 'if (parsed != 0 && parsed != 100)' in core)
chk('PostFX INI documents 0/100', '0   = Disabled' in ini and '100 = Enabled' in ini)
chk('Debug Xbox default enabled', 'PostProcessDebugXbox=100' in ini)
chk('Retail Xbox default disabled', 'PostProcessRetailXbox=0' in ini)
chk('PostFX parser strict 100', all(x in post for x in [
    's_postProcessFixEnabled = publicFixValue == 100;',
    's_postProcessRetailXboxEnabled = publicRetailXboxValue == 100;',
    's_postProcessDebugXboxEnabled = publicDebugXboxValue == 100;']))

# WRAP integration.
chk('WRAP magic present', 'XESM3_WRAP_MAGIC = 0x50415257u' in core)
chk('WRAP decoder present', 'TryUnwrapLooseResource' in core)
chk('WRAP-aware loose reader present', 'ReadLooseResourceFile' in core)
chk('WRAP hash normalization present', 'Normalize it before explicit-hash parsing and fallback hashing' in core)

# V10.5.72 reset ownership.
chk('Reset vtable slot 16 present', 'kD3DResetVtableIndex = 16' in post)
chk('DrawPrimitive slot 81 present', 'kD3DDrawPrimitiveVtableIndex = 81' in post)
chk('Reset hook present', 'HRESULT WINAPI RawReset_Hook' in post)
chk('Reset releases xeSM3 resources first', 'ReleasePostFXDeviceResourcesForReset();' in post)
chk('device-ready gate present', 's_postFxDeviceReady' in post)
chk('failed/lost device falls through to stock draw', 'InterlockedCompareExchange(&s_postFxDeviceReady, 0, 0) == 0' in post and 'return s_rawDrawPrimitive(device, primitiveType, startVertex, primitiveCount);' in post)
chk('narrow production hook pair documented', 'slot 16 Reset + slot 81 DrawPrimitive' in post)

# Final build/package behavior.
chk('final builder targets x86 Release', '/p:Configuration=Release /p:Platform=x86' in build)
chk('final builder calls public packager', 'MAKE_RELEASE_PACKAGE.cmd' in build)
chk('final builder creates final ZIP', 'xeSM3_v0.1.0_FINAL.zip' in build)
chk('final builder writes SHA-256', 'Get-FileHash -Algorithm SHA256' in build)
chk('public version remains v0.1.0', 'xeSM3 v0.1.0' in readme and 'PublicVersion=0.1.0' in mods)

print()
if fail:
    print(f'FINAL SOURCE VALIDATION FAILED: {len(fail)} check(s)')
    sys.exit(1)
print('FINAL SOURCE VALIDATION PASS')
