# Historial de versiones

Cambios de cada versión. Solo las entradas marcadas como publicadas tienen una
release en GitHub; el estado comprobado y sus evidencias están en
[ROADMAP.md](ROADMAP.md).

## 0.2.0 — en preparación (no publicada)

- Android arm64: preparación del juego en el dispositivo desde la ISO propia,
  sin PC ni `game.kon`. Una candidata anterior fue probada por el usuario en un
  Samsung Galaxy S24: funciona con rendimiento mejorable y menos opciones que
  la versión Windows. La APK y sus fuentes correspondientes están retenidas
  hasta cerrar la revisión de licencias y procedencia de terceros.
- Glifos: CHECK! conserva su aro, la X del cartel «PUSH» queda dentro del
  círculo y los badges se centran; las notas no cambian.
- Menú de canciones: espera corregida. El contador `[+64] = 1×fps` de
  `0x0886B128` se decrementaba con `kon_logic_step30`; ahora usa 30, así que el
  tramo «PLEASE WAIT» dura ~1 s a cualquier tasa (a 165: lista a 44,5 s frente
  a 48,5 s antes).
- Actualizaciones: diálogo propio en el lanzador de Windows (`--check-updates`)
  con descarga e instalación en hilo aparte, y menú Android con verificación
  SHA-256/firma y `PackageInstaller`. Partidas y ajustes se conservan.

No hay release `v0.2.0`. Los paquetes Windows y la APK no se publican hasta
completar la auditoría del conjunto exacto.

## 0.1.1 — no publicada

Cambios de mantenimiento preparados; no se creó una release `v0.1.1`. Parte de
estos cambios se incorpora a la rama 0.2.

- Empaquetado y seguridad: el paquete incluye la versión exacta de FFmpeg y el
  enlace a su código fuente (LGPL), y `tools/make_release.py` ejecuta
  automáticamente `tools/audit_public.py --release` sobre la carpeta y los dos
  ZIP, abortando si falla.
- Capa de plataforma para Android (archivos, esperas, calendario, energía,
  bibliotecas, imágenes, red, audio, teclado, mandos, Discord y auriculares).
- Ayuda F1 rediseñada: el panel y la fuente escalan con la resolución interna,
  con texto ajustado y legible de 272p a 1088p (antes eran 5 px a 1088p).
- Ayuda F1 y guía de controles aclaran que el botón de confirmar/volver lo
  decide cada pantalla: en la traducción inglesa ✕ (Z) confirma y ○ (X) vuelve.
- Ventana online: avisos al crear una sala sin red o con el puerto ocupado, al
  copiar sin sala y al unirse con el campo vacío; errores de conexión y router
  en español y ventana más amplia para los códigos.
- Lanzador, instalación y actualización: los errores dicen qué hacer (ISO
  correcta, paquete completo, espacio en disco, reinicio manual).
- Parche reproducible de PSPRecomp (`tools/psprecomp_patch.py`) y herramientas
  de publicación (auditoría, exportación e importación segura).
- Base de REA y pruebas de portabilidad a aarch64.

## 0.1.0 — 10 de octubre

- Primera release pública.
- Modelo «trae tu ISO»: el paquete no incluye datos ni código del juego; el
  lanzador instala la ISO del jugador y prepara el juego en su PC.
