"""Refresh local progress from a successful build of the currently verified binary."""
import hashlib
import json
from pathlib import Path
import re
from datetime import date

root = Path(__file__).resolve().parents[1]
report = json.loads((root / "analysis/native_boot_verification.json").read_text(encoding="utf-8"))
exe = root / "build/native_full/kon_boot_probe.exe"
with exe.open("rb") as f:
    digest = hashlib.file_digest(f, "sha256").hexdigest()
if digest != report["executable_sha256"]:
    raise RuntimeError("Progress evidence belongs to an older executable")
build = (root / "build/native_full/build_native.log").read_text(encoding="utf-8-sig")
match = re.search(r"100% tests passed out of (\d+)", build)
if not match or not report["bootstrap_checks_passed"]:
    raise RuntimeError("Latest complete build did not pass; refusing to mark it verified")
tests = int(match[1])
today = date.today().isoformat()
cases = [c for c in report["cases"] if c["bootstrap"]]
stops = "\n".join(f"- {c['language']}: `{c['stop']}`." for c in cases)
audio = json.loads((root / "analysis/native_audio_verification.json").read_text(encoding="utf-8"))
buffers = {}
for case in cases:
    log = (root / case["log"]).read_text(encoding="utf-8")
    buffers[case["language"]] = [int(n) for n in re.findall(r"HLE: AudioOutput[^\n]*source_nonzero_samples=(\d+)", log)]
audio["original_output"] = {"checked_at": report["checked_at"], "executable_sha256": digest,
    "native_pcm_calls_verified": all(buffers.values()), "nonzero_samples_per_buffer": buffers,
    "music_auditioned": False, "logs": [c["log"] for c in cases]}
(root / "analysis/native_audio_verification.json").write_text(json.dumps(audio, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
state = f"""# Estado del port â€” {today}

El port sigue en desarrollo. **El teclado real permite avanzar desde el aviso y el tÃ­tulo por la introducciÃ³n, completar las seis pÃ¡ginas del tutorial, abrir el menÃº principal y entrar en la selecciÃ³n de canciones. La vuelta al menÃº y la reentrada en Play estÃ¡n comprobadas. TodavÃ­a no se ha verificado una canciÃ³n completa.** El [roadmap](ROADMAP.md) conserva las fases terminadas y pendientes.

El binario ejecuta cÃ³digo original convertido a C++ nativo. Se detiene explÃ­citamente cuando alcanza una operaciÃ³n pendiente; no incluye un intÃ©rprete de CPU ni un lanzador de emulador.

## Progreso comprobado

- Un ejecutable carga el paquete japonÃ©s o inglÃ©s y comparte el sonido original. El montaje descubre paquetes adicionales bajo `assets/locales/<identificador>` sin recompilar; pruebas aisladas cubren `es` y `pt-br`. No existe todavÃ­a una traducciÃ³n espaÃ±ola ni un selector funcional en Ajustes.
- Las 126 unidades generadas compilan. El mÃ³dulo, `user_main` y tareas originales leen los dos contenedores y gestionan memoria, prioridades, bloqueos, esperas y callbacks.
- Los bloqueos ligeros usan el Ã¡rea PSP real de 32 bytes, incluidos los cambios que hace directamente el cÃ³digo original. La salida y eliminaciÃ³n de las tareas liberan sus pilas; la cancelaciÃ³n de semÃ¡foros conserva su cuenta inicial y devuelve el error de cancelaciÃ³n a los participantes en espera.
- El reloj usa Windows, su zona horaria y horario de verano. Las pruebas cubren precisiÃ³n, aÃ±os lÃ­mite y dÃ­a bisiesto. La direcciÃ³n de red se obtiene de un adaptador real del equipo.
- El arranque reserva cuatro canales de audio sobre una salida continua propia de Windows y configura tres contextos ATRAC3+. Los primeros buffers son silencio; pruebas posteriores al tÃ­tulo registran PCM original con muestras no nulas. **No se ha realizado una escucha de la mÃºsica.**
- Una muestra ATRAC3+ original produce 1081344 bytes PCM idÃ©nticos a la referencia. Este resultado independiente no demuestra reproducciÃ³n de una canciÃ³n.
- El juego inicializa el decodificador H264 real. La reproducciÃ³n de sus vÃ­deos sigue pendiente.
- Se ejecutan listas grÃ¡ficas originales, borrados y texturas RGB, indexadas y DXT1, con swizzle, paletas y filtrado. El dibujado propio admite triÃ¡ngulos de color uniforme con transparencia, culling, pruebas de alfa/profundidad, mÃ¡scaras y recorte; las pruebas verifican que no se dibuja dos veces un borde compartido.
- Se corrigiÃ³ un fallo del generador que omitÃ­a servicios PSP llamados desde el mismo bloque nativo. Las pruebas cubren llamadas directas e indirectas dentro de un bloque y entre bloques.
- El guardado AUTOLOAD/AUTOSAVE conserva los datos originales sin cifrar, metadatos e imÃ¡genes en un contenedor de PC `.konpc`, con versiÃ³n, comprobaciÃ³n de integridad y escritura mediante archivo temporal. Las pruebas recargan desde una instancia nueva y rechazan datos daÃ±ados o destinos pequeÃ±os. El cÃ³digo original ya creÃ³ `SYSTEM00` en japonÃ©s y el ejecutable inglÃ©s lo recargÃ³ tras volver a arrancar. Importar partidas PSP cifradas y comprobar progreso de canciones siguen pendientes.
- La partida `DATA 1` guardada despuÃ©s del tutorial se recargÃ³ desde la interfaz original en una instancia nueva y entrÃ³ directamente al menÃº del club. Se conservan copias inicial y posterior, y evidencia del archivo, su integridad y la recarga. Las prÃ³ximas pruebas usan este punto; todavÃ­a no hay progreso de canciÃ³n verificado.
- El audio ATRAC3+ por paquetes coincide exactamente con el PCM de referencia. Las pruebas cubren bÃºferes circulares, muestras iniciales descartadas, final de pista y reinicio/bÃºsqueda en cinco posiciones. El reinicio de bucles con precarga circular pasa comparaciones PCM exactas en bucles finitos e infinitos y tamaÃ±os de bÃºfer no alineados. Los registros del juego original ya muestran reinicios reales y decodificaciÃ³n posterior sin error. Escucha, sincronizaciÃ³n y canciÃ³n completa siguen pendientes.
- La lectura original de controles consume muestras con sus tiempos reales. Los doce botones de juego PSP y el stick estÃ¡n mapeados al teclado; las pruebas cubren todos los botones, combinaciones, diagonales y direcciones opuestas. Enter, X y las flechas se han utilizado en la ventana real; la guÃ­a F1 se muestra y oculta correctamente. XInput estÃ¡ conectado pero no se ha probado con un mando fÃ­sico. La reasignaciÃ³n y las acciones durante una canciÃ³n siguen pendientes. VÃ©ase [CONTROLES.md](CONTROLES.md).
- Las ventanas JA/EN muestran FPS cada segundo, dentro de la imagen y en el tÃ­tulo. Se ha inspeccionado el contador en una ventana real; cuenta entregas de framebuffer presentadas, sin sumar repintados de Windows.
- El procesamiento grÃ¡fico se separa del envÃ­o de listas y usa una pila propia para interrupciones. El handler compilado preserva los registros de la tarea interrumpida. La proyecciÃ³n propia usa matrices de modelo/vista/perspectiva, subpÃ­xeles, UV con correcciÃ³n de perspectiva, profundidad/color interpolados, luz ambiente y recorte cercano. La mezcla de huesos admite pesos de 8/16 bits o float. Una captura de ejecuciÃ³n original ya muestra la sala del club, cuatro personajes y el primer diÃ¡logo. Se corrigiÃ³ la inversiÃ³n del descarte de caras que ocultaba superficies. La fidelidad completa sigue pendiente; el menÃº principal sÃ­ se recorriÃ³ despuÃ©s con teclado.
- Se conserva cada pulsaciÃ³n breve del teclado hasta que estÃ¡ presente en todo un lote leÃ­do por el juego. La ventana presenta su imagen y contador mediante un bÃºfer de pintura propio para evitar repintados parciales.
- El menÃº principal y la selecciÃ³n de canciones ya se recorren con teclado. Dibujo general y efectos 3D, mÃºsica/vÃ­deos completos, canciÃ³n completa, recuperaciÃ³n de progreso de partidas, idiomas en Ajustes y multijugador siguen pendientes.

## Ãšltima verificaciÃ³n

**{tests} de {tests} pruebas aprobadas**, con ejecuciÃ³n JA/EN y pruebas de los servicios nativos. Ãšltima integraciÃ³n: `{report['checked_at']}`.

Paradas comprobadas:

{stops}

Hotchkiss ya supera la mezcla RGB inicial, la consulta de auriculares, el recorte cercano y las esperas de audio/semÃ¡foros. Una ejecuciÃ³n con teclado mostrÃ³ Bus Stop y notas originales. La ejecuciÃ³n posterior sin pulsaciones de notas llegÃ³ a los resultados originales: NO RANK / TOO BAD, 11 Miss y puntuaciÃ³n 0. La prueba sintÃ©tica con botones mostrÃ³ juicios Great en el escenario y Perfect/Great en resultados; su animaciÃ³n quedÃ³ incompleta al agotarse el tiempo de diagnÃ³stico. No acredita una canciÃ³n completada con Ã©xito, teclado fÃ­sico ni sincronizaciÃ³n. El estado de audio y WaitThreadEnd/CB conservan el evento y resultados reales, comprobados en producciÃ³n. Las ejecuciones histÃ³ricas del escenario registraron aproximadamente 12,3 FPS, 17,8 tras optimizaciones de geometrÃ­a y 15,1 con esperas mÃ¡s precisas; son escenas y ejecuciones distintas, sin comparaciÃ³n controlada de FPS. La integraciÃ³n del nuevo mezclador detectÃ³ una regresiÃ³n con una tarea de audio en bucle e interrupciones desactivadas. La entrega diferida de interrupciones GE permitiÃ³ que el ensayo posterior de 240 segundos avanzase por el escenario y llegase a GAME OVER con pulsaciones sintÃ©ticas. Otra prueba posterior volviÃ³ a reproducir un bloqueo con interrupciones desactivadas: la correcciÃ³n es parcial y queda pendiente localizar la transiciÃ³n que causa esta variante. Se midieron 8,29 presentaciones por segundo y unas 40.340 muestras originales reproducidas por segundo frente a las 44.100 del dispositivo. Sigue habiendo huecos en la entrega de sonido; no acredita sincronizaciÃ³n ni canciÃ³n completada. El filtrado bilineal empaquetado conserva el BMP exacto y mejora un dibujo aislado alrededor del 6 %, sin demostrar una mejora robusta de FPS del juego. La rasterizaciÃ³n admite de uno a diecisÃ©is trabajadores nativos, cuatro por defecto, y una barrera por dibujo, con filas independientes y orden de mezcla conservado. Las pruebas comparan toda la VRAM y el BMP original exacto; las texturas que comparten memoria con la salida mantienen la ruta serial. La geometrÃ­a reutiliza memoria y recorta con almacenamiento acotado. Se han detectado las GPU AMD y NVIDIA y sus capacidades D3D11, el juego conserva 480Ã—272 internos. Vulkan ejecuta un shader propio de borrado; el primer ensayo se realizo en Radeon 780M: 8192 palabras exactas y 17 palabras protegidas comprueban la ejecucion aislada. El backend opcional `--vulkan-clear` se conecta al mismo renderizador del juego; 67 comparaciones de toda la VRAM validan los cuatro formatos, profundidad, mascaras y actualizaciones de memoria. La ruta opcional texturada aÃ±ade muestreo, perspectiva, mezcla, alfa y profundidad a GPU, conservando CPU para geometrÃ­a, decodificaciÃ³n de texturas, realimentaciÃ³n/alias y presentaciÃ³n GDI. Agrupa dibujos con almacenamiento local GPU, cachÃ© exacta de texels y compromiso al terminar. Las cuatro pruebas hardware y sus hashes se registran en `analysis/native_vulkan_integration_verification.json`; Vulkan completo y FPS superiores siguen pendientes. La evidencia del recorrido original se conserva separada de estas pruebas. Las pruebas de desarrollo actuales se centran en AMD Radeon 780M. La comprobación anterior en NVIDIA se conserva como histórica; se revisará de nuevo en el hito posterior acordado. El lector propio de charts reconstruye 759 partituras distintas con bytes idÃ©nticos; la unidad temporal y el significado completo de sus campos quedan pendientes. Las pruebas usan `work/song_regression_poll_20261005/ms0`, una copia aislada del guardado posterior al tutorial. CanciÃ³n completa, escucha, sincronizaciÃ³n, progreso de canciones y ventajas de PC siguen pendientes.

## Evidencia

- `analysis/native_boot_verification.json` y los registros JA/EN.
- `analysis/native_audio_verification.json`: PCM original y buffers realmente enviados.
- `build/native_full/build_native.log`: compilaciÃ³n y pruebas.
- `analysis/native_codegen_provenance.json`: identidad y generaciÃ³n del programa.
- `docs/PROCEDENCIA.md`: componentes y referencias de contratos.
- `analysis/native_keyboard_batch_before_ja.log`: llegada real a la primera operaciÃ³n 3D pendiente.
- `analysis/native_3d_pending_ja.log`: otra ruta de arranque que llega a guardado automÃ¡tico de modo 1; el nombre del archivo no acredita geometrÃ­a dibujada.
- `analysis/native_projection_savedata_ja.log/.bmp`: creaciÃ³n real del guardado de sistema y selector de espacios de partidas.
- `analysis/native_projection_weight_pending_en.log`: recarga del sistema creado en JA y ejecuciÃ³n del primer dibujo 3D; siguiente parada en geometrÃ­a animada de 74 vÃ©rtices, formato 0x4583.
- `analysis/native_culling_scene_en.log/.bmp`: ejecuciÃ³n original de la sala y el primer diÃ¡logo, tras corregir el descarte de caras; parada al alcanzar el lÃ­mite de diagnÃ³stico de 160 segundos.
- `analysis/native_loop_club_en.log/.bmp`: ejecuciÃ³n original con entradas sintÃ©ticas hasta el tutorial de tensiÃ³n, pÃ¡gina 6/6; no es evidencia de teclado fÃ­sico ni de una canciÃ³n completa.
- `analysis/native_fast_loop_club_en.log/.bmp`: mismo tutorial con el acceso grÃ¡fico optimizado; contiene `AtracLoopRestart` y decodificaciÃ³n posterior correcta. Parada intencionada a los 300 segundos; no acredita una canciÃ³n completa.
- `analysis/native_first_song_blend_pending_en.log/.json/.kondraw`: recorrido con teclado fÃ­sico hasta menÃº, selecciÃ³n y carga de Hotchkiss / Normal / Yui / Bus Stop / No Item; parada grÃ¡fica al entrar al escenario. Captura conservada antes de cambiar de ejecutable.
- `analysis/native_first_song_blend_replay_en.bmp`: reproducciÃ³n aislada del dibujo capturado con la mezcla admitida; cero pÃ­xeles cubiertos por su matriz degenerada. No es una prueba de canciÃ³n.
- `analysis/native_saved_menu_verification.json`: carga real de DATA 1 posterior al tutorial, con acceso directo al menÃº en el nuevo ejecutable.
- `analysis/native_song_eye_plane_pending_en.log/.json/.kondraw/.bmp`: carga con teclado hasta la pantalla original de inicio y bloqueo al confirmar, en recorte cercano de geometrÃ­a.
- `analysis/native_song_eye_plane_replay_en.bmp`: dibujo exacto de esa captura despuÃ©s de corregir el recorte; cielo original y 130560 pÃ­xeles dibujados. No ejecuta la canciÃ³n completa.
- `analysis/native_song_sema_status_pending_en.log/.bmp/.json`: escenario y notas originales; SleepThreadCB/WakeupThread superados y parada en ReferSemaStatus, con identidad del ejecutable conservada. La canciÃ³n completa sigue pendiente.
- `analysis/native_song_poll_sema_pending_en.log/.bmp/.json`: ReferSemaStatus superado y parada WaitSemaCB; el nombre histÃ³rico del archivo dice poll, pero el NID identifica WaitSemaCB.
- `analysis/native_song_wait_cb_correct_mount_en.log/.bmp/.json`: escenario y notas; WaitSemaCB superado en ejecuciÃ³n original y siguiente parada en estado de la tarea de audio. Entrada sintÃ©tica; no acredita canciÃ³n completa.
- `analysis/native_song_audio_event_en.log/.bmp/.json`: resultados originales de fallo, 11 Miss y puntuaciÃ³n 0; consulta de la tarea de audio superada.
- `analysis/native_song_input_stress_en.log/.bmp/.json`: pulsaciones sintÃ©ticas con juicios Perfect/Great y medida de unas 12,3 presentaciones por segundo. No acredita canciÃ³n completada.
- `analysis/native_packed_bilinear_verification.json`: dibujo aislado con BMP exacto y tiempos antes/despuÃ©s.
- `analysis/music_chart_layout_audit_en.json`: 759 charts distintos con round-trip idÃ©ntico, sin prueba de mecÃ¡nica.
- `analysis/native_song_wait_sema_cb_regression_en.json`: ensayo sintÃ©tico que quedÃ³ en el tÃ­tulo por un error de montaje del guardado; no es evidencia de canciÃ³n.
- `analysis/native_edge_increment_verification.json`: comparaciÃ³n exacta de captura antes/despuÃ©s, sin afirmaciÃ³n de mejora de FPS.

- `analysis/native_parallel_raster_final_verification.json`: comparaciÃ³n de VRAM/dibujo original entre uno y cuatro trabajadores.
- `analysis/native_reused_geometry_isolated_verification.json`: BMP exacto y tiempo de dibujo aislado; no demuestra FPS del juego.
- `analysis/native_gpu_capabilities.json`: GPU detectadas por Direct3D; backend GPU del juego todavÃ­a pendiente.
- `analysis/native_vulkan_capabilities.json`: prueba historica de dispositivo y transferencia GPU.
- `analysis/native_vulkan_shader_verification.json`: ejecucion real del shader propio y limites exactos.
- `analysis/native_vulkan_renderer_verification.json`: borrados GPU del renderizador, 67 comparaciones exactas de toda la VRAM. No es un recorrido del juego.
- `docs/RENDERIZADO_VULKAN.md`: alcance, activacion y limites de la integracion.
- `analysis/native_song_cri_mask_trace_en.json`: resultados originales de fallo con 1 Great, 11 Miss y 100 puntos; esa ejecuciÃ³n no reprodujo el bloqueo, que continÃºa siendo intermitente.
- `analysis/native_song_preroll6_en.json`: hipÃ³tesis de introducciÃ³n de 6 s, GAME OVER y resultados de fallo con 11 Miss; no valida temporizaciÃ³n ni canciÃ³n completa.
- `analysis/hotchkiss_timing_hypothesis.json`: duraciÃ³n original de 111,245 s y posible introducciÃ³n de 6 s; hipÃ³tesis de reloj y pulsaciones que no logrÃ³ completar la canciÃ³n en la prueba posterior.
- `analysis/native_scheduler_timer_verification.json`: espera privada de 1 ms, mediana de aproximadamente 1,5 ms frente a 14,1 ms de la espera anterior; no mide sincronizaciÃ³n musical.
- `analysis/native_song_resume_retry_en.json/.bmp`: pausa, reanudaciÃ³n y reintento observados con entradas sintÃ©ticas en un binario anterior.
- `analysis/native_audio_continuous_warm_stream_probe.json`: prueba silenciosa de reproducciÃ³n real de muestras, cerca de 44.100 por segundo con bloques PSP de 256/512/1024/2048; no es una escucha ni una canciÃ³n.
- `analysis/native_song_first_note_anchor_en.json`: segunda hipÃ³tesis de pulsaciones; bloqueo intermitente reproducido.
- `analysis/native_projection_cache_verification.json`: BMP idÃ©ntico y menos proyecciones de vÃ©rtices compartidos; no mide FPS del juego.
- `analysis/native_decoded_texture_cache_verification.json`: BMP exacto y comparaciÃ³n aislada con cachÃ© activada/desactivada.
- `analysis/native_song_mixer_masked_irq_en.json`: una ejecuciÃ³n superÃ³ el bloqueo anterior, GAME OVER original, FPS y entrega de fuente medidos; canciÃ³n completa pendiente.
- `analysis/native_song_continuous_mixer_en.json` y `analysis/native_song_mixer_semaphore_trace_en.json`: regresiÃ³n del nuevo audio, escenario detenido y espera de semÃ¡foro repetida con interrupciones desactivadas.

El mezclador propio conserva la copia del PCM y su reproducciÃ³n real como estados distintos. Las muestras se suman con precisiÃ³n fraccionaria antes del recorte final; las pruebas comprueban seÃ±ales muy bajas, mono/estÃ©reo, lÃ­mites y reinicio de canales. La salida continua introduce aproximadamente 93 ms de bÃºfer inicial que necesitan calibraciÃ³n. Las interrupciones GE pendientes se adaptan para respetar las secciones crÃ­ticas de CPU; su prueba de producciÃ³n comprueba entrega Ãºnica y conservaciÃ³n de registros y mÃ¡scara. Una ejecuciÃ³n posterior superÃ³ el bloqueo, pero otra lo reprodujo de nuevo; no se considera resuelto ni se confirma una canciÃ³n completada. La cachÃ© de texturas por dibujo mantiene el BMP exacto y reduce de 5,54 a 3,86 ms la mediana de un dibujo aislado en 500 repeticiones; no acredita los FPS de una canciÃ³n. Las entradas temporizadas desde un canal de audio son exclusivamente diagnÃ³sticas y todavÃ­a requieren verificar el reloj y la correspondencia de notas.

Las ISO permanecen intactas. Todo el trabajo estÃ¡ en esta carpeta y no se ha publicado.
"""
state += "\nLa cache persistente experimental conserva hasta 16 entradas y 8 MiB de datos. Compara exactamente los bytes y la paleta antes de reutilizar una textura; escrituras originales, formatos y realimentacion conservan la referencia. En el dibujo aislado evita 499 de 500 decodificaciones y mantiene el BMP exacto, pero los tiempos variaron y no prueban una mejora robusta. Esta desactivada por defecto; `--persistent-texture-cache` permite verificarla dentro del juego. Evidencia: `analysis/native_persistent_texture_verification.json`.\n\nEl historial de interrupciones conserva las ultimas 128 observaciones y se congela ante la primera restauracion de una mascara cero desde un estado habilitado. Su prueba comprueba el limite, el registro de la transicion y que el diagnostico no repara ni modifica la mascara. Los dos ensayos de 230 segundos `analysis/native_song_interrupt_history_en.json` y `analysis/native_song_retry_interrupt_history_en.json` no reprodujeron el bloqueo intermitente; no lo consideran corregido y no verifican una cancion completa. La lectura de controles rechaza correctamente recuentos de 64 o mas sin escribir ni cambiar la tarea.\n"
gpu_evidence_path = root / "analysis/native_vulkan_integration_verification.json"
if gpu_evidence_path.exists():
    gpu_evidence = json.loads(gpu_evidence_path.read_text(encoding="utf-8"))
    game_gpu = gpu_evidence.get("game_run")
    if game_gpu and game_gpu["executable_sha256"] == digest:
        state += (f"\nLa ejecucion original con Vulkan realizo {game_gpu['actual_submissions']} envios reales a {game_gpu['adapter']} y termino en {game_gpu['stop']}. "
                  "Es un recorrido diagnostico con entradas sinteticas. La medicion de FPS se registra por separado con su referencia y el mismo tramo de cancion. "
                  "Vulkan permanece desactivado por defecto; texturas, mezcla, profundidad y presentacion siguen en CPU. "
                  "Evidencia: `analysis/native_vulkan_integration_verification.json`; alcance: [RENDERIZADO_VULKAN.md](RENDERIZADO_VULKAN.md).\n")

triangle_evidence_path = root / "analysis/native_vulkan_triangle_verification.json"
if triangle_evidence_path.exists():
    triangle_evidence = json.loads(triangle_evidence_path.read_text(encoding="utf-8"))
    if triangle_evidence.get("native_flat_triangle_gpu_verified"):
        state += "\nLa nueva ruta `--vulkan-triangles` comprueba 121 comparaciones completas de VRAM, 119 envios reales de prueba, ocho funciones alfa, cuatro formatos y bordes compartidos. Conserva CPU para texturas, mezcla, profundidad, colores interpolados, coordenadas extremas y accesos espejados. La comparacion sintetica de 32 triangulos planos superpuestos da unos 8,57-9,84 ms en CPU frente a 1,28-1,36 ms en GPU; con dos triangulos no hay una mejora estable. Estos tiempos no miden FPS ni la escena original del juego. Permanece desactivada por defecto. Evidencia: `analysis/native_vulkan_triangle_verification.json` y `analysis/native_flat_triangle_timing_verification.json`.\n"
scene_evidence_path = root / "analysis/native_song_stage_gpu_capture_en.json"
if scene_evidence_path.exists():
    scene = json.loads(scene_evidence_path.read_text(encoding="utf-8"))
    if scene.get("executable_sha256") == digest:
        reference = scene["cpu_reference"]
        state += (f"\nLa captura representativa conserva un dibujo original de {scene['snapshot']['vertices']} vertices: {reference['covered_pixels']} pixeles cubiertos y {reference['drawn_pixels']} escritos en CPU, con referencia completa de 2 MiB de VRAM para comparar la siguiente ruta GPU. "
                  f"El recorrido de 145 segundos hizo {scene['actual_gpu_triangle_submissions']} envios de triangulos GPU: la escena mantiene CPU porque necesita texturas, mezcla y profundidad. "
                  f"Se observaron {scene['fps_observation']['fps']:.2f} FPS en un tramo, sin comparacion controlada ni prueba de mejora. Cancion completa y sincronizacion siguen pendientes. Evidencia: `analysis/native_song_stage_gpu_capture_en.json`.\n")
cache_text = (root / "build/native_full/CMakeCache.txt").read_text(encoding="utf-8")
if "KON_OPTIMIZED_AOT_UNITS:STRING=ALL" in cache_text:
    state += "\nEl corpus recompilado completo y su registro se compilan con `-O2`, sin activar fast-math. La compilacion con O0 o unidades seleccionadas sigue disponible para diagnostico. Las pruebas de arranque JA/EN pertenecen al hash actual; no sustituyen la validacion de todos los recorridos del juego.\n"
state += "\nEl reparto paralelo de filas se limita al area vertical de la geometria validada, conservando orden y filas disjuntas. Dos dibujos originales conservan exactamente los 2 MiB de VRAM; la pieza 2D reduce su tiempo aislado entre un 29 y un 37 %, mientras el dibujo 3D no cambia de forma significativa. Esto no equivale a esa mejora en FPS. Evidencia: `analysis/native_roi_isolated_verification.json`.\n"
state += "\nSe corrigio el NID `sceDisplay:0x77ED8B3A`: requiere `sceDisplayWaitVblankStartMultiCB`, no consultar el framebuffer. Ahora suspende su tarea el numero de refrescos solicitado y conserva callbacks y continuacion. Pruebas del planificador real verifican la espera de dos periodos y rechazan conteos invalidos. La aceleracion observada por el usuario y los ensayos anteriores quedan registrados en `analysis/native_user_speed_report_20261005.json`; sus FPS no certifican velocidad normal. Verificar todas las animaciones y desacoplar FPS de logica/audio siguen pendientes.\n"
perf_path = root / "analysis/native_performance_release_roi_20261005.json"
if perf_path.exists():
    perf = json.loads(perf_path.read_text(encoding="utf-8"))
    candidates = [run for run in perf["runs"] if run["label"] == "candidate"]
    if perf.get("comparison_completed") and candidates and all(run["executable_sha256"] == digest for run in candidates):
        gains = perf["paired_fps_gain_percent"]
        state += ("\nComparacion repetida de rendimiento con copias identicas de partida, las mismas entradas sinteticas y ambas versiones con la espera de pantalla corregida: "
                  + ", ".join(f"{gain:.1f}%" for gain in gains) + " de cambio en FPS del intervalo medido tras el inicio del audio original. "
                  "Los cambios comparados son O2 en todo el corpus y el reparto de filas por geometria. Ambos recorridos activan las mismas opciones Vulkan. Los tiempos y fotogramas presentados se registran cada 100 ms; las capturas permiten comprobar la escena. Es una prueba headless acotada, no una cancion completada ni una validacion perceptiva de velocidad/sincronizacion. "
                  "Evidencia: `analysis/native_performance_release_roi_20261005.json`; cambios: [CAMBIOS_RECIENTES.md](CAMBIOS_RECIENTES.md).\n")
state += "\n## Renderizado independiente pendiente\n\nEl reloj y la interpolaciÃ³n de geometrÃ­a se prueban a 30/60/120/165 y tasas personalizadas. Falta conectarlos a fotogramas completos del juego; no existe todavÃ­a un ajuste de FPS que garantice velocidad, mÃºsica y notas correctas. El programa original conserva sus esperas. VÃ©ase [RENDERIZADO_VULKAN.md](RENDERIZADO_VULKAN.md).\n"
(root / "docs/ESTADO_ACTUAL.md").write_text(state, encoding="utf-8")
path = root / "docs/ROADMAP.md"
lines = path.read_text(encoding="utf-8").splitlines()
for n, line in enumerate(lines):
    if line.startswith("Actualizado:"):
        lines[n] = f"Actualizado: {today}."
    elif line.startswith("**Estado actual:**"):
        lines[n] = f"**Estado actual:** ejecuciÃ³n original de aviso, tÃ­tulo, introducciÃ³n animada, tutorial completo, menÃº principal y selecciÃ³n de canciones, recorridos con teclado real. Las Ãºltimas **{tests} pruebas** pasan. CanciÃ³n completa todavÃ­a pendiente."
    elif line.startswith("- [ ] **10."):
        lines[n] = "- [ ] **10. Llegar al menÃº original y poder usarlo - EN CURSO.** IntroducciÃ³n y tutorial terminados con teclado; menÃº principal, entrada en Play, selecciÃ³n de canciones y vuelta al menÃº comprobados. Falta recorrer las demÃ¡s pantallas y modos accesibles sin bloqueos."
    elif line.startswith("- [ ] **9."):
        lines[n] = "- [ ] **9. Incorporar teclado y mando - EN CURSO.** Doce botones y stick PSP mapeados al teclado, combinaciones comprobadas y guÃ­a F1 visible. Teclado usado en tÃ­tulo y diÃ¡logos reales; XInput conectado. Faltan mando fÃ­sico, reasignaciÃ³n y acciones durante una canciÃ³n."
    elif line.startswith("| GrÃ¡ficos iniciales"):
        lines[n] = "| GrÃ¡ficos iniciales | Texturas RGB/indexadas/DXT1, proyecciÃ³n, perspectiva, luz ambiente y huesos. Captura original de la sala y cuatro personajes; fidelidad general pendiente. |"
    elif line.startswith("- [ ] **12."):
        lines[n] = "- [ ] **12. Guardar y recuperar el progreso - EN CURSO.** Sistema compartido JA/EN y DATA 1 posterior al tutorial recargados desde una instancia nueva; acceso directo al menÃº comprobado. Falta recuperar progreso de canciones, ajustes y desbloqueos tras cerrar el programa."
    elif line.startswith("| Dibujo y ventana"):
        lines[n] = "| Dibujo y ventana | TÃ­tulo, tutorial, menÃº principal y selecciÃ³n de canciones recorridos con teclado fÃ­sico. Bus Stop y notas originales visibles. Contador de FPS verificado; canciÃ³n completa y fidelidad general pendientes. |"
    elif line.startswith("| Dispositivo de sonido"):
        lines[n] = "| Dispositivo de sonido de Windows | Cuatro canales sobre un mezclador propio y salida continua de Windows. Copia de fuente y reproducciÃ³n real medidas por separado; bloqueo intermitente con interrupciones desactivadas y entrega de fuente todavÃ­a insuficiente en Hotchkiss. Escucha y sincronizaciÃ³n pendientes. |"
    elif line.startswith("| Renderizado GPU"):
        lines[n] = "| Renderizado GPU | RasterizaciÃ³n texturada opcional: perspectiva, filtrado, mezcla, alfa y profundidad en GPU. Cuatro pruebas hardware y comparaciones completas de VRAM, con lotes ordenados. GeometrÃ­a, decodificaciÃ³n, feedback y ventana siguen CPU; backend completo pendiente. VÃ©ase [RENDERIZADO_VULKAN.md](RENDERIZADO_VULKAN.md). |"
    elif line.startswith("| PreparaciÃ³n de decodificadores"):
        lines[n] = "| PreparaciÃ³n de decodificadores | Tres contextos ATRAC3+ y llamada original de inicializaciÃ³n H264. Falta reproducciÃ³n y sincronizaciÃ³n dentro del juego. |"
    elif line.startswith("**Trabajo inmediato:**"):
        lines[n] = '**Trabajo inmediato:** completar y acelerar Vulkan, y conectar un hilo de renderizado independiente para 30/60/120/165 FPS y valores personalizados sin acelerar lógica, audio, notas ni puntuación. Las pruebas de desarrollo se centran en AMD Radeon 780M; NVIDIA se revisará en el hito posterior acordado, manteniendo un único ejecutable y selección automática. La reconstrucción privada de tres fotogramas de canción coincide con el framebuffer original. La presentación independiente y la sincronización a distintas tasas siguen pendientes. Véanse [RENDERIZADO_VULKAN.md](RENDERIZADO_VULKAN.md) y [ESTADO_ACTUAL.md](ESTADO_ACTUAL.md).'
path.write_text("\n".join(lines)+"\n", encoding="utf-8")
print(f"Progress refreshed from verified executable; {tests} tests; gameplay remains unverified")
