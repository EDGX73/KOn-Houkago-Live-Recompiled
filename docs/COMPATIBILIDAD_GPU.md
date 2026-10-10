# Compatibilidad con las gráficas del portátil

Prioridad acordada: Vulkan completo y ventajas de FPS, con desarrollo y mediciones en AMD Radeon 780M. NVIDIA se revisará en el hito posterior, manteniendo un único ejecutable de selección automática.

El uso normal selecciona automáticamente una GPU compatible. No hay versiones separadas, selector obligatorio ni cambios manuales en Windows. El forzado de adaptador pertenece únicamente a los diagnósticos internos. No se promete la misma tasa de FPS para gráficas de potencia distinta.

## Evidencia actual en AMD

El ejecutable de Windows actual es `build/native_full/kon_boot_probe.exe`, SHA-256 `2857806409d8c9d9e0db14479cd70a6372ff430869e0280b6f0e26b71d1b6b68`. Pasa las 35 pruebas generales y las cuatro suites Vulkan en AMD Radeon 780M. Las suites conservan 67 comparaciones completas de borrados, 121 de triángulos planos y 161 de rasterización texturada. Incluyen nueve accesos originales con propiedad CPU/GPU y 1.100 dibujos que cruzan la capacidad de un lote. Informes: `analysis/native_gpu_compatibility_amd_active_20261005.json` y `analysis/native_vulkan_integration_verification.json`.

La prueba adicional del hilo privado Vulkan también usa AMD y compara los 2 MiB completos de un dibujo original: `analysis/native_vulkan_render_worker_verification.json`. No acredita FPS configurables dentro del juego.

## Comprobación histórica en ambas GPU

AMD Radeon 780M y NVIDIA RTX 5050 Laptop GPU pasaron las mismas cuatro suites y 161 comparaciones texturadas con los mismos binarios de aquella revisión. Evidencia conservada en `analysis/native_gpu_compatibility_20261005.json`. Es evidencia histórica; las modificaciones posteriores se revisan actualmente en AMD. `tools/check_gpu_compatibility.py` usa AMD por defecto y permite incluir NVIDIA en la revisión posterior.

## Rendimiento y límites

La primera comparación de políticas del binario `51cf907ee72e7f7560d93d13b3723f2af716a2fec9ab15db32d4fff654de1bfd` dio 5,33 a 7,02 FPS en AMD (+31,69 %) y 6,20 a 8,94 en NVIDIA (+44,22 %), con el mismo guardado, entradas y tramo de canción. Son pruebas iniciales, no tasas estables ni finalizadas. La ruta parcial histórica rondó 20,6 FPS y sigue siendo más rápida que la ruta texturada completa de estas pruebas.

La caché posterior produjo 7,09 FPS en un único ensayo AMD contra la referencia conservada de 7,02: alrededor de +1,12 %, sin mejora notable demostrada. La reconstrucción de tres fotogramas de canción coincide con el framebuffer original, pero su prueba añade trabajo y no se usa como comparación de rendimiento. Canción completa, escucha, sincronización y FPS arbitrarios siguen pendientes.
