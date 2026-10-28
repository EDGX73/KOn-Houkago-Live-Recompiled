# Publicación y auditoría

Este repositorio público contiene solo el código y la documentación del port.
No incluye ISO, EBOOT, CPK, código generado a partir del juego, partidas,
claves ni datos personales. El modelo es «trae tu ISO»: el juego se prepara en
el equipo del jugador.

## Antes de un pull request

```powershell
python tools\audit_public.py --tree .
python tools\audit_public.py --git-commits . origin/main..HEAD
python -m unittest discover -s tools\tests -p "test_*.py"
```

`tools/audit_public.py` bloquea formatos y nombres del juego, código traducido,
binarios no previstos, rutas personales y secretos. Las excepciones deben estar
justificadas en `tools/audit_allowlist.json`; no se añaden datos del juego,
datos personales ni secretos. El workflow
`.github/workflows/publication-audit.yml` repite la auditoría en cada pull
request y push a `main`.

## Releases

Una release se crea solo con autorización del autor y después de auditar el
conjunto exacto que se va a publicar: código, binarios, avisos, licencias,
fuentes correspondientes, manifiestos y sumas SHA-256. Los binarios que
redistribuyan componentes de terceros deben incluir sus avisos y, cuando la
licencia lo exija, la fuente correspondiente y un medio de reenlace.

No se publican activos que contengan datos del juego, claves, partidas o
componentes cuya procedencia y permiso de redistribución no estén acreditados.

## Auditoría de la release v0.1.0 (2026-10-20)

Los tres archivos publicados coinciden byte a byte con los locales. La
auditoría revisó 16.149 archivos (paquete descomprimido y los dos ZIP) sin
datos del juego, código traducido, datos personales ni secretos; las
excepciones aceptadas están justificadas en `tools/audit_allowlist.json`.
Evidencia conservada localmente. No se publicó una release `v0.1.1`.

## Estado a 2026-10-28

La última release publicada es **v0.1.0** (Windows). La **0.2.0** está en
preparación. Su APK Android y sus paquetes Windows no están publicados: falta
cerrar la revisión de licencias y procedencia de componentes de terceros y
repetir la prueba en el dispositivo. No existe una release `v0.2.0`.
