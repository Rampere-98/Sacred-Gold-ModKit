# Estudio de mejoras del agente (2026-11-13)

Datos: registros de las 8 copias (≈ 2 h por copia, 08:20-10:20) y `Criterios-medir.py` sobre todo el historial de combate (17 257 peleas, 86,6 horas-copia). Ver también [[Criterios]], [[Builds]], [[Mentalidades]], [[Analisis]].

## 1. Foto actual

| Copia | Clase | Nivel | Exp./hora | Peleas (min peleando) | «Mirando alrededor» | Diagnóstico |
|---|---|---|---|---|---|---|
| A7 | Enano | 7 → 11 | **41 030** | 84 (41 min) | 35 | Farmea: el mejor con diferencia |
| A6 | Vampiresa | 15 | 12 739 | 164 (46 min) | 20 | Farmea; 27 retiradas por vida baja |
| A2 | Gladiador | 12 → 13 | 7 602 | 41 (14 min) | 327 | Mitad misión, mitad pelea |
| A4 | Elfa | — | 5 309 | 57 (14 min) | 233 | Igual |
| A1 | Serafín | 3 (partida nueva) | 1 634 | 37 (7 min) | 338 | Recién rotada |
| A3 | Mago | — | 1 396 | 31 (5 min) | 539 | **Atascada** en una meta de misión |
| A5 | Elfo oscuro | 13 | **40** | 13 (1 min) | **590** | **Atascado** 2 h en la meta 3468,2753 |
| A8 | Daemon | 13 | **23** | 11 (0 min) | 308 | **Atascado**: región 2 (niv. 4-7) y vuelve a la 1 |

**Lo que dice la tabla:** las copias que farmean suben de nivel; las que siguen misiones pierden horas «mirando alrededor» de una meta que no se cumple. 3 de 8 copias llevan 2 h sin experiencia.

Casi muertes (vida < 20 %): **6,6 por hora**. El 81 % ocurre en peleas contra enemigos de nivel ≥ héroe + 2, o contra 5 o más de nivel ≥ héroe + 1 (criterios C1+C2). El 51 % llega tras beber 3 o más pociones en esa pelea.

## 2. Problemas por orden de coste (horas perdidas)

### P1. Meta de misión que nunca se cumple → bucle «mirando alrededor» (A3, A5, A8)
- A5 va 2 h a la meta 3468,2753 (misión de la oveja, 15005, y la 152): cada 4 s hace clic a 7-8 casillas y vuelve; «going to look» 590 veces; 1 min peleando en 2 h.
- El vigilante de atascos no salta: el héroe **se mueve** (7-8 casillas cada clic), así que no cuenta como atasco.
- **Mejora:** tiempo máximo **por meta** («combustible por objetivo», ya en [[Pendientes]]): 6 min en el radio de la meta sin que el diario cambie → meta aparcada 30 min y la siguiente. Si es la única → farmear. Medir con «Intent: quest (at the quest's goal…)» seguido.
- **Mejora:** usar `0x54FBC0` shouldShowQuestNpcIcon (Manolito) para saber **qué PNJ da o cierra la misión**. Así, «hablar con X» deja de ser «buscar en un radio».

### P2. Ir a regiones por debajo del nivel (A8)
- Daemon nivel 13 en la región 2 (niveles 4-7): «nada que hacer aquí» y se va a la región 1 a 272 casillas, para un «report».
- **Mejora:** con el héroe 5 o más niveles por encima de una región, las misiones de esa región valen poco (ya ×2,5 a la distancia). Añadir: sin misión principal pendiente allí, **farmear en la región de su nivel** antes que volver atrás. Seguir la principal, que lleva a las regiones de su nivel.

### P3. Caballo: montar, desmontar y perseguirlo
- «Off the horse (on foot a while)» 3 369 veces en 6 copias; «walking to horse» 184 veces; 401 llamadas con la herradura; 164 «AgentStuck en caballo».
- A3, en el último minuto: 6 «walking to horse 8-9 tiles» sin alcanzarlo.
- **Mejora:** tras 2 intentos fallidos de llegar al caballo, llamarlo con la herradura y esperar 5 s, sin perseguirlo. Desmontar solo si el «a pie un rato» dura más de 20 s. Tope de 1 montar/desmontar por minuto fuera de pelea.

### P4. Peleas perdidas por subestimar al enemigo
- C1+C2 explican el 81 % de las casi muertes, con precisión del 17 % (5 de cada 6 peleas así salen bien). Evitarlas todas cuesta el 38 % de la experiencia. No hay que evitarlas: **hay que entrar mejor**.
- **Mejora** (de las guías, ya instalada hoy): abrir con los buffs, arte de pánico y tope de enemigos por clase. **Medir** la tasa C1+C2 antes y después con `Criterios-medir.py`.
- **Mejora:** con 3 pociones bebidas en una pelea y win < 0,6 → retirarse (C4: 38 % de precisión, la mejor señal).

### P5. El inventario se abre solo (A8: 17 veces seguidas en 30 s, A4: 12)
- Algo abre el inventario cada 1,5 s y el agente lo cierra: un bucle entre dos rutinas. Coincide con «nada que hacer en la región».
- **Mejora:** localizar quién lo abre (Gear al revisar el equipo, o la tecla I del mando) y no abrirlo sin motivo.

### P6. Pociones de curación agotadas
- 395 «stepping back… no healing potion» en 5 copias. «No healing potion on sale» en 3 copias.
- **Mejora:** volver a una tienda al quedar ≤ 2 (regla de [[Builds]] §1.1). Ya compra hasta el stock, pero no sale a buscar tienda por pocas pociones.

## 3. Lo que ya funciona (no tocar)
- Las puertas de interior: 8 «lies indoors: in by the door» en 2 copias, desde hoy.
- La lectura del terreno a nivel de dirección (agua, edificios, puertas, barreras).
- Farmeo por nivel de zona (A7, A6).
- Canje de runas: 3 canjes 3:1 hechos. **El 4:1 aún no se ha visto en vivo.**

## 4. Brechas de las guías aún abiertas
1. Plantillas de combo por clase (`comboArts`), con un 2.º combo.
2. Armadura puntuada por resistencia dominante ([[Builds]] §1.5).
3. Leer runas ajenas de artes compartidas.
4. Artes de escape y formas (Transformación 0x24, formas del Daemon).
5. Vampiresa de día y de noche (falta leer el reloj del juego).

## 5. Material nuevo de Manolito por usar
- `0x54FBC0` shouldShowQuestNpcIcon → P1 (quién da la misión).
- `0x550E50` calculateAndApplyDamage → medir el daño real de cada arte y elegir la mejor (hoy se aprende por ensayo).
- `0x5529B0`, el tope de nivel de arte por tipo de objeto → valorar amuletos y anillos de +artes.

## 6. Orden propuesto

| # | Mejora | Ganancia esperada | Coste |
|---|---|---|---|
| 1 | P1: tiempo máximo por meta + aparcarla | 3 copias pasan de ~0 a 5-10 mil exp./h | bajo |
| 2 | P2: no volver a regiones bajas; farmear a su nivel | A8 y similares | bajo |
| 3 | P3: caballo sin persecución ni vaivén | minutos por hora en todas | bajo |
| 4 | P5: bucle del inventario | A8, A4 | bajo (tras localizarlo) |
| 5 | P6: volver a la tienda con ≤ 2 pociones | menos retiradas y muertes | medio |
| 6 | P1b: PNJ de misión por `0x54FBC0` | misiones «hablar con» | medio (verificar firma) |
| 7 | P4: retirada con 3 pociones y win < 0,6 + medir buffs | menos casi muertes | bajo |
| 8 | Brechas 1-5 de las guías | build de experto | medio-alto |

**Cómo medir cada cambio:** exp./hora por copia (§1 de este estudio, guion `Estudio-medir.py`), casi muertes por hora y C1+C2 (`Criterios-medir.py`), y el recuento de «going to look» y «walking to horse» por hora.
