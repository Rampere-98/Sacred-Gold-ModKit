# Tareas pendientes del agente (lista viva)

Volver: [[Inicio]] · ver también [[Estudio-mejoras]], [[Pendientes]] (mods futuros), [[Atascos-diagnostico]], [[Equipo]].

Lista única de lo que queda por hacer, lo que está a medias y lo que se deja aparcado a propósito. **Regla:** al terminar una tarea se tacha aquí con su commit; al aparcar una, se apunta aquí con el motivo. Lo mantiene el coordinador. Última puesta al día: 2026-11-13, 13:15.

Cómo se elige la siguiente: primero lo que **rompe** (caídas, cortes), luego lo que deja copias **a cero de experiencia**, luego **misiones que no salen**, luego las guías, luego lo demás.

## 0. En marcha ahora mismo

| Qué | Quién | Estado |
|---|---|---|
| Tanda de misiones (oveja, clic a 6 casillas, ronda de 25, última misión bloqueada): commits dd45044, 9da596b, f55652c | coordinador | compilando en la CI; instalar y comprobar en los registros |
| Quién mata las 8 copias cada ~31 min | auditor del entorno | vigilancia de procesos en marcha desde las 12:37; esperar un ciclo |
| Pasada 4 de la descompilación (lotes 303-364, 2461 funciones) | Manolito | parada: Codex y Gemini sin cuota; retomar al volver la cuota o el domingo 15 a las 21:00 |

## Reparto entre sesiones (2026-11-13, 13:40)

Cuota semanal al **89 %** (tope de trabajo complejo: 95 %; renovación: domingo 15-11 a las 21:00). Por eso solo hay dos encargos nuevos en marcha y el resto queda asignado para después de la renovación. Solo el coordinador edita `src/` e instala.

| Sesión | Modelo | Encargo | Cuándo |
|---|---|---|---|
| Coordinador (esta) | Fable | instalar la tanda de misiones, comprobarla en los registros, aplicar lo que devuelvan las demás | ahora |
| Revisor de código | Sonnet | revisar el código de hoy que lee memoria del juego (mapa, canje 4:1) en busca de caídas | **en marcha** |
| Solucionador de atascos | Sonnet | «20 s quieto a caballo» (A3, A8) y quién abre el inventario | **en marcha** |
| Auditor del entorno | Sonnet | quién mata las 8 copias cada ~31 min | **en marcha** (vigilancia) |
| Analista | Sonnet | medir exp./hora y misiones (`Misiones-medir.py`) antes y después; misiones sin coger frente a [[QUESTS]] | cuando no haya cortes o tras la renovación |
| Investigador web | Sonnet | plantillas de combo por clase con sus artes; estrategia de Adelina; caballos gratis | tras la renovación |
| Bóveda de Sacred | Sonnet | los 61 corchetes falsos; qué se sube a git de `docs/` | tras la renovación |
| Manolito | Opus + Codex | pasada 4 (lotes 303-364); verja del cementerio (oso); clase de suelo 3 = mar; marca real de interior; `0x54FBC0` | cuando vuelvan Codex y Gemini o tras la renovación |
| Verificador de hallazgos | Sonnet | comprobar en vivo lo instalado hoy (§4 y §5 «verificar») | tras la renovación |

## 1. Roturas (lo primero)

- [ ] **Las 8 copias mueren a la vez cada ~31 min, sin guardar** (08:49, 09:21, 11:02, 11:33, 12:05 del 13-11). Descartado: instalaciones, tareas programadas, `vigilante.ps1`, `agentes.sh`, Pulso, `ExitProcess` del mod, falta de memoria. Sospecha: Steam (mismo identificador de juego 12320 en las 8 copias y en el juego del dueño). Mientras dure, ninguna medición de 90 min es válida y las reglas de «6 minutos» casi no llegan a contar.
- [ ] **Comprobar que no vuelven las 3 caídas del 13-11** (dos al leer sectores del mapa, una en la lista de runas del canje 4:1), corregidas en d2981b2. Mirar los `.dmp` nuevos y el registro de eventos tras 3 horas.
- [ ] **Caída 4502AE del Elfo oscuro en el canje** (modelo de la runa): se reactivó el canje confiando en la protección; vigilar.
- [ ] **A3 no guardó antes del reinicio** en dos instalaciones: una copia en pelea o con poca vida no llega a guardar en 90 s. Valorar esperar más o guardar al salir de la pelea.

## 2. Copias sin progresar (experiencia ≈ 0)

- [ ] **Medir de nuevo** exp./hora, «going to look» y casi muertes cuando no haya cortes (analista, `Estudio-medir.py` y `Criterios-medir.py`). La medición de las 12:26 no vale por los cortes.
- [ ] **Meta que no se cumple** (P1): hay tres reglas (6 min en la meta, 6 min mirando alrededor, 3 veces sin camino). Comprobar que se disparan; solo se vio 1 vez.
- [ ] **«20 s quieto a caballo»**: sigue alto en A3 y empeoró en A8 (de 1 a 24 por hora) tras el arreglo del caballo. Causa sin encontrar.
- [ ] **Regiones bajas** (P2): comprobar que el Daemon de nivel 13 ya no vuelve a la región 1; falta que busque la región de su nivel por la misión principal.
- [ ] **Inventario que se abre solo** (P5): se cierra ya con la función de la ventana (3 veces vistas), pero no se sabe quién lo abre.
- [ ] **Volver a la tienda con 2 pociones o menos** (P6): compra hasta el stock al pasar, pero no sale a buscar tienda.
- [ ] **Retirarse con 3 pociones bebidas y pocas opciones de ganar** (P4): la mejor señal de casi muerte (38 % de acierto), sin implementar.
- [ ] **A3 (Mago): 12,2 casi muertes por hora**, el doble que las demás; ver si la distancia de 8 casillas se aplica.

## 3. Misiones (objetivo: completar todas; el final de campaña queda bloqueado hasta entonces)

Medir con `python -I docs/Misiones-medir.py` (lee el diario de las 8 copias). Pasos de cada una en [[WEB_QUESTS_ANCARIA]] («Pasos de las misiones que el agente no completa») y causas en [[Atascos-diagnostico]] («Misiones que no avanzan»).

| Misión | Hechas/abiertas (13-11) | Causa | Estado |
|---|---|---|---|
| Oveja perdida (15005) | 0/6 | no hablaba con animales; luego es escolta lenta | arreglo dd45044 por comprobar; falta ver la entrega a Hank |
| Libro de cuentas (15004) | 0/6 | libro en el suelo dentro de una casa | clic a 6 casillas y puerta de interior por comprobar; quizá falte recoger con más alcance |
| Despacho a Arogarn (9005) | 0/5 | personaje dentro de una posada | por comprobar |
| Mensajero (152) | 0/3, 1 fallada | personaje en la taberna de Silver Creek; plazo de 3 días | por comprobar |
| Oso de batalla (15003) | 1/7 | no llega a la meta; sospecha: verja del cementerio | **sin arreglar**: registrar puertas y palancas cerca y pasarlo a Manolito |
| ¿Dónde está el herrero? (154) | 1/5 | se para en un paso intermedio (lobos o espada del jefe goblin) | **sin arreglar**: registrar el texto del objetivo en cada paso |
| Ojos de lobo (151) | 2/4 | jefe en un claro junto a la meta | ronda de 25 casillas por comprobar |
| Trágica pérdida (15011) | 2/4 | jefe al que se llega por el oeste; puerta o edificio | ronda de 25 por comprobar; puede faltar la ruta |
| Adelina (201) | 2/3, 1 fallada | la escoltada muere (13 lobos, no lucha) | **sin arreglar**: limpiar el camino antes de recogerla, ir despacio, no montar su caballo |
| Mensaje del mercader (10258), 44, 31 | 0/3, 0/3, 0/2 | zona de nivel 25 | aparcadas hasta tener nivel; no es fallo |

- [ ] **Misiones sin coger**: comparar el diario con las 251 de [[QUESTS]] por región visitada, para saber cuáles no se han aceptado nunca.
- [ ] **Poner nombre a los números 9, 20, 31, 43, 44** (hace falta la clave de texto `+0x24` del registro contra `global.res`; por la web no sale).
- [ ] **Bloqueo del final de campaña**: hoy solo cuenta las misiones que el agente ha visto. Falta compararlo con la lista completa, y cubrir el jefe final de Underworld (Anducar).
- [ ] **PNJ de misión por dirección**: `0x54FBC0` (decide el icono de misión de un PNJ, de Manolito, sin verificar) para saber quién da o cierra cada misión.
- [ ] **Vampiresa: puerta del cementerio** para coger su misión (pendiente desde el 11-11).
- [ ] **Vaivén entre objetivos de misión** (histéresis, visto en el Daemon el 11-11).
- [ ] Verificar en vivo: vacas (303) y Susurros (302) ya salen en 4 copias; abandono de misión fallada mientras se sigue.

## 4. Mapa y movimiento

- [ ] **Instalado y sin verificar a ojo**: tipos de casilla por dirección (agua, lava, edificio, puerta, barrera) y colores del visor. Falta que el dueño lo mire en el visor.
- [ ] **Mar abierto = suelo de clase 3 no pisable**: es una deducción (23 628 de 27 446 «obstáculos» en la costa); confirmarlo con Manolito (`drawWaterTiles 0x62DD00`).
- [ ] **Marca «dentro de casa»** (`+0x1E` bit 2): sale en miles de casillas por pueblo; no sirve sola. Buscar la marca real de interior o de techo.
- [ ] **Entrar a una casa por su puerta**: 8 veces visto «lies indoors»; falta ver una entrada completa hasta el objetivo.
- [ ] **Rutas precalculadas** (red de navegación) en lugar de A* grande en vivo.
- [ ] **AgentCursor fase 2**: clics reales por la entrada del juego y tramos de ClickWalk.
- [ ] Ideas aparcadas: vigilante por progreso neto, distancia por BFS, mapa de calor de atascos, 2-opt de rutas ([[Skiller]]).

## 5. Guías de clase ([[Builds]], [[Mentalidades]])

Hecho: tablas de habilidades y atributos, arte de pánico, tope de enemigos, Mago a distancia, tope de Weapon Lore, caballo desde nivel 4, buffs al abrir, pociones extra.

- [ ] Plantillas de combo por clase y segundo combo (`comboArts`).
- [ ] Armadura puntuada por resistencia dominante (§1.5).
- [ ] Leer runas ajenas de artes compartidas.
- [ ] Artes de escape y formas (Transformación 0x24, formas del Daemon).
- [ ] Vampiresa de día y de noche (leer el reloj del juego).
- [ ] Serafín: abrir con hechizo a 5-6 casillas y luego cuerpo a cuerpo.
- [ ] Caballos gratis (Light Nag en Porto Vallum y otros) antes de pagar.
- [ ] Enano: comprar un arma a distancia.
- [ ] Verificar en vivo: arma de fuego en la mano correcta, runas de la Vampiresa, canje 4:1 (visto 3 veces), compra de antídoto, Mentor y concentración (vista 1 vez).
- [ ] Etiqueta del registro «sin saber aún» en artes que sí están en la tabla (solo cosmético).

## 6. Material de Manolito por usar

- [ ] `0x550E50` cálculo del daño: medir el daño real de cada arte y elegir la mejor.
- [ ] `0x5529B0` tope de nivel de arte por tipo de objeto: valorar amuletos y anillos de +artes.
- [ ] `0x4A5420` búsqueda de objetivo del juego (hoy el agente usa la suya).
- [ ] Coste por habilidad en Tactics y población de parámetros (ideas de Universo Programado).
- [ ] Al cerrar cada pasada: ejecutar `Descompilacion/enlaces.py` para que no queden ficheros sin enlazar.

## 7. Bóveda y herramientas

- [ ] 61 «enlaces» falsos: direcciones entre dobles corchetes (`[[0xAD5C40]`) que Obsidian pinta como notas inexistentes.
- [ ] 35 archivos sin confirmar en git en `docs/` (incluida toda `Descompilacion/`): decidir qué se sube y qué se ignora.
- [ ] `cc-safety-net` (aviso de Skiller): hook que bloquea órdenes peligrosas; lo decide el dueño.
- [ ] El investigador web y el verificador de hallazgos: sesiones en reposo; `verificar-hallazgos` falló y hay que relanzarla o archivarla.

## 8. Aparcado a propósito (no olvidar)

- **Mods futuros** (control del mercenario, 8 agentes en un mundo con baúl compartido): en [[Pendientes]]; el dueño tiene más ideas para cuando el agente esté perfecto.
- **Pelear montado solo algunas clases** (lo dice [[Mentalidades]]): el dueño decidió que todas pelean montadas; no tocar.
- **Misiones de zonas de nivel 25** (oasis de Ahil-Tar): hasta que las copias tengan nivel.
- **Cuota**: trabajo complejo hasta el 95 % de la semanal; Manolito hasta el 78 %. Renovación: domingo 15-11 a las 21:00.

## Cómo no parar el flujo

1. Al empezar: leer esta lista y `get_usage`; coger lo primero sin hacer del apartado más alto.
2. Repartir: código e instalaciones el coordinador (Fable); medir, el analista (Sonnet); atascos y registros, el solucionador (Sonnet); pasos de misiones, el investigador web (Sonnet); direcciones del exe, Manolito (Opus, con Codex).
3. Cada cambio: commit → CI → instalar → comprobar la línea del registro que lo prueba → medir → tachar aquí.
4. Juntar arreglos en tandas para no reiniciar las copias más de lo necesario.
