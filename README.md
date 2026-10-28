# K-On! Houkago Live!! Recompiled

![icono](assets/branding/icon_1024_v2.png)

Port nativo para Windows y Android de **K-On! HÅkago Live!!** (PSP), hecho por
recompilaciÃ³n estÃ¡tica: el cÃ³digo del juego original se traduce a C++ y se
compila en el dispositivo del jugador. Las opciones avanzadas de PC pertenecen a la versiÃ³n Windows.

**Este proyecto no incluye el juego.** Ni sus datos ni su cÃ³digo: todo se crea
en tu dispositivo a partir de tu propia ISO, igual que Ship of Harkinian o los
"Recomp" de N64.

*English: native Windows and Android port of K-On! Houkago Live!! (PSP).
Bring your own ISO. Android arm64 0.2.0 is a beta with on-device preparation,
performance limitations and fewer settings than the Windows version.*

VersiÃ³n propuesta: **0.2.0 beta**. Windows y Android tienen paquetes distintos.
Consulta las [notas beta](docs/RELEASE_NOTES_0.2.0.md).

**Estado de publicaciÃ³n:** la Ãºltima release publicada es **v0.1.0** (Windows).
La 0.2.0 estÃ¡ en preparaciÃ³n. Su APK Android y sus paquetes Windows todavÃ­a no
estÃ¡n publicados: falta cerrar la revisiÃ³n de licencias y procedencia de
componentes de terceros y repetir la prueba en el dispositivo.

## Android arm64 â€” beta 0.2.0

Cuando se publique, descarga `KOn-Houkago-Live-Recompiled-0.2.0-android.apk`
desde la [release v0.2.0](../../releases/tag/v0.2.0).
Instala la APK, selecciona tu propia ISO mediante el selector de archivos de
Android (SAF), pulsa Â«Preparar juegoÂ» y, al finalizar, Â«JugarÂ».
El proceso genera y compila localmente el mÃ³dulo necesario para jugar,
sin requerir un PC ni importar un archivo `game.kon`.
No compartas la ISO ni el mÃ³dulo generado.

El equipo tÃ©cnico estima 20â€“40 minutos de preparaciÃ³n y aproximadamente 1 GB
libre; son estimaciones, no medidas verificadas en el S24 por el publicador.

El usuario comunicÃ³ que la candidata anterior funciona en su Samsung Galaxy S24,
con rendimiento mejorable. Faltan opciones/mejoras de PC en los ajustes Android.
Ese comentario no verifica la futura build corregida: requiere un nuevo test.
No se han acreditado aquÃ­ mÃ©tricas de rendimiento, audio correcto, una partida
completa ni funcionamiento en otros dispositivos. Solo se ofrece `arm64-v8a`.
Consulta [instalaciÃ³n y compatibilidad](docs/INSTALACION_COMPATIBILIDAD_0.2.md).

## Descargar y jugar en Windows

1. Descarga `K-On.Houkago.Live.Recompiled-v<versiÃ³n>-windows.zip` de la
   [Ãºltima release](../../releases/latest) y descomprÃ­melo donde quieras.
   Todo se guarda dentro de esa carpeta (partidas, ajustes, el juego
   instalado): se puede mover o copiar entera.
2. Abre `K-On Houkago Live Recompiled.exe`.
3. La primera vez te pide tu ISO (o arrÃ¡strala encima del .exe). Vale la
   japonesa original (ULJM05709) o la traducciÃ³n inglesa *K-ON! Houkago
   Live!! / After School Live!!*. Con las dos se puede cambiar el idioma en
   Ajustes (fila Â«VideoÂ»).
4. DespuÃ©s prepara el juego para tu PC: traduce y compila su cÃ³digo. Tarda
   unos minutos y solo pasa la primera vez y tras cada actualizaciÃ³n.
5. A partir de ahÃ­, abrir el .exe abre el juego directamente.

Al abrirlo mira si hay una versiÃ³n nueva en las releases de este repositorio y
pregunta si actualizar (descarga solo lo que cambia; tus partidas se
conservan). `updates=0` en `config/settings.ini` lo desactiva.

Requisitos: Windows 10 u 11 de 64 bits, una GPU con Vulkan y unos 4 GB libres.

## QuÃ© incluye (Windows)

- 30, 60, 120 y 165 FPS reales con la lÃ³gica a la velocidad original, y
  resoluciÃ³n interna hasta 1088p; interfaz 2D nÃ­tida (las texturas 3D siguen a
  resoluciÃ³n original; G-1 pendiente).
- JaponÃ©s e inglÃ©s seleccionables en Ajustes.
- Mandos de Xbox, PlayStation, Switch Pro y genÃ©ricos, con los iconos del
  botÃ³n en menÃºs, notas y ayuda (quedan pantallas por cubrir: historia y otros
  modos); teclado y reasignaciÃ³n de botones.
- Multijugador en banda por red local y **online con cÃ³digo de sala** (F8).
- Estado en Discord con la canciÃ³n que tocas.
- Mods de archivos y de texturas.

Estado: beta. Probado en AMD Radeon 780M y, en pruebas puntuales, NVIDIA RTX
5050; la revisiÃ³n final de NVIDIA estÃ¡ pendiente. Intel no se ha probado. Ver
el [roadmap](docs/ROADMAP.md) para lo comprobado y lo pendiente.

## Mandos y teclado (Windows)

Teclado: flechas, Z (âœ•), X (â—‹), A (â–¡), S (â–³), Q/W (L/R), Intro (START),
Retroceso (SELECT). F1: ayuda. F3: mostrar/ocultar FPS. F8: jugar online.
F11 o Alt+Intro: pantalla completa. Mandos: Xbox y compatibles (XInput),
DualShock 4 y DualSense, Switch Pro y mandos genÃ©ricos HID/DirectInput,
conectables en caliente. En mandos de Xbox, A y B siguen la posiciÃ³n de â—‹ y
âœ•: en japonÃ©s A acepta y B vuelve; en la traducciÃ³n inglesa, al revÃ©s.

El botÃ³n de confirmar/volver depende de la pantalla: fÃ­jate en la barra
inferior del juego (en la traducciÃ³n inglesa, Â«âœ• Enter / â—‹ BackÂ»; Z es âœ• y
X es â—‹).

Para reasignar botones, edita `config/controls.ini` (se crea comentado la
primera vez que arranca el juego): una secciÃ³n por dispositivo
(`[keyboard]`, `[xbox]`, `[playstation]`, `[nintendo]`, `[generic]`) y lÃ­neas
como `circle=K` o `cross=A`.

- [GuÃ­a completa de controles](docs/CONTROLES.md)

## Jugar online (Windows)

Pulsa **F8** dentro del juego para abrir Â«Jugar onlineÂ».

1. **TÃº:** pulsa **Crear sala**. El juego abre el puerto TCP 41700, pide al
   router por UPnP que lo redirija y muestra el cÃ³digo de la sala (por ejemplo
   `7K2QM-4XH0D`). **Copiar cÃ³digo** lo copia para pasÃ¡rselo a tu amigo.
2. **Tu amigo:** pulsa F8, escribe el cÃ³digo y pulsa **Unirse**.
3. **Los dos:** entrad en **ã¤ã†ã—ã‚“! (Local)**. Uno crea la banda como lÃ­der y
   el otro se une como miembro, igual que en la PSP.

La primera vez Windows puede pedir permiso al Cortafuegos: acÃ©ptalo. No hace
falta ningÃºn servidor: los datos van directamente entre los PC (el de quien
crea la sala hace de punto de encuentro, hasta 5 jugadores).

Si el router no abre el puerto (UPnP desactivado) o tu proveedor comparte la
IP pÃºblica (CG-NAT), la ventana lo indica y ofrece alternativas:
- **CÃ³digo IPv6:** si los dos tenÃ©is IPv6.
- **CÃ³digo de red local:** sirve en la misma red; si usÃ¡is una VPN de juego
  (Radmin VPN, ZeroTier, Tailscale), usad el cÃ³digo que la ventana F8 muestra
  con el nombre de esa VPN.
- **Abrir el puerto a mano:** el TCP 41700 en el router.

## Discord (Windows)

Con Discord abierto en el PC, tu estado muestra que juegas a K-On! Houkago
Live!!, la canciÃ³n que tocas, la parte, si estÃ¡s en solitario o con la banda,
y los resultados. No hay que configurar nada. Para desactivarlo: `discord=0`
en `config/settings.ini`. En Discord tiene que estar activado *Ajustes de
usuario â†’ Privacidad de actividad â†’ Compartir tu actividad detectada*.

## Latencia de audio (Windows)

El sonido sale con unos 46 ms de bÃºfer. Si se oyen cortes, sube
`audio_block_ms` en `config/settings.ini` (por ejemplo `audio_block_ms=46`);
para menos latencia, `audio_block_ms=12`.

## Mods (Windows)

Cada mod es una carpeta dentro de `mods/` junto al juego:

- `mods/<nombre>/disc/<ruta del disco>` sustituye ese archivo del disco
  (por ejemplo `disc/PSP_GAME/USRDIR/std.cpk`);
- `mods/<nombre>/textures/<hash>.png` (o `.bmp`) sustituye una textura del
  mismo tamaÃ±o. Los hashes se obtienen ejecutando el juego con
  `KON_TEXTURE_DUMP=work/texdump`, que guarda cada textura como
  `<hash>_<ancho>x<alto>.bmp`.

Las carpetas se aplican por orden de nombre y la Ãºltima gana (usa prefijos:
`10-...`, `20-...`). Para desactivar un mod, pon `-` delante de su nombre.

## Problemas (Windows)

- Si el juego se cuelga, deja `ultimo_fallo.txt` en su carpeta: adjÃºntalo en
  un [issue](../../issues).
- Si falla la preparaciÃ³n, mira `recomp_cache\compilacion.log`.
- Para reconstruir el juego desde cero, borra la carpeta `recomp_cache`.

## CÃ³mo funciona

- `src/launcher.cpp`: el programa que abre el jugador. Instala la ISO
  (`src/native_install.hpp`), construye el juego (`src/native_build.hpp`) y
  lo actualiza (`src/native_update.hpp`).
- [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) con el parche
  `patches/psprecomp/kon-port.patch`: traduce el ejecutable de la PSP a C++.
  `tools/apply_logic_rate_patch.py` hace que la lÃ³gica funcione a cualquier FPS.
- `src/boot_probe.cpp` y `src/native_*`: el runtime del port (sistema de la
  PSP, grÃ¡ficos Vulkan, audio, mandos, red, Discord).
- `tools/make_release.py`: crea los paquetes de una release.

Compilar desde el cÃ³digo requiere las herramientas descritas en
[docs/TOOLCHAIN.md](docs/TOOLCHAIN.md) y [docs/PROCEDENCIA.md](docs/PROCEDENCIA.md)
(zig 0.15.2, CMake, Ninja, cabeceras de Vulkan, FFmpeg, pspdecrypt) y tu
propia ISO:

```powershell
python tools\psprecomp_patch.py fetch
.\tools\build_framework.ps1 -Jobs 8
.\tools\generate_native_code.ps1 -Plain
.\tools\build_native_probe.ps1 -FullAot -Jobs 8
python tools\make_release.py
```

Contribuir: los cambios llegan por pull request; antes de cada commit,
`python tools/audit_public.py --tree .` (nada del juego ni datos personales).
Tareas abiertas en [docs/BACKLOG.md](docs/BACKLOG.md); proceso de publicaciÃ³n
en [docs/PUBLICACION.md](docs/PUBLICACION.md).

- [Roadmap](docs/ROADMAP.md) Â· [Plan del port](docs/PLAN_DEL_PORT.md) Â·
  [Renderizado Vulkan](docs/RENDERIZADO_VULKAN.md) Â·
  [Formatos de recursos](docs/ASSET_FORMATS.md) Â·
  [Paquetes de idioma](docs/PAQUETES_DE_IDIOMA.md)

## Licencia

El cÃ³digo propio del port usa [MIT](LICENSE). Los componentes de terceros conservan sus licencias y avisos respectivos; MIT no sustituye GPL/LGPL ni otras obligaciones.
Componentes de terceros: PSPRecomp (MIT), zig (MIT), CPython (PSF), FFmpeg
(LGPL), pspdecrypt (GPLv3). `pspdecrypt` se obtiene por separado: este
repositorio no incluye su binario ni su fuente.

K-On! y Houkago Live!! son marcas y obras de sus respectivos propietarios.
Este es un proyecto de aficionados sin relaciÃ³n con ellos y no incluye ningÃºn
contenido del juego: necesitas tu propia copia.
