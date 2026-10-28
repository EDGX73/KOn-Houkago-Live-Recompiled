"""Release package: the port without any of the game (no data and no code).
The player's own ISO is installed, and the game's code is translated and
compiled on the player's PC by the launcher (src/launcher.cpp,
src/native_build.hpp), the way ROM-based PC ports work.

Usage: python tools/make_release.py [--out dist] [--no-zip]
Run after building (tools/build_native_probe.ps1 -FullAot, or the
kon_launcher and kon_host targets).

Writes dist/release/K-On Houkago Live Recompiled/ and two zips:
  K-On.Houkago.Live.Recompiled-v<version>-windows.zip        complete (new players)
  manifest.json (also inside the package), the three files of a GitHub release
  K-On.Houkago.Live.Recompiled-v<version>-actualizacion.zip  without the
      compiler and Python (recomp/zig, recomp/python): what the launcher's
      updater downloads when the toolchain did not change (manifest.json).

Package contents:
  K-On Houkago Live Recompiled.exe   the only program to open (installs, builds, plays)
  LEEME.txt / README.txt
  manifest.json                       version and toolchain identity (updater)
  recomp/                             toolkit to build the game from the ISO:
    psp_recomp.exe, patches/*.py, include/ (PSPRecomp, MIT), lib/*.a (the port),
    app.res (icon), zig/ (compiler, MIT), python/ (embeddable CPython, PSF),
    kit_id.txt
  tools/pspdecrypt.exe                decrypts the disc's EBOOT (GPLv3)
  third_party/ffmpeg                  FFmpeg libraries (LGPL) and licence
  config/
"""
import argparse, hashlib, json, re, shutil, subprocess, sys, zipfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/native_full'
ZIG = ROOT / 'third_party/toolchain/zig-x86_64-windows-0.15.2'
ZIG_VERSION = '0.15.2'
PYTHON = ROOT / 'third_party/python_embed/py'
PYTHON_VERSION = '3.12.10'
# Exact LGPL libraries shipped in third_party/ffmpeg/bin (SEC-2): version and
# corresponding source, so the package carries the offer the licence requires.
FFMPEG_VERSION = 'n8.1.3-14-g330caae0c1-20261003'
FFMPEG_COMMIT = '330caae0c1acccd2222edc52a05940c574561ce5'
FFMPEG_SOURCE = f'https://github.com/FFmpeg/FFmpeg/tree/{FFMPEG_COMMIT}'
FFMPEG_BUILD = 'https://github.com/BtbN/FFmpeg-Builds'
NAME = 'K-On Houkago Live Recompiled'

README = '''K-On! Houkago Live!! Recompiled {version} (Windows)

Este paquete NO incluye el juego: se crea en tu PC a partir de tu propia ISO
de PSP, como Ship of Harkinian o los "Recomp" de N64.

INSTALAR Y JUGAR
1. Descomprime esta carpeta donde quieras. Todo se guarda dentro de ella
   (partidas, ajustes, el juego instalado): puedes moverla o copiarla entera.
   Ocupa unos 3-4 GB tras instalar.
2. Abre "K-On Houkago Live Recompiled.exe".
3. La primera vez te pide tu ISO (o arrastrala encima del .exe). Vale la
   japonesa original (ULJM05709) o la traduccion inglesa
   "K-ON! Houkago Live!! / After School Live!!". Con las dos se puede cambiar
   el idioma en Ajustes (fila Video): para anadir la otra, arrastrala encima
   del .exe.
4. Despues prepara el juego para tu PC (traduce y compila su codigo): tarda
   unos minutos y solo pasa la primera vez y tras cada actualizacion.
5. A partir de ahi, abrir el .exe abre el juego directamente.

ACTUALIZACIONES
Al abrirlo mira si hay una version nueva en GitHub y te pregunta si
actualizar. Tus partidas y ajustes se conservan.

JUGAR ONLINE CON UN AMIGO
1. Uno pulsa F8 dentro del juego y "Crear sala". Sale un codigo de sala;
   "Copiar codigo" lo copia para mandarselo al otro.
2. El otro pulsa F8, escribe el codigo y pulsa "Unirse".
3. Los dos entrais en tsuushin! (Local, el icono de la antena) en el menu del
   club: uno crea la banda como lider y el otro se une como miembro.
La primera vez Windows puede preguntar por el Cortafuegos: dale a Permitir.
Si el router de quien crea la sala no abre el puerto (lo explica la ventana F8),
podeis usar el codigo IPv6, una VPN de juego (Radmin VPN, ZeroTier, Tailscale)
con el codigo de red local, o abrir a mano el puerto TCP 41700 en su router.

CONTROLES (F1 los muestra en el juego)
Teclado: flechas, X = Circulo, Z = Cruz, A = Cuadrado, S = Triangulo, Q/W = L/R,
Intro = START, Retroceso = SELECT. Mandos de Xbox, PlayStation, Switch Pro y
genericos funcionan conectandolos. F11: pantalla completa. F8: jugar online.
F3: mostrar u ocultar los FPS.
FPS, resolucion, pantalla completa e idioma: Ajustes > fila "Video".

PROBLEMAS
- Si el juego se cuelga, deja ultimo_fallo.txt en esta carpeta: mandalo.
- Si falla la preparacion, mira recomp_cache\\compilacion.log.
- Para reconstruir el juego desde cero, borra la carpeta recomp_cache.

LICENCIAS
recomp\\zig: MIT. recomp\\python: PSF. recomp\\include: PSPRecomp (MIT).
tools\\pspdecrypt.exe: GPLv3 (codigo fuente en su LEEME).
third_party\\ffmpeg: LGPL v3. Version exacta {ffmpeg} (compilacion BtbN
FFmpeg-Builds, win64-lgpl-shared). Codigo fuente correspondiente:
{ffmpeg_source}
K-On! y Houkago Live!! son de sus propietarios; este proyecto no tiene relacion
con ellos y no incluye ningun contenido del juego.
'''

def version():
    text = (ROOT / 'src/kon_version.h').read_text(encoding='utf-8')
    return re.search(r'#define KON_VERSION_STRING "([^"]+)"', text).group(1)

def copy_zig(to):
    """The compiler without the C libraries of other systems (Linux, macOS...)."""
    def ignore(folder, names):
        rel = Path(folder).relative_to(ZIG).as_posix()
        skip = set()
        if rel == '.': skip |= {'doc', 'README.md'}
        if rel == 'lib': skip |= {'libtsan', 'docs', 'build-web', 'init', 'fuzzer.zig'}
        if rel == 'lib/libc': skip |= {n for n in names if n not in ('mingw', 'include')}
        if rel == 'lib/libc/include': skip |= {n for n in names if n != 'any-windows-any'}
        return skip
    shutil.copytree(ZIG, to, ignore=ignore)
    (to / 'version.txt').write_text(ZIG_VERSION, encoding='ascii')

def tree_hash(paths):
    h = hashlib.sha256()
    for base in paths:
        files = [base] if base.is_file() else sorted(p for p in base.rglob('*') if p.is_file())
        for f in files:
            h.update(f.name.encode()); h.update(hashlib.sha256(f.read_bytes()).digest())
    return h.hexdigest()

def write_zip(archive, out, skip=()):
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(out.rglob('*')):
            rel = f.relative_to(out)
            if f.is_file() and not any(rel.as_posix().startswith(s) for s in skip):
                z.write(f, Path(out.name) / rel)
    print(f'Zip: {archive.name} ({archive.stat().st_size // 1048576} MB)')

def audit_release(paths):
    """Nothing is releasable until the publication audit passes (SEC-5)."""
    audit = ROOT / 'tools/audit_public.py'
    for path in paths:
        print(f'Publication audit: {path}')
        result = subprocess.run([sys.executable, str(audit), '--release', str(path), '--quiet'], cwd=ROOT)
        if result.returncode != 0:
            raise SystemExit(f'Publication audit FAILED for {path}; the package is not releasable.')

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--out', type=Path, default=ROOT / 'dist/release')
    p.add_argument('--no-zip', action='store_true')
    a = p.parse_args()
    ver = version()
    out = (a.out / NAME).resolve()
    if out.exists():
        shutil.rmtree(out)  # only ever a previous output of this script (dist/release)
    out.mkdir(parents=True)
    shutil.copy2(BUILD / 'kon_launcher.exe', out / f'{NAME}.exe')
    kit = out / 'recomp'
    (kit / 'lib').mkdir(parents=True)
    for lib in ('libkon_host.a', 'libkon_native_media.a', 'libkon_native_vulkan.a'):
        shutil.copy2(BUILD / lib, kit / 'lib' / lib)
    shutil.copy2(ROOT / 'build/framework/libpsprecomp_core.a', kit / 'lib/libpsprecomp_core.a')
    shutil.copy2(ROOT / 'build/framework/psp_recomp.exe', kit / 'psp_recomp.exe')
    shutil.copytree(ROOT / 'third_party/PSPRecomp/include', kit / 'include')
    shutil.copy2(ROOT / 'third_party/PSPRecomp/LICENSE', kit / 'include/PSPRecomp-LICENSE.txt')
    (kit / 'patches').mkdir()
    shutil.copy2(ROOT / 'tools/apply_logic_rate_patch.py', kit / 'patches/apply_logic_rate_patch.py')
    shutil.copy2(BUILD / 'app.res', kit / 'app.res')
    copy_zig(kit / 'zig')
    shutil.copytree(PYTHON, kit / 'python')
    (kit / 'kit_id.txt').write_text(ver + ' ' + tree_hash([kit / 'lib', kit / 'psp_recomp.exe', kit / 'include', kit / 'patches', kit / 'app.res', kit / 'zig/version.txt']), encoding='ascii')
    (out / 'tools').mkdir()
    shutil.copy2(ROOT / 'third_party/pspdecrypt-bin/pspdecrypt.exe', out / 'tools/pspdecrypt.exe')
    shutil.copy2(ROOT / 'third_party/pspdecrypt-bin/Readme.md', out / 'tools/pspdecrypt-LEEME.md')
    with open(out / 'tools/pspdecrypt-LEEME.md', 'a', encoding='utf-8') as f:
        f.write('\n\nGPLv3. Source code: https://github.com/John-K/pspdecrypt (release 1.0).\n')
    media = out / 'third_party/ffmpeg/bin'
    media.mkdir(parents=True)
    for dll in ('avutil-60.dll', 'avcodec-62.dll', 'avformat-62.dll', 'swresample-6.dll'):
        shutil.copy2(ROOT / 'third_party/ffmpeg/bin' / dll, media / dll)
    shutil.copy2(ROOT / 'third_party/ffmpeg/LICENSE.txt', out / 'third_party/ffmpeg/LICENSE.txt')
    (out / 'third_party/ffmpeg/README.txt').write_text(
        'FFmpeg shared libraries used by K-On! Houkago Live!! Recompiled.\n\n'
        f'Version: {FFMPEG_VERSION} (8.1 branch)\n'
        'Libraries: avutil-60, avcodec-62, avformat-62, swresample-6\n'
        'Binary build: BtbN FFmpeg-Builds, ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip\n'
        f'Build scripts: {FFMPEG_BUILD}\n'
        'Licence: LGPL v3 (see LICENSE.txt in this folder).\n\n'
        'Corresponding source code of these libraries:\n'
        f'  {FFMPEG_SOURCE}\n', encoding='utf-8')
    (out / 'config').mkdir()
    (out / 'LEEME.txt').write_text(README.format(version=ver, ffmpeg=FFMPEG_VERSION,
                                                 ffmpeg_source=FFMPEG_SOURCE).replace('\n', '\r\n'), encoding='utf-8')
    manifest = {'version': ver, 'toolchain': f'zig {ZIG_VERSION}; python {PYTHON_VERSION}'}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'Package in {out} (version {ver})')
    audited = [out]
    if not a.no_zip:
        stem = f'K-On.Houkago.Live.Recompiled-v{ver}'
        full_zip = a.out / f'{stem}-windows.zip'
        update_zip = a.out / f'{stem}-actualizacion.zip'
        write_zip(full_zip, out)
        write_zip(update_zip, out, skip=('recomp/zig/', 'recomp/python/'))
        audited += [full_zip, update_zip]
        # Uploaded with the zips: the launcher compares its toolchain with the installed one.
        shutil.copy2(out / 'manifest.json', a.out / 'manifest.json')
    # SEC-5: the package is not releasable if the publication audit finds anything.
    audit_release(audited)

if __name__ == '__main__':
    main()
