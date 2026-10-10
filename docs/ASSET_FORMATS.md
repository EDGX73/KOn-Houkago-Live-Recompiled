# Formatos y extracción de recursos

Estos lectores propios funcionan sobre copias extraídas de las ISO. Las ROM permanecen intactas. Ningún lector incorpora código de teatimetools, CriPakTools o el script UNO de Minmode.

## Herramientas disponibles

- `tools/cpk_archive.py`: `list` y `extract`, TOC por nombres, ITOC por identificadores, tablas @UTF simples/ofuscadas y descompresión CRILAYLA. No recompone archivos CPK.
- `tools/fmdx_archive.py`: `list` y `extract` de contenedores FMDX sin compresión interna.
- `tools/test_asset_archives.py`: fixtures propios de tablas constantes/cero/por fila, CPK ofuscado, base de offsets, ITOC ordenado/alineado, CRILAYLA, FMDX, límites, rutas y preservación del contenido existente.

Ejemplos desde la raíz del proyecto:

```powershell
python -B tools/cpk_archive.py list analysis/extracted/jp/PSP_GAME/USRDIR/std.cpk --output analysis/std_jp_listing.json
python -B tools/cpk_archive.py extract analysis/extracted/jp/PSP_GAME/USRDIR/std.cpk --output assets/source/jp/std --pattern "PSP_GAME/USRDIR/*" --manifest analysis/std_jp_manifest.json
python -B tools/fmdx_archive.py list assets/source/jp/std/PSP_GAME/USRDIR/bind/backup.bin
python -B tools/test_asset_archives.py
```

`list --hash` calcula SHA-256 sobre el contenido decodificado de cada miembro y sobre el archivo fuente. La extracción calcula estas huellas siempre. Si un destino existe y tiene exactamente la misma huella, se reutiliza; si difiere, se rechaza la sobrescritura. Las rutas absolutas, rutas ascendentes, nombres reservados de Windows y enlaces que escapan del destino se rechazan. Metadatos limitados a 128 MiB y miembros individuales a 512 MiB por defecto (`--max-file-size` permite un límite explícito distinto).

## CPK y @UTF

Paquete exterior: firma `CPK ` / `TOC ` / `ITOC`, entero de control LE de 32 bits y longitud LE de 64 bits, seguidos de una tabla @UTF. Si el payload no comienza por @UTF se desofusca con XOR: clave inicial de byte 0x5F, multiplicación por 0x15 módulo 256 por byte (equivale a los valores de 32 bits 0x655F y 0x4115 de las referencias).

@UTF usa escalares big endian. Su header mide 32 bytes: firma; longitud menos ocho; offsets de filas, cadenas y datos relativos al byte 8; offset del nombre de tabla relativo a cadenas; número de columnas u16, tamaño de fila u16 y número de filas u32. Una columna es flags u8 + offset de nombre u32 y opcional valor constante. Nibble alto: 0x00 ausente, 0x10 cero, 0x30 constante, 0x50 por fila. Nibble bajo: enteros 8/16/32/64 bits con/sin signo, float32/float64, string (0xA) y blob (0xB).

TOC identifica `DirName`, `FileName`, `FileSize`, `ExtractSize`, `FileOffset`, `ID`. El offset absoluto de cada payload es `FileOffset + min(ContentOffset,TocOffset)`. ITOC contiene DataL/DataH como tablas anidadas; los identificadores se ordenan, comenzando en ContentOffset y alineando cada tamaño almacenado al valor Align. Si existe TOC, tiene prioridad para conservar nombres; no se duplica ITOC sobre los mismos recursos. ETOC/GTOC no se interpretan: no hacen falta para las entradas de estas dos ISO.

CRILAYLA preserva los primeros 256 bytes sin comprimir al final del payload. Header: firma de ocho bytes, tamaño del resto decodificado u32 LE y tamaño del stream comprimido u32 LE. Se recorre el stream desde su último byte hacia el primero, bits MSB primero, generando la salida de atrás hacia delante. Bit 0: literal de ocho bits. Bit 1: distancia de trece bits +3, longitud base 3 incrementada con grupos de 2,3,5,8 bits y extensiones de ocho bits si todos se saturan. El lector comprueba tamaños y referencias antes de escribir.

Fuente primaria de comparación: [CriPakTools, CPK.cs](https://github.com/esperknight/CriPakTools/blob/master/CriPakTools/CPK.cs), autor EsperKnight a partir del trabajo de Falo/Nanashi3. El repositorio consultado no tiene archivo de licencia explícito en la raíz; se utiliza para entender el formato, no se incorpora su código.

## FMDX

Header de 0x54 bytes: firma FMDX, tamaño declarado u32 LE @0x04, cantidad u32 LE @0x08 y campos restantes todavía no interpretados completamente. Directorio desde 0x54; cada entrada de 0x90 bytes contiene nombre ASCII terminado en cero [0x80], offset absoluto u32 LE @0x80, tamaño u32 LE @0x84 y campos desconocidos u32 @0x88/@0x8C. Estos últimos se conservan en listados. Se extrae el intervalo indicado; los payloads pueden ser modelos, animaciones, gráficos u otros recursos.

Fuentes primarias: [Scarlet/FMDX.cs](https://github.com/xdanieldzd/Scarlet/blob/master/Scarlet.IO.ContainerFormats/FMDX.cs) y [teatimetools/fmdx.bt](https://github.com/Frain-Breeze/teatimetools/blob/master/doc/templates/fmdx.bt). Scarlet tiene [licencia MIT](https://github.com/xdanieldzd/Scarlet/blob/master/LICENSE.md), autor xdaniel (Daniel R.). Teatimetools, del autor del parche Frain-Breeze, no presenta licencia explícita en la raíz del checkout consultado.

## Texturas GBIX / UVRT (.uvr)

GBIX es la firma exterior; UVRT comienza @0x10. Tamaño UVRT u32 LE @0x14, datatype u8 @0x18, imagetype u8 @0x19, campos desconocidos u16 @0x1A, anchura/altura u16 LE @0x1C/0x1E. Paleta/pixels comienzan @0x20. Tipos básicos: datatype 0 ABGR1555,1 BGR565,2 ABGR4444,3 ABGR8888; 0xA DXT1 PSP. Indexed4 usa imagetype 0x86/0x88 y 16 colores; Indexed8 0x8A/0x8C y 256 colores. Los pixels sin compresión DXT requieren deswizzle PSP. Scarlet indica formas todavía sin soporte y un DXT 0xC desconocido: no se debe asumir que toda textura puede exportarse ya.

Fuente primaria: [Scarlet/GBIX.cs](https://github.com/xdanieldzd/Scarlet/blob/master/Scarlet.IO.ImageFormats/GBIX.cs), bajo la licencia MIT anterior. El script de Minmode aporta ejemplos más limitados de UVR, pero debe preferirse leer datatype explícito para las paletas.

## Modelos UNO y UNA

El script público [fmt_K-On_uno.py](https://drive.google.com/file/d/1G0Wu-UStICtsf2pSOOBbvPRuVz1U4pOc/view) identifica UNO por firma NUIF y carga la pareja UNA del mismo nombre para los nombres/huesos. Está firmado `By Minmode`; no incluye licencia explícita. Su lógica lee tablas de texturas externas UVR, materiales, mapas de huesos y strips. Tipos de vértice observados: 0x01 (20 bytes: UV floats+posición), 0x05/0x45 (24 bytes: pesos+UV+posición), 0x85/0xC5 (28 bytes: cuatro pesos+UV+posición). Construye huesos con matrices inversas y relaciones parentales.

Este script es evidencia útil para un importador propio de geometría/esqueleto; no resuelve animaciones del juego, escenarios generales, comportamiento o motor nativo. Los casos de escenario deben verificarse individualmente antes de declarar compatibilidad.

## Textos y lenguaje

Los contenedores FMDX permiten alinear recursos por nombre japonés/inglés. La comparación usa SHA-256 **del contenido decodificado**, no el envase CPK comprimido. Así se detectan cambios reales del parche y recursos compartidos. Las tablas de texto de eventos requieren además relacionar índices de glifos con su fuente local; no son necesariamente Shift-JIS directamente.

La plantilla primaria [tts_event_text.bt](https://github.com/Frain-Breeze/teatimetools/blob/master/doc/templates/tts_event_text.bt) describe filas de 64 bytes: líneas usadas u16, máscara de hablantes u16 y tres filas de diez índices u16. [tts_event.bt](https://github.com/Frain-Breeze/teatimetools/blob/master/doc/templates/tts_event.bt) describe otro tipo de archivo TTS como guion con contadores, referencias y opcodes de escenas/poses/sonidos/cuadros de texto. La extensión compartida no basta para tratar ambos como cadenas legibles.

Página primaria del parche: [Frain-Breeze / K-On](https://frain-breeze.github.io/kon/), créditos Frain_Breeze, Wxnder, KingSizzle. Documenta áreas de texto no traducidas/corruptas: algunas descripciones de desbloqueo de eventos, algunos menús/popup de Sing, y aviso de obtención de objetos. La futura versión nativa debe importar y validar datos, no reproducir automáticamente estos errores.

## Límites del resultado actual

Estos lectores convierten envases de datos a archivos inspeccionables; no ejecutan el juego ni constituyen un port. El motor, timings y charts, render/animación, persistencia, eventos, desbloqueos y pruebas comparativas permanecen trabajos separados. La extracción de padding.bin se omite porque es relleno del disco; no contiene recursos del juego. El audio se lista sin decodificación masiva inicial.


## Partituras KSD: estructura observada

El lector propio `tools/music_chart.py` conserva un encabezado de ocho bytes,
una tabla de grupos de ocho bytes y registros de notas de doce bytes. Comprueba
los índices, sus cantidades y la pertenencia de cada nota a su grupo, sin
ordenar ni descartar los campos desconocidos. Algunos grupos finales sin notas
reinician su valor temporal; se preservan. Dentro de un grupo, el original
ordena notas por tipo, no siempre por tiempo.

La auditoría de todos los paquetes ingleses encontró 1520 apariciones y 759
partituras distintas por hash. Todas se reconstruyen con bytes idénticos;
`analysis/music_chart_layout_audit_en.json` conserva los resultados. El paquete
de Hotchkiss contiene variantes para los cinco instrumentos y dificultades.
Normal/Yui tiene 150 registros, incluidos tres con duración y un marcador.
El significado completo de flags, unidad temporal, botones y duraciones requiere
verificación durante el juego. Un round-trip exacto no acredita que un chart
modificado sea jugable ni que las notas estén sincronizadas con el sonido.
