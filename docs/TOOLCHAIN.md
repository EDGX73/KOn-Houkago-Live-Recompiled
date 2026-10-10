# Herramientas locales de análisis y recompilación

Raíz del proyecto: la carpeta de este repositorio.
Las herramientas están preparadas dentro de esta raíz. No se instaló ningún
compilador, emulador o dependencia de construcción global, ni se cambió el PATH
persistente del usuario.

## Procedencia y versiones

| Componente | Versión/revisión | Ubicación |
| --- | --- | --- |
| PSPRecomp | `f6e7d415c7f447b934cc3865a31eb725f353d659` | `third_party/PSPRecomp` |
| Zig portable | 0.15.2, Clang 20.1.2 | `third_party/toolchain/zig-x86_64-windows-0.15.2` |
| CMake | 4.4.3 | `third_party/python_build_deps/cmake/data/bin` |
| Ninja | distribución Python 1.13.2 | `third_party/python_build_deps/bin/ninja.exe` |

PSPRecomp procede del [repositorio de su autora](https://github.com/jessicanataliagta/PSPRecomp)
y conserva la licencia MIT. Se hizo una copia Git superficial y sparse de
`src`, `include`, `tools`, `tests`, `configs` y `docs`, más los archivos de la raíz.
No se descargó ni se seleccionó ningún perfil de GTA, ni su corpus generado.
Los cambios locales posteriores se registran por separado respecto a esta
revisión de base.

Zig se descargó de [ziglang.org](https://ziglang.org/download/0.15.2/zig-x86_64-windows-0.15.2.zip).
El SHA-256 del ZIP coincide con el [índice oficial](https://ziglang.org/download/index.json):

```text
3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c
```

El ZIP ocupa 92.614.574 bytes. El archivo del índice y el ZIP se conservan
junto al compilador. `third_party/toolchain/provenance.json` recoge la revisión,
versiones, direcciones de origen y la validación inicial.

[CMake](https://pypi.org/project/cmake/4.4.3/) y
[Ninja](https://pypi.org/project/ninja/1.13.2/) se instalaron como paquetes
binarios con `pip --target third_party/python_build_deps --no-cache-dir`.
La preparación utiliza el Python del sistema (o el indicado en `KON_PYTHON`); el script de construcción ejecuta los
binarios nativos locales y no depende de módulos Python instalados globalmente.
Ninja informa `1.13.2.git.kitware.jobserver-pipe-1` como versión nativa.

## Reconstrucción y pruebas

Desde la raíz del proyecto:

```powershell
.\tools\build_framework.ps1 -Jobs 2
```

El script configura Ninja, Release, C++20 y target `x86_64-windows-gnu`.
`PSPRECOMP_PROFILE` queda vacío; solo se construyen el framework y sus pruebas.
Los adaptadores `zig-cc.cmd`, `zig-cxx.cmd`, `zig-ar.cmd` y `zig-ranlib.cmd`
están en `third_party/toolchain`, junto a `zig-windows.cmake`.

Durante la construcción se usan `work/zig-cache` y `work/zig-global-cache`.
Los temporales de las pruebas quedan en `work/framework-temp`. El script restaura
las variables de entorno que modifica al terminar.

Resultados:

- `build/framework/psp_analyze.exe`: análisis ELF/PRX y mapas CSV.
- `build/framework/psp_recomp.exe`: generación C++ manual o automática.
- `build/framework/dump_function.exe`: volcado de funciones para inspección.
- `build/framework/psprecomp_tests.exe`: pruebas genéricas de upstream.
- `build/framework/libpsprecomp_core.a`: biblioteca del framework.

La validación inicial del checkout upstream limpio terminó correctamente:
CTest 1/1, 100 % de pruebas aprobadas. El registro inicial completo es
`build/framework/build_framework.first.log`; la ejecución más reciente se guarda
en `build/framework/build_framework.log`. Hubo avisos de conversión de signo
upstream en `common.hpp`; no fueron errores de compilación. Reconstruir tras
cualquier parche del proyecto y revisar de nuevo ese registro.

## Primer análisis del ELF de K-On

Se ejecutó el analizador, sin generar un corpus del juego, sobre
`analysis/executables/pkon.elf`, SHA-256
`eda890135ea4aee1a8d0a82b2f00a7159060c58498a6705df7518b36e46b85b1`.

El informe inicial de upstream está en
`analysis/recomp_framework/pkon_framework.json`, junto a los CSV y el registro.
Reconoció y aplicó 118.256 relocaciones con cero inválidas o sin soporte.
Sin embargo, no encontró module_info: `module: null` y cero imports son una
limitación del análisis inicial. Además, el mapa automático trató datos del
segmento ejecutable como código. Ese primer mapa no debe usarse para generar
un corpus. La corrección y el nuevo análisis del proyecto se documentan aparte.

`psp_recomp --help` mostró los modos disponibles y terminó con código 2, que
es el comportamiento de uso definido por upstream. El registro está en
`analysis/recomp_framework/psp_recomp_help.log`.

## Prueba del montaje de contenido

`src/content_mount_test.cpp` se compiló con este Zig, C++20 y `-O2` hacia
`build/content_mount_test.exe`. Se ejecutó con la raíz del proyecto y terminó
con código 0. El registro es `analysis/content_mount_test.log`.

La prueba comprobó los archivos reales JP/EN, sonido compartido, persistencia
del idioma en `work/tests/language.ini`, rechazo de rutas inválidas y rechazo
del idioma español mientras falta su pack. Esto valida el componente de montaje;
no prueba la ejecución del motor, gráficos, canciones, controles o guardado del
juego. Estas herramientas y pruebas de componentes no constituyen un port jugable.
