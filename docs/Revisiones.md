# Revisiones de código (revisor-codigo)

Revisión previa a cada instalación de la rama `modkit`: llamadas al exe, punteros, antitrampas, bucles, estado que no se reinicia y, desde el 2026-11-11, **anormalidades** (que el agente haga solo lo que haría un jugador normal; ver [[Mecanicas]], [[QUESTS]], [[Builds]]). Lo más reciente arriba.

Gravedad: 🔴 grave o anormalidad · 🟡 sugerencia · 💭 nota. Estado = lo que sé por los avisos de la coordinadora, **sin reverificar en el código** salvo donde se dice.

Ver también: [[Inicio]] · [[Equipo]] · [[AGENT_ADDRESSES]] · [[FINDINGS_REVIEW]]

## 2026-11-13

### 70d8484..afec5fc: lecturas SEH de ClickWalk, escaneo del mapa, runas 4:1 (solo caídas y memoria)
| Gravedad | Dónde | Hallazgo | Arreglo mínimo | Estado |
|---|---|---|---|---|
| 🔴 corrompe la pila | `map_memory.cpp:244` y `:276` | `long v[8]` y `numbers(line, v, 9, 5)`: `numbers` escribe `out[n]` antes de comprobar `end == p`, así que con 9 campos (lo que guarda ahora `SacredBild-map2.txt`) escribe `v[8]`, fuera del array. Cookie de pila o pila rota al cargar el mapa. | `long v[9];` | abierto |
| 🔴 corrompe el montón | `gear.cpp:1497-1500` | `0x6D4160` crece el vector con el asignador del juego (`0x4B7C70`): bloques de ≤0x80 bytes del pool de nodos (`0x8321A0`), mayores con `operator new`. Se libera siempre con `0x849410` (operator delete): un bloque del pool (lista de ≤32 runas) va a `HeapFree` con un puntero interior. | No liberar (fuga ≤512 B por canje), o `bytes<=0x80 ? 0x832350(p,bytes) : 0x849410(p)` con `bytes = vec[2]-vec[0]` | abierto |
| 💭 | `gear.cpp:1461` | La rutina solo escribe `vec[0..2]` (descompilación), así que ensanchar el array no explica el fallo de cookie de A6 12:24; la causa sigue sin aclarar. | — | por aclarar |
| ✔ | `click_walk.cpp` `liveWalkable`, `readKind`, `readByte`, `readFlags`, `tileKind`, `tileGround` | Todo acceso a sectores vivos y al árbol `[0xAB44C8]` va dentro de `__try`; índices acotados (sector validado contra su origen, árbol ≤64 niveles, índice del registro contra `end-base`). Solo lecturas. | — | correcto |
| ✔ | `map_memory.cpp` `scanStep`, `doorInto` | Índices de `g_kindSeen`/`g_obstacleGround` acotados; BFS dentro de la caja de 49x49 (sale con `false` al tocar el borde), sin invalidación de `todo`. | — | correcto |
| ✔ | `gear.cpp` convenciones | `0x6D4160` cdecl (vec, tipo), `0x43AF10` ret 4, `0x581D40` ret 4, `0x6D4250` ret 8: coinciden con el bloque AL. | — | correcto |

## 2026-11-12

### bd469fc..835e243: guarda del sprintf 4502AE, previsión, retirada acorralada, armadura de caballo, criterios C1-C8
Sin llamadas al exe con convención equivocada ni problemas de hilos; sí condiciones que pueden dejar al agente sin objetivo y un arreglo que no hace nada.
| Hallazgo | Gravedad | Estado |
|---|---|---|
| C2/C1 (`auto_attack.cpp` ~2685): `continue` para enemigos 2+ niveles por encima salvo `atHero && d <= melee`: un arquero o mago fuerte que dispara de lejos nunca es objetivo; el héroe sigue su camino bajo fuego | 🟡 | abierto. Arreglo: no vetar al que ya golpea (`engagedFoe`/`atHero`) sea cual sea la distancia |
| C5: las casillas «sin acercarse» pasan a celdas de 6 casillas con descansos de 2 min, 1 h, 6 h y un día (y compartidas entre copias): un objetivo de misión en una celda marcada tres veces queda vetado 6-24 h | 🟡 | abierto. Arreglo: tope de 1 h para objetivos de misión y reinicio al avanzar la misión |
| C7 runas: `CanUse(…, 1) == 6` nunca se cumple (`canUseItem` devuelve bool; 6 y 8 son el sonido de queja de la UI): el arreglo no hace nada | 🟡 | abierto. Arreglo: `CanUse(…, 1) == 0` (clase no válida) |
| `equipHorseArmour` (`gear.cpp` ~2040): ignora el resultado de `Equip`; si falla, la armadura sale de la mochila y no queda puesta (objeto perdido) | 🟡 | abierto. Arreglo: tras equipar, `GetEquipped(horse,0) == best`; si no, devolverla a la mochila |
| Armadura de caballo: llamadas `0x426220` (ret 4), `0x556A30` (ret 8, devuelve el id), `0x5796A0` (ret 8) y el orden canUse → quitar → poner → equipar → recalcular coinciden con el bloque del doc; ejecuta en el hilo del juego con `ModelLock` | — | correcto |
| `drained` (≥2 pociones y ≥2 enemigos) huye «sea cual sea la vida»; `!quickOne` es redundante con `foes >= 2` | 💭 | decisión de C4 |
| Retirada acorralada: baseline reiniciado por `g_retreatStartAt`; sin bucle | — | correcto |
| `kMaxRate = 0.25`, `expected = loss * exp(bias)`, `fightPotions`, `drinkWantedAt` (atómico): coherentes | — | correcto |
| Guarda del sprintf: `redirectCall` solo si el byte es `E8` y el destino es `0x849AE1`; cdecl de 3 argumentos como el original; nombres de más de 64 caracteres se darían por colgados (poco probable) | 💭 | correcto |
| C8 (no buscar con <60 % de vida): sin pociones y sin regeneración rápida el farmeo se detiene; las peleas que llegan se pelean | 💭 | aceptado |

### Auditoría del caballo (HEAD b4c6fe9): `ride`, `sendRide`, `callHorse`, `mendHalfOff`, `footFor`, recoger montado, AgentStuck, AgentSave, `engine->controlled`
Contrastado con AGENT_ADDRESSES.md (bloques U2, U3, Y, AD, AG) y leído el código; **nada probado en vivo**. Ordenado por gravedad. Línea = HEAD b4c6fe9.

Idea central: montado, `engine->controlled` es el caballo (AD/AG), y el montado real es `hero+0x1EC != 0`. El código de `ride()` ya lo respeta, pero **el resto del ModKit sigue comparando con el héroe o infiere «montado» por la posición**, y el evento 0x20A no devuelve el control (AG), así que cada bajada pasa por un estado «medio desmontado».

| # | Gravedad | Fallo | Dónde | Arreglo propuesto |
|---|---|---|---|---|
| 1 | 🔴 | **ClickWalk deja de funcionar montado.** `hookSendOrder` solo trata órdenes cuyo `creature == hero` (554) y `askedWalk`/`continueWalk` abortan con `controlled != hero->id` («no hero to walk», 818 y 848); `walkThere` manda la orden a `controlled` (el caballo, `auto_attack.cpp:801`) y `sendLeg` las manda al héroe. Resultado: una ruta larga en curso muere en cuanto el agente monta (se monta justo cuando `ClickWalk::routing()`), y montado no hay rutas propias (A*), solo el buscador del juego, que falla lejos. Encaja con A5 «el que más veces no se acerca» y con el escalado a pie de 20/90 s. | `click_walk.cpp:554, 818, 848`; `sendLeg` | Definir «caminante» = `controlled` si `hero+0x1EC != 0`, si no el héroe. En el hook, aceptar `creature == walker`; en `askedWalk`/`continueWalk`, comparar `controlled` con el caminante (no con `hero->id`) y mandar las etapas al caminante; el progreso se mide con la posición del héroe (igual a la del caballo). |
| 2 | 🔴 | **Tactics no lanza ningún arte montado, y «montado» se decide por posición.** `controlled != hero->id → return` (828) anula todo cuando va montado, así que `castMounted` es código muerto salvo en el estado medio desmontado. A la vez `mounted` se calcula como «caballo a ≤3 unidades del héroe» (840): recién desmontado el caballo se queda en el sitio y el héroe a pie pasa por montado (artes de cuerpo a cuerpo vetadas sin motivo hasta que se mueve). El comentario 833 («controlled sigue siendo el héroe») contradice AD/AG. | `tactics.cpp:828, 840` (y 1587 `artsBarredMounted`) | Montado = `hero+0x1EC != 0` en todos los sitios; decidir en Tactics si lanza o no con `castMounted`, sin depender de `controlled`. Si montado `sendArt` debe ir al héroe (jinete): `sendArt` usa `controlled` (533), revisarlo. |
| 3 | 🔴 | **Recoger montado atrapa puertas, cofres y cualquier objeto que no sea criatura** (mi aviso de 0c98c49, aún sin corregir): `hero != self && !asCreature()` envía el 0x113 y devuelve `true` sin abrir nada; «medio desmontado» (controlled = caballo, héroe a pie) cuenta como montado, así que el héroe a pie tampoco puede abrir. | `auto_attack.cpp:949-961` | Solo 0x113 si el objetivo es botín del suelo; para puertas/cofres/PNJ-objeto: `footFor(…)` y usar la orden de uso del héroe cuando ya esté a pie. Decidir «montado» con `hero+0x1EC`, no con `hero != self`. |
| 4 | 🔴 | **Cosas fijas (pozos, escondites, oro) se usan con la orden mandada al caballo.** La rama `isFixed` usa `self = creature(controlled)`; montado es el caballo, que no usa nada. | `auto_attack.cpp:910-935` (`walkFirst(self…)`, `clickOn(self…)`) | Mandar la orden de uso al héroe (como la rama de criaturas), o bajar antes. |
| 5 | 🔴 anormalidad | **Montar con 0x209 sin comprobar la habilidad de montar** (caballo salvaje y propio): el evento interno no pasa por la comprobación de la UI y la tabla de Reiten del doc (`[0x95B0D4]`, niveles 0…50) no se mira. Además el montar/desmontar por evento interno (hay alternativa: andar hasta el caballo y la orden del jugador, que el doc marca como no localizada, P). | `auto_attack.cpp:1545-1549, 1693-1697`; `gear.cpp` `buyHorse` | Comprobar Reiten ≥ requisito de la clase del caballo antes de montar o comprar; mantener el 3×fallo→descanso como red de seguridad. |
| 6 | 🟡 | **Cada bajada deja al héroe «medio desmontado» por construcción** (AG: 0x20A no toca `controlled`). Entre el evento y la reparación de la siguiente vuelta (`mendHalfOff` en otra llamada de `Inject`) pasan frames con `controlled ≠ hero` y `+0x1EC = 0`: `attack()` lo toma por montado, Tactics se calla, ClickWalk aborta, AgentSave espera. | `sendRide` (1341-1358), `mendHalfOff` (1412-1434), `ride` (1451-1465) | Hacer la reparación dentro de la misma llamada de `sendRide` tras un 0x20A (poner `controlled = hero->id` al terminar). Así no existe el estado intermedio. |
| 7 | 🟡 | **Caballo salvaje montado «sin vincular»: no se puede bajar.** `horse` = `hero+0x570` (0) → `ride` sale en `!horse` (1555) y `mendHalfOff` no manda 0x20A; el héroe cabalga sin fin. AG dice que el id del 0x20A sale de `hero+0x570` **o del `+0x1EC` antes de bajar**. | `auto_attack.cpp:1450, 1555`; `mendHalfOff` 1422 | `horse = +0x570 ? : +0x1EC`; usarlo en el 0x20A y en `mendHalfOff`. |
| 8 | 🟡 | **Caballo muerto o no apto:** no se mira su vida (`steed->health`) antes de montar/llamar; el 0x116 lo ignora el juego, pero se gastan 3 intentos y 10 min de descanso. | `ride` 1568-1700; `callHorse` | `steed->health > 0`; si está muerto, no intentar y dejar que `Gear` compre otro. |
| 9 | 🟡 | **`mendHalfOff` devuelve sin hacer nada durante 5 s** (`return` incluso en el hueco entre intentos): si la reparación no prospera (caballo no cargado, `controlled` legítimo distinto), `ride()` queda anulado y AgentSave espera sin tiempo máximo ni log. | `auto_attack.cpp:1451-1465`; `agent_save.cpp:556-559` | Sin `return` en el hueco; contador de intentos y LOG; en AgentSave, log una vez cada 30 s mientras espere por «medio desmontado». |
| 10 | 🟡 | **Rebote desmontar/montar en peleas:** `hardOnFoot` baja con ≥3 enemigos a <8 casillas y, en cuanto bajan a <3, vuelve a montar (`!mounted && longWalk && nearest > 5`, solo frenado por `s_at` 8 s). | `ride` 1617-1642 | Histéresis: no volver a montar hasta 20 s sin enemigos a <8 casillas. |
| 11 | 💭 | Comentarios viejos: `+0x200 & 0x10` como montado (1324, y 1565-1566) es erróneo (AD: `&8`, héroe `&0x14`). `sendRide` sale sin log si el caballo no está cargado, pero `ride` ya registró «getting off/mounting». | `auto_attack.cpp:1324, 1565, 1693` | Actualizar comentarios; LOG en `sendRide` cuando no envía. |

**Lo que está bien:** `ride` usa el héroe (no `controlled`) para `+0x570`/posición/capa; `mounted` = `hero+0x1EC`; el 0x116 respeta las condiciones del bloque Y (firma comprobada, 30 s entre llamadas, capa 0); `mendHalfOff` copia el reseteo de `0x6173E7` y manda el 0x20A con el id correcto (`hero+0x570`); AgentSave espera con `footFor(20000)` y registra; `dodgeArrow` ya compara contra ambos ids; `engaged(…, controlled, hero->id)` trata los dos.

## 2026-11-11

### 90c8661: compra urgente de pociones, retirada alargada, veto por pronóstico
| Hallazgo | Gravedad | Estado |
|---|---|---|
| Retirada alargada acotada (3 prórrogas de 12 s); sin bucle | — | correcto |
| `s_extended` no se reinicia en la rama «got away», muerte ni cambio de partida | 🟡 | abierto |
| `questLost` usa el pronóstico global, no el del objetivo | 💭 | aceptado |
| Compra urgente: suelo de 300 de oro solo con menos de la mitad del stock; paga con `addGold` | — | correcto |
| `g_heroGold` puede leerse ya tras la muerte (log) | 💭 | abierto |

### 65b292c: no atacar objetivo de misión «demasiado fuerte» o sin pociones; `fc.loss` acotado
| Hallazgo | Gravedad | Estado |
|---|---|---|
| Bloqueo total por 0 pociones puede atascar misiones con objetivos triviales | 🟡 | corregido en 90c8661 (nivel+3) |
| `fc.loss ≤ 10`, `fc.seconds ≤ 600`: seguro | — | correcto |

### HEAD~1 (`noteArrowHits`, castigo por flechas)
| Hallazgo | Gravedad | Estado |
|---|---|---|
| El impacto se detecta por *cualquier* bajada de vida (cuerpo a cuerpo, veneno…) y se siguen flechas propias y aliadas: falsos impactos que solo suben los parámetros | 🔴 | abierto. Arreglo: contar solo si `+0x25C == heroId` tras clavarse, o el filtro de hostilidad de `dodgeArrow` |
| Tres pasadas por la tabla de objetos en cada llamada (hasta 65 k) | 🟡 | abierto. Una sola pasada y límite de ~100 ms |
| `g_dodge` solo sube, no se reinicia al cambiar de partida ni de héroe, y a 500 ms de espera deja al héroe esquivando en vez de atacar | 🟡 | abierto |
| Índices de `s_coming` con la tabla cambiante | — | correcto (comprobados contra `entries` y vtable) |

### HEAD~2 (rutas por carretera, `WorldTiles::ground`/`road`)
| Hallazgo | Gravedad | Estado |
|---|---|---|
| ~3,6 M de bloqueos del mutex por ruta; carga fría de hasta ~300 sectores | 🟡 | corregido (`WorldTiles::fill`, un bloqueo) |
| Heurística 12/7 sobreestima 2× sobre carretera (coste 6) | 🟡 | corregido (8/4) |
| `walkable()` mantiene el mismo resultado (-1 y 0 fuera del mundo) | — | correcto |

## 2026-11-10

### 0200c82..626d58d: caballos salvajes, mejora de caballo en establo
| Hallazgo | Gravedad | Estado |
|---|---|---|
| Un caballo salvaje tomado con el evento 0x209 puede no enlazar `hero+0x570`: «not taken» aunque esté montado; el resto de `ride()` no sabría desmontar | 🟡 | corregido (tomado también si va montado en él) |
| Caballos de establo con `+0x570 == 0` se tomarían como salvajes | 🟡 | por confirmar en vivo |
| `buyHorse` se salta el botón de la UI y no comprueba el nivel de Reiten; evento 0x209 sobre caballo ajeno | 🔴 anormalidad | abierto (no hay vía alternativa trazada, Manolito U) |
| `g_horseUpgrade` y `ui+0x154` pueden no coincidir | 🟡 | abierto |

### fcbf7b7..0200c82: `Mind`, desmontar sin puerta de 8 s, tienda ≤150 casillas, firmas
| Hallazgo | Gravedad | Estado |
|---|---|---|
| Las firmas de `castMounted` (`memcmp`) llevaban bytes altos mal codificados: nunca coincidían y no se lanzaba ningún arte a caballo | 🔴 | corregido (arrays de bytes y LOG) |
| `tactics.cpp` pasó de LF a CRLF entero (diff de 3000 líneas) | 💭 | abierto |
| `Mind::now()` solo lectura, seguro en el hilo de `gear.cpp` | — | correcto |

### 0c98c49..fcbf7b7: `g_craftHoldUntil`, `g_socketBase`, arte montado
| Hallazgo | Gravedad | Estado |
|---|---|---|
| `castMounted` no replicaba la regla del juego (`(f&2 && !arco) → no`, si no `!(f&1)`) y no traducía el id con `0x552370` | 🟡 | corregido |
| Las gemas seguían vendiéndose en el camino al herrero | 🟡 | corregido |
| `g_craftHoldUntil` de 15 s caducaba con la forja en marcha; log de ventanas de UI en cada visita | 🟡 | corregido (25 s, una vez) |

### 0c98c49: recoger montado con el evento 0x113 (tecla A)
| Hallazgo | Gravedad | Estado |
|---|---|---|
| El desvío «montado y no criatura» también atrapaba puertas y cofres: a caballo no se abrían | 🔴 | avisado; estado por confirmar |
| Reenvío cada 1 s reinicia el modo recoger (`+0xFC = 0xC`, `+0x104 = 0`) | 🟡 | abierto |
| Convención y campos del 0x113 según el doc | — | correcto |

### 8593317..b6b780f (+ fb05496): acercarse al caballo andando
| Hallazgo | Gravedad | Estado |
|---|---|---|
| Detección de montado por igualdad exacta de posición | 🟡 | corregido (holgura) |
| El acercamiento no respetaba la retirada | 🟡 | corregido |
| Tras 3 montadas sin éxito el héroe seguía reintentando 0x209 | 💭 | corregido (descanso tras 3) |

### a61fdc4..8593317: quest book por plazo y prioridad; mano 0 en `slotDamage`
| Hallazgo | Gravedad | Estado |
|---|---|---|
| Plazo (`deadlineLeft`): unidad y clave del temporizador [I]; tier 2 con plazos largos adelanta a misiones prioritarias | 🟡 | por confirmar en vivo |
| `slotDamage` clave por mano 0: con la mano 0 vacía no aprende y dos slots con la misma mano 0 chocan | 🟡 | abierto |

### 6a19b02..a61fdc4: `attack(horse)` para montar
| Hallazgo | Gravedad | Estado |
|---|---|---|
| Sin límite de distancia al caballo: tira y afloja con la misión | 🟡 | sustituido en b6b780f |
| No ataca al caballo propio (bando 4 no hostil) | — | correcto |

## Primera pasada: 15 commits hasta 7078ae9 (combos, engarce, caballo, esquiva, misiones primero)
| Hallazgo | Gravedad | Estado |
|---|---|---|
| Retirada: «got away» se disparaba bajo tierra porque `g_aggroFoes` solo existe en superficie | 🔴 | avisado; estado por confirmar |
| `ride()`: montado por `+0x200 & 0x10` [I] frente a `+0x1EC`; `ret` de `0x416B30` sin confirmar | 🔴 | montado corregido (posición); `ret` sin confirmar |
| `dodgeArrow`: recorrido de 65 k punteros sin límite, `shooter & 0xFFFF`, `s_lastArrow` por índice | 🟡 | abierto |
| `makeCombo`: rama de cambio de página sin tope de intentos | 🟡 | abierto |
| `socketAtSmith`: objeto base y gemas pueden quedar en la mesa al salir antes de tiempo | 🟡 | abierto |
| `buyHorse`: sin nivel de Reiten; `ret` de `0x5498F0` sin confirmar | 🟡 | abierto (ver anormalidad arriba) |
| Antitrampas: sin escrituras en experiencia, nivel ni atributos; el oro solo por `addGold` | — | correcto |
