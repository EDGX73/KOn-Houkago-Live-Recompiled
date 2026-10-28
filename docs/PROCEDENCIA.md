# Procedencia de componentes

El análisis y los recursos comerciales proceden exclusivamente de las dos ISO
suministradas en `Roms PSP`. Se conservan sus hashes en `analysis/iso_jp.json`
y `analysis/iso_en.json`. No se han publicado las imágenes, el código generado
ni sus recursos. El repositorio local excluye estos materiales mediante
`.gitignore`.

| Componente | Origen y uso | Licencia/registro |
| --- | --- | --- |
| PSPRecomp | [Repositorio](https://github.com/jessicanataliagta/PSPRecomp), base `f6e7d415c7f447b934cc3865a31eb725f353d659`; análisis y generación nativa. Los cambios del port se publican como parche (`patches/psprecomp/kon-port.patch`) y `tools/psprecomp_patch.py fetch` lo aplica sobre esa revisión | MIT; licencia en `patches/psprecomp/PSPRecomp-LICENSE.txt` |
| pspdecrypt 1.0 | [Herramienta](https://github.com/John-K/pspdecrypt), usada por separado sobre una copia de EBOOT | GPLv3; binario y fuente conservados en `third_party` |
| Zig, CMake y Ninja | Compilación local portable | Versiones, hashes, enlaces y licencias conservados; véase TOOLCHAIN.md |
| PPSSPP | [Código de referencia](https://github.com/hrydgard/ppsspp), consultado para ABI, semántica de instrucciones y nombres de servicios | Proyecto GPL; no se enlaza ni se copia su implementación en el runtime propio |
| FFmpeg compartido | [Proveedor enlazado por FFmpeg](https://ffmpeg.org/download.html), BtbN n8.1.3-14-g330caae0c1-20261003; bibliotecas DLL y cabeceras dentro de `third_party/ffmpeg` | LGPLv3; `LICENSE.txt` y configuración comprobada sin GPL/nonfree; hashes en `analysis/ffmpeg_provenance.json` |
| PSPSDK | [Documentación](https://pspdev.github.io/pspsdk/), consultada para contratos de las llamadas PSP | Referencia de API; no es el runtime del port |
| TeaTimeTools | [Referencia de recursos](https://github.com/Frain-Breeze/teatimetools) y [parche inglés](https://frain-breeze.github.io/kon/) | Consulta de formatos; sin incorporar su código al parser propio |

Los parsers `iso_audit.py`, `cpk_archive.py`, `fmdx_archive.py` y
`executable_audit.py`, la integración y los servicios de arranque son código
escrito para este proyecto. Todas las correcciones a PSPRecomp (184 líneas en 8
archivos, 2026-10-20) están en `patches/psprecomp/kon-port.patch`, que se
reproduce exactamente sobre la revisión fijada (`tools/psprecomp_patch.py`
`fetch`/`write`/`check`). Se escribieron primero con estos scripts, que se
conservan como historial; los cambios posteriores (observador de escrituras,
accesos directos al scratchpad, etc.) solo están en el parche, así que los
scripts ya no se aplican sobre las fuentes actuales:

- `tools/apply_framework_patches.py`: module-info sin nombres y regiones de código.
- `tools/extend_framework.py`: MADD, BREAK y generación sin optimizaciones opcionales.
- `tools/fix_import_codegen.py`: terminar el bloque tras una llamada a import y conservar la frontera del servicio también en saltos dentro de la misma unidad nativa.
- `tools/fix_break_delay_slot.py`: excepción BREAK en ranuras de retardo válidas.
- `tools/add_scratchpad_memory.py`: memoria auxiliar Allegrex de 16 KiB, con regiones y límites propios.

`analysis/native_codegen_provenance.json` registra la revisión, parches, hash del
ELF y conteos. El corpus generado no demuestra que todos los caminos o sistemas
del juego estén resueltos.

La adaptación del modo de idioma PSP usa un estado propio; sus NIDs y el contrato
Set/Get se contrastaron con [sceImpose.cpp](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceImpose.cpp).
Ese estado del sistema PSP es independiente de la selección de recursos JA/EN.

La capacidad y las reglas de asignación parcial de los contextos ATRAC se
contrastaron con [sceAtrac.cpp](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceAtrac.cpp).
Los NIDs de inicialización y cierre MPEG se contrastaron con
[sceMpeg.cpp](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceMpeg.cpp).
La adaptación propia reserva contextos de FFmpeg y abre su decodificador H264;
no copia ni enlaza código de esas implementaciones de referencia. La reserva
no demuestra reproducción de música o vídeo durante el juego.

Los contratos de [bloqueos ligeros y tareas](https://github.com/pspdev/pspsdk/blob/master/src/user/pspthreadman.h),
[semáforos](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceKernelSemaphore.cpp),
[calendario](https://github.com/pspdev/pspsdk/blob/master/src/rtc/psprtc.h),
[controles](https://github.com/pspdev/pspsdk/blob/master/src/ctrl/pspctrl.h) y
[dirección de red](https://github.com/pspdev/pspsdk/blob/master/src/wlan/pspwlan.h)
se usan como referencias de API para las implementaciones propias.
Los registros gráficos se contrastan con [GE](https://github.com/hrydgard/ppsspp/blob/master/GPU/ge_constants.h)
y el tratamiento de paletas y mezcla con las referencias de estado de GPU.
Windows aporta zona horaria, teclado, XInput y enumeración de adaptadores;
estas conexiones no demuestran todavía manejo interactivo ni multijugador.

El lector propio de RIFF/ATRAC3+ y el búfer circular usan las definiciones de
[PSPSDK](https://github.com/pspdev/pspsdk/blob/master/src/atrac3/pspatrac3.h)
y los contratos de muestras y offsets de la referencia ATRAC de PPSSPP.
FFmpeg LGPL aporta el decodificador nativo de paquetes; no se copia ni se
enlaza la implementación de servicios de PPSSPP. Las pruebas comparan los
paquetes y el búfer circular con PCM original de referencia, incluido el
descarte inicial y el recorte final. Reiniciar y buscar en cinco posiciones
(0, 1000, 50000, 100000 y 267000 muestras) coincide exactamente con la referencia
para búferes completos y circulares. Bucles y canciones completas siguen pendientes.

El servicio propio de comprobación inicial de partidas sigue la estructura de
[savedata de PSPSDK](https://github.com/pspdev/pspsdk/blob/master/src/utility/psputility_savedata.h)
y sus estados de inicio, operación, fin y cierre. Inspecciona archivos reales;
AUTOLOAD/AUTOSAVE escriben y leen un contenedor propio de PC con versión,
integridad CRC32, datos originales sin cifrar, metadatos e imágenes. Un archivo
temporal se vacía a disco y se renombra al terminar; las pruebas verifican una
recarga desde una instancia nueva y rechazan archivos dañados. Este formato no
simula el cifrado de PSP ni modifica archivos PSP existentes. La importación
de partidas cifradas y los modos de diálogo adicionales siguen pendientes.

El orden de los bloques DXT1, colores y selectores se contrastó con las
referencias de decodificación de texturas y sampler de software de PPSSPP.
Se implementó un decodificador propio con pruebas de colores, interpolación,
transparencia y paso entre bloques; no se enlaza código de esas referencias.

La cola gráfica nativa separa el envío de listas de su procesamiento cooperativo.
Las esperas de listas y dibujo siguen el contrato de
[sceGe de PSPSDK](https://pspdev.github.io/pspsdk/pspge_8h.html).
Los handlers gráficos originales se ejecutan con una pila propia de interrupción
y preservan el contexto de la tarea interrumpida; una prueba ejecuta realmente
el código compilado del handler y su llamada al servicio. No se enlaza una GPU
de emulador ni se marca trabajo pendiente como terminado para liberar esperas.

La geometría propia utiliza matrices column-major según
[sceGuSetMatrix](https://github.com/pspdev/pspsdk/blob/master/src/gu/sceGuSetMatrix.c)
y el [viewport de PSPSDK](https://github.com/pspdev/pspsdk/blob/master/src/gu/sceGuViewport.c).
La escala de pesos/UV, recorte cercano, rango de pantalla y productos de luz
ambiente se contrastan con [la documentación de GE](https://www.ppsspp.org/docs/psp-hardware/gpu/ge-overview/)
y [sus medidas aritméticas](https://www.ppsspp.org/docs/psp-hardware/gpu/arithmetic/).
Las fórmulas de proyección, recorte, mezcla de huesos e interpolación son propias;
no se copia ni enlaza el renderer de PPSSPP. Hay pruebas analíticas que distinguen
UV afines de UV corregidas por perspectiva y verifican la disposición de los
pesos de 8/16 bits y float. `replay_native_draw` sirve para depurar un dibujo
capturado sin ejecutar el juego; su éxito no verifica una escena jugable.

El descarte de caras usa el bit del registro GE, no el valor de la API GU:
[sceGuFrontFace](https://github.com/pspdev/pspsdk/blob/master/src/gu/sceGuFrontFace.c)
invierte el enum antes de escribir CULL. Con coordenadas de pantalla, CULL=1
conserva el producto cruzado positivo y CULL=0 el negativo. Se corrigió la
inversión y se comprueban ambos sentidos, tiras y descarte desactivado.

Los bucles ATRAC usan precarga en el búfer circular original y reinicio del
decodificador FFmpeg con paquetes de historia, siguiendo los contratos de
[streaming ATRAC](https://www.ppsspp.org/docs/development/ppsspp-internals/atrac/).
La implementación es propia. Las pruebas comparan todas las muestras con PCM
de referencia, en bucles finitos e infinitos, puntos de inicio distintos,
tamaños de búfer pares/impares y lecturas partidas. Los bucles streaming con
una cola posterior que necesite segundo búfer siguen siendo una parada explícita.

La mezcla RGB propia utiliza los factores y operaciones descritos por
[sceGuBlendFunc de PSPSDK](https://github.com/pspdev/pspsdk/blob/master/src/gu/sceGuBlendFunc.c)
y los productos enteros medidos en la
[documentación aritmética de GE](https://www.ppsspp.org/docs/psp-hardware/gpu/arithmetic/).
Se admiten factores de color, alfa normal y colores fijos, con suma, resta,
resta inversa, mínimo, máximo y diferencia absoluta. Los factores de alfa
doblado siguen sin admitirse. Las pruebas comprueban extremos, saturación,
canales independientes, máscaras y el modo de transparencia anterior.
No se copia ni enlaza un mezclador de otro renderer.

La consulta de auriculares sigue el contrato de
[sceHprmIsHeadphoneExist de PSPSDK](https://github.com/pspdev/pspsdk/blob/master/src/hprm/psphprm.h).
Lee el endpoint de reproducción predeterminado de Windows con Core Audio y
[PKEY_AudioEndpoint_FormFactor](https://learn.microsoft.com/en-us/windows/win32/coreaudio/pkey-audioendpoint-formfactor),
clasificando Headphones y Headset. Las consultas se almacenan durante un
segundo y se vuelven a leer; los fallos de la API quedan explícitos. Esto
depende de la descripción del controlador, no mide físicamente un conector.
Cambiar el dispositivo durante una canción sigue pendiente de comprobación.

El recorte cercano propio sigue el orden descrito por las mediciones de la
[tubería de vértices GE](https://www.ppsspp.org/docs/psp-hardware/gpu/ge-vertex-pipeline/):
recortar Z=-W antes de dividir por W, incluidos extremos detrás de la cámara;
comprobar después los límites de pantalla. Sin recorte, un cruce parcial del
volumen Z no se descarta hasta comprobar la profundidad del viewport.
Se prueban cruces de uno y dos extremos, W=0 y atributos interpolados.
La captura de Hotchkiss en 0x08B55D4C se reproduce con 130560 píxeles dibujados.
Esta comprobación aislada no prueba la canción completa. La aritmética de
transformación/recorte usa todavía precisión de PC; equivalencia exacta con
todos los redondeos float24 del GE sigue pendiente. No se copia código ajeno.

Las funciones propias SleepThread/SleepThreadCB/WakeupThread siguen los NID y
contratos de [PSPSDK ThreadMan](https://pspdev.github.io/pspsdk/group__ThreadMan.html).
Las señales anticipadas se conservan y se consumen de una en una, según la
[prueba de espera PSP](https://github.com/hrydgard/pspautotests/blob/master/tests/threads/wakeup/wakeup.c)
y su resultado registrado. La tarea permanece en espera después de atender
un callback salvo que reciba una señal; los callbacks también se atienden
cuando no queda ninguna tarea lista. Se comprueban los handlers de producción,
la continuación, registros, estado PSP y reanudación de otra tarea desde un
callback. Desbordar el contador sigue siendo una parada explícita pendiente
de medir, y este test no equivale a verificar todos los modos del juego.

ReferSemaStatus exporta el objeto de semáforo real y cuenta sus tareas en
espera, siguiendo la estructura de
[PSPSDK](https://pspdev.github.io/pspsdk/structSceKernelSemaInfo.html) y los
[resultados medidos de PSP](https://github.com/hrydgard/pspautotests/blob/master/tests/threads/semaphores/refer.expected).
Se prueban nombre truncado y terminado, atributos, contadores, tareas que
se despiertan, error de UID desconocido, tamaño cero y tamaño mayor que la
estructura. Los tamaños cortos no medidos detienen explícitamente la ejecución;
no se inventa la extensión de copia. SleepThreadCB/WakeupThread ya pasaron
también en la canción original: véase native_song_sema_status_pending_en.json.

PollSema intenta consumir señales sin suspender la tarea; devuelve el error
SEMA_ZERO si faltan. Se comprueban señales consumidas, cuenta insuficiente,
validación de cantidad antes del UID y ausencia de cambio de tarea, con el
[resultado público medido en PSP](https://github.com/hrydgard/pspautotests/blob/master/tests/threads/semaphores/poll.expected).
ReferSemaStatus ya pasó dentro del juego original antes de la parada WaitSemaCB
registrada en native_song_poll_sema_pending_en.json. El replay de diagnóstico
deriva de la secuencia de teclado registrada, con pulsos ensanchados; es entrada
sintética del test y no acredita una nueva prueba de teclado físico.

El NID 0x6D212BAC corresponde a WaitSemaCB y 0x58B1F937 a PollSema, comprobado
en [ThreadManForUser.S de PSPSDK](https://github.com/pspdev/pspsdk/blob/master/src/user/ThreadManForUser.S).
La primera implementación confundió esos identificadores y su ensayo fue
interrumpido antes de llegar a la canción; no constituye evidencia de éxito.
El handler corregido conserva la espera después de ejecutar callbacks y
consume señales al reanudar; estos casos se comprueban en producción.

DeleteSema elimina el objeto y libera sus tareas con WAIT_DELETE, según
[la prueba de eliminación](https://github.com/hrydgard/pspautotests/blob/master/tests/threads/semaphores/delete.expected)
y [el código de error del SDK](https://github.com/pspdev/pspsdk/blob/master/src/user/pspkerror.h).
La prueba de producción comprueba también que un UID eliminado no puede
consultarse ni borrarse otra vez. La tabla de nombres/NID de ThreadMan está
fijada a una revisión pública en analysis/threadman_contracts.json; el nuevo
test compara etiquetas explícitas con esa referencia. Dos etiquetas de
firmware posterior ausentes de la tabla quedan fuera de esa comprobación.
Esta prueba de metadatos no mide cobertura de ejecución del juego.

La rasterización propia usa incrementos enteros para las funciones de borde
por píxel, conservando las operaciones de interpolación y muestreo anteriores.
Las pruebas gráficas pasan y los BMP del dibujo original antes/después son
idénticos por SHA256. No se ha medido todavía una mejora de rendimiento.


La consulta de tareas bloqueadas por audio expone espera de evento (tipo 4),
con un solo objeto privado SceAudio y bits por canal. El contrato se comprobó
contra [uofw audio.c](https://github.com/uofw/uofw/blob/master/src/kd/audio/audio.c):
el controlador crea un evento con atributos 0x201 y espera con CLEAR 0x20.
No se ha copiado su implementación. El adaptador usa su propio UID reservado
0x7F000001; no pretende reproducir el UID dinámico asignado por una consola.
La versión inicial activaba el bit con WOM_DONE. Se corrigió el contrato: el mezclador propio lo activa cuando termina de copiar la fuente, como describe el controlador PSP; el escritor lo consume al reanudarse. La reproducción física de Windows se mide por separado con waveOutGetPosition.
La prueba de producción entrega buffers reales en dos canales, consulta sus
esperas y verifica continuación, registros y cantidad de muestras devuelta.
No acredita escucha, sincronización musical ni una canción completa.

WaitThreadEnd/CB y GetThreadExitStatus conservan el resultado de salida del
hilo, con reconsulta del UID tras callbacks. Los estados y resultados se
contrastaron con [threadend.expected](https://github.com/hrydgard/pspautotests/blob/master/tests/threads/threads/threadend.expected)
y [exitstatus.expected](https://github.com/hrydgard/pspautotests/blob/master/tests/threads/threads/exitstatus.expected).
La prueba usa los handlers y el planificador de producción para comprobar
espera, finalización con código 7, consulta posterior y errores de UID.
No se han ejecutado esas pruebas en una PSP física propia.


El lector KSD se ha escrito a partir de los bytes de los recursos suministrados,
sin copiar un parser externo. La auditoría de 759 charts distintos preserva todos
sus bytes, campos y orden. La prueba no valida unidades temporales o mecánica.

El filtrado bilineal usa dos canales empaquetados por operación, conservando la
truncación horizontal y vertical de la versión escalar. Las 256 combinaciones
de fracciones con RGBA independientes pasan la comparación; la captura original
mantiene el BMP exacto. El benchmark aislado reduce su mediana de 7157,8 a 6729,7
microsegundos en este equipo. Esto no es una medición de FPS del juego completo.
El descarte temprano de profundidad se limita a dibujos sin prueba de alfa y
conserva sus contadores y escrituras. Triángulos sin cruce cercano evitan crear
un segundo polígono; se reservan los vectores de vértices y triángulos una vez.


El mezclador continuo de `native_audio_mixer.hpp` y `native_audio.hpp` es propio.
La referencia de audio uofw se usó para comprobar bloques de 64 muestras,
liberación de la fuente y precisión fraccionaria antes de sumar los canales;
no se copió código del controlador. Un único dispositivo WinMM mezcla los
canales en PCM estéreo a 44.100 Hz. La prueba silenciosa usa la posición real
del controlador; no verifica audición, calidad ni sincronización de una canción.
Su búfer inicial de aproximadamente 93 ms requiere calibración posterior.

La espera privada usa [CreateWaitableTimerExW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
con temporizador de alta resolución cuando Windows lo admite. No modifica
la resolución global de los temporizadores. La medición aislada conserva los
tiempos reales y no simula progreso.

Las interrupciones GE se aplazan mientras la CPU conserva su máscara de
interrupciones desactivada, de acuerdo con el [contrato de IntrMan de PSPSDK](https://pspdev.github.io/pspsdk/group__IntrMan.html)
y los [callbacks GE de tipo interrupt](https://pspdev.github.io/pspsdk/pspge_8h.html).
La entrega usa una pila dedicada y restaura registros y máscara al retornar.
La prueba utiliza los handlers y el planificador de producción. La regresión
observada antes de esta adaptación queda conservada en los registros de Hotchkiss.

La caché de texturas de `src/native_texture.hpp` es propia: decodifica una textura para un dibujo, invalida al reconstruir su estado y comparte una vista inmutable entre trabajadores. Las texturas que se solapan con la salida conservan el acceso original para mantener efectos de realimentación. Pruebas comparan texels, filtrado, VRAM y un BMP original idéntico; no procede de un renderizador externo.

`third_party/vulkan_headers/Vulkan-Headers-1.3.290` contiene las declaraciones oficiales de Khronos, versión fijada v1.3.290, descargadas de https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.290.zip; SHA256 `918a8bb31b8312fd649fd1d1826443da870f33f3187c16c4528aa94927409bab`. Conserva su LICENSE.md. `src/native_vulkan_probe.cpp` es propio: carga Vulkan del sistema, crea un dispositivo, envía un borrado por GPU y compara la lectura tras una barrera y un fence. Esta prueba aislada no integra gráficos Vulkan en el juego ni ejecuta shaders.

La lectura nativa de controles conserva la seleccion de las muestras mas recientes y rechaza recuentos de 64 o mas antes de acceder al destino, contrastado con `_sceCtrlReadBuf` del [codigo de firmware documentado por uOFW](https://raw.githubusercontent.com/uofw/uofw/master/src/kd/ctrl/ctrl.c). Las pruebas de produccion cubren el limite valido, errores, memoria intacta y ausencia de cambios de tarea. No se copio codigo del modulo.

La cache persistente experimental es propia y compara bytes de origen y paleta de forma exacta antes de reutilizar texels. Conserva un maximo de 16 entradas y 8 MiB de datos, expulsa la menos reciente y excluye destinos que comparten memoria con la textura. Pruebas comparan filtrado y VRAM completa tras cambios de fuente, paleta, formato, proyeccion y expulsiones. Las mediciones de tiempo aisladas son variables; permanece desactivada por defecto.


## Shader y backend propio de Vulkan (borrados)

`tools/create_vulkan_clear_shader.py` genera nuestro kernel entero de borrado de rectangulos, con mascaras RGB/alfa y limites de acceso. No contiene un interprete de shaders ni codigo grafico de un emulador. CMake genera el SPIR-V, su cabecera C++ y un manifiesto de hashes antes de compilar `kon_native_vulkan`; el modulo queda incorporado al ejecutable y no requiere un archivo de shader externo al arrancar.

Se utilizan exclusivamente las declaraciones y la gramatica de Khronos SPIRV-Headers, etiqueta `vulkan-sdk-1.3.290.0`: [archivo oficial](https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.3.290.0.zip). El archivo original `work/spirv-headers-1.3.290.0.zip` tiene SHA-256 `9a00219c5c4696ad5317b87f77d5aa3383d1fa99799ce3e9789e9b3023f31ad1`; las licencias se conservan bajo `third_party/spirv_headers/SPIRV-Headers-vulkan-sdk-1.3.290.0`. La gramatica suministra los identificadores de las instrucciones, no la implementacion del kernel. La generacion valida su estructura; no se ha ejecutado `spirv-val`. La creacion del pipeline y su ejecucion real se verifican por separado en el controlador instalado.

`src/native_vulkan_clear.cpp` es nuestro adaptador Windows/Vulkan: cargador del sistema, seleccion de cola, almacenamiento persistente, pipeline y comandos propios. Cada envio conserva el orden de rectangulos superpuestos, termina con una barrera para lectura de CPU y espera una fence real. Color y profundidad se escriben en la memoria del juego solo cuando todos los pases han terminado; no se ocultan errores del dispositivo con resultados inventados.

`src/native_vulkan_renderer_test.cpp` comprueba el mismo `NativeRenderer` que utiliza el juego: 67 comparaciones de los 2 MiB completos de VRAM frente a la ruta CPU, los cuatro formatos de color, profundidad, mascaras, scissor, vertices invertidos y escrituras directas entre dibujos. Los destinos solapados y los accesos que cruzan el espejo de VRAM conservan la ruta CPU. La prueba simula tambien un error en el segundo pase y comprueba que no se compromete una VRAM parcialmente dibujada. `analysis/native_vulkan_renderer_verification.json` acredita esa integracion aislada; su campo `game_run_verified` permanece falso porque no es un recorrido del juego. La ejecucion original se documenta en un informe distinto cuando se comprueba.


## Shader propio de triangulos planos

`tools/create_vulkan_triangle_shader.py` ensambla nuestro kernel SPIR-V de triangulos planos usando la misma gramatica oficial fijada. Sus operaciones, cobertura, bucle ordenado, mascaras y contadores son propios; no se copia codigo de un emulador ni se introduce un interprete. La cabecera del shader y su manifiesto de hashes se generan antes de compilar y se incorporan al binario. `src/native_gpu_triangles.hpp` define el contrato y los limites comprobados; `src/native_vulkan_clear.cpp` comparte recursos Vulkan con el nuevo pipeline. `src/native_vulkan_triangle_test.cpp` verifica el renderizador de produccion frente a CPU y expectativas independientes; `src/benchmark_native_triangles.cpp` mide unicamente dibujos planos sinteticos, sin demostrar FPS del juego.


## Optimizaciones nativas y espera de pantalla (2026-10-05)

La lectura directa de vertices usa nuestra `NativeMemoryView` tras validar el rango completo y conserva la ruta comprobada si cruza el espejo de memoria. La mezcla RGB empaquetada, la modulacion SSE2, la lectura de palabras CLUT y la seleccion de muestreo de UV son propias. Para UV de modo directo se omiten comprobaciones por pixel solamente tras validar atributos finitos y acotados antes de dibujar; la interpolacion cubierta es convexa y Q vale uno. Coordenadas proyectadas, extremas o no finitas conservan comprobaciones. Pruebas comparan redondeo, saturacion, formatos, errores y VRAM completa con la ruta escalar/comprobada. No se activa fast-math.

El NID `0x77ED8B3A` se identifica como `sceDisplayWaitVblankStartMultiCB` en la documentacion original de [SilverSpring conservada por uofw](https://uofw.github.io/upspd/docs/SilverSpring_Blog/my.malloc.us/silverspring/index.html). La tabla oficial de [PSPSDK](https://raw.githubusercontent.com/pspdev/pspsdk/master/src/display/sceDisplay.S) identifica `sceDisplayGetFrameBuf` con otro NID, `0xEEDA2E54`. Se corrigio la asociacion anterior; el calculo de limites a 60000/1001 refrescos por segundo y la espera nativa son nuestros. No se copio un modulo grafico o de temporizacion de un emulador.

`tools/benchmark_native_game.py` ejecuta comparaciones privadas con copias de las partidas, conserva los argumentos y hashes, registra presentaciones reales y compara un intervalo relativo al primer audio original del canal de cancion. Sus entradas son sinteticas y su salida de diagnostico no acredita una cancion completada ni velocidad, sonido o sincronizacion perfectos.

El reparto de filas por area vertical de geometria de `src/native_renderer.hpp` es propio. Usa limites calculados de vertices validados y conserva cada dibujo como barrera sincrona, orden de primitivas y filas disjuntas. La particion anterior permanece seleccionable solo para comparaciones; dos capturas originales conservan toda la VRAM exactamente. El umbral paralelo de 8192 pixeles no cambia.


## Rasterización texturada y herramientas (2026-10-05)

`src/shaders/native_raster.comp`, `native_gpu_raster.hpp`, la integración de `NativeRenderer` y el agrupamiento en `native_vulkan_clear.cpp` son propios. La cobertura, atributos, muestreo, combinación, mezcla, alfa, profundidad, bins y contadores no proceden de un renderizador externo. Se reutilizan las declaraciones oficiales Vulkan ya registradas.

El compilador oficial [Khronos glslang](https://github.com/KhronosGroup/glslang) se conserva en `third_party/glslang/bin/glslang.exe`, junto con licencia y `provenance.json`. El archivo descargado tiene SHA-256 `1a5ea07a2138782a21044d070b8fc8028e80920a701db4d13def236311aa8d1a`; el manifiesto conserva URL, identidad de release/asset y fecha. El compilador convierte nuestro GLSL en SPIR-V y no suministra código de ejecución del juego. `tools/build_vulkan_raster_shader.py` incorpora el shader en C++ y registra hashes de fuente, generador, compiler y SPIR-V. La generación usa `-Os`, Float64/Int64 precisos y no activa fast-math.

La [guía oficial de sincronización Vulkan](https://docs.vulkan.org/guide/latest/synchronization_examples.html) se consultó para barreras HOST/TRANSFER/COMPUTE y lectura después de fence. El orden de los dibujos, paquetes de lotes, validación y compromiso de memoria son nuestros.

El reloj `native_render_timeline.hpp` es propio y no ejecuta lógica del juego ni modifica el audio. Su prueba conserva idéntico avance de simulación a distintas tasas y comprueba interpolación de geometría inmutable. Es una base de implementación, no evidencia de FPS superiores dentro del juego. El reparto hasta 16 trabajadores conserva las comparaciones de VRAM y los límites sincrónicos de cada dibujo.

## Propiedad de VRAM y lotes residentes

`src/native_vram_ownership.hpp` y la adaptación local de `third_party/PSPRecomp/src/guest_memory.cpp` son propios. Un observador asociado a la memoria original sincroniza resultados GPU antes de accesos CPU solapados; no modifica los caminos rápidos AOT de RAM principal. La supresión anidada se limita a accesos internos del renderizador y el observador es local al hilo y a la instancia. Los fallos rechazan el acceso antes de exponer datos antiguos. Los lectores por bloques notifican el rango completo, incluidos los espejos. `native_vram_ownership_test` y nueve casos del test Vulkan verifican lecturas, escrituras y VRAM completa. `build_native_probe.ps1` recompila la biblioteca importada para evitar enlazar una versión antigua.

## Selección automática y matriz de GPU

`native_gpu_selection.hpp` utiliza propiedades estándar de Vulkan para la selección automática. Las opciones por vendor/tipo/nombre/índice son diagnósticos internos. `tools/check_gpu_compatibility.py` ejecuta los mismos binarios en AMD/NVIDIA con un entorno privado para cada proceso, valida la identidad real y registra resultados separados, sin cambiar preferencias del sistema ni generar versiones diferentes del juego. La reutilización de texturas de lotes compara todos sus bytes; el cierre por capacidad publica el grupo anterior antes de reintentar una vez el dibujo nuevo. No se usan extensiones de fabricante.

## Propietario de renderizado independiente

`src/native_render_worker.hpp` y sus pruebas son propios. El hilo fabrica y destruye su backend en el mismo propietario; sus salidas usan VRAM privada y geometría inmutable capturada del programa original. No se llama al juego, al audio, a los controles ni al sistema de puntuación desde ese hilo. La cola retiene las últimas fuentes y no acumula plazos vencidos. No se ha conectado todavía a la presentación del juego.

La reducción de barreras del lote conserva orden compute/compute para mezcla/profundidad y transferencia/compute para paquetes nuevos; se basa en regiones disjuntas e inmutables, con escrituras coherentes anteriores al envío. Referencia de semántica de envío y dependencia: https://docs.vulkan.org/spec/latest/chapters/synchronization.html . Las cuatro suites de exactitud se han vuelto a verificar en AMD; la referencia NVIDIA anterior se conserva como histórica.
