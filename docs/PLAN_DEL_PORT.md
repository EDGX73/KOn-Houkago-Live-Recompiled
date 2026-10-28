# Plan del port de K-On! Hōkago Live!!

> Nota (2026-10-28): este documento describe la fase inicial centrada en
> Windows. El proyecto incluye además una beta Android arm64; consulta el
> [README](../README.md) y [ANDROID.md](ANDROID.md).

Seguimiento de fases: [roadmap completo con tareas tachadas](ROADMAP.md).

El objetivo es una versión nativa para Windows que conserve el juego original,
con japonés e inglés seleccionables desde Ajustes y preparada para incorporar
un paquete español. No se considera terminada por extraer los datos, convertir
el código o compilar un ejecutable de diagnóstico.

## Hitos y pruebas de aceptación

1. **Conocer las dos versiones.** Inventario, identidad de archivos, programa,
   formatos de recursos y diferencias de la traducción. Conservar las ISO.
2. **Ejecutar el programa en PC.** Recuperar el ejecutable, convertir Allegrex a
   C++, resolver sus instrucciones y adaptar los servicios del sistema de PSP.
   Primero el arranque y su tarea principal; después los inicializadores.
3. **Mostrar el juego.** Implementar las listas gráficas, memoria de vídeo,
   texturas, presentación y controles. La prueba es llegar al menú original
   usando el programa convertido.
4. **Jugar una canción completa.** Implementar el audio, los relojes y las
   esperas. Comprobar sonido, eventos de notas, respuesta al mando/teclado,
   puntuación y pantalla de resultados. Comparar con el comportamiento de PSP.
5. **Incorporar el selector de idioma.** Separar los recursos comunes y los
   localizados. Cambiar de idioma desde un punto seguro, vaciar las cachés y
   recargar el menú. Persistir la preferencia. El paquete español requiere
   traducir texto y gráficos; no requiere volver a portar el programa.
6. **Completar y verificar todos los sistemas.** Guardado/carga, desbloqueables,
   personajes, canciones y dificultades, escenas, personalización, modos
   adicionales y multijugador. Registrar también diferencias del parche inglés.
7. **Preparar una entrega utilizable.** Ejecutable y controles de PC, ajustes,
   instalación local de recursos, instrucciones y pruebas de una ejecución
   completa. No publicar datos comerciales ni el código generado del juego.

## Decisiones ya comprobadas

- Las dos ISO son ULJM05709 y comparten exactamente EBOOT.BIN y snd.cpk.
- La traducción modifica std.cpk, principalmente recursos gráficos y fuentes.
- Se utiliza un solo programa convertido y un montaje de contenido por idioma.
- Las voces y la música son recursos comunes. Cambiar los textos no implica
  traducir o volver a grabar las voces.
- Los servicios sin implementar detienen la ejecución con su nombre y dirección.
  No se sustituyen por resultados de éxito inventados.

## Orden de trabajo actual

Arranque nativo → memoria e inicializadores → gestión de tareas/archivos →
gráficos y menú → audio y primera canción → idiomas en Ajustes → resto de modos.
Los próximos cambios los determina la ejecución del programa original.


## Qué significa port nativo en este proyecto

No disponemos del código fuente original. La vía elegida es recompilar las
instrucciones del programa PSP a código de Windows y escribir una capa propia
que satisfaga sus llamadas al sistema y al hardware. Los datos y la lógica del
juego siguen siendo los originales. El objetivo no se cumple con envolver una
ISO en un emulador ni con crear otro juego de ritmo parecido.

El usuario ha aclarado que acepta la técnica necesaria para lograr el resultado:
un `.exe` del juego que funcione bien, con mayor resolución, más FPS, idiomas
en Ajustes y modificaciones de contenido más sencillas. La recompilación con
servicios adaptados es una base aceptada, no el criterio de terminación.
La entrega debe comprobar resolución interna superior (no solo estirar la
ventana), rendimiento y ritmo estables, selector de idioma persistente y un
sistema documentado de mods que permita sobrescribir recursos sin editar las
ISO. Cada función se verificará con una prueba real dentro del juego.

La tasa de presentación debe admitir 30, 60, 120, 165 FPS y un valor
personalizado. El usuario requiere que cambiarla conserve la velocidad de
animaciones, audio, ventanas de acierto y puntuación. Se separarán renderizado,
simulación y reloj de audio, con interpolación cuando corresponda; contar
repintados o acelerar toda la lógica no satisface este requisito. Se verificarán
las mismas acciones y resultados a distintas tasas, además de latencia y fluidez.

La generación automática ya compila y ejecuta parte del arranque. Todavía hay
que comprobar sus caminos indirectos y el comportamiento real de instrucciones,
memoria y servicios. Que el archivo compile no prueba que el port sea viable
en todos sus modos: el menú y la primera canción serán los siguientes puntos
para comprobarlo.

## Adaptaciones principales

| Sistema | Trabajo necesario | Prueba visible o de comportamiento |
| --- | --- | --- |
| Tareas y tiempo | Prioridades, esperas, callbacks, sincronización y reloj del juego | Arranque y progresión sin bloqueos; ritmo estable |
| Archivos y memoria | Accesos a los archivos originales, asignaciones y rutas de guardado en PC | Carga real de recursos; guardar, cerrar y volver a cargar |
| Gráficos | Convertir las órdenes gráficas de PSP, texturas, geometría y presentación a una ventana de PC | Menú y escenas originales; comparar con PSP |
| Sonido | Decodificar formatos originales, reproducir canales y sincronizarlos | Una canción completa con notas, audio y resultados coherentes |
| Controles | Teclado y mando, reasignación y calibración de latencia | Respuesta correcta de todas las acciones durante una canción |
| Idiomas | Un programa, recursos compartidos, paquetes JA/EN y futuro ES | Cambio desde Ajustes, recarga segura y preferencia persistente |
| Contenido y modos | Personajes, canciones, dificultades, desbloqueables, escenas y personalización | Revisión explícita de cada modo y persistencia |
| Multijugador | Adaptar la comunicación y la sincronización que usa la PSP | Partida entre dos instancias de PC y resultados concordantes |
| Presentación en PC | Resolución, ventana/pantalla completa y rendimiento | Imagen correcta y ritmo independiente de la tasa de presentación |

El español requerirá traducir los recursos que contienen texto, incluidos los
gráficos y las fuentes que lo necesiten. No será una inversión automática del
parche inglés; será otro paquete para el mismo programa. El selector se expondrá
cuando la recarga de recursos sea segura, no durante una canción en curso.

La estructura debe admitir otros idiomas además de JA/EN/ES sin recompilar el
programa. El montaje ya valida y descubre paquetes adicionales; véase
[PAQUETES_DE_IDIOMA.md](PAQUETES_DE_IDIOMA.md). El selector de Ajustes y las
herramientas de traducción siguen pendientes. El mapeo de los doce botones y
el stick PSP está documentado en [CONTROLES.md](CONTROLES.md), con guía F1.

## Dónde estamos

Análisis de ambas versiones: completado. Primera conversión y compilación
nativa: completadas para el código descubierto. Arranque: en adaptación, con
las inicializaciones originales ya comprobadas en JA/EN. La introducción, las
seis páginas de tutorial, el menú principal y la selección de canción se han
recorrido con teclado físico. La carga de Hotchkiss supera la mezcla gráfica
y la consulta de auriculares y muestra su pantalla de inicio. Se ha corregido
el siguiente recorte de geometría y comprobado con la captura del cielo.
Una ejecución original ya muestra Bus Stop y las notas. SleepThreadCB y
WakeupThread se han superado dentro de la canción; la siguiente parada fue
ReferSemaStatus, ya superada en una ejecución original. La siguiente parada
fue WaitSemaCB (0x6D212BAC), ya superada en ejecución original con el
guardado correctamente montado. El siguiente bloqueo apareció al consultar
el estado de un hilo de audio. El handler expone el evento de copia completa
de la fuente al mezclador nativo, con estado, registros y retorno comprobados
para dos canales. La repetición llegó a los resultados originales de fallo, con 11 Miss
y puntuación 0. Una prueba sintética mostró Perfect/Great; todavía no
acredita una canción completada con éxito o audio sincronizado. El
escenario todavía requiere mejoras sustanciales de rendimiento. Las
mediciones históricas están en ESTADO_ACTUAL.md y la última adaptación de
audio está en validación tras una regresión de integración. También se
conservan los resultados originales de WaitThreadEnd/CB y GetThreadExitStatus.
Un ensayo sintético anterior no llegó a la canción por una ruta de guardado
incorrecta y se conserva como prueba fallida, sin acreditar gameplay.
Canción completa, idioma en Ajustes y validación del juego completo: pendientes.

El [estado comprobado](ESTADO_ACTUAL.md) recoge la llamada exacta que detiene el
arranque y las pruebas realizadas. Se avanzará por esos hitos; todavía no hay
base para dar una fecha fiable ni un porcentaje de port completado.

## Renderizado por GPU previsto

El renderizador conserva 480×272 internos. Un shader propio de Vulkan ya ejecuta borrados de rectangulos en Radeon 780M, con 8192 palabras exactas y limites comprobados. El backend opcional de borrados se ha conectado al renderizador: 67 comparaciones de toda la VRAM validan color de 16/32 bits, profundidad, mascaras, scissor y escrituras originales. Se activa con `--vulkan-clear`; la ejecucion del juego tiene evidencia independiente. Triangulos, texturas y presentacion siguen en CPU. Esta primera integracion copia y recupera los destinos por dibujo y no acredita mejora de FPS. La migración deberá conservar texturas y destinos en GPU, agrupar dibujos y respetar dependencias y realimentación sin lecturas continuas de vuelta a CPU. El renderizador propio por CPU se conserva como referencia para comparar escenas. Lógica, notas, puntuación, controles y servicios continúan en CPU; audio mantiene su reloj de muestras. Mayor resolución y tasas configurables requieren desacoplar dibujo, lógica y sonido, y comprobar velocidad, sincronización y puntuación a cada tasa. Referencias oficiales: [pipeline de Vulkan](https://docs.vulkan.org/spec/latest/chapters/pipelines.html) y [sincronización](https://docs.vulkan.org/guide/latest/synchronization.html).
