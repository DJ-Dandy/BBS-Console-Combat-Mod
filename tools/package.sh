#!/bin/bash
# Build the OpenKH Mod Manager package dist/BBS-KH2-Style.zip from out/dinput8.dll and dist/bbskh2.ini.
# The DLL is the same one as the stand-alone DINPUT8.dll: under any other name it does not act as a system-DLL
# stand-in, and Panacea loads it from <mod folder>/bbs/dll/.
set -e
cd "$(dirname "$0")/.."
[ -f out/dinput8.dll ] || { echo "run build.sh first"; exit 1; }
P=out/pkg/BBS-KH2-Style
rm -rf out/pkg && mkdir -p $P/dll
python3 modmanager/make_images.py >/dev/null
cp modmanager/mod.yml modmanager/README.md modmanager/icon.png modmanager/preview.png $P/
cp out/dinput8.dll $P/dll/bbskh2.dll
cp dist/bbskh2.ini $P/dll/bbskh2.ini
rm -f dist/BBS-KH2-Style.zip
(cd $P && python3 -c "
import zipfile, os
z = zipfile.ZipFile('../../../dist/BBS-KH2-Style.zip', 'w', zipfile.ZIP_DEFLATED)
for r, d, fs in os.walk('.'):
    for f in sorted(fs):
        p = os.path.join(r, f)[2:]
        z.write(p, p.replace(os.sep, '/'))
z.close()")
python3 -c "
import zipfile; z = zipfile.ZipFile('dist/BBS-KH2-Style.zip')
for i in z.infolist(): print('  %8d  %s' % (i.file_size, i.filename))"
ls -la dist/BBS-KH2-Style.zip
