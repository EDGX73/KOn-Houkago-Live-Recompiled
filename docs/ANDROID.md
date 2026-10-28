# Android 0.2.0 beta — preparación en el dispositivo

El proyecto tiene entregas Windows y Android. Esta guía describe la beta Android arm64, que prepara el juego en el propio teléfono desde la ISO del jugador. Las instrucciones Windows siguen en el README.

1. Descarga la APK arm64 de la release v0.2.0 cuando esté publicada.
2. Instálala y selecciona tu ISO con el selector de archivos de Android (SAF).
3. Pulsa **Preparar juego**. La aplicación genera, compila y enlaza localmente el módulo del juego.
4. Al finalizar, pulsa **Jugar**.

No necesitas un PC ni importar `game.kon`. El equipo estima 20–40 minutos y aproximadamente 1 GB libre para preparar; no son medidas del publicador en el S24. La antigua APK base y el flujo de módulo precompilado son antecedentes de desarrollo, no esta entrega.

La candidata anterior fue probada por el usuario en Samsung Galaxy S24: funciona con rendimiento mejorable. Faltan en Android las opciones de vídeo/FPS/resolución de PC. No se acreditan aquí audio correcto, canción completa, rendimiento medido ni compatibilidad con otros dispositivos. La build corregida vc201 requiere nuevo test; no es idéntica a la candidata probada.

## Paquete previsto

Solo arm64-v8a, versión 0.2.0 y versionCode 201. El paquete lleva runtime y herramientas de preparación, avisos y fuentes correspondientes embebidas. Esta composición es una decisión de empaquetado: su presencia e integridad deben verificarse en la APK final. El tamaño y SHA256 finales están pendientes.

No se distribuyen ISO, EBOOT, CPK, corpus generado, pkon.elf, libkon_game.so, partidas ni claves. La ISO y los resultados de preparación pertenecen al entorno privado del jugador y no deben adjuntarse a incidencias.

El runtime nuevo incorpora instrumentación de diagnóstico posterior a la prueba del usuario. El equipo informa que no se activa en uso normal; eso no sustituye un re-test. No se publican hashes del APK antiguo como si fueran de esta build.

## Licencias

El código propio y cada componente conservan sus licencias respectivas. LLVM/NDK, libiconv, FFmpeg y pspdecrypt requieren inventario y avisos concretos. Las fuentes correspondientes y los medios de reemplazo/reenlace deben corresponder a los binarios distribuidos. El cierre de cumplimiento sigue pendiente de la auditoría final.
