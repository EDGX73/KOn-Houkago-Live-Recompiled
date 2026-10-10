# Cambios recientes y ejecutable de pruebas

El ejecutable actualizado se mantiene siempre en [build/native_full/kon_boot_probe.exe](../build/native_full/kon_boot_probe.exe). `boot_probe.cpp` es codigo fuente; no es el programa que se abre para jugar.

Para abrirlo en ingles: [Probar ingles.cmd](../Probar%20ingles.cmd). Para la ruta Vulkan experimental: [Probar Vulkan ingles.cmd](../Probar%20Vulkan%20ingles.cmd). Hay accesos equivalentes para japones. Ambos usan el mismo exe. Ese acceso conserva la ruta anterior de borrados y triangulos planos. La ruta texturada nueva se activa con `--vulkan --vulkan-batch`; todavía es experimental y no está activada por defecto.

## Archivos modificados

| Archivo | Cambio |
| --- | --- |
| [CMakeLists.txt](../CMakeLists.txt) | Optimizacion O2 del corpus recompilado completo y del registro. Se conserva la opcion de compilar solo unidades elegidas o usar O0 para diagnostico. Sin fast-math. |
| [src/boot_probe.cpp](../src/boot_probe.cpp) | Correccion de la espera de varios refrescos de pantalla y de su NID; muestras de fotogramas entregados cada 100 ms, captura de un dibujo 2D original y diagnosticos acotados. |
| [src/native_display.hpp](../src/native_display.hpp) | Calculo del plazo exacto para varios refrescos a la frecuencia original, sin modificar la velocidad del reloj ni el audio. |
| [src/native_renderer.hpp](../src/native_renderer.hpp) | Lectura de vertices validada una vez, seleccion de mezcla/muestreo fuera del bucle de pixeles y especializacion para coordenadas 2D finitas/acotadas y reparto paralelo de las filas que ocupa cada dibujo. |
| [src/native_blend.hpp](../src/native_blend.hpp) | Operaciones enteras empaquetadas para los modos habituales de transparencia, con el mismo redondeo y saturacion de la ruta original. |
| [src/native_texture.hpp](../src/native_texture.hpp) | Modulacion SSE2 y lectura directa de palabras de la paleta. Se conservan filtrado, formato, alpha y comprobaciones para coordenadas proyectadas/extremas. |
| [src/native_renderer_test.cpp](../src/native_renderer_test.cpp) | Comparaciones de mezcla rapida/escalar para alpha, colores y casos de redondeo/saturacion. |
| [src/native_texture_test.cpp](../src/native_texture_test.cpp) | Comparaciones de modulacion rapida/escalar, RGB/RGBA y brillo doble. |
| [src/native_parallel_raster_test.cpp](../src/native_parallel_raster_test.cpp) | Comparaciones completas de VRAM entre referencias comprobadas y rutas optimizadas; entradas no finitas/extremas. |
| [src/native_display_test.cpp](../src/native_display_test.cpp) | Limites de refresco individuales y multiples, incluidos conteos grandes. |
| [src/native_sleep_test.cpp](../src/native_sleep_test.cpp) | La llamada real de pantalla suspende a su tarea, conserva continuacion/registros y permite callbacks. |
| [tools/benchmark_native_game.py](../tools/benchmark_native_game.py) | Comparacion repetida del mismo tramo del juego, con partidas copiadas, entradas sinteticas y hashes de los ejecutables. |

## Lo comprobado

Las 34 pruebas generales pasan. Las cuatro pruebas opcionales sobre Vulkan se ejecutaron en NVIDIA GeForce RTX 5050 Laptop GPU: 67 comparaciones completas de VRAM para borrados y 121 para triangulos planos. Dos dibujos originales reproducidos con las optimizaciones conservan los 2 MiB completos de VRAM byte a byte frente a sus referencias.

La espera de varios refrescos estaba asociada por error a una consulta de framebuffer. Se ha corregido y se prueba con el planificador real. La velocidad visual de todas las escenas y la sincronizacion de una cancion completa siguen pendientes. Las medidas antiguas anteriores a esta correccion no certifican velocidad normal.

Se midieron tambien cambios que no se activaron: compartir la preparacion de triangulos no dio una mejora estable y elevar el umbral de trabajo paralelo empeoro el dibujo 2D capturado. La cache persistente de texturas conserva una opcion experimental; su mejora inicial de FPS fue pequena y sigue desactivada por defecto.

La comparacion repetida de rendimiento historica esta completada y corresponde al hash 96d5ff; sus resultados se detallan al final. El port sigue incompleto: esto no acredita una cancion terminada, audio escuchado y sincronizado, Vulkan completo ni FPS arbitrarios independientes de la logica.

Los registros historicos y ejecutables de referencia se mantienen bajo `analysis/` y `build/perf_*`; para probar la version actual se debe usar `build/native_full`.

El reparto de filas se comprobo con tres comparaciones alternadas del mismo binario: la pieza 2D original tarda entre un 29 y un 37 % menos; el dibujo 3D permanece equivalente en tiempo. Ambos conservan exactamente la VRAM completa. La mejora durante gameplay se mide aparte, con O2 en todo el corpus y la misma espera de pantalla corregida en las dos versiones.

## Comparacion completada de gameplay

| Ejecucion | Referencia | Build 96d5ff | Mejora |
| --- | ---: | ---: | ---: |
| 1 | 15.92 FPS | 20.64 FPS | 29.64 % |
| 2 | 15.91 FPS | 20.63 FPS | 29.68 % |

Mismo guardado inicial, mismas entradas sinteticas, opciones Vulkan iguales y espera de pantalla corregida en las dos versiones. Orden alternado: referencia/actual, actual/referencia. Se compara aproximadamente el intervalo de 1 a 5 segundos tras el inicio del audio original de Hotchkiss, con presentaciones registradas cada 100 ms. Capturas inspeccionadas: Bus Stop y notas originales. Es una prueba headless acotada; no certifica la velocidad y sincronizacion de toda una cancion ni 30 FPS estables.

Hash SHA-256 del ejecutable probado: `96d5ff84c2f389a31f2e9c054fb882c7ef121523f176caa178bdf3477380b218`. Informe completo: `analysis/native_performance_release_roi_20261005.json`. Las mejoras son del codigo nativo y el reparto de filas; no se atribuyen a un renderizador Vulkan completo.

## Nueva integración texturada y multihilo (2026-10-05)

Build histórico texturado SHA-256: `242760da21130ca5a95b6008c07f0443044d7a255cc2abd05d83bef995fe78e6`.

- `src/shaders/native_raster.comp`: rasterización GPU propia con textura/perspectiva, mezcla, alfa y profundidad.
- `src/native_vulkan_clear.cpp`: memoria local GPU, transferencias y lecturas verificadas, bins y lotes con barreras.
- `src/native_renderer.hpp`: adaptación de geometría, preparación compartida de texels y publicación atómica del lote antes de volver al programa original.
- `src/native_raster_pool.hpp`: hasta 16 trabajadores; `--raster-threads 1-16` permite medirlos.
- `src/native_render_timeline.hpp`: base de reloj independiente e interpolación; falta integración de fotogramas completos y no se presentan repintados como FPS adicionales.
- `src/native_vulkan_raster_test.cpp`: 151 comparaciones completas de VRAM, incluyendo 24 dibujos superpuestos agrupados.
- `tools/record_vulkan_progress.py`: cuatro pruebas hardware y hashes de los shaders actuales.

La captura del juego tras 35 segundos muestra carga completada con 7174 envíos de rasterización texturada real. Aún no prueba una canción completada ni FPS superiores. El rendimiento de gameplay actual se mide en `analysis/native_performance_vulkan_raster_batch_20261005.json`; el informe solo es concluyente cuando `comparison_completed` vale true.


La ruta nueva reutiliza texturas decodificadas solo si fuente y CLUT coinciden exactamente; cambios entre dibujos invalidan la entrada. Las lecturas/escrituras de color y profundidad se transfieren en bloque conservando los accesos comprobados para rangos espejados. La nueva prueba incluye las ocho funciones alfa y profundidad y conserva 151 comparaciones completas. La primera comparación de gameplay se conserva como fallida: la ruta GPU llegó a la confirmación de inicio, pero las pulsaciones temporizadas no iniciaron el audio de canción. No se publicó una ganancia de FPS inexistente. La repetición usa el mismo nuevo archivo de pulsaciones para ambas versiones.

## Build con sincronización de lotes residentes

SHA-256 histórico: `d197597c322521f1e6a82136559a3fff6017db0ec19353e837628b9c0a643906`. Pasa 34 pruebas generales y cuatro pruebas GPU reales, con 160 comparaciones completas en la ruta texturada. Incorpora `--vulkan-resident-batch`, comprobación de bloques completos, captura con FPS y salida de pruebas visibles a su registro propio. Rendimiento de los lotes residentes todavía en medición.

## Compatibilidad AMD y NVIDIA

Build único histórico: `51cf907ee72e7f7560d93d13b3723f2af716a2fec9ab15db32d4fff654de1bfd`. Selección automática para uso normal. La ruta residente cierra lotes antes de llenar su almacenamiento fijo y reutiliza texturas idénticas dentro del lote. Las cuatro suites pasan en ambas GPU con los mismos binarios, 161 comparaciones texturadas por adaptador y una prueba de 1.100 dibujos. No hay versiones separadas ni selección manual obligatoria. [Compatibilidad y evidencia](COMPATIBILIDAD_GPU.md).

## Prioridad Vulkan y FPS: actualización de esta sesión

Prioridad acordada: Vulkan completo y ventajas de FPS, con desarrollo y mediciones en AMD Radeon 780M. NVIDIA se revisará en el hito posterior, manteniendo un único ejecutable de selección automática.

Build actual SHA-256: `2857806409d8c9d9e0db14479cd70a6372ff430869e0280b6f0e26b71d1b6b68`. 35 pruebas generales aprobadas; cuatro suites Vulkan AMD con 161 comparaciones texturadas completas. Se ha reducido el enlace y las barreras redundantes dentro de lotes, compartido texturas inmutables y preparado un hilo de dibujo propietario de su GPU y memoria privada.

Tres fotogramas completos de escena de canción (180/192/213 comandos) coinciden con el framebuffer original, cero rechazos; la evidencia corresponde al binario `4a7dccea...`. El hilo independiente también reconstruye un dibujo original en AMD con coincidencia de los 2 MiB completos. Los cambios de tasa y la geometría intermedia se prueban aisladamente. Presentación independiente en el juego, swapchain, canción completa y sincronización a FPS arbitrarios pendientes.

La caché redujo decodificaciones pero el ensayo AMD pasó de la referencia conservada 7,02 a 7,09 FPS (+1,12 %): sin mejora notable. No se atribuye una ganancia al ensayo que añade reconstrucción privada. Evidencia y captura con FPS: `analysis/native_render_worker_integration_verification.json`, `analysis/native_visible_vulkan_resident_frame_private_ordered_20261005_amd_en.json`, `analysis/native_visible_vulkan_resident_frame_private_ordered_20261005_amd_en_125110ms.bmp`.
