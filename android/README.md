# Aplicación Android pública — 0.2.0 beta arm64

La app prepara el juego en el dispositivo: instalar APK → seleccionar ISO propia por SAF → **Preparar juego** → **Jugar**. No requiere importar game.kon desde un PC. Consulta [la guía Android](../docs/ANDROID.md) para límites y prueba del usuario en S24.

La candidata corregida se prepara con versionName 0.2.0, versionCode 201 y solo arm64-v8a. La integración conserva el certificado de la instalación anterior; debe comprobarse en la APK final. Las claves y contraseñas de firma son privadas y nunca se publican.

## Empaquetado

La receta vigente usa `tools/make_android_public.py` con el runtime, toolchain, icono geométrico nuevo y directorios de avisos y fuentes correspondientes. El script y sus opciones pertenecen al equipo Android: consultar su ayuda y receta auditadas; esta guía no autoriza ejecutar builds ni publicar.

Se prevén avisos en `assets/licenses/`, NOTICE en `assets/NOTICE.txt` y fuentes correspondientes en `assets/toolchain/SOURCES/`. Verificar el contenido real, CRC, ausencia de duplicados, arquitectura, firma, versión y hashes del ZIP final. La fuente provisional recortada de pspdecrypt está descartada. El inventario incluye FFmpeg estático del runtime además de las bibliotecas del toolchain.

No se incluyen ISO/EBOOT/CPK/corpus/pkon.elf/libkon_game.so/partidas/claves. Las recetas fuente no deben arrastrar datos del juego, rutas personales ni secretos.

## Actualización y caché

La documentación anterior indicaba que el actualizador ignora draft/prerelease; revisar el código final antes de elegir el estado de la release beta. Los manifiestos deben corresponder a la nueva APK: paquete, ABI, versionCode, certificado, bytes y SHA256. Cambiar de certificado impide actualizar sobre la instalación existente.

El equipo informa que `GameBuilder.ensureToolchain` usa `assets/toolchain/toolchain_id.txt` para invalidar la caché. La build con bibliotecas sustituidas debe llevar el nuevo identificador; elevar versionCode por sí solo no actualiza esa caché. No se requiere ni se propone borrar los datos de la aplicación.

## Evidencia y límites

La prueba S24 comunicada corresponde a la candidata anterior. El runtime corregido tiene cambios de diagnóstico posteriores; necesita nuevo test. La auditoría estática, compilación, firma y diagnóstico del núcleo no demuestran una canción completa ni audio correcto. No se trasladan instrucciones antiguas de APK base/x86_64 a esta entrega arm64.
