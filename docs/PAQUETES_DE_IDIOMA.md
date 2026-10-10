# Añadir idiomas al mismo programa

El montaje de recursos ya acepta paquetes adicionales sin recompilar el
ejecutable. Los identificadores usan letras minúsculas, números y guiones:
`es`, `fr`, `pt-br`, etc. Japonés (`ja`) e inglés (`en`) usan las extracciones
existentes; los demás se buscan bajo `assets/locales/<identificador>`.

Un paquete debe contener:

```
assets/locales/es/PSP_GAME/USRDIR/std.cpk
```

Ese archivo debe ser un contenedor válido con los recursos localizados del
juego. Añadir una carpeta vacía no crea una traducción. Texto, gráficos con
texto y fuentes pueden requerir modificaciones. El sonido se comparte desde
la extracción japonesa; los archivos ausentes del paquete usan esa base.

`available_languages()` descubre los paquetes instalados y `select()` valida
el identificador y su ruta. El diagnóstico puede iniciarse con
`--language es` cuando exista el paquete. Las pruebas usan recursos sintéticos
aislados para comprobar español y un idioma adicional; no prueban una
traducción española del juego.

Todavía falta conectar esta lista a Ajustes, guardar la preferencia del jugador
y hacer la recarga segura de recursos y cachés. El cambio durante una canción
no se expondrá. También siguen pendientes las herramientas para editar texto,
fuentes, gráficos y reconstruir los paquetes. El objetivo final es instalar
idiomas y mods sin modificar las ISO originales.
