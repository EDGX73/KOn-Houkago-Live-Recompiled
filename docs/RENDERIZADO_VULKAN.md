# Renderizado Vulkan en desarrollo

Prioridad acordada: Vulkan completo y ventajas de FPS, con desarrollo y mediciones en AMD Radeon 780M. NVIDIA se revisará en el hito posterior, manteniendo un único ejecutable de selección automática.

## Ruta actual

`--vulkan` conecta borrados y rasterización texturada propia: filtrado nearest/bilinear, perspectiva, combinación de texturas, mezcla, alfa, profundidad y máscaras de los cuatro formatos. `--vulkan-resident-batch` conserva destinos entre turnos gráficos y publica antes de los accesos originales que dependen de ellos. Una propiedad explícita de VRAM impide leer o sobrescribir resultados incompletos. Cuando el almacenamiento fijo se llena, se termina el grupo anterior y se reintenta el dibujo actual con la memoria publicada.

La GPU mantiene almacenamiento local y paquetes inmutables. Las texturas solo se reutilizan tras comparar sus bytes y la caché decoded valida fuente y CLUT. La ruta GPU usa una caché de 128 entradas y 64 MiB; el límite CPU por defecto permanece en 16 y 8 MiB. Los fotogramas privados comparten recursos inmutables idénticos dentro del fotograma, con límites explícitos de memoria.

La grabación de lotes enlaza pipeline y descriptores una vez. Los paquetes nuevos y las extensiones de destino no se solapan con trabajos anteriores; sus transferencias se permiten antes de esperar al dibujo anterior. Una dependencia combinada conserva el orden de lectura/escritura de color y profundidad antes del siguiente despacho. Las escrituras coherentes de staging terminan antes de enviar el lote. Esta reducción de trabajo conserva las cuatro suites hardware AMD y 161 comparaciones completas de rasterización; no se atribuye una ganancia de FPS sin medirla.

Quedan en CPU transformación y recorte de geometría, interpretación de estado, decodificación de texturas, feedback/alias y presentación GDI. Todavía no hay swapchain Vulkan. La ruta precisa exige float64 e int64; una capacidad ausente produce un diagnóstico explícito. **Vulkan completo y rendimiento estable siguen pendientes.**

## FPS y dibujo independiente

`NativeRenderTimeline` admite 30/60/120/165 y tasas personalizadas, sin despachar lógica original ni cambiar audio. `NativeRenderWorker` posee un hilo, una GPU y memoria privada; recibe fotogramas inmutables sin esperar a una rasterización bloqueada, conserva solo las últimas fuentes, omite trabajo antiguo y devuelve salidas terminadas. Interpola únicamente comandos compatibles y no cuenta repintados de una fuente congelada como fotogramas nuevos. Los fallos se devuelven al consumidor sin escribir la VRAM original.

Las 35 pruebas generales incluyen bloqueo artificial de GPU mientras el productor sigue publicando, cambios de tasa, interpolación intermedia, extremo exacto, cola acotada y errores de creación/renderizado. La prueba adicional con Vulkan real en AMD reconstruye un dibujo original en su hilo y compara los 2 MiB completos. Evidencia: `analysis/native_render_worker_integration_verification.json` y `analysis/native_vulkan_render_worker_verification.json`.

El ensayo visible de canción reconstruyó tres fotogramas con 180, 192 y 213 comandos, cero rechazos y framebuffer idéntico, sin ejecutar lógica original privada. Binario de ese ensayo: `4a7dccea796d5e76e18ec259d80f97d2158cdd2fccc7e75c5889ffd3e8ab0ccb`. Informe: `analysis/native_visible_vulkan_resident_frame_private_ordered_20261005_amd_en.json`. El ensayo añade reconstrucciones y no es una medida controlada de ganancia de FPS.

**Falta conectar este propietario independiente a la ventana y validar notas, audio, velocidad y puntuación de canciones a distintas tasas. No hay todavía un selector de FPS funcional para jugar a 60/120/165.** Los contadores de las capturas actuales miden presentaciones originales.

## Rendimiento observado

La agrupación residente inicial mejoró el tramo AMD de 5,33 a 7,02 FPS (+31,69 %). El ensayo posterior de caché dio 7,09 FPS, sin mejora notable. La ruta parcial histórica rondaba 20,6 FPS y continúa siendo más rápida. Estos números corresponden a sus informes y hashes; no son promesas para el binario actual ni canciones completas. Se conservarán las referencias anteriores y se harán pruebas AMD centradas en cambios que lo justifiquen.

## Historial anterior

Los apartados siguientes conservan mediciones de versiones anteriores. Sus afirmaciones de operaciones pendientes y hashes corresponden a esas versiones.


El ejecutable permite activar los borrados propios de GPU con `--vulkan-clear`. La opcion esta desactivada por defecto y requiere un controlador Vulkan instalado. El shader se genera e incorpora automaticamente al compilar; no es necesario instalar el SDK de Vulkan para ejecutarlo.

La primera ruta GPU resuelve borrados originales de color (565, 5551, 4444 y 8888) y profundidad de 16 bits. La CPU interpreta los registros graficos, valida vertices y recorta los rectangulos; Vulkan aplica sus mascaras a los pixeles, en el orden de envio. La CPU conserva texturas, mezcla, profundidad y presentacion en la ventana. Los triangulos planos tienen ahora una ruta GPU opcional descrita al final. No constituye todavia un backend grafico GPU completo.

Los recursos Vulkan se reutilizan. Antes de cada envio se copia el destino actualizado, porque el codigo original puede escribir directamente en su memoria; despues se espera la finalizacion real y se recupera el resultado. Si color y profundidad comparten memoria, si el destino cruza un espejo de VRAM o si estan activadas las vigilancias de escritura, se conserva el dibujo CPU. Un fallo de GPU detiene el diagnostico sin publicar un destino incompleto.

Esta estrategia sirve para integrar y comprobar exactitud. Las copias, la espera y la recuperacion por dibujo pueden costar mas que un borrado en CPU: no hay una mejora de FPS demostrada. Para acelerar las escenas se necesitan destinos y texturas residentes en GPU, triangulos/effects nativos, agrupacion de dibujos y sincronizacion de lecturas originales sin recuperar toda la imagen continuamente. La resolucion y la frecuencia de dibujo configurables tambien requieren separar logica, audio y presentacion.

Los contadores `gpu_clear_primitives`, `gpu_clear_pixels` y `native_vulkan_clear_submissions` registran trabajo real del juego. Un dispositivo inicializado o un shader probado por separado no se cuentan como ejecucion grafica original.

Evidencia aislada: `analysis/native_vulkan_shader_verification.json` y `analysis/native_vulkan_renderer_verification.json`. La primera prueba comprueba tambien 17 palabras protegidas tras un despacho redondeado. La segunda compara 67 veces los 2 MiB de VRAM completos entre CPU y GPU e inyecta un error antes de comprometer la memoria. No verifican una cancion completa ni la mejora de FPS del juego.

Compilacion normal: `tools/build_native_probe.ps1 -FullAot -Jobs 2`. Las pruebas de hardware se pueden incluir en CTest con `KON_GPU_TESTS=ON`; se mantienen opcionales para que una maquina sin Vulkan pueda ejecutar las pruebas de CPU.


## Medida de la primera ruta de borrado

En un borrado sintetico de color/profundidad, 480×272, ocho franjas y 100 repeticiones por ensayo (con ocho de calentamiento), las tres medianas fueron 12,91/12,96/13,02 ms en el primer kernel y 11,42/11,54/11,27 ms al limitar cada despacho al rectangulo afectado. La CPU obtuvo 0,493/0,490/0,496 ms. Todas las salidas se compararon exactamente con la referencia CPU. La reduccion de trabajo del shader mejora esta ruta experimental, pero sigue siendo mucho mas lenta que la CPU; permanece desactivada por defecto. No es una comparacion de FPS del juego. Se conservan todos los ensayos en `analysis/native_vulkan_clear_timing_verification.json`.

Las invocaciones que sobran al redondear el ancho de los grupos salen antes de acceder al destino. Ademas de las 17 palabras protegidas del shader, la integracion comprueba cuatro destinos de 4/12/28/60 pixeles de paso: un hilo sobrante no debe tocar la fila siguiente. La optimizacion conserva los 67 resultados de VRAM completos.


## Ejecucion original con GPU

El recorrido de 210 segundos registrado en `analysis/native_song_vulkan_clear_roi_en.json` y su log hizo 6642 envios Vulkan reales y llego a GAME OVER en el escenario de Hotchkiss, observado en su captura final. Uso una copia aislada del guardado y entradas sinteticas. No se reprodujo el bloqueo intermitente de interrupciones en este ensayo; no se considera corregido. No se verifica cancion completa, controles fisicos, escucha, sincronizacion ni mejora de FPS. La ejecucion previa con el primer shader queda conservada por separado y solo llego a la confirmacion de inicio de la cancion antes de agotarse el tiempo.

El tramo de gameplay entre 140,098 y 145,099 segundos registra 51 presentaciones originales en 5,001 segundos (10,20 FPS). La captura de 145,151 segundos muestra notas y Tension Down. Se conserva como observacion de este escenario y binario, sin atribuir una mejora a Vulkan ni extrapolarlo a todos los modos. El siguiente tramo ya llega a GAME OVER.


## Correccion de la lectura de memoria Vulkan

La medida por etapas encontro unos 9,5 ms en la lectura CPU del buffer Vulkan, mientras las consultas de tiempo GPU midieron unos 0,14 ms en sus comandos. El tipo de memoria elegido tenia las banderas HOST_VISIBLE y HOST_COHERENT, sin HOST_CACHED. El backend ahora prefiere memoria que conserva ambas propiedades y añade HOST_CACHED; si no existe, mantiene la opcion coherente disponible.

Los tres ensayos con el mismo binario (opcion cached/uncached para diagnostico) dieron medianas de 12,64/11,10/10,90 ms sin cache y 1,62/1,84/1,91 ms con cache. En el segundo ensayo la lectura CPU bajo de unos 9,89 ms a 0,084 ms. Se conservaron todas las comparaciones exactas de VRAM, y el test integrado sigue aprobando sus 67 comparaciones completas y cuatro casos de filas estrechas. Las consultas de GPU distinguen su ejecucion de la espera del hilo CPU; no se confunden esos tiempos con FPS del juego. Evidencia: `analysis/native_vulkan_cached_memory_verification.json` y los informes de etapas.

Esta correccion elimina el coste de lectura detectado y prepara la siguiente migracion grafica. La ruta de borrado Vulkan sigue desactivada por defecto porque el borrado CPU es mas rapido. El recorrido anterior de 10,2 FPS pertenece al binario anterior; se necesita otra ejecucion original para medir los FPS del cambio actual.


## Ejecucion con la memoria corregida

El binario verificado por las 32 pruebas de regresion hizo 8190 envios Vulkan en otro recorrido original de 210 segundos. Entre 110,017 y 115,017 segundos presento 56 frames en cinco segundos: 11,2 FPS. La captura de 115,032 segundos muestra notas y Tension Down; la de 120,034 segundos muestra GAME OVER. La pulsacion de confirmacion tardia de la prueba afecta al recorrido posterior; el final del diagnostico vuelve a mostrar notas y Tension Down. No se verifica una cancion completada ni se considera corregido el bloqueo intermitente, que no se reprodujo en este ensayo.

En ese tramo de cinco segundos, el tiempo acumulado de borrados fue 0,145 s; dibujo 2D 0,330 s y dibujo 3D 2,614 s. El mayor coste grafico observado ahora esta en el dibujo 3D por CPU. Las escenas y sus tiempos de entrada difieren del binario anterior: 11,2 frente a los 10,2 FPS anteriores son observaciones, no una comparacion controlada de rendimiento. Evidencia: `analysis/native_song_vulkan_cached_en.json` y `analysis/native_vulkan_integration_verification.json`.


## Triangulos planos propios en Vulkan

La opcion `--vulkan-triangles`, desactivada por defecto, reutiliza el dispositivo, almacenamiento, comandos y sincronizacion del backend. Un shader propio aplica cobertura entera de 1/16 de pixel, bordes inclusivos y orden de triangulos superpuestos. La CPU conserva la transformacion, recorte cercano, culling, empaquetado del color y comparacion alfa de colores constantes. Un hilo GPU recorre en orden los triangulos del dibujo para su pixel; los contadores de cobertura y rechazo se recuperan junto con el destino.

La ruta admite los cuatro formatos de color y sus mascaras. Texturas, mezcla, profundidad, colores interpolados, coordenadas fuera del rango entero demostrado, destinos espejados o vigilancia de escrituras mantienen CPU. La memoria original y el avance de vertices se comprometen solo cuando termina la GPU; una excepcion no publica una imagen parcial. El shader no necesita Float64 ni Int64: las coordenadas acotadas y los productos de los bordes se validan antes del envio.

`analysis/native_vulkan_triangle_verification.json` comprueba 121 comparaciones completas de 2 MiB de VRAM, 119 envios reales en Radeon 780M, ocho funciones alfa, cuatro formatos, dibujo proyectado, mascaras, escritura original entre dibujos y fallos antes del compromiso. Una expectativa independiente comprueba 16 pixeles de dos triangulos adyacentes sin duplicar el borde; cuatro pasos estrechos conservan 17 palabras finales protegidas. La prueba se incluye opcionalmente con `KON_GPU_TESTS=ON`, junto a las otras dos pruebas GPU.

En tres ensayos alternados del mismo binario, 32 triangulos planos superpuestos a 480 por 272 dieron medianas CPU de 8,57/9,84/9,69 ms frente a 1,28/1,36/1,35 ms en GPU. Con dos triangulos, CPU 0,67/0,35/0,68 ms y GPU 0,66/0,77/0,91 ms: no hay una mejora estable para dibujos pequenos. Cada resultado conserva exactamente la VRAM CPU y su cobertura. Es una carga sintetica sin texturas ni profundidad; no acredita mejora de FPS del escenario. Evidencia: `analysis/native_flat_triangle_timing_verification.json` y su log.

El ejecutable tambien permite `--profile-render archivo.json` para agrupar tiempo y pixeles por estado real del dibujo. La clave contiene categoria (clear/through/projected), tipo, vertices, vtype, formato de destino, textura, mezcla, profundidad, alfa, formato de textura, filtros, funcion de textura, mezcla y comparacion Z. `--capture-3d-draw archivo.kondraw --capture-3d-after-ms tiempo` conserva una instantanea anterior al primer dibujo proyectado de al menos 64 vertices tras ese instante; `--capture-3d-min-vertices` ajusta el minimo para elegir un dibujo representativo. Son herramientas diagnosticas y sus costes deben separarse de una medicion controlada de FPS.


La primera captura de 355 vertices produjo 0 pixeles cubiertos al reproducirla: conserva geometria original, pero no sirve para verificar la rasterizacion texturada del escenario. Por eso se anadio `--capture-3d-min-vertices 3000` y un segundo recorrido para seleccionar un dibujo grande con salida visible. La nueva referencia se registra por separado en `analysis/native_song_stage_gpu_capture_en.json`, incluyendo los bytes completos de VRAM tras el dibujo CPU; no se interpreta como ejecucion GPU.


La segunda captura contiene 3818 vertices y su repeticion CPU cubre 43430 pixeles y escribe 40641. La referencia binaria conserva los 2097152 bytes completos de VRAM, SHA-256 `619c9dda756eba8b01aa8945133f5a77285ab3ecea468709229a0cae8d241e6f`. En 80 repeticiones con cuatro trabajadores y las opciones de cache de produccion, su mediana aislada fue 5,59 ms. El recorrido de captura observo 57 frames en cinco segundos (11,4 FPS) y 0 envios de triangulos GPU; las capturas muestran notas, Tension Down y GAME OVER. El framebuffer final fue negro al detener el diagnostico. No se acredita mejora de FPS, sincronizacion o una cancion completada. Esta es la referencia real para implementar despues texturas, mezcla y profundidad en GPU, conservando las pruebas planas anteriores.

## Verificacion actual de rendimiento (2026-10-05)

Las tres pruebas de hardware del build histórico 96d5ff seleccionaron NVIDIA GeForce RTX 5050 Laptop GPU. Las comparaciones repetidas del mismo tramo de gameplay dieron 15,9 frente a 20,6 FPS (+29,6 % y +29,7 %), usando las mismas opciones Vulkan en ambas versiones. Los cambios medidos son O2 en todo el corpus recompilado y el reparto de filas por geometria. Las escenas con texturas todavia usan CPU; no es evidencia de un backend Vulkan completo ni de una mejora atribuible a Vulkan. Informe: `analysis/native_performance_release_roi_20261005.json`.
