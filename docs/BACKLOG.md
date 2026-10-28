# Backlog

Tareas pendientes con identificador estable. El estado comprobado y sus
evidencias están en [ROADMAP.md](ROADMAP.md); aquí solo se organiza el trabajo.

**Quién:**
- **Cloud**: posible sin datos del juego (Claude Code en GitHub, pull request).
- **Local**: necesita el PC de desarrollo (juego instalado desde la ISO, GPU, audio).
- **Usuario**: necesita al propietario (hardware, decisiones, cuentas, publicar).

**Estado:** `pendiente` · `en curso` · `hecho (pendiente de verificar)` · `hecho`.
Una tarea de Cloud se reclama abriendo un PR en borrador con su identificador
en el título; se cierra actualizando esta tabla en el mismo PR.

## P0 — Publicación y seguridad

| ID | Tarea | Quién | Estado |
| --- | --- | --- | --- |
| SEC-1 | Exportación con auditoría, hooks locales y parche reproducible de PSPRecomp (`docs/PUBLICACION.md`) | Local | hecho (2026-10-20); falta el primer push |
| SEC-2 | Incluir en el paquete la versión exacta de FFmpeg y el enlace a su código fuente (LGPL) en `tools/make_release.py` | Cloud | hecho (2026-10-25) |
| SEC-3 | Decidir sobre el icono derivado de ilustraciones oficiales y sobre distribuir `pspdecrypt` (claves de la consola) | Usuario | pendiente |
| SEC-4 | GitHub: activar "Keep my email addresses private" y "Block command line pushes that expose my email"; protección de rama `main` (PR obligatorio, check *Publication audit*) | Usuario | pendiente |
| SEC-5 | Auditoría automática del paquete en `make_release.py` (llamar a `audit_public.py --release` y abortar si falla) | Cloud | hecho (2026-10-25) |

## P1 — Infraestructura de desarrollo y pruebas

| ID | Tarea | Quién | Estado |
| --- | --- | --- | --- |
| INF-1 | Build de Linux de las partes portables (PSPRecomp, renderer de referencia, mezclador, lógica de FPS, parsers) y sus pruebas con CMake, para CI | Cloud | pendiente |
| INF-2 | Compilación cruzada de `kon_host`/`kon_launcher` para Windows desde Linux con zig 0.15.2 (sin juego), en CI | Cloud | pendiente |
| INF-3 | Workflow de CI que ejecute INF-1/INF-2 en cada PR (solo lectura, sin publicar) | Cloud | pendiente |
| INF-4 | Tareas programadas: auditoría y pruebas locales diarias; resumen de PR abiertos | Local | pendiente |
| INF-5 | Probar una actualización real 0.1.0 → 0.1.1 desde GitHub | Local + Usuario | pendiente |

## P1 — Multiplataforma y Android ([ANDROID.md](ANDROID.md))

| ID | Tarea | Quién | Estado |
| --- | --- | --- | --- |
| AND-1 | Capa de plataforma (`src/platform/`, `docs/ANDROID.md` §2): archivos, espera, calendario, energía, bibliotecas, imágenes (PNG propio), red, audio, teclado/mandos, Discord y auriculares hechos sin cambiar Windows; quedan ventana/OSK, presentación, proceso/fallos y lanzador | Cloud (diseño y código) + Local (verificación con canción completa) | en curso (primera fase hecha, 2026-10-20) |
| AND-2 | SDK, NDK y JDK portables en `third_party/android`; toolchain CMake de Android | Local | hecho (2026-10-26): JDK 21 en `third_party/java`; SDK (`android-36`, `build-tools 36.0.0`, adb r37.0.1) y NDK 28.2.13676358 instalados en `%LOCALAPPDATA%\Programs\android-sdk`; toolchain en `cmake/android.toolchain.cmake` |
| AND-3 | Compilar `psprecomp_core` y el runtime portable para `arm64-v8a`; compilar el corpus generado para arm64 | Local | fase 0 hecha (2026-10-26): núcleo 8/8 y corpus 127/127 compilados **y enlazados** con el clang del NDK (`build/android_arm64`, sección 8 de `ANDROID.md`); probes sin ventana ejecutados en emulador x86_64; falta ejecutar arm64 en un móvil real |
| AND-4 | Arranque real del juego en Android (emulador x86_64): título, guardado, hub, canción y resultado; comparación exacta de fotogramas con Windows pendiente | Local + Usuario | parcial (2026-10-27) |
| AND-5 | App Android: superficie `ANativeWindow`, AAudio, táctil y mandos, selector de ISO (SAF), almacenamiento y Activity de juego | Cloud + Local | hecho x86_64 (2026-10-27); falta S24 arm64 |
| AND-6 | Opción A: `libkon_game.so` compilado en el PC desde la ISO/corpus y empaquetado en una APK personal (no publicada) | Local | hecho (2026-10-27): APK x86_64 probada, APK arm64 generada |
| AND-7 | Opción B (objetivo final): preparar el juego en el propio teléfono; estudio de viabilidad (compilador en el APK, tiempo, memoria, W^X) | Local | pendiente |
| AND-8 | `shaderInt64`: el renderer Android actual es CPU; Vulkan/Adreno queda para la fase de rendimiento | Local | no aplica a la primera APK jugable |
| AND-9 | SSE4.1: el runtime y el corpus compilan y ejecutan en x86_64 sin SSE específico; NEON sin medir | Cloud + Local | parcial |

## P2 — Análisis con REA

| ID | Tarea | Quién | Estado |
| --- | --- | --- | --- |
| REA-1 | Registrar REA en Claude Code con alcance local del proyecto | Local | hecho (REA 6.7.0 en `third_party/rea`, ver [REA.md](REA.md)) |
| REA-2 | WSL2 + Ghidra 12.1.3 + ghidra-allegrex v21.4 + JDK 21; proyecto en `analysis/` (privado). La copia ET_EXEC (`tools/psp_prx_to_exec.py`) ya pasa la admisión del perfil PSP de REA | Usuario (permiso WSL2) + Local | pendiente |
| REA-3 | Piloto: comparar los llamadores de `0x088EAC44`/`0x088EAC3C` con los sitios conocidos del parche de FPS | Local | pendiente |
| REA-4 | Usar REA para errores de recompilación y fidelidad (texto, efectos, pantallas sin iconos) | Local | pendiente |

## P2 — Juego y fidelidad

| ID | Tarea | Quién | Estado |
| --- | --- | --- | --- |
| G-1 | Texturas 3D ampliadas a la resolución interna | Local | pendiente: 4× probado (130,9 frente a 160,3 FPS en la canción), no aceptado; probar 2× o un umbral de tamaño |
| G-2 | Iconos de botón en pantallas no escaneadas (historia, otros modos) | Local | en curso: volcado de 40 texturas (título, carga, sala, selector, hub del álbum) y 24 badges, todos ya en la tabla; 0 nuevos. Faltan las cinco secciones del álbum, OPTION, guardado real e historia |
| G-3 | Peor tramo de 2 s a 1088p (~155 FPS) en Humming Bird y Sunday Siesta | Local | pendiente |
| G-4 | Medir Fuwa Fuwa Time | Local | pendiente |
| G-5 | Herramientas de texto, gráficos con texto y fuentes para el paquete de español | Cloud + Local | pendiente |

## Necesitan al usuario o hardware

| ID | Tarea | Estado |
| --- | --- | --- |
| U-1 | Mandos físicos distintos de Xbox (DualShock 4, DualSense, Switch Pro, genéricos) | pendiente |
| U-2 | Multijugador entre dos PC reales (LAN y online, UPnP del router) | pendiente |
| U-3 | Latencia de extremo a extremo con cámara o sensor | pendiente |
| U-4 | Revisión en NVIDIA RTX 5050 | pendiente |
| U-5 | Sesiones largas jugadas; revisión visual de la interfaz nítida | pendiente |
