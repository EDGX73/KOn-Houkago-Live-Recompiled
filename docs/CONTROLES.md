# Controles del port para PC

Abre `K-On Houkago Live Recompiled.exe` (el lanzador prepara el juego desde tu
ISO la primera vez) o el ejecutable del juego. La ventana debe tener el foco.
**F1 muestra u oculta esta guía dentro del juego**; no pausa la ejecución.

| Control PSP | Tecla |
| --- | --- |
| Cruceta arriba / abajo / izquierda / derecha | Flechas |
| Triángulo △ | S |
| Círculo ○ | X |
| Cruz × | Z |
| Cuadrado □ | A |
| L | Q |
| R | W |
| START | Enter |
| SELECT | Retroceso (Backspace) |
| Stick arriba / izquierda / abajo / derecha | I / J / K / L |

Se pueden mantener varias teclas a la vez. El stick admite diagonales; las
direcciones opuestas se cancelan. El stick de teclado alcanza el extremo de
cada eje, mientras que un mando puede dar posiciones intermedias.

Las etiquetas de los diálogos siguen mostrando botones PSP: **○ corresponde a
X del teclado** y **× corresponde a Z**. El botón de confirmar/volver lo decide
cada pantalla del juego: la barra inferior muestra cuál es cuál. En la
traducción inglesa aparece «✕ Enter / ○ Back» (Z confirma, X vuelve); en
japonés puede invertirse. START y SELECT conservan su significado original:
no todas las pantallas del juego responden a ellos.

El mapeo cubre todos los controles de juego de PSP. HOME, brillo, volumen,
interruptor WLAN y encendido son controles del sistema de la consola y no
acciones enviadas al juego. El cierre y el volumen se gestionan desde Windows.
La reasignación se hace editando `config/controls.ini` (ver README); una
pantalla de reasignación dentro de Ajustes sigue pendiente.

El port está en beta: si una función pendiente detiene el juego, el juego deja
`ultimo_fallo.txt` junto a su carpeta; adjúntalo en un issue. Una parada del
port no debe confundirse con un fallo de tecla.
