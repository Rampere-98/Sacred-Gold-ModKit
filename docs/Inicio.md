# Sacred Gold ModKit: conocimiento del agente

> **Estudiar el ModKit completo:** [[ModKit/ModKit]] (mapa de notas por módulo, el agente, configuración, compilación, historia).

> **Mapa de todo Sacred Gold en el equipo** (juego, copias A1..A8, repo, skills, memorias): [[Sacred/Sacred]].

Índice de lo que sabemos del juego y de lo que hace el agente. Las sesiones escriben en estas notas; esta bóveda es la carpeta `docs/` del repo, no una copia.

- [[Equipo]]: los agentes (sesiones) que desarrollan el ModKit y su papel.
- [[Pendientes]]: mods futuros del dueño (solo planes, sin implementar).

## Cómo funciona el juego
- [[GAME_GUIDE]]: mecánicas sacadas del tutorial, los consejos y el manual, con la regla de decisión para el agente en cada tema.
- [[Guias/Guias|Guías de internet (Investigador web)]]: campaña, secundarias por región, mapas, builds, equipo, mecánicas avanzadas y Underworld, con fuentes.
- [[QUESTS]]: las misiones (principal, secundarias, GQ/NQ/Underworld) sacadas de los textos del juego: qué pide cada una, trampas, plazos, objetos que no vender y «Reglas para el agente».

## Cómo está hecho el agente
- Código: `src/game/auto_attack.cpp` (combate, botín, exploración, misiones, atascos), `src/game/gear.cpp` (equipar, vender, comprar, runas, pergaminos), `src/game/whereabouts.cpp` (dónde está el héroe), `src/game/map_memory.cpp`, `src/game/findings.cpp`.
- [[Development]]: ficheros fuente y compilación.
- [[Hallazgos]]: lo que el agente anota mientras juega (atascos, tiradores, misiones, botín que no alcanza).
- [[WEB_RESEARCH]]: lo que se sabe del juego en internet (resistencias, builds, ranuras, enemigos, campaña, objetos), con fuentes. Aplicado: fórmula de resistencias en la elección de ranura. Por aplicar: regeneración de artes (repartir PR/Concentración/+RSM), engastar runas mejor que comerlas, arma a distancia para el bonus de supervivencia, trampas de misiones (Riddle in the Desert lo último, no rechazar Camp of the Undead), ciclo de stock de los mercaderes.
- [[Analisis]]: análisis periódico de los logs del agente (tiempo productivo, combates, misiones, armas, problemas detectados). Lo más reciente arriba.
- [[FINDINGS_REVIEW]]: revisión de esos hallazgos (cuáles son reales).
- [[Ideas]]: qué le falta al agente para jugar como un experto normal (diagnóstico, ideas por prioridad, top 5); lo escribe el ideador.

## Clases del héroe
- [[Builds]]: plan de experto por clase (atributos, habilidades, combos, equipo, runas, caballo, oro) con tabla de reglas para `hero_build.cpp`
- [[Mentalidades]]: un `struct Mind` por clase (distancia, retirada, agresividad, artes, caballo, prioridades, sitio de nacimiento, miedos), con qué parámetro existente se reutiliza y cuáles son nuevos
- [[Mecanicas]]: mecánicas del juego (web y textos del propio juego) con qué debería hacer el agente en cada una.
- [[AGENT_ADDRESSES#Clases del héroe (2026-11-09, GAME05 Gladiador nv 1; solo lectura)|Clases: identificación, artes, armas, restricciones]]
- [[AGENT_ADDRESSES#Build: puntos, habilidades, barra, runas y combos (2026-11-09)|Build: puntos, habilidades, combos]]
- [[AGENT_ADDRESSES#Habilidades y artes (2026-11-09, GAME05, Seraphim nivel 59; solo lectura)|Habilidades y artes]]
- [[AGENT_ADDRESSES#Panel del personaje y último combate (2026-11-09; Gladiador nuevo nv 1, juego recién reiniciado)|Panel del personaje, estadísticas y regiones]]
- [[AGENT_ADDRESSES#Combate en vivo (2026-11-09, Gladiador nv 2 en la arena, sin enemigos a ≤ 30 casillas; solo lectura)|Combate en vivo: héroe, enemigos, estados]]
- Rotación de clases y posiciones en la pantalla de selección: memoria `rotar-personajes`.

## Enemigos
- [[AGENT_ADDRESSES#Bloque I|Bloque I: supervivencia, engaste, recarga de artes, experiencia, plazos]]
- [[AGENT_ADDRESSES#Ranuras de armas (bloque H)|Ranuras de armas: estructura, cambio, requisitos]]
- [[AGENT_ADDRESSES#Resistencias y debilidades de enemigos (2026-11-09; el juego volvió al menú: `[0xAD5C40]` = 0, sin vivo nuevo)|Resistencias, debilidades y razas]]
- [[AGENT_ADDRESSES#Enciclopedia (2026-11-09, estático; vivo NO posible: el juego estaba en el menú, `[0xAD5C40]` y `[0x182EBE8]` =|Enciclopedia]]
- [[GAME_DATA#1. Creatures / enemies|Criaturas (datos)]]

## Objetos
- [[ITEM_FUNCTIONS]]: qué hace cada tipo de objeto y qué debe hacer el agente con él.
  - Pendiente según el catálogo: la poción de los no muertos solo contra no muertos; la concentración con condición; vender más tipos (equipo de caballo, gemas, libros leídos); comprar pociones de curación; cambiar runas en el Maestro de combos (por la vía de la interfaz, nunca llamando a 0x57FD80 que toca el oro: ver Comerciante de runas §4; precio 100/500/2000 oro); engarzar en el herrero; usar la brida (vía de uso pendiente).
- [[ITEMS_RE]]: inventario, equipo, mercaderes, bonificaciones, sets y rareza.
- [[AGENT_ADDRESSES#Pergaminos (2026-11-09, partida nueva, vampiresa nv 1; solo lectura)|Pergaminos y libros]]
- [[AGENT_ADDRESSES#Comerciante de runas (2026-11-09; [ESTÁTICO] salvo lo marcado; el juego vivo no se tocó)|Comerciante de runas]]
- [[AGENT_ADDRESSES#Piezas: bonificaciones y sets (2026-11-09)|Bonificaciones y sets]]
- [[AGENT_ADDRESSES#Objeto en el cursor (2026-11-09, Gladiador escolta de Rocheford, poción en el cursor; solo lectura)|Objeto en el cursor]]

## Mundo, misiones y movimiento
- [[AGENT_ADDRESSES#Misiones, PNJ y diálogo (2026-11-09)|Misiones, PNJ y diálogo]]
- [[AGENT_ADDRESSES#Mapa: lugares, niebla, megamapa (TAB), minimapa y casillas (2026-11-09)|Mapa y casillas]]
- [[AGENT_ADDRESSES#Dónde está el héroe (Whereabouts) (2026-11-09, partida nueva, Gladiador nv 1 en la arena; solo lectura)|Dónde está el héroe]]
- [[AGENT_ADDRESSES#Interiores y arena del Gladiador (2026-11-09, GAME05 Gladiador en el coliseo; solo lectura, parcial)|Interiores, capas y la arena]]
- [[AGENT_ADDRESSES#Interactivos (2026-11-09, GAME05, cementerio al oeste del punto de carga; solo lectura)|Interactivos: cofres, pozos, escondites]]
- [[AGENT_ADDRESSES#Objetos del mundo con texto y contenedores (2026-11-09, parcial)|Objetos del mundo con texto]]
- [[AGENT_ADDRESSES#Descubrir el mundo (2026-11-09, GAME05, héroe nv 59 en sector 2653; solo lectura)|Descubrir el mundo]]
- [[AGENT_ADDRESSES#Mercenarios y acompañantes (2026-11-09)|Mercenarios y acompañantes]]
- [[AGENT_ADDRESSES#Etiquetas de ALT (radar de botín) (2026-11-09)|Etiquetas de ALT]]
- [[AGENT_ADDRESSES#Interfaz, tiempo y anti-trampas (2026-11-09, Gladiador nv 1 jugando; solo lectura)|Interfaz, hora del día, caballo y anti-trampas]]

## Datos y motor
- [[ARQUITECTURA]]: mapa completo de la arquitectura (módulos, hilos, frame, subsistemas, hooks).
  - Notas por subsistema en `Arquitectura/`: [[Formatos-de-datos]] (PAK, World, bin) · [[Guardado]] · [[Misiones-y-dialogos]] · [[Sonido]] · [[Red]] · [[UI]] · [[Capa-ModKit]] (arranque, hooks, hilos) · [[Motor-y-bucle]] · [[Render-y-Granny]] · [[Criaturas-e-IA]] · [[Objetos]] · [[Mapa-colisiones-navegacion]].
- [[GAME_DATA]]: ficheros de datos, estadísticas, regiones, dificultad.
- [[RE_NOTES]]: motor, render, red, interfaz y anti-trampas.
- [[AGENT_ADDRESSES#Firmas que ya usa el ModKit (`Sig::Entry kSigs` en src/game)|Firmas que ya usa el ModKit]]
- [[AGENT_ADDRESSES#Previsión de combate (bloque J)|Previsión de combate (bloque J)]]
- [[AGENT_ADDRESSES#Puertas y palancas (bloque K)|Puertas y palancas (bloque K)]]
- [[AGENT_ADDRESSES#Interiores y llaves (bloque L)|Interiores y llaves (bloque L)]]
- [[AGENT_ADDRESSES#Cinemáticas (bloque M)|Cinemáticas (bloque M)]]
- [[AGENT_ADDRESSES#Megamapa y teletransporte (bloque O)|Megamapa y teletransporte (bloque O)]]
- [[AGENT_ADDRESSES#Casillas pisables (bloque P)|Casillas pisables (bloque P)]]
- [[AGENT_ADDRESSES#Zonas seguras (bloque Q)|Zonas seguras (bloque Q)]]
- [[AGENT_ADDRESSES#Varias instancias (bloque R)|Varias instancias (bloque R)]]
- [[AGENT_ADDRESSES#Partidas guardadas (bloque S)|Partidas guardadas (bloque S)]]
- [[AGENT_ADDRESSES#Menú inicial (bloque S 11)|Menú inicial (bloque S 11)]]
- [[AGENT_ADDRESSES#Tutoriales (bloque T)|Tutoriales (bloque T)]]
- [[AGENT_ADDRESSES#Combos y engarce por código (bloque V)|Combos y engarce por código (bloque V)]]
- [[AGENT_ADDRESSES#Misiones (bloque W)|Misiones (bloque W)]]
- [[AGENT_ADDRESSES#Aggro de enemigos (bloque N)|Aggro de enemigos (bloque N)]]
- [[AGENT_ADDRESSES#Animales inofensivos|Animales inofensivos]]
- [[AGENT_ADDRESSES#Proyectiles (bloque U)|Proyectiles (bloque U)]]
- [[AGENT_ADDRESSES#Caballo y variable Tutorial (bloque V3)|Caballo y variable Tutorial (bloque V3)]]
- [[AGENT_ADDRESSES#Identificar misiones (bloque X)|Identificar misiones (bloque X)]]
- [[AGENT_ADDRESSES#Pendiente|Pendiente de investigar]]

## Reglas que el agente respeta
- No tocar el anti-trampas; adaptarse a él.
- Nunca vender runas: las de otra clase se cambian en el comerciante de runas, las propias se leen en cuanto se pueda.
- Leer pergaminos y libros (enciclopedia).
- Recoger todo mientras haya poco oro (equiparse y vender); filtrar por rareza solo con oro de sobra.
- Usar las debilidades de cada enemigo (pendiente de las direcciones de Manolito).
- Ranuras de armas (encargo del dueño, 2026-11-10): 2 al empezar, hasta 5 subiendo de nivel; cada una es una build (espada y escudo, lanza, bastón, dos espadas, dos dagas...). El agente debe llenarlas con armas de elementos distintos (físico, fuego, magia, veneno) y cambiar a la de más daño contra las resistencias del enemigo. Hoy solo usa la activa. Datos pedidos a Manolito (bloque H).
- Aceptar también las misiones secundarias (señal gris): verificado el 2026-11-09 con el Gladiador, de 1 a 3 misiones en curso con la respuesta 1 (Enter).

## Agente en vivo

- [[Agente/Mente|Mente del agente]]: mapa mental que el ModKit reescribe cada 3 s (activo o no).
- [[AGENT_ADDRESSES#Proyectiles 2 y clic en caballo (bloque U2)|Proyectiles 2 y clic en caballo (bloque U2)]]
- [[AGENT_ADDRESSES#Artes a caballo y pozos (bloque U4)|Artes a caballo y pozos (bloque U4)]]
- [[AGENT_ADDRESSES#Caballos salvajes (bloque H2)|Caballos salvajes (bloque H2)]]
- [[AGENT_ADDRESSES#Comparar caballos (bloque H2b)|Comparar caballos (bloque H2b)]]
- Mente de cada copia: [[Agente/Mente-a1|A1]] · [[Agente/Mente-a2|A2]] · [[Agente/Mente-a3|A3]] · [[Agente/Mente-a4|A4]] · [[Agente/Mente-a5|A5]] · [[Agente/Mente-a6|A6]] · [[Agente/Mente-a7|A7]] · [[Agente/Mente-a8|A8]]

## Hallazgos e informes de las sesiones

Todo lo que encuentra cualquier sesión (o el agente jugando) acaba aquí. Lo más reciente arriba en cada nota.

- [[Hallazgos]]: resumen de lo que anotan las 8 copias al jugar (por tipo y copia, sitios que se repiten); detalle por copia: [[Hallazgos/A1|A1]] · [[Hallazgos/A2|A2]] · [[Hallazgos/A3|A3]] · [[Hallazgos/A4|A4]] · [[Hallazgos/A5|A5]] · [[Hallazgos/A6|A6]] · [[Hallazgos/A7|A7]] · [[Hallazgos/A8|A8]]. Se regenera con `hallazgos_md.py`.
- [[Revisiones]]: el revisor de código (hallazgos por commit y gravedad antes de cada instalación).
- [[FINDINGS_REVIEW]]: el verificador (qué hallazgos son reales, avisos al coordinador).
- [[Verificacion]]: autentificación de hallazgos, valores aprendidos y direcciones (✅ confirmado · ⚠️ dudoso · ❌ falso), una línea por hallazgo.
- [[Analisis]]: el analista continuo (medianas de las 8 copias cada ~30 min).
- [[Atascos-diagnostico]] y [[Atascos]]: el solucionador de atascos (rondas, arreglos A–H, mapas `mapa-atascos.png`).
- [[Tareas-pendientes]]: **lista viva** de todo lo pendiente, lo que está a medias y lo aparcado; se lee al empezar cada sesión.
- [[Estudio-mejoras]]: estudio de mejoras del agente (2026-11-13) con datos de las 8 copias; lo mide [[Estudio-medir.py]] y [[Criterios-medir.py]].
- [[Capturas]]: las imágenes sueltas (tooltips, sincronización, mapas de atascos).
- [[Criterios]]: estudio de errores del agente (muertes, atascos, compras, caídas) con criterios propuestos y cómo medirlos.
- [[Mercenarios]]: mercenarios y acompañantes.
- [[WEB_QUESTS_ANCARIA]]: misiones de Ancaria sacadas de la web.
- [[Skiller]]: ideas de los vídeos de IA en juegos (distancia BFS, histéresis, coste por habilidad, 2-opt, evolución con 8 copias).
- [[2026-11-11]]: diario del día.

## Descompilación (Manolito, encargo del 2026-11-11)

Objetivo: todas las direcciones del exe en la enciclopedia, base de una futura versión de 64 bits. Índice maestro y fichas por subsistema: [[Descompilacion/Descompilacion]] (lo crea Manolito según avanza). Todos sus ficheros de trabajo (pseudocódigo, lotes, herramientas), enlazados por clasificación: [[Indice-descompilacion]]. Catálogo actual: [[AGENT_ADDRESSES]], [[RE_NOTES]], [[ITEMS_RE]], [[ITEM_FUNCTIONS]], [[GAME_DATA]], [[UDP_PROTOCOL]].

## Documentación del ModKit (inglés)

- [[Configuration]] · [[Controller]] · [[Display]] · [[Languages]] · [[Rendering]] · [[Multiplayer]] · [[Multijugador]]

- [[Auditoria]]: auditoria del entorno de entrenamiento (enlaces duros, copias, ModKit).
