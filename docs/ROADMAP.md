# Roadmap de K-On! Hōkago Live!! para PC

Actualizado: 2026-10-08 (sesión de Claude).

**Objetivo:** el juego original completo como port nativo de Windows. Un único `.exe` con selección automática de GPU (AMD Radeon 780M y NVIDIA RTX 5050), japonés e inglés seleccionables en Ajustes, estructura para añadir español y otros idiomas sin recompilar, resolución interna superior, FPS elegibles (30/60/120/165/personalizado) sin acelerar el juego, mando y teclado configurables, y mods de recursos sin editar las ISO.

Una casilla se marca solo cuando su criterio está comprobado con evidencia (binario, GPU, guardado, entradas y tramo medido). Las pruebas sintéticas no sustituyen a una canción completa jugada.

---

## Plan de ejecución actual (orden de trabajo)

### 1. Vulkan rápido — COMPLETADO

Hecho y comprobado en esta sesión:

- [x] ~~Lotes GPU: leer solo la parte nueva del destino en lugar del framebuffer completo en cada dibujo.~~
- [x] ~~Binning de teselas sin reservas de memoria por dibujo.~~
- [x] ~~Lote completo en un solo dispatch por teselas (sin barreras entre dibujos).~~
- [x] ~~Shader sin Float64: interpolación entera idéntica en CPU y GPU (`src/native_fixed_raster.hpp`). Ya no hace falta soporte Float64 en la GPU.~~
- [x] ~~Envío asíncrono de lotes: la CPU no espera a la GPU salvo que el juego o la pantalla lean esa VRAM.~~
- [x] ~~Caché de texturas por épocas y tokens de contenido (sin comparar texturas enteras en cada dibujo).~~
- [x] ~~Clears en CPU con lotes residentes (el clear GPU síncrono costaba ~2 ms por fotograma).~~
- [x] ~~Presentación GDI solo con imagen nueva; bombeo de mensajes limitado a 1 ms.~~
- [x] ~~Herramientas: perfilador por muestreo (`--sample-profile`), traza por fotograma (`--trace-frames`), captura de fotogramas reales (`--dump-render-frames`) y benchmark offline exacto (`native_frame_replay_benchmark`).~~

Pendiente para cerrar el punto 1:

- [x] ~~Canción estable a 30 FPS en AMD: 29,6/29,7 FPS y 299 de 299 fotogramas trazados dentro de dos vblanks (opciones por defecto).~~
- [x] ~~Ruta rápida por defecto sin opciones (Vulkan residente con GPU automática y CPU de respaldo) y accesos `.cmd` actualizados.~~
- [x] ~~Ventana, mensajes y pintado en un hilo de presentación propio.~~
- [x] ~~Suites de exactitud en AMD y NVIDIA con el mismo binario.~~
- [x] ~~Presentación por swapchain Vulkan con GDI de respaldo: activa en AMD (`presenter=vulkan:AMD Radeon 780M Graphics`, canción 29,1/29,5 FPS) y en NVIDIA (menús 27,7–30 FPS).~~ Pendiente solo la confirmación visual del usuario.
- [x] ~~Reproducción privada del fotograma real (base del punto 3): 28 ms → 7,6–8,8 ms por fotograma de canción, exacta frente al framebuffer original (4 fotogramas verificados en el juego, 0 rechazados).~~

**Punto 1 completado** con el binario `baceff5a`: suites de exactitud en AMD y NVIDIA, prueba hardware del worker y 35 pruebas generales aprobadas.

### 2. FPS reales más altos (60 y superiores) y menos input lag — COMPLETADO

**Punto 2 completado** (6 oct., AMD Radeon 780M, binario de `2066cc6`): 30/60/120/165 FPS reales elegibles en Ajustes, sin interpolación, con el juego a su velocidad original en canciones, menús, sala del club, escenas y cargas. Comprobado: canción a 120,0 y 165,0 FPS (también con batería: 120,0 y 164,6); sesión de 4 min a 165 en la sala del club con media de 165,0 FPS (mínimo 163,7 por segundo, ninguna ventana por debajo de 150) sin errores; fila FPS de Ajustes de punta a punta (30 → 60 → 120 → 60, guardado `logic_fps=60`). El usuario probó dos canciones más y van bien, y no ha notado desgarro. Lo que queda fuera de este punto y pasa a otros:
  - Juicio y puntuación con una canción completa jugada de verdad → punto 4 (las entradas sintéticas son una hipótesis de tiempos: a 30 y a 120 caen distinto y no sirven para validar puntuación).
  - Latencia de extremo a extremo con cámara o sensor (necesita hardware externo) y NVIDIA (aplazada por decisión del usuario) → punto 7.
  - Recorrido largo del modo historia y de los demás modos a 120/165 → puntos 6 y 7 (escena de historia, menús y selección ya auditados).

Requisito del usuario: **solo fotogramas reales**, calculados por el propio juego. No se usan interpolación ni extrapolación en el modo normal. El juego original calcula 30 fotogramas por segundo (`sceDisplayWaitVblankStartMulti(2)`).

- [x] ~~Caracterizar la temporización del juego.~~ Comprobado (2026-10-06, AMD, semilla fija `work/test_seed_hotchkiss_20261005`):
  - Toda la lógica avanza por fotograma; nada se basa en el reloj del audio. El bucle principal (0x088F165C) espera `60/fps` vblanks; el campo fps del motor (+3944) es 30 en todas las escenas medidas (`vblank_waits`).
  - El motor cuenta el tiempo en ticks de 1/60 s y cada fotograma pregunta a **0x088EAC44** (`jr ra; li v0,2`) cuántos ticks pasan: un 2 fijo, usado desde 30 sitios (reloj de la canción, etc.).
  - Quedan contadores que suman +1 por fotograma sin pasar por esa función (animaciones de personajes, cámara, efectos de texto).
- [x] ~~Experimento de lógica a 60 Hz en bruto~~ (`--logic-vblank-cycles 1`): 59,4–60 FPS reales en todos los tramos, pero todo va al doble de velocidad y la canción se desincroniza (Game Over a los 4 s).
- [x] ~~**Parche de 60 FPS**~~ (sustituido por el sistema por tiempo de juego de 120/165, que también da 60): `tools/apply_logic_rate_patch.py` (idempotente, se aplica al regenerar el código) y `--logic-fps 60` (lanzador `Ultima version 60 FPS <fecha>.cmd`):
  - 0x088EAC44 (ticks por fotograma) devuelve `kon_logic_frame_ticks` (2 → 1) y 0x088EAC3C (FPS lógicos) devuelve `60/ticks` (30 → 60).
  - 9 llamadas que usan los ticks para convertir duraciones a fotogramas clave de 30 FPS mantienen el 2 (cámara/escenario, animaciones con objetivo).
  - 19 temporizadores ±1 por fotograma (cuentas atrás de intro/menús, estados de las chicas, contadores globales) y el fotograma de las animaciones 2D avanzan solo uno de cada dos fotogramas a 60 (`kon_logic_half_frame`). Encontrados de forma sistemática: `tools/scan_frame_counters.py` (501 candidatos) → `tools/instrument_frame_counters.py` → `tools/classify_frame_counters.py` (21 por fotograma; 2 descartados por ser recuentos de referencias).
  - Medido en AMD (semilla fija, entradas sintéticas): 59,4–60 FPS reales en todos los tramos; la canción empieza a 119,46 s frente a 119,51 s a 30 (antes del parche, 98,8 s); notas, línea de juicio, cámara y animaciones de las chicas coinciden con 30 FPS en los mismos instantes del audio. El modo 30 queda idéntico (119,51 s, 29,3 FPS).
  - Suavizados `x += (objetivo-x)/2` por fotograma: a 60 usan el factor 1-√½ (`kon_smooth_step`), misma convergencia por segundo.
  - Comprobación final en la canción (mismo tiempo de juego, 8 fotogramas a 30 frente a 16 a 60): 34 de 36 enteros y 1303 de 1480 floats con avance constante van a la misma velocidad; los 2 enteros restantes siguen el tiempo real del audio (no son fallos) y los 9 floats dudosos son matrices de huesos sueltas.
  - Las animaciones 2D de interfaz son tablas con un registro por fotograma a 30 por segundo (sin fotogramas clave que interpolar): en el modo 60 se muestran con su cadencia original de 30. Mostrar más sería inventar fotogramas.
  - [x] ~~Fundido del logo: medido con capturas cada 100 ms (6 oct.). Las animaciones y fundidos duran lo mismo a 30 y a 120 (intro del título 0,8/0,3/0,4/0,2 s frente a 0,6/0,3/0,4/0,2 s); lo que se acorta son las cargas (negro antes del logo 1,0 → 0,2 s; logo, que se muestra mientras carga, 4,2 → 3,3 s; negro antes del guardado 2,4 → 0,7 s): el juego carga un elemento por fotograma (0x089EFEA4), así que a más FPS carga antes. Es carga más rápida, no juego más rápido.~~
  - [x] ~~Usos de 0x088EAC3C (FPS lógicos) revisados uno a uno: 66 llamadas; 40 convierten segundos en fotogramas (× fps) y 11 ritmos por segundo en pasos por fotograma (÷ fps), correctos porque se aplican por fotograma; el resto son comparaciones y movimientos. Un fallo: la espera mínima de la pantalla de carga ([+56] = 3 × fps en 0x0896B1E4/B980/BC80) se contaba en fotogramas de 30 y a 120/165 duraba 12–16,5 s (o casi nada si la carga hacía bajar los FPS en ese momento). Esos tres sitios usan ahora 30: 3 s a cualquier tasa.~~
  - Juicio y puntuación con entradas reales → punto 4.
- Herramientas: `--ram-snapshots dir start_ms count` + `tools/find_frame_steps.py` (valores que avanzan un paso fijo por fotograma) y `PSPRECOMP_WATCH_WRITE=<dir>` con pila nativa resuelta a líneas del código generado (`native_watch_stack`).
- [x] ~~«Parche de 60 FPS» propio~~: hecho con el sistema por tiempo de juego (`tools/apply_logic_rate_patch.py` + `src/native_logic_rate.hpp`); es una opción (fila FPS de Ajustes) y el modo 30 queda idéntico al original (canción a 119,50–119,52 s, fotogramas verificados).
- [x] ~~120 y 165 FPS reales con delta time~~ (`src/native_logic_rate.hpp`, prueba `native_logic_rate_test`): cada fotograma avanza el tiempo real transcurrido (ticks enteros acumulados, ticks fraccionarios para animaciones y pasos, fotogramas de 30 para temporizadores, FPS y factor de suavizado). Ritmo por plazo: si un fotograma llega tarde, el siguiente empieza sin esperar otro refresco; la imagen se presenta al terminar cada fotograma. El mando se muestrea una vez por fotograma lógico (antes limitaba a 90 FPS). Medido en AMD: menús 118–120 (120) y 160–164 (165); la canción empieza a 119,44 s a 120 frente a 119,51 s a 30 y coincide visualmente; en la canción la 780M da ~76–100 FPS a 120 (la velocidad sigue siendo correcta). A 165 las pantallas de carga duran algo más (el hilo principal deja menos CPU a la carga).
- [x] ~~Selector de FPS dentro del menú de Ajustes del juego~~ (`src/native_settings_fps.hpp`): la fila «Install» (instalación en Memory Stick, sin uso en PC) se dibuja como «FPS» con 30/60/120/165, con los dígitos y la fuente de ayuda del propio juego; Izquierda/Derecha cambian la tasa al instante (esos botones y Círculo no llegan al juego en esa fila, así que nunca se inicia la instalación de PSP) y se guarda en `config/settings.ini` (`logic_fps`). Comprobado en inglés y japonés (en japonés la ayuda de esa fila está en inglés).
- [x] ~~Captura con START (sala del club): faltaban `sceIoGetstat` y la utilidad de captura y el juego se detenía; ahora guarda `PICTURE/ULJM05709/SCRNxxxx.BMP`.~~
- [x] ~~120/165 completos en la canción~~ (6 oct., medido con batería y plan «silencioso»; los FPS absolutos varían mucho entre ejecuciones, así que se comparan proporciones del perfilador):
  - [x] ~~Pruebas ocultas: `--silent-test` (ventana fuera de pantalla, sin foco ni barra de tareas; audio a cero con el reloj de muestras intacto) es lo predeterminado en `tools/run_native_amd_probe.py` (`--visible` para verlo).~~
  - [x] ~~Perfilador con pilas de llamadas (`--sample-profile`: secciones hoja, inclusivo y pilas) y columnas `main_thread_cycles`/`gpu_collect_wait_us` en `--profile-frames`.~~
  - [x] ~~`sceWlanGetEtherAddr` se llamaba cada fotograma y consultaba los adaptadores de Windows: ~14 % del hilo principal en la canción. Ahora se consulta una vez (0 % en el perfil siguiente). La comprobación de Memory Stick tampoco toca el disco cada fotograma.~~
  - [x] ~~El `memcmp` de la biblioteca de C de zig/mingw compara byte a byte (0,63 ms/MB frente a 0,08 ms/MB por palabras de 64 bits, también detrás de `std::equal`): validación de texturas, paletas y lote GPU usan `kon::bytes_equal` (`src/native_bytes.hpp`). Una textura con token nuevo ya no se compara con todas las del lote del mismo tamaño (~5 % del hilo principal).~~
  - [x] ~~Lectura de listas GE por la ruta rápida de memoria; conversión del framebuffer de 16 bits por tabla.~~
  - [x] ~~Modos PC: un cambio de framebuffer inmediato (el juego usa sync 0) se muestra al final de su fotograma (el refresco de 60 Hz no vuelve a leer el framebuffer a mitad de dibujo). Aplazarlo al fotograma siguiente cuando va tarde no dio más FPS una vez CPU y GPU van en paralelo (56,8 frente a 58,0) y añadía hasta un fotograma de retraso: retirado. El modo 30 no cambia.~~
  - [x] ~~El bloque de la fila FPS de Ajustes vive en RAM de núcleo (0x08400000, fuera de la partición del juego): el juego vuelve a recibir los mismos UID y direcciones de pila que en PSP (`native_game_initialization` volvía a fallar desde el selector de FPS). Pendiente: repetir la prueba del menú de punta a punta (con batería las entradas temporizadas se desfasan ±10 s).~~
  - Resultado: en la parte jugada de la canción no se puede afirmar una mejora de FPS con batería (versión anterior 35–48, nueva 23–53 en la misma ventana). Las proporciones sí cambiaron: la red pasó de ~14 % a 0 %, las comparaciones de lote de ~5 % a 0 %.
  - [x] ~~CPU y GPU en paralelo: el backend Vulkan tiene dos zonas de staging (comando, fence y marcas de tiempo propios), así que el lote del fotograma siguiente se graba mientras la GPU sombrea el anterior. Un dibujo solo espera al lote en vuelo si toca sus salidas o va por otra ruta; el borrado de profundidad del juego (8 franjas de 64 px) cubre todo lo que escribió el lote y anula su profundidad en lugar de esperarla. Medido (120, batería, segundos 1–9 de la canción): espera a la GPU 3,3 → 0,3 ms por fotograma; 48,7–49,4 → 58,7 FPS. Exactitud: 4/4 fotogramas reales verificados durante la canción (124 s) y 4/4 en la transición a resultados (130,5 s); `native_vulkan_raster_test` y las 36 pruebas pasan. `KON_NO_GPU_OVERLAP=1` lo desactiva para comparar; `overlap_waits=` en el registro cuenta los motivos de espera.~~
  - [x] ~~Un vértice no finito ya no detiene el juego: sus triángulos se descartan como en la PSP (`nonfinite_triangles=` en el registro). Se vio una vez (matriz de mundo con NaN enviada por el juego al reiniciar la canción a 120, con verificación de fotogramas activa) y no se ha repetido en 4 ejecuciones del mismo recorrido.~~
  - [x] ~~La ruta GPU ya no calcula ni valida dos veces el triángulo en punto fijo (el lote lo rehace desde el trabajo): ~3 % del hilo principal en la canción.~~
  - [x] ~~Geometría en paralelo: los dibujos del lote GPU se encolan; hilos auxiliares construyen su geometría (vértices, transformación, recorte, triángulos) y validan y codifican los trabajos GPU; se graban en orden. Prueba nueva: 1100 dibujos reales de la canción por la cola, idénticos a la CPU.~~
  - [x] ~~Accesos del código recompilado al scratchpad de la PSP por la ruta directa (antes resolvían byte a byte: ~7 % del hilo principal).~~
  - [x] ~~Caché persistente de texturas en la GPU por token de decodificación (antes ~5 MB por fotograma copiados a cada lote).~~
  - [x] ~~Hilo de render asíncrono: el hilo del juego solo encola el trabajo GE (dibujos con sus vértices copiados y su textura fijada, borrados con sus vértices, inicio/envío de lote y presentación); un hilo de render lo ejecuta en orden mientras el juego sigue. Lo que necesita resultados (otros dibujos, VRAM tocada por el juego —~1 vez por fotograma—, contadores) espera antes. La presentación espera a su lote sin bloquear la grabación; como mucho dos fotogramas esperan pantalla. `KON_NO_ASYNC_RENDER`/`KON_NO_DRAW_QUEUE` lo desactivan.~~
  - [x] ~~Sin copias por lote de los búferes de destino, conversión del framebuffer directa, borrados por filas, menos señales entre hilos, búfer de presentación reutilizado.~~
  - [x] ~~Ritmo por línea de tiempo fija: tras un fotograma lento los siguientes empiezan sin esperar hasta recuperar la línea (reinicio si va más de dos periodos tarde); el tiempo de juego sigue siendo el real. Periodo exacto en modos PC (1 000 000/FPS µs: 165 es 165, no 164,8).~~
  - [x] ~~**Resultado (6 oct., canción, segundos 1–9, Hotchkiss):** con batería y plan «silencioso»: modo 120 → 120,0 FPS, modo 165 → 164,6 FPS. Enchufado, ventana visible y con sonido: 30 → 30,0 (canción a 119,52 s), 60 → 60,0 (119,46 s), 120 → 120,0 (119,44 s), 165 → 165,0. Fotogramas reales verificados idénticos en 30/60/120 (2 por prueba) y en las pruebas de cada paso. Esta mañana, con batería: ~49 FPS a 120.~~
  - [x] ~~Fila FPS de Ajustes comprobada de punta a punta tras mover su bloque a RAM de núcleo (6 oct., enchufado, `work/input_settings_fps.txt`): entra en Ajustes, 30 → 60 → 120 → 60 con el juego siguiendo la tasa al instante y `logic_fps=60` guardado.~~ Desgarro: el usuario no ha notado ninguno (6 oct., «a priori diría que no hay», sin certeza total). Otras canciones: el usuario probó dos más y van bien. Recorridos largos, NVIDIA y latencia con cámara → puntos 6–7.
- [x] ~~**Velocidad fuera de las canciones a 120/165**~~ (informe del usuario, 6 oct.: en Ajustes, Play, selección de personaje y de objeto la selección va «aceleradísima»; en el menú principal las chicas de la sala del club van muy aceleradas; las cinemáticas —diálogos y personajes— también; dos canciones más sí van bien).
  - [x] ~~Herramientas: `--speed-audit inicio_ms duración_ms` alterna fotogramas de 0,6 y 1,4 periodos (mismo promedio, paso fijo) y busca palabras de RAM que cambian igual en fotogramas cortos y largos (avanzan por fotograma, no por tiempo); `tools/scan_step_sites.py` instrumenta los 2040 almacenamientos de una suma (enteros y floats) y `tools/classify_step_sites.py` lista los que mueven su valor igual en ambos tipos de fotograma, con la pila nativa (solo el hilo lógico); `PSPRECOMP_WATCH_WRITE` acepta varias direcciones y la auditoría vigila las palabras marcadas; `--ram-variation` + `tools/compare_ram_variation.py` comparan una ejecución a 30 con otra a 120; capturas cada 50 ms como mínimo (`--capture-interval-ms`).~~
  - [x] ~~Repetición de teclas (0x089F803C): cada botón tiene dos contadores por fotograma (repetición A: pulsación, luego cada fotograma desde el 10.º; B: pulsación y cada 5). A 165 la repetición empezaba a los 0,05 s y daba 165 pasos por segundo. Ahora cuentan fotogramas de 30 (el primer fotograma pulsado sigue contando 1, sin latencia añadida) y solo repiten cuando pasa uno. Comprobado: mantener Arriba 0,6 s en la rueda del menú deja la misma opción («save») a 30, 120 y 165.~~
  - [x] ~~Chicas de la sala del club: la IA (0x0889CA54: andar, girar, esperas), el parpadeo (0x08894A00: suma aleatoria por fotograma hasta 181 y parpadeo de 6 fotogramas) y el gesto aleatorio (0x08894AE8) se ejecutan solo en los fotogramas lógicos en que pasa un fotograma de 30 (cadencia original); la mezcla entre animaciones (peso += ritmo por fotograma, 0x088F4254) se escala por el tiempo. La animación esquelética y el dibujo siguen a 120/165. La auditoría posterior ya no marca esos sitios.~~
  - [x] ~~Atasco al confirmar en la rueda del menú principal a 120/165: una cola de órdenes diferidas (0x088A5E64, cuenta atrás por fotograma) pedía «¿terminó?» a los 14 fotogramas lógicos a un temporizador ya convertido a tiempo de juego (todavía en 8): la rueda se quedaba en «play!» rosa para siempre. La cola cuenta ahora fotogramas de 30 (saturando en 0). Encontrado por bisección de los 19 temporizadores y traza de la máquina de estados; a 120 la rueda queda igual que a 30.~~
  - [x] ~~Transición de pantalla (0x088813A0/C8, esperas de 1 y 10 fotogramas) en fotogramas de 30.~~
  - [x] ~~Sin regresiones: canción Hotchkiss a 30 empieza a 119,50 s (antes 119,52) y a 120 a 119,43 s (antes 119,44), 2/2 fotogramas reales verificados en cada una; 36 pruebas pasan.~~
  - [x] ~~Cinemáticas: escenario con una copia privada de la partida del usuario (`--seed userdata/ms0`; la partida original no se toca): tras cargar se abre la escena «Hey, everyone! I discovered something!». La espera del guion (0x088B1970, [+12]) y la espera de comportamiento de las chicas (0x088A0764, [+172]) cuentan fotogramas de 30; las chicas de la escena usan la IA, el parpadeo y la mezcla ya corregidos. ([+10] del guion es su contador de programa dentro de un mismo fotograma: convertirlo dejaba la escena sin bocadillo; descartado.)~~
  - [x] ~~Medido con capturas cada 50 ms alrededor de una pulsación (`--capture-begin-ms`): la escritura letra a letra va igual a 30 y a 120 (~7–8 letras cada 50 ms); la animación del bocadillo es una animación 2D ya en fotogramas de 30. Lo único más rápido a 120 es la reacción a la pulsación (menos latencia: cada etapa espera 8 ms en vez de 33 ms).~~
  - [x] ~~Auditoría de las pantallas de selección (canción, personaje, objeto) y de la escena: sin más sitios por fotograma que los corregidos (lo demás son matrices, listas de dibujo, pilas, generador aleatorio y audio). `tools/scan_step_sites.py` tiene modo amplio (`KON_STEP_LOOSE=1`: sigue el valor a través de saltos y recortes) y `KON_STEP_ALL_THREADS=1` incluye otros hilos.~~
  - [x] ~~Sin regresiones tras todo lo anterior: canción a 30 a 119,50 s con 2/2 fotogramas verificados; a 165 el recorrido llega al escenario de la canción sin bloqueos; 36 pruebas pasan.~~
  - [x] ~~Espera mínima de las pantallas de carga a 3 s a cualquier tasa (ver usos de 0x088EAC3C arriba).~~
  - [x] ~~Sesión larga: 4 min a 165 en la sala del club, media 165,0 FPS, sin errores.~~
  - Falta que el usuario lo pruebe jugando (menús, sala del club, escenas). Nota: capturas diagnósticas muy densas (100 ms) durante una carga la alargan; no afecta al juego normal.
- [x] ~~Menos input lag real~~ (lo medible desde el programa):
  - [x] ~~Presentación sin cola: MAILBOX si existe; si no, IMMEDIATE en ventana (compuesta por DWM, sin desgarro); FIFO como respaldo. La 780M no ofrece MAILBOX: usa `immediate` (`presenter=vulkan:AMD Radeon 780M Graphics:immediate`, 59,3–60 FPS en todos los tramos con `--logic-fps 60`).~~ Falta que el usuario confirme que no hay desgarro.
  - [x] ~~La muestra de controles más reciente refleja el estado en el momento de la lectura del juego (hasta un intervalo de muestreo menos de retraso); las repeticiones sintéticas siguen idénticas.~~
  - [x] ~~Latencia interna medida (`input_latency_avg_us`/`input_latency_max_us` en el registro): desde la lectura del mando que usó un fotograma hasta su entrega a Vulkan. A 120 con batería: menús 5–13 ms de media, canción ~20 ms (máx. ~44 ms), es decir, aproximadamente un fotograma de proceso a ~58 FPS. A 30 la media es 5–10 ms, pero no es comparable (allí se lee el mando en cada vblank y cuenta la última lectura).~~
  - Tramo que el programa no ve (composición de Windows y monitor): medirlo con cámara o sensor → punto 7. Desgarro con `immediate`: el usuario no ha notado ninguno.

El hilo de dibujo con interpolación heredado (`NativeRenderWorker`) queda solo como herramienta de diagnóstico.

### 3. Resolución interna superior — COMPLETADO

El hilo de dibujo reproduce cada fotograma a escala N× sobre un destino propio. La VRAM original sigue a 480×272 para las lecturas del juego.

- [x] ~~Segunda pasada N× (2×–4×) en Vulkan para el búfer que se muestra: planos persistentes de color y profundidad, sincronizados con la VRAM original por mosaicos de 16×16 (lo que escribe la CPU o el juego se amplía en GPU), conversión a BGRA en GPU y lectura solo de las filas cambiadas. La pasada original exacta no cambia (36 pruebas pasan).~~
- [x] ~~Presentación por mosaicos: donde la VRAM coincide con lo que dejó el lote se usan las muestras ampliadas; si no, el píxel original repetido (sin huecos ni imágenes negras: 0 mosaicos distintos en la traza de 120 FPS).~~
- [x] ~~Ajustes: la fila «Install» (solo PSP) es ahora «Video», con dos columnas: FPS (30/60/120/165) y resolución interna (272p original, 544p, 816p ≈720p, 1088p ≈1080p; se ofrecen los múltiplos de 272 hasta la altura de la pantalla, redondeando). Círculo cambia de columna, Izquierda/Derecha el valor; texto de ayuda propio. Se guarda en `settings.ini` (`internal_scale`) y se aplica en caliente en el hilo de dibujo, sin lote en vuelo. Verificado con capturas (`work/cap_rows120`, `work/cap_res5`).~~
- [x] ~~Cambio en caliente medido (binario `f0b68505`, AMD 780M, 120 FPS, `KON_SCALE_SCHEDULE`): 272p→544p→816p→1088p→272p→544p, cada cambio visible en < 1 s (capturas de 960×544, 1440×816, 1920×1088).~~
- [x] ~~Ajustes: tercera columna «Screen» (Window/Full) en la fila Video; F11 o Alt+Intro alternan pantalla completa sin bordes en cualquier momento y la fila lo refleja; se guarda como `fullscreen`. Verificado: arranque a 1920×1200 reales, F11 → ventana → pantalla completa, `settings.ini` actualizado.~~
- [x] ~~Teclado en la fila Video: las pulsaciones que el filtro quita al juego se dan por entregadas, así que Derecha repetida recorre todas las resoluciones (antes había que salir y volver).~~
- [x] ~~Nitidez: el proceso declara PPP por monitor (a 125 % Windows estiraba y emborronaba toda la ventana); los dibujos 2D ampliados usan bilineal «afilado» (los menús quedan tan nítidos como el original en vez de suavizarse). Título de la ventana con la resolución dibujada (p. ej. `1920x1088 (1088p)`).~~
- [x] ~~Rendimiento 1088p en la canción, AMD 780M, objetivo 165, enchufado (binarios de esta sesión): 64 → 97 (la presentación ya no espera a dibujos en cola del fotograma siguiente) → 126 (sombreado por bloques 2×2 con cobertura, profundidad y mezcla por muestra) → 139 (sin segunda pasada exacta del búfer mostrado; imagen final doble y escrita directamente en memoria del host) → 157 (mezcla vectorizada, reutilizada en el bloque; borrados de la CPU dentro de la pasada) → 163,0 (presentación diferida por defecto; composición directa en la memoria de subida de Vulkan). Ventana visible: 164,9–165,5 FPS en el título.~~
- Medido con `final_s*_165` (canción 103–118 s / menús 40–90 s): 4× 160,4–163 / 164,9; 3× 142,8 (canción); 2× 164,6 / 164,9; 1× 164,8–165,0 con 4/4 fotogramas exactos. Las cuatro suites Vulkan pasan en la 780M.
- Exactitud: a 2×–4× la VRAM del búfer que se muestra se toma de la imagen ampliada (una muestra por píxel), ya no bit a bit (`KON_EXACT_DISPLAYED=1` vuelve a la pasada exacta); a 1× todo sigue exacto.
- [x] ~~Sunday Siesta a 1088p (escenario rosa) bajaba a 78–112 FPS: cada fotograma el juego hace un borrado a mitad de fotograma (entre el fondo 2D y la escena 3D) y eso cerraba el lote de GPU y esperaba a la GPU en mitad del fotograma (GPU ocupada solo el 62 %, línea de tiempo `KON_RENDER_TIMELINE`). Ahora el borrado se sombrea dentro del mismo lote, en orden (dos triángulos por rectángulo, color plano, profundidad escrita sin prueba; `KON_NO_BATCH_CLEAR` vuelve al camino anterior). Medido (AMD 780M, 1088p, 165, copia privada, entradas sintéticas): media 100–150 s de 96,5 → 161,6 FPS (tramos de 152–165). Exacto a 1×: 4/4 fotogramas verificados en mitad de la canción con 6116 borrados en lote.~~
- [x] ~~Rendimiento a 3× y 4× igual al de 272p en las cuatro canciones medidas (binario de esta sesión, AMD 780M, 165, copia privada de prueba, entradas sintéticas; media 100–150 s / peor tramo de 2 s):~~

  | Canción | 272p | 544p | 816p | 1088p |
  |---|---|---|---|---|
  | Watashi no Koi wa Hotchkiss | 164,8 / 162,2 | 164,8 / 161,3 | 164,9 / 161,7 | 164,3 / 158,3 |
  | Humming Bird | 164,9 / 163,0 | 164,4 / 161,9 | 164,7 / 163,2 | 162,3 / 154,8 |
  | Sunday Siesta | 164,8 / 162,6 | 164,8 / 162,4 | 164,4 / 159,9 | 163,7 / 156,4 |
  | Hello Little Girl | 164,9 / 163,4 | 164,9 / 163,2 | 164,9 / 163,2 | 164,9 / 162,8 |

  Cambios: borrados dentro del lote (también el primero con lote en vuelo, comprobando su dueño); pipelines especializadas por pase (bloques 4×, bloques 3×, por muestra 2×) con constantes de especialización; 3× sombrea por bloques de 2×2 muestras (bloques que cruzan píxeles originales: límites y palabras originales por muestra; `KON_SAMPLES_3X` vuelve al sombreado por muestra); lecturas de cabeceras, triángulos, tablas y texturas por un enlace de solo lectura; tres imágenes presentables por plano (la ventana ya no frena al lote); comparación de planos por franjas; la parte de color del plan y la ordenación por mosaicos se preparan mientras la GPU termina el lote anterior (`KON_NO_EARLY_PLAN`); el hilo de dibujo no se bloquea esperando a la GPU con la cola vacía (`KON_BLOCKING_SHOW`). Diagnósticos: `KON_SUBMIT_SPLIT`, `KON_SHADER_INFO` (registros del driver AMD: 64–70 VGPR, sin scratch), `KON_SKIP_WINDOW_PRESENT`, `KON_RENDER_TIMELINE` con esperas del juego.
- Exactitud: a 272p 4/4 fotogramas verificados en Hotchkiss y en mitad de Sunday Siesta; cuatro suites Vulkan AMD aprobadas; 36/36 pruebas. Sin bloques negros a 3× ni 4× (detector de zonas negras sobre capturas de 100–160 s; un intento intermedio que no actualizaba la referencia de profundidad reemplazada los producía y se descartó).
- Probado sin mejora y revertido: bordes incrementales por bloque, descarte de triángulos por grupo con prueba de bordes, desenrollado forzado, primer borrado del fotograma en GPU, menos hilos de CPU, recuperación de ritmo de 4 periodos.
- Pendiente fuera del punto: el peor tramo de 2 s a 1088p baja a ~155 en Humming Bird y Sunday Siesta (272p: ~162); Fuwa Fuwa Time sin medir (no localizada en la partida de prueba).
- [x] ~~Revisión visual a 4× en pantalla completa real (1920×1200, capturas de pantalla, opción de prueba `--fullscreen`): pantalla de título, carga, sala del club, diálogos, tutorial «Local», teclado de la PSP y menús, sin juntas ni bordes entre mosaicos; personajes y objetos 3D nítidos, fondos con la resolución de sus texturas originales (`work/scene4/caps`).~~
- Nota: el «GAME OVER» de las pruebas de canción viene de la repetición sintética de notas (también falla a 1×), no de la escala.

### 4. Canción completa válida (fase 11) — COMPLETADO

Audio continuo y sincronizado, notas, controles, combo, puntuación y resultados, de principio a fin y a distintas tasas de FPS.

- [x] ~~Canción completa superada de principio a fin con entradas derivadas de la partitura original (Watashi no Koi wa Hotchkiss, Yui, Normal; copia privada de la partida de prueba, AMD 780M, silenciado). Partitura: `tools/music_chart.py` + `tools/chart_replay.py`; un tick = 1/60 s; carril 3 = Cruz, carril 2 = Círculo (la hipótesis anterior los tenía al revés); primera nota 1,1 s después del primer PCM del canal 3. Menú de prueba que solo llega al inicio (`work/input_hotchkiss_song_only*.txt`; el anterior seguía pulsando Círculo durante la canción y la reiniciaba). Resultados (rango, puntuación): 30 FPS S 50 880 · 60 FPS S 49 090 · 120 FPS A 41 160 · 165 FPS A 43 140 · 165 FPS a 1088p A 36 980; en todas «CLEARED», objeto «Cake» y nueva mejor marca en pantalla.~~
- [x] ~~Reloj de CRI: el hilo «CRI ADX Audio» cuenta vblanks con `sceDisplayWaitVblankStart`; un despertar tardío del planificador cooperativo perdía vblanks (59,0/s a 165 FPS). Ahora cada hilo recibe el vblank siguiente al último entregado (sin esperar si ya pasó; más de 4 de retraso reinicia): 59,96/s. A/B (`KON_LOSSY_VBLANK=1`): con el comportamiento anterior la canción también se supera (A 42 790), así que no era la causa de los GAME OVER de las pruebas; se mantiene por fidelidad.~~
- Diagnósticos: `KON_CHART_AUTOPLAY`/`KON_CHART_CLOCK`/`KON_CHART_OFFSET` (pulsa según un contador del juego; con el de CRI y 66 ticks: rango S 54 780 a 165 FPS), `logic_ticks`/`logic_steps30`/`vblank_caught_up` en la línea de progreso.
- [x] ~~Audio continuo: la música (canal 3, bloques de 256 muestras ≈ 5,8 ms) se quedaba sin datos porque la salida bloqueante solo dejaba un bloque por delante y el planificador cooperativo despierta tarde al hilo de CRI (cortes de la canción completa: 30 FPS 161, 60 FPS 684, 120 FPS 933 con uno de 203 ms, 165 FPS 879, 165 FPS a 1088p 1643). Ahora el escritor bloqueante se adelanta hasta tener ~24 ms en cola (`KON_AUDIO_QUEUE_MS`, 0 = original; prueba de mezclador para ambos modos): 0 cortes a 165, 120 y 165 FPS a 1088p. Además mejora el juicio: rango S en las tres (165: 51 790; 120: 54 690 con 149 Perfect, 1 Great y combo 150; 1088p: 51 600).~~
- [x] ~~Congelación tras resultados (estrés game over → Retry a 1088p, `work/input_retry_loop.txt`). Dos formas observadas: (a) el hilo principal con `CriCsPsp` esperando al hilo «CRI ADX File» que espera ese mutex; (b) bucle activo del hilo «CRI ADXATR Waveout» (prioridad 16) que acapara la CPU. Causa de (b), confirmada en el registro: el estado de interrupciones (`sceKernelCpuSuspendIntr`) era global; el hilo principal cedió el turno con las interrupciones suspendidas («MaskedContextSwitch») y todos los demás hilos siguieron enmascarados (sin desalojo, callbacks ni interrupciones de GE). Ahora es parte del contexto de cada hilo, como en la PSP (`KON_GLOBAL_INTERRUPT_STATE=1` = anterior). Con el audio antiguo (`KON_AUDIO_QUEUE_MS=0`), donde se congelaba 1 de cada 2–3 rondas: 0 de 4 rondas (≈20 ciclos), una de ellas con 54 986 cesiones enmascaradas que antes congelaban; con el audio nuevo, 0 de 3. Canción completa tras el cambio: rango S, 53 690 (139 Perfect, 11 Great). El último bloqueo de cada mutex queda en `ultimo_fallo.txt` si (a) reapareciera.~~
- [x] ~~Resultados y guardado: tras superar la canción (145 Perfect, 5 Great, combo 150, 54 280) el juego entrega objetos (Cake, Tea set, Chalk, Cassette player); con END hace un autoguardado (`SavedataInitStart mode=1`, 4 `SavedataUpdate` sin error) y pasa al evento de la sala del club. Al recargar ese guardado en una partida nueva: fecha de la partida actualizada (2026/10/08 16:48), «%completed» de 0 % a 2 % y el evento continúa donde quedó.~~
- [x] ~~Otra canción: Sunday Siesta (Yui, Normal, 184 notas) superada a 165 FPS con rango A, 52 980 y combos de 120–149. Referencia de tiempo general: el tick 0 de la partitura es la pulsación de Círculo en la tarjeta «PUSH ○ BUTTON» (Hotchkiss a 30 y 165 FPS, Siesta a 165); el primer PCM del canal 3 llega 7,4–10,8 s después según la canción y no sirve como ancla común. `tools/chart_replay.py --menu … --start-us …` genera la entrada absoluta.~~
- **Punto 4 completado** (binario de esta sesión, AMD 780M, copias privadas de la partida de prueba, entradas sintéticas derivadas de la partitura original, silenciado): canción de principio a fin con audio continuo (0 cortes), notas y controles correctos, combo, puntuación, resultados, objetos y guardado, a 30/60/120/165 FPS y a 1088p; congelación tras resultados resuelta.
- Pasa a otros puntos: calibración de la latencia percibida audio–pulsación (punto 5, el juicio va con el reloj del juego desde el inicio de la canción y el audio sale con el búfer del dispositivo, ~93 ms + hasta 24 ms de cola); otros personajes, instrumentos y dificultades (punto 6); prueba con mando físico y jugador real.

### 5. Ajustes de PC (fases 14 y 16) — COMPLETADO (falta probar con mandos físicos)

Ventana y pantalla completa, FPS, resolución, mando y reasignación, calibración de latencia, idioma JA/EN desde Ajustes con recarga segura, y mods por carpetas con prioridad y desactivación.

- [x] ~~Todos los tipos de mando (`src/native_gamepads.hpp`, `src/native_gamepad_reports.hpp`): Xbox y compatibles por XInput; DualShock 4 y DualSense (USB y Bluetooth), Switch Pro (informe completo y simple) y mandos genéricos HID/DirectInput por Raw Input, con conexión y desconexión en caliente. Botones por posición (abajo ✕, derecha ○, izquierda □, arriba △); en Xbox, A acepta (○) y B vuelve (✕), como en PC. Prueba `native_gamepad_reports` con informes construidos según el formato de cada mando. **Pendiente: probar con mandos físicos** (aquí no hay ninguno conectado).~~
- [x] ~~Iconos de botones según el dispositivo en uso, cambiando en caliente: teclado → teclas (X aceptar, Z volver, S, A, ENTER, BKSP), Xbox → A/B/X/Y con sus colores y MENU/VIEW, Nintendo → A/B/X/Y y +/−, PlayStation → ○✕△□ originales, genérico → 1–4. Se redibujan al decodificar las texturas que los contienen (reconocidas por el hash de su contenido; tabla generada por `tools/glyph_scan.py` a partir de un volcado `KON_TEXTURE_DUMP`): 12 cabezas de nota, 20 iconos y 8 rótulos START/SELECT de la barra de ayuda, el cartel «PUSH ○ BUTTON!!» y el aviso CHECK!. Al cambiar de dispositivo la caché vuelve a decodificar esas texturas. Verificado en el juego (copia privada, AMD, silencioso): barra «X Enter / Z Back», cartel «PUSH X BUTTON!!» y notas X/Z con teclado; B/A/Y/X en colores de Xbox con `KON_GLYPH_STYLE=xbox` (antes del cambio A=aceptar). `native_button_glyphs` comprueba etiquetas y el hash de los volcados. Otras pantallas con iconos que no salieron en el volcado (historia, otros modos) quedan por cubrir.~~
- [x] ~~Mods por carpetas (`src/native_mods.hpp`): `mods/<nombre>/disc/<ruta>` sustituye archivos del disco y `mods/<nombre>/textures/<hash>.png|bmp` sustituye texturas del mismo tamaño (hash del volcado `KON_TEXTURE_DUMP`; PNG por WIC de Windows, BMP de 32 bits con transparencia). Prioridad por orden de nombre (el último gana) y desactivación con `-` delante del nombre; el registro lista los mods. Prueba `language_content` (orden, desactivación, prioridad y píxeles). Verificado en el juego con un mod temporal que tiñó el cartel «PUSH BUTTON» (ya eliminado). Limitación: un archivo dentro de un `.cpk` solo se cambia sustituyendo el `.cpk` entero.~~
- [x] ~~Reasignación de botones (`config/controls.ini`, `src/native_button_map.hpp`): una sección por tipo de dispositivo (teclado con nombres de tecla; Xbox, PlayStation y Nintendo con sus nombres de botón; genérico por número), plantilla comentada creada al primer arranque y avisos `controls_ini_problem=` en el registro. Los iconos siguen la asignación (la tecla o botón real; PlayStation conserva ○✕△□). Pruebas en `native_gamepad_reports` (rutas, etiquetas, nombres desconocidos). Verificado en el juego: con `circle=K` y `cross=J` la barra muestra «K Enter / J Back». Se aplica al iniciar (no en caliente).~~
- [x] ~~Idioma JA/EN desde Ajustes: cuarta columna de la fila «Video» (English / 日本語). Se guarda en `config/settings.ini` (`language=`) y, cuando el jugador sale de Ajustes (el juego ya guardó sus opciones), el juego se reinicia en ese idioma con la misma línea de órdenes sin `--language`; elegir de nuevo el idioma actual cancela el reinicio. Sin `--language`, el juego arranca con el idioma guardado (el lanzador solo lo fuerza si se le pasa `ja` o `en`). Verificado (AMD, silencioso, copia privada): `SettingsLanguage language=ja saved=1`, `language_restart=ja`, proceso nuevo sin `--language` que arranca en japonés (sala del club en japonés); arranque con `language=ja` guardado muestra el título japonés. Columnas reubicadas para que no se solapen.~~
- [x] ~~Iconos de dispositivo también en japonés: el atlas japonés de la barra de ayuda (`c008c9…`, mismo diseño que el inglés) y el aviso CHECK! japonés añadidos a la tabla (72 iconos en total). Verificado en el juego en japonés con teclado: «X 決定 / Z 戻る».~~
- [x] ~~Latencia de audio: el búfer de salida pasa de 2×2048 muestras (~93 ms) a 2×1024 (~46 ms) por defecto, ajustable con `audio_block_ms` en `config/settings.ini` (256–4096 muestras por bloque); el registro cuenta los vaciados del dispositivo (`native_audio_device … underruns=`). Medido con la canción completa a 165 FPS (AMD, silenciado, copia privada): bloques de 2048, 1024 y 512 → 0 vaciados y 0 huecos en el canal de la música; el juicio no cambia (150 Great, rango A en las tres: el juicio sigue el reloj del juego). Pendiente para el punto 7: medir la latencia percibida de extremo a extremo (cámara o sensor) y, si hace falta, un desfase de juicio.~~

### 6. Guardado, contenido, modos y multijugador (fases 12, 13 y 15) — COMPLETADO (falta probar el multijugador entre dos PC)

- [x] ~~Cierre al pulsar START en la historia (diálogo tras el tutorial): el juego abre el teclado de la PSP (`sceUtilityOsk*`, «名前を入力», hasta 5 caracteres) y no estaba implementado. Ahora se abre un cuadro de texto de Windows sobre el juego (Intro acepta, Esc cancela; con texto inicial) y el resultado vuelve al juego en UTF-16. Verificado con una copia privada de la partida del usuario en ventana visible: se escribió «Mio», se aceptó y la historia siguió.~~
- [x] ~~Congelaciones (pantalla negra, FPS 0, sin parada): al jugar desde el lanzador, si la ventana no recibe imágenes nuevas durante 10 s se escribe `ultimo_fallo.txt` con hilos, esperas, mutex ocupados y las últimas 150 llamadas; el registro de la sesión anterior se guarda como `native_interactive_<idioma>_anterior.log`. Sin falsas alarmas en 70 s de arranque y menús.~~
- [x] ~~Congelación tras la pantalla de resultados: resuelta en el punto 4 (estado de interrupciones por hilo; 0 congelaciones en 7 rondas de estrés).~~
- [x] ~~Todas las partes en modo individual (Watashi no Koi wa Hotchkiss, NORMAL, copia privada de la partida de prueba, AMD, silenciado; partituras originales con `tools/chart_replay.py --by-kind --origin-tick 513`): Yui S (verificada en el punto 4), Mio S 33 530 (96 Perfect), Ritsu S 62 500 (169 Perfect), Azusa S 44 940 (125 Perfect), Tsumugi A 32 900 (127 Great); todas «STAGE CLEARED» con objeto. Botón por tipo de nota comprobado: 0 △, 1 ○, 2 ✕, 3 □, 5 →, 6 ↓. La dificultad HARD aparece bloqueada en la partida de prueba (la desbloquea el progreso del propio juego).~~
- [x] ~~Modos del menú del club con la partida del usuario (copia privada, AMD, silenciado): play!, album! (COLLECTION con los objetos desbloqueados; EVENT, GRAPHIC, SOUND y VOICE en el menú), save! (pantalla «Save your progress»; solo se abrió), option! (Ajustes, puntos 2 y 5), つうしん!/local (multijugador). Las entradas «????» y la dificultad HARD las bloquea el progreso del propio juego.~~
- [x] **Multijugador local adaptado a PC** (falta probar entre dos PC). Red ad hoc de la PSP sobre la red del PC, sin servidores (`src/native_adhoc.hpp/.cpp`, prueba `native_adhoc_loopback`): descubrimiento por balizas UDP (este PC y, en modo LAN, multicast), grupos de `sceNetAdhocctl` (escanear, crear, unirse, pares, eventos al manejador del juego), PDP sobre UDP y PTP sobre TCP; las llamadas bloqueantes esperan en el planificador con los tiempos de la PSP. Corregido además: `0xD7763699` es `sceWlanGetSwitchState` (se tomaba por `GetEtherAddr`: el menú «つうしん!» decía «WLAN OFF» y se escribían 6 bytes en memoria arbitraria). Ingeniería inversa del protocolo de la sala del juego (sala `room01`, anuncios PDP de banda en el puerto 2673, escucha PTP 2773, hasta 5 bandas y 5 miembros) con `tools/disasm.py` y el trazado `KON_TRACE_PCS`; `PtpListen`/`PtpAccept` tenían los NID cruzados. Verificado con dos instancias en este PC (`KON_ADHOC_LOCAL=1`, silenciadas, copias privadas de la partida del usuario, AMD): el líder crea «バンド01», el miembro la ve en BAND SELECT y se une (1P y 2P con «よろしく!»); el líder elige «わたしの恋はホッチキス» NORMAL, el miembro elige a Mio (bajo) y **las dos instancias tocan la canción completa juntas** con sus partituras (`work/chart_hk_gt1_anchor.txt`, `work/chart_hk_bas_anchor.txt`) y llegan a resultados con la **misma BAND SCORE (20 553)**: líder 40 100 (151 Great, A, PERFECT CLEARED), miembro 200 con una partitura de prueba mal asignada. Corregido: el botón lo da el campo `kind` de la nota (guitarra 1=○, 2=✕; bajo 5=→, 6=↓; `tools/chart_replay.py --by-kind`); repetida la partida: Mio 96 Perfect, 3 Miss, combo 96, 33 530; Yui 152 Great, 40 410; misma BAND SCORE en las dos instancias (4770…), «STAGE CLEARED» y «PERFECT CLEARED». Pendiente: prueba entre dos PC por LAN (modo por defecto; pedirá permiso al Cortafuegos de Windows) y hasta 5 jugadores.
- [x] ~~Cierre en el tutorial de «Local» (página 2/5): texturas con ancho de búfer 0 detenían el juego; ahora leen filas de 16 bytes, como PPSSPP. Verificado con una copia privada de la partida del usuario: pasa las 5 páginas. Al jugar desde el lanzador, cualquier parada por error deja `ultimo_fallo.txt` junto al juego.~~

### 7. Revisión completa en AMD y NVIDIA, sesiones largas y entrega (fases 17 y 18)

Incluye lo heredado del punto 2: latencia de extremo a extremo con cámara o sensor, revisión en NVIDIA (aplazada por el usuario) y recorridos largos de historia y modos a 120/165.

- [x] ~~Instalación limpia desde las ISO del usuario (`tools/make_install.py --out <carpeta nueva>`): extrae las dos ISO comprobando cada archivo, descifra el EBOOT y comprueba su SHA-256 (`eda89013…`), copia el ejecutable («K-On Hokago Live.exe»), las cuatro bibliotecas de FFmpeg que usa (LGPL, con licencia), `Jugar.cmd` y `LEEME.txt`; nunca sobrescribe. Abierto con doble clic, el ejecutable usa su propia carpeta si contiene los datos. Verificado en `work/install_clean` (2,9 GB): la copia instalada, con GPU automática (Radeon 780M) y una copia privada de la partida de prueba, recorre los menús y toca Hotchkiss (combo 100, «Great»); la prueba acabó por tiempo antes de los resultados. La primera prueba descubrió que faltaban las DLL de FFmpeg, ya incluidas.~~
- [x] ~~Cierre al entrar en «Sweet Bitter Beauty Song» (informado por el usuario, 1088p): «GE primitive requires native renderer … type=4 vertices=4». El dibujo usaba una textura DXT5 (formato 10) y el decodificador solo conocía DXT1. Añadidos DXT3 (9) y DXT5 (10) según el bloque de la PSP (parte de color DXT1 de cuatro colores y luego el alfa: 4 bits por texel en DXT3; índices de 3 bits y dos extremos en DXT5); pruebas en `native_texture_sampling`. El dibujo exacto que falló (`work/sbbs_failed_draw.kondraw`) se reproduce ahora sin error. La prueba en el juego con entradas sintéticas no llegó a esa canción (tiempos de carga variables); pendiente de confirmar por el usuario.~~
- [x] ~~Parada a media canción tras ~19 min de juego (Fuwa Fuwa Time, informado por el usuario): «Dispatch limit reached». No era un fallo del juego: el límite de diagnóstico de 100 millones de despachos se aplicaba también al jugar. Ahora, en ventana normal y sin `--max-dispatches`, no hay límite; las pruebas lo siguen fijando.~~
- [ ] Pendientes que necesitan al usuario o hardware: mandos físicos, multijugador entre dos PC, latencia de extremo a extremo con cámara o sensor, revisión en NVIDIA (aplazada por el usuario), sesiones largas jugadas.

### 8. Peticiones del usuario del 10 de octubre — EN CURSO

Confirmado por el usuario: los mandos de Xbox funcionan y la interfaz cambia a sus iconos.

- [ ] **A. Multijugador online por Internet — HECHO, falta la prueba entre dos PC reales.**
  - F8 abre «Jugar online». **Crear sala** convierte esta instancia en un relé TCP (puerto 41700, IPv4 e IPv6) y, en segundo plano, pide al router por UPnP que redirija el puerto (cesión de 2 h, se quita al salir). Después muestra códigos de sala:
    - Internet (IP pública).
    - IPv6.
    - Red local o VPN (Radmin, ZeroTier, Tailscale).
  - Los códigos van en base 32 de Crockford y toleran mayúsculas, guiones, O/0 e I/L/1; también vale `dirección:puerto`. El amigo escribe el código y pulsa **Unirse**.
  - Balizas, datagramas PDP y flujos PTP (como flujos virtuales) viajan en tramas por el relé, así que el modo つうしん! del juego funciona igual que en local. Sin servidor externo.
  - Si el router no abre el puerto o hay CG-NAT, la ventana lo explica y ofrece las alternativas.
  - Archivos: `src/native_adhoc.cpp`, `src/native_online.cpp`, `src/native_display.hpp`.
  - Verificado (AMD, silencioso, copias privadas de la partida del usuario):
    - Prueba del transporte por el relé (`native_adhoc_loopback`): 3 jugadores, PDP unicast y difusión, PTP en ambos sentidos, rechazo, cierre y salida de la sala; además, códigos y análisis UPnP.
    - Dos instancias del juego con el descubrimiento UDP desactivado (`KON_ADHOC_RELAY_ONLY`) formaron la banda por el relé y tocaron Hotchkiss juntas, con la misma BAND SCORE en las dos: 47 653 (Mio 33 540, Yui 40 290).
    - Repetido con 150 ms de ida y vuelta simulados (`KON_RELAY_DELAY_MS=75`): misma BAND SCORE, 48 668.
  - Paquete para otro jugador (`tools/make_friend_package.py`, ZIP de 59 MB sin datos del juego). `INSTALAR.cmd` instala desde su propia ISO: elegida en una ventana o arrastrada, japonesa o inglesa, basta una.
    - El propio ejecutable lee la ISO, comprueba `std.cpk` y el EBOOT por SHA-256, descifra el EBOOT y comprueba el programa.
    - Verificado con las ISO del usuario: instalación solo inglesa (llega hasta la selección de objeto de la canción) y solo japonesa (arranca en japonés).
    - Con un solo disco, la columna de idioma de Ajustes queda fija.
  - Pendiente del usuario:
    - Partida real entre dos PC por Internet.
    - UPnP en su router (no se probó para no cambiar su configuración).
    - Revisión de la ventana F8.
- [ ] **B. Iconos de botón en toda la interfaz — EN CURSO.** Hecho y verificado en el juego (AMD, silencioso, copia privada, `KON_GLYPH_STYLE=xbox`):
  - Notas de Xbox con sus colores (A verde, B rojo, X azul, Y amarillo): se cambia el tono de toda la nota (cabeza, plica y bandera), conservando el sombreado.
  - El cartel «PUSH ○ BUTTON!!» es una textura distinta en cada canción. Por eso con Sunday Siesta y otras seguía saliendo ○.
  - Los carteles de las 19 canciones se localizaron fuera del juego, decodificando `std.cpk` (`play_mNN_2.bin`, iguales en los dos discos) con las reglas de paleta del juego. Los hashes coinciden con los volcados del juego en los cuatro formatos de paleta (1 956 texturas comprobadas).
  - El ○ de los 18 carteles que lo tienen se redibuja con el botón del dispositivo; Let's Go no tiene cartel.
  - Verificado en el juego: cartel de Sunday Siesta con la A verde y notas A verde/B roja.
  - Escaneo de las 719 texturas de interfaz del disco inglés, decodificadas fuera del juego (`tools/glyph_scan.py`): solo apareció una textura sin cubrir, el atlas del álbum (`gallery.bin`), con dos ○. Están añadidas, también la japonesa equivalente. Las flechas L/R de ese atlas siguen como en la PSP.
- [ ] **C. Interfaz y texturas nítidas a la resolución interna — EN CURSO.** Hecho:
  - Los dibujos 2D a 2×–4× usan una copia ampliada de su textura (`src/native_upscale.hpp`):
    - 2× por bordes (reglas tipo xBR) y un paso Catmull-Rom para 4×.
    - Los bordes se ajustan al rango local del original, sin halos.
    - La copia se genera en hilos de baja prioridad; hasta que está lista se usa la original.
  - La GPU la muestrea por muestra, también en la pasada de bloques 4×.
  - Verificado a 1088p (AMD 780M): el texto, los botones y las ilustraciones de los menús tienen bordes suaves y definidos en lugar de bloques de 4×4 (`work/c_hd6`).
  - Rendimiento en la canción, 1088p y 165 FPS, segundos 1–5 de cuatro pasadas: con ampliación 129–131 FPS, sin ella 127–150. No se aprecia diferencia más allá de la variación entre pasadas.
  - Pantalla de 1080 líneas con imagen de 1088: se muestra 1:1 recortando 4 líneas arriba y abajo, en lugar de reescalar a 0,99× (que emborronaba toda la imagen).
  - Pendiente:
    - Texturas 3D ampliadas.
    - Revisión visual del usuario.
    - Pantallas con texto generado en búferes intermedios, si las hay.
- [x] ~~**D. Discord Rich Presence.**~~ Verificado por el usuario (10 oct.): con el juego abierto, su perfil de Discord muestra «Jugando a K-On! Houkago Live!! Recompiled» con el icono del proyecto, la actividad y el tiempo. La aplicación de Discord del port (ID 1558336943383904258) va dentro del ejecutable: nadie tiene que configurar nada; `discord=0` lo desactiva.
  - Las lecturas del juego dentro de `std.cpk` se traducen a su archivo (índice del CPK en C++, `src/cpk_index.hpp`). De ahí sale la actividad (`src/native_activity.hpp`):
    - Sala del club, menús, la canción que suena (19 títulos), en solitario o con la banda y la parte.
    - Resultados, modo PV, canciones tocadas en la sesión y tiempo transcurrido.
  - Un hilo la envía al cliente local de Discord por su tubería IPC (`src/native_discord.hpp`): saludo, SET_ACTIVITY, se borra al salir y se reconecta si Discord está cerrado.
  - Se activa con `discord_client_id=` en `config/settings.ini`; opcionalmente `discord_large_image=` con una imagen subida a la aplicación.
  - Verificado:
    - Prueba con una tubería privada (`native_discord_presence`).
    - Contra el Discord real de este PC, un ID inválido recibe «4000 Invalid Client ID», es decir, el protocolo llega.
    - En el juego (copia privada) la actividad sigue la partida: club 32 s → menús 43 s → «Playing Watashi no Koi wa Hotchkiss · Solo · Yui (lead guitar)» 79 s → resultados → menús con «N songs played».

### 9. Versión 0.1.0 para publicar (10 de octubre) — HECHO, falta publicar la release

- [x] ~~**El paquete no lleva nada del juego** (ni datos ni código), como Ship of Harkinian: `K-On Houkago Live Recompiled.exe` (`src/launcher.cpp`) instala la ISO del jugador, traduce su ejecutable con `psp_recomp`, aplica el parche de FPS con un Python portátil, compila con zig y enlaza con el código propio del port (biblioteca `kon_host`). Todo dentro de la carpeta elegida (`recomp/`, `recomp_cache/`, `bin/`, partidas y ajustes). Sin `.cmd`.~~
  - Verificado en la 780M: instalación limpia de las dos ISO en 5 min; la traducción es idéntica al corpus de desarrollo y los 127 objetos tienen las mismas secciones de código y datos; el juego creado pasa el arranque JA/EN y toca Hotchkiss.
- [x] ~~**Actualizaciones automáticas** desde las releases de `EDGX73/KOn-Houkago-Live-Recompiled` (`src/native_update.hpp`): descarga solo el paquete pequeño si el compilador no cambió y reconstruye de forma incremental (2 s si solo cambia el port).~~ Verificado con un servidor local que publicaba una 0.1.1: la 0.1.0 instalada pasó a 0.1.1 (lanzador y juego) conservando partidas.
- [x] ~~**Versión 0.1.0** (`src/kon_version.h`) en el título, F1, F8, Discord y propiedades del exe.~~
- [x] ~~Paquete sin datos personales: compilado sin información de depuración (las bibliotecas llevaban rutas de este PC); comprobado buscando el usuario, el nombre y el correo en todos los archivos.~~
- [ ] Publicar el repositorio y la release 0.1.0 en GitHub (cuenta del usuario) y probar una actualización real.

---

## Rendimiento medido (Hotchkiss, entradas sintéticas, copia privada del guardado)

FPS = presentaciones originales nuevas. El juego apunta a 30 FPS de lógica (`sceDisplayWaitVblankStartMulti(2)`).

| Binario | GPU | Canción 1–5 s | Canción 1–10 s | Menús (tramos de 10 s, 0–120 s) |
| --- | --- | --- | --- | --- |
| `28578064` (inicio de sesión) | AMD 780M | 7,70 / 7,51 | 11,31 / 10,89 | 13,4–30,0 |
| `770e0bee` (relectura parcial) | AMD 780M | 11,10 / 10,75 | 11,46 / 12,18 | — |
| `2ec1dab5` (shader entero) | AMD 780M | 18,6 | 19,5 | — |
| intermedio (menús aún lentos) | NVIDIA RTX 5050 | 17,3 | — | 13,0–29,4 |
| `37c6331b` (actual) | NVIDIA RTX 5050 | 29,3 | 29,5 | 28,4–30,0 |
| `37c6331b` (actual) | **AMD 780M** | **28,1** | **28,4** | **28,4–30,0** |

Shading GPU de un fotograma real de canción en la 780M (benchmark offline, exacto): 34 ms → 8 ms → 2,4–4,2 ms.

Informes: `analysis/native_visible_vulkan_resident_*_amd_en.json`, `analysis/native_paths_*.log`, `analysis/native_paths_*_frames.json`, `analysis/native_sample_profile_song_*.txt`.

Exactitud: 35 pruebas generales; cuatro suites Vulkan en AMD con 163 comparaciones completas de VRAM (2 MiB) entre CPU y GPU. Tras pasar a interpolación entera, la referencia CPU cambió de aritmética: los valores pueden diferir ±1 frente a binarios anteriores, pero CPU y GPU coinciden bit a bit.

---

## Fases del port completo (desde el inicio hasta la entrega)

- [x] ~~**1. Preparar el proyecto y conservar los originales.**~~
- [x] ~~**2. Comparar las versiones japonesa e inglesa.**~~ Comparten programa y audio; la traducción cambia `std.cpk`.
- [x] ~~**3. Inventariar y extraer los contenedores de datos.**~~ Lectores propios de CPK y FMDX.
- [x] ~~**4. Recuperar y analizar el ejecutable original.**~~
- [x] ~~**5. Primera conversión y compilación nativa.**~~ 126 unidades AOT compilan y ejecutan el comienzo original.
- [x] ~~**6. Completar el arranque y la adaptación a Windows.**~~ Arranque, menús, canciones, guardado, red y salida de Windows en un único ejecutable (puntos 1–6).
- [ ] **7. Dibujar los gráficos originales — EN CURSO.** Texturas, geometría, escenas y efectos de las canciones visibles a 272p–1088p en Vulkan (puntos 1 y 3); la fidelidad general se revisa en el punto 7.
- [x] ~~**8. Audio y vídeos originales.**~~ ATRAC3+ exacto, música continua y sincronizada (punto 4), latencia de salida reducida y ajustable (punto 5). Vídeos: el disco no contiene ninguno (ninguna entrada PSMF/MPEG en `std.cpk`; el juego solo llama a `sceMpegInit`).
- [x] ~~**9. Teclado y mando.**~~ Teclado, todos los tipos de mando, iconos según el dispositivo y reasignación (punto 5). Pendiente solo: probar con mandos físicos.
- [x] ~~**10. Menú original utilizable.**~~ Menú principal, Ajustes, selección de canción/personaje/objeto, historia, album!, save!, option! y つうしん! (puntos 2, 5 y 6).
- [x] ~~**11. Jugar una canción completa.**~~ Hotchkiss y Sunday Siesta superadas a 30/60/120/165 FPS (punto 4).
- [x] ~~**12. Guardar y recuperar el progreso.**~~ Sistema y DATA 1 recargan; el autoguardado tras END se verificó recargando (punto 4).
- [x] ~~**13. Completar y revisar el contenido y los modos.**~~ Las cinco partes superan la canción y los modos del club funcionan (punto 6); lo bloqueado lo desbloquea el progreso del juego.
- [x] ~~**14. Cambio de idioma desde Ajustes**~~ y paquetes de idioma extensibles (`assets/locales/<id>`; español futuro).
- [x] ~~**15. Multijugador adaptado a PC.**~~ Dos instancias juegan juntas con la misma puntuación de banda (punto 6). Pendiente: dos PC por LAN.
- [x] ~~**16. Ajustes y ventajas de PC:**~~ ventana/pantalla completa, resolución interna, FPS elegibles, idioma, sonido, controles, latencia y mods (puntos 2, 3 y 5).
- [ ] **17. Revisión completa** en ambos idiomas, ambas GPU y sesiones largas.
- [ ] **18. Entrega final** con instalación limpia verificada.

La traducción española completa es una ampliación posterior; el port base debe permitir añadirla sin volver a convertir el programa.

Documentos relacionados: [RENDERIZADO_VULKAN.md](RENDERIZADO_VULKAN.md), [COMPATIBILIDAD_GPU.md](COMPATIBILIDAD_GPU.md), [ESTADO_ACTUAL.md](ESTADO_ACTUAL.md), [PLAN_DEL_PORT.md](PLAN_DEL_PORT.md).
