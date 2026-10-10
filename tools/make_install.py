"""Clean installation of the native port from the user's own ISOs.

Usage: python tools/make_install.py --out "D:\\Games\\K-On Houkago Live Recompiled"
       [--iso-jp PATH] [--iso-en PATH] [--exe PATH]

Creates a new folder (it must not exist or be empty) with:
  K-On Houkago Live Recompiled.exe the port (one executable, GPU chosen automatically)
  Jugar.cmd                       launcher (language from Settings; "Jugar.cmd ja" / "en" forces one)
  analysis/extracted/jp, en       both discs extracted, every file checked against the ISO
  analysis/executables/pkon.elf   EBOOT decrypted with pspdecrypt, checked by SHA-256
  third_party/ffmpeg              the FFmpeg libraries the port loads (LGPL) and their licence
  LEEME.txt                       controls, settings and mods
No game data is shipped with the port: everything comes from the ISOs given here.
"""
import argparse, hashlib, shutil, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import iso_audit

ELF_SHA256 = 'eda890135ea4aee1a8d0a82b2f00a7159060c58498a6705df7518b36e46b85b1'
EXE_NAME = 'K-On Houkago Live Recompiled.exe'

def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--iso-jp', type=Path, default=ROOT / 'Roms PSP/K-ON! Houkago Live!! (Japan).iso')
    p.add_argument('--iso-en', type=Path, default=ROOT / 'Roms PSP/K-on After School (English).iso')
    p.add_argument('--exe', type=Path, default=ROOT / 'build/native_full/kon_boot_probe.exe')
    a = p.parse_args()
    out = a.out.resolve()
    if out.exists() and any(out.iterdir()):
        sys.exit(f'{out} is not empty: choose a new folder (nothing is overwritten).')
    for path in (a.iso_jp, a.iso_en, a.exe):
        if not path.is_file():
            sys.exit(f'Missing {path}')
    if ROOT in out.parents and (ROOT / 'analysis') in out.parents:
        sys.exit('Choose a folder outside analysis/.')
    out.mkdir(parents=True, exist_ok=True)
    extracted = out / 'analysis/extracted'
    for label, iso in (('jp', a.iso_jp), ('en', a.iso_en)):
        iso_audit.audit(iso, label, out / f'analysis/iso_{label}.json', extracted)
    elf = out / 'analysis/executables/pkon.elf'
    elf.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(ROOT / 'third_party/pspdecrypt-bin/pspdecrypt.exe'), str(extracted / 'jp/PSP_GAME/SYSDIR/EBOOT.BIN'), '-o', str(elf)], check=True, capture_output=True)
    if sha256(elf) != ELF_SHA256:
        sys.exit('The decrypted EBOOT is not the expected executable (different disc?).')
    shutil.copy2(a.exe, out / EXE_NAME)
    # FFmpeg (LGPL, shared build): the four libraries the port loads, with their licence.
    media = out / 'third_party/ffmpeg/bin'
    media.mkdir(parents=True, exist_ok=True)
    for dll in ('avutil-60.dll', 'avcodec-62.dll', 'avformat-62.dll', 'swresample-6.dll'):
        shutil.copy2(ROOT / 'third_party/ffmpeg/bin' / dll, media / dll)
    shutil.copy2(ROOT / 'third_party/ffmpeg/LICENSE.txt', out / 'third_party/ffmpeg/LICENSE.txt')
    (out / 'config').mkdir(exist_ok=True)
    (out / 'Jugar.cmd').write_text(
        '@echo off\r\n'
        'rem K-On! Houkago Live!! Recompiled. Idioma: el elegido en Ajustes; para forzarlo: "%~nx0" ja o "%~nx0" en\r\n'
        'setlocal\r\nset LANG_OPT=\r\n'
        'if /I "%~1"=="ja" set LANG_OPT=--language ja\r\n'
        'if /I "%~1"=="en" set LANG_OPT=--language en\r\n'
        'pushd "%~dp0"\r\n'
        f'"%~dp0{EXE_NAME}" --root "%~dp0." %LANG_OPT% --window --hold-on-stop 300000\r\n'
        'popd\r\n', encoding='ascii')
    readme = ROOT / 'README.md'
    text = readme.read_text(encoding='utf-8')
    keep = [s for s in text.split('\n## ') if s.startswith(('Mandos y teclado', 'Latencia de audio', 'Jugar online', 'Discord', 'Mods'))]
    (out / 'LEEME.txt').write_text(
        'K-On! Houkago Live!! Recompiled - para Windows\r\n\r\n'
        'Abre "Jugar.cmd" (o el .exe con doble clic). La primera vez que uses el modo\r\n'
        'local (multijugador por la red), Windows puede pedir permiso al Cortafuegos.\r\n'
        'FPS, resolucion, pantalla completa e idioma: Ajustes > fila "Video".\r\n'
        'Partidas guardadas: carpeta userdata. Registro: analysis\\native_interactive_*.log\r\n\r\n'
        + '\r\n\r\n'.join('## ' + s.strip().replace('\n', '\r\n') for s in keep) + '\r\n', encoding='utf-8')
    print(f'Installed in {out}')

if __name__ == '__main__':
    main()
