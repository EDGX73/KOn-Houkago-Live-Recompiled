# Auditoría del ejecutable de K-On! Hōkago Live!!

## Resultado confirmado

Las dos imágenes suministradas contienen el mismo ejecutable, según `analysis/iso_comparison.json`. El parche inglés no aporta un binario distinto ni un ELF ya expuesto. El juego puede estudiarse a partir del ELF recuperado por el proceso de extracción del proyecto, sin modificar las imágenes originales.

| Archivo examinado | Tamaño | Resultado |
|---|---:|---|
| `analysis/extracted/jp/PSP_GAME/SYSDIR/BOOT.BIN` | 4.019.288 bytes | Todos los bytes son cero: marcador, sin código |
| `analysis/extracted/jp/PSP_GAME/SYSDIR/EBOOT.BIN` | 4.019.632 bytes | Contenedor `~PSP`, módulo `pkon`, versión 1.1 |
| `analysis/extracted/jp/PSP_GAME/SYSDIR/OPNSSMP.BIN` | 880 bytes | Contenedor `~PSP` auxiliar; no analizado como código |
| `analysis/executables/pkon.elf` | 4.019.288 bytes | ELF32, little-endian, MIPS, PRX `e_type=0xFFA0` |

SHA-256 del ELF recuperado: `eda890135ea4aee1a8d0a82b2f00a7159060c58498a6705df7518b36e46b85b1`.

El encabezado `~PSP` de EBOOT declara tag `0xD91613F0`, modo `0x00000009`, tamaño ELF 4.019.288 y entrada `0x000FC1F8`. El auditor solo lee metadatos; no incorpora claves, descifra ni ejecuta el juego. El ELF fue recuperado por separado en una copia de trabajo. El tag es un identificador del encabezado, no una clave.

## ELF y módulo

El ELF tiene 55 encabezados de sección: 42 PROGBITS, 10 PSPREL, 1 NOBITS, 1 STRTAB y 1 NULL. Todos los nombres de sección están ausentes. No hay tablas SYMTAB/DYNSYM: faltan nombres originales y límites de funciones. La localización de funciones seguirá necesitando análisis de flujo, referencias, registros de ejecución y revisión de casos indirectos.

| Programa | Tipo | VA original | Offset de archivo | Bytes en archivo | Bytes en memoria | Flags |
|---:|---|---|---|---:|---:|---|
| 0 | PT_LOAD | `0x00000000` | `0x000000A0` | 3.062.688 | 3.062.688 | R/W/X |
| 1 | PT_LOAD | `0x002EBBA0` | `0x002EBC80` | 5.736 | 754.620 | R/W |
| 2 | PT_NULL | — | — | 0 | 0 | — |

`SceModuleInfo` se localiza en el offset de archivo `0x001F7C7C`, codificado en el `p_paddr` del primer programa. Nombre `pkon`, atributos 0, versión 1.1, GP declarado 0. La entrada ELF es `0x000FC1F8`. Son direcciones originales del PRX, anteriores a elegir la dirección base del proceso PSP. No se ha supuesto una base de carga ni se han aplicado relocaciones.

La tabla de imports ocupa `[0x001F79E4, 0x001F7BD8)`, con 25 entradas de 20 bytes. Se ha recorrido completa y las cadenas, NIDs y stubs quedan dentro de regiones PT_LOAD respaldadas por el archivo. Hay 238 funciones importadas y 0 variables. Cada NID se conserva en hexadecimal junto a su dirección y sus dos palabras de stub; el informe no adivina nombres de funciones.

| Biblioteca | Funciones |
|---|---:|
| sceAudio | 12 |
| IoFileMgrForUser | 18 |
| Kernel_Library | 7 |
| LoadExecForUser | 2 |
| ModuleMgrForUser | 5 |
| StdioForUser | 3 |
| SysMemUserForUser | 6 |
| ThreadManForUser | 44 |
| UtilsForUser | 5 |
| sceRtc | 5 |
| sceCtrl | 3 |
| sceDisplay | 5 |
| sceGe_user | 10 |
| sceDmac | 1 |
| sceUmdUser | 8 |
| sceMpeg | 31 |
| scePower | 2 |
| sceAtrac3plus | 15 |
| sceUtility | 24 |
| sceNet | 4 |
| sceNetAdhocctl | 11 |
| sceNetAdhoc | 14 |
| sceWlanDrv | 1 |
| sceHprm | 1 |
| sceImpose | 1 |

Estos imports identifican trabajo real pendiente en vídeo/audio, archivos, hilos, entrada, presentación, GE y red local. La presencia de un import no prueba qué modos del juego lo usan.

## Relocaciones e instrucciones

Hay 118.256 registros de relocación Type-A en 10 secciones, sin destinos de escritura fuera del tamaño de sus PT_LOAD y sin referencias a segmentos no cargables:

| Tipo MIPS | Registros |
|---|---:|
| R_MIPS_26 | 37.445 |
| R_MIPS_HI16 | 24.856 |
| R_MIPS_LO16 | 30.884 |
| R_MIPS_32 | 25.071 |

No hay Type-B en este ELF. La herramienta reconoce ambos formatos; su parser Type-B valida los tamaños, tablas de índices, extensiones y destinos, pero no aplica ninguna transformación a las instrucciones. Los tests sintéticos cubren su lectura mínima y la detección de truncamiento. Aún no constituye validación contra una colección de módulos PSP Type-B reales.

Se han leído 515.698 palabras de regiones SHF_EXECINSTR. El inventario de patrones produce 351 candidatos de familias VFPU, 3.741 ramas likely, 9.496 saltos/llamadas indirectos, 16.964 candidatos COP1, 22 instrucciones de familias caché/atómicas, 65 patrones break y 0 patrones syscall. Se detectan además 15 patrones vpfxs, 3 vpfxt y 2 vpfxd.

Estos son patrones estáticos, no un desensamblado completo ni cobertura de ejecución. Puede haber constantes dentro de regiones ejecutables. Cero syscalls en el archivo no implica ausencia de llamadas al sistema: el cargador PSP resuelve los imports y escribe los stubs correspondientes durante la carga.

Hay 2.010 cadenas ASCII que coinciden con nombres de API, rutas/extensiones o mensajes de diagnóstico; el JSON conserva las primeras 200 con su offset. Entre ellas aparecen rutas a `PSP_GAME/USRDIR/bind/static.bin` y archivos de música `.at3`. No se ha tratado este muestreo como extracción de textos localizables ni se han buscado cadenas sobre el ciphertext.

## Reproducir

Desde la raíz autorizada del proyecto:

```powershell
python tools/executable_audit.py analysis/extracted/jp/PSP_GAME/SYSDIR/BOOT.BIN analysis/extracted/jp/PSP_GAME/SYSDIR/EBOOT.BIN analysis/extracted/jp/PSP_GAME/SYSDIR/OPNSSMP.BIN --out analysis/executable_audit_jp.json
python tools/executable_audit.py analysis/executables/pkon.elf --out analysis/executable_audit_elf.json
python tools/tests/test_executable_audit.py
```

Seis tests sintéticos han pasado: imports/relocaciones válidos, encabezados truncados, arquitectura incorrecta, tamaño de import cero, NIDs apuntando a BSS y Type-B truncado. El input siempre se abre para lectura; `--out` es el único archivo de salida de cada ejecución. Requiere Python 3 y su biblioteca estándar.

La salida confirma que existe código Allegrex analizable y un inventario verificable de servicios PSP. Todavía faltan traducir el código, implementar el entorno de ejecución, decodificar los recursos y comprobar gráficos, audio, sincronización, guardado y todos los modos del juego en PC. Este informe no afirma que el port sea jugable.

## Fuentes primarias

- [PPSSPP: tipos ELF/PRX](https://github.com/hrydgard/ppsspp/blob/master/Core/ELF/PSPElfTypes.h): encabezados y constantes PSP.
- [PSPSDK: SceModuleInfo](https://github.com/pspdev/pspsdk/blob/master/src/user/pspmoduleinfo.h): disposición ABI del módulo.
- [PPSSPP: cargador de módulos](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceKernelModule.cpp): localización de module info, biblioteca de imports, NIDs y stubs.
- [PPSSPP: lector ELF](https://github.com/hrydgard/ppsspp/blob/master/Core/ELF/ElfReader.cpp): codificación de relocaciones A/B.
- [PPSSPP: encabezado PSP](https://github.com/hrydgard/ppsspp/blob/master/Core/ELF/PrxDecrypter.h): offsets públicos del contenedor; el auditor omite sus campos de material criptográfico.
- [PPSSPP: tablas MIPS](https://github.com/hrydgard/ppsspp/blob/master/Core/MIPS/MIPSTables.cpp): familias de opcode usadas únicamente para el inventario de patrones.

Fuentes consultadas el 3 de octubre de 2026. El parser es una implementación independiente en Python basada en formatos públicos; no copia rutinas del emulador. Las fuentes no sustituyen las medidas directas sobre estos archivos.

## Integración con el analizador de PSPRecomp

La primera ejecución del framework (`analysis/recomp_framework/pkon_framework.json`) confirma exactamente los mismos 118.256 registros de relocación, todos soportados y válidos, pero su resultado inicial `module: null` e `import_count: 0` procede de un supuesto de su lector: `src/elf32.cpp::find_module_info` busca exclusivamente nombres `.rodata.sceModuleInfo` o `.sceModuleInfo`. Este ELF conserva secciones pero carece de sus nombres. No es evidencia de que el juego no importe servicios PSP.

La corrección necesaria es usar el fallback de `p_paddr` del primer programa cuando no existe una sección identificable. Validado para este archivo:

```text
module_info_file_offset = first_segment.paddr & 0x7FFFFFFF = 0x001F7C7C
module_info_original_va = first_segment.vaddr + module_info_file_offset - first_segment.offset
                        = 0 + 0x001F7C7C - 0x000000A0 = 0x001F7BDC
module_info_runtime_va  = first_segment_runtime_va + module_info_file_offset - first_segment.offset
                        = 0x08804000 + 0x001F7C7C - 0x000000A0 = 0x089FBBDC
```

Antes de usar el fallback deben verificarse PT_LOAD, tamaño de la estructura de 52 bytes, respaldo en el archivo del primer segmento y ausencia de desbordamiento. El lector del framework recibe memoria después de la relocación; debe leer ahí los punteros del módulo y no sumar la base otra vez. Se ha comprobado que los cuatro campos `ent_top`, `ent_end`, `stub_top` y `stub_end` tienen registros R_MIPS_32 contra el segmento 0 en los sitios originales `0x001F7C00`, `0x001F7C04`, `0x001F7C08` y `0x001F7C0C`.

Además, `src/program_analysis.cpp::executable_ranges_for` toma actualmente todo PT_LOAD con PF_X. En K-On! el primer PT_LOAD tiene flags R/W/X e incluye datos. Las 26 secciones PROGBITS con SHF_EXECINSTR ofrecen un límite más preciso y forman un intervalo contiguo original `[0, 0x001F79C8)` de 2.062.792 bytes. El PT_LOAD entero abarca `[0, 0x002EBBA0)` y añade 999.896 bytes de datos a la búsqueda automática de código.

Se recomienda priorizar las secciones SHF_EXECINSTR, comprobar su respaldo PT_LOAD, fusionar intervalos contiguos y usar PF_X como fallback solo cuando falten esas secciones. Los 26.217 candidatos de función y las 729.376 apariciones de instrucciones no soportadas del informe inicial no deben publicarse como censo definitivo: se analizaron datos como instrucciones y los totales también incluyen solapamiento entre candidatos. Las correcciones del framework corresponden al proceso de integración, manteniendo intacto el ELF recuperado.
