# 0.2.0 beta — borrador pendiente de artefactos finales

Port para Windows y Android. Android arm64 incorpora preparación del juego en el teléfono desde la ISO del usuario: instalar APK → seleccionar ISO por el selector de archivos (SAF) → «Preparar juego» → «Jugar». No necesita PC ni importación de `game.kon`. No incluye el juego ni su módulo generado. El equipo técnico estima 20–40 minutos y aproximadamente 1 GB libre para preparar: estimaciones pendientes de validar en el dispositivo.

El usuario probó la candidata Android anterior en Samsung Galaxy S24 y comunicó funcionamiento con rendimiento mejorable y menos opciones que PC. La build corregida vc201 requiere repetir esa prueba. No se acreditan métricas, audio correcto ni una partida completa.

Correcciones previstas: branding geométrico original, avisos y licencias de dependencias, fuentes correspondientes de pspdecrypt y de cualquier otro componente que las requiera. Este texto no declara todavía satisfechas esas obligaciones.

Pendiente para cerrar la propuesta: archivos definitivos de Windows y Android, fuentes y avisos, bytes/SHA256, manifiestos de actualización, commits y diff completos, auditoría de historial y branding Windows, resultado del re-test y confirmación final expresa del usuario. No descargar la APK base antigua como sustituto de esta beta.

Destino propuesto: EDGX73/KOn-Houkago-Live-Recompiled, rama main, tag v0.2.0. La marca prerelease y el comportamiento de los actualizadores deben acordarse tras revisar su código; no convertir una beta en estable solo para que el actualizador la detecte.
