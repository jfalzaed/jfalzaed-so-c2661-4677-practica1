# Guía de Estudio Completa — Ejecutor de Lotes (ST0257)

> Cubre **todo** para la sustentación de 45 min: qué es cada cosa, por qué existe, cómo se
> implementó en C, qué JSON viaja, cómo correrlo a mano y qué pide el enunciado (PDF
> `ST0257-C2661-4677-Practica-V-II`). Lee de arriba a abajo.

---

## 0. La idea en una frase

Simulamos cómo un **mainframe** ejecuta trabajos "por lotes" (batch): registras
**ficheros** (datos) y **programas** (ejecutables) en un almacén, y luego pides que un
programa se ejecute leyendo/escribiendo esos ficheros. Todo se coordina por un único punto
de entrada (`ctrllt`) y los componentes se hablan por **tuberías nombradas** (FIFOs) con
mensajes **JSON**.

**"Proceso por lotes" (batch):** un programa **no interactivo**. Lee su entrada estándar
(stdin), la procesa y escribe su salida estándar (stdout); no le pregunta nada al usuario
mientras corre. Por eso redirigimos stdin/stdout/stderr a ficheros.

---

## 1. Arquitectura: 4 procesos + 1 cliente

```
 cliente ──> ctrllt ──> gesfich  ──┐
                 │  ──> gesprog  ──┤──> aralmac (carpeta en disco)
                 │  ──> ejecutor ──┘
```

- **`cliente`**: quien manda peticiones. **No lo implementamos** (PDF §3.2: es de otra
  entrega). En la práctica el "cliente" somos nosotros escribiendo JSON al FIFO con
  `printf`/`echo`, o los scripts `test.sh`.
- **`ctrllt`** (Control de Lotes): **el corazón / la pasarela**. Único con el que habla el
  cliente. Recibe cada petición, mira el campo `"servicio"` y la **reenvía** al servicio
  correcto, espera la respuesta y se la devuelve al cliente. También apaga el sistema.
- **`gesfich`** (Gestor de Ficheros): CRUD de ficheros de datos en `aralmac`.
- **`gesprog`** (Gestor de Programas): guarda/gestiona los ejecutables y sus metadatos.
- **`ejecutor`**: lanza los programas como **procesos hijos reales** (`fork`+`exec`),
  controla su estado y los mata.
- **`aralmac`** ("Área de Almacenamiento"): es simplemente **un directorio en disco** (por
  defecto `/tmp/aralmac`). Ahí viven los ficheros y los programas copiados. El PDF §3.4 dice
  que aralmac "puede ser la ruta de un directorio, los parámetros de una base de datos,
  etc." — elegimos **un directorio** porque es lo más simple y portable. Por eso el flag
  `-x <aralmac>` recibe esa ruta.

**Regla de oro (§3.7):** los tres servicios internos (`gesfich`, `gesprog`, `ejecutor`)
**nunca** se hablan entre sí. Solo con `ctrllt`. Esto da una sola pasarela y un solo
protocolo.

---

## 2. Comunicación: tuberías nombradas (FIFOs) + JSON

### ¿Qué es una tubería nombrada (FIFO)?
Un **named pipe** es un archivo especial creado con `mkfifo`. Lo que un proceso **escribe**,
otro lo **lee**, en orden (FIFO = First In First Out). Comunica procesos que no son
padre/hijo. Se ve como un archivo (`/tmp/gesfich_req`) pero no guarda datos en disco: es un
canal.

- **half-duplex** (lo que usamos): un FIFO va en **un solo sentido**. Por eso usamos **dos**
  por servicio: uno de peticiones (`_req`) y otro de respuestas (`_resp`).
- **full-duplex**: un FIFO bidireccional. Si omites el FIFO de respuesta, nuestro código usa
  el mismo para ambos sentidos.

### ¿Por qué JSON?
El PDF §3.7 lo exige: mensajes **JSON terminados en salto de línea `\n`**. Es legible y fácil
de extender. Para parsearlo usamos **cJSON** (un `.c`/`.h`, se descarga solo en `make`).

### El protocolo (§3.8) — lo que SIEMPRE preguntan
Cada mensaje es una sola línea JSON. Tres formas:

**Petición** (cliente → ctrllt → servicio):
```json
{"servicio":"<svc>","operacion":"<op>", ...campos...}
```
**Respuesta OK** (servicio → ctrllt → cliente):
```json
{"estado":"ok", ...campos...}
```
**Respuesta error:**
```json
{"estado":"error","mensaje":"descripción"}
```

- `servicio` ∈ `gesfich | gesprog | ejecutor | ctrllt`.
- `operacion` = la acción (Crear, Leer, Guardar, Ejecutar…).
- **Tamaño máximo:** `MSG_MAX_LEN = 4096` bytes por mensaje (constante en `proto.h`).

### Identificadores (§3.8.3)
| Tipo | Formato | Lo genera |
|------|---------|-----------|
| Fichero | `f-XXXX` | gesfich |
| Programa | `p-XXXX` | gesprog |
| Ejecución | `e-XXXX` | ejecutor |

`XXXX` = 4 dígitos (`f-0001`, `f-0002`…). Cada servicio lleva un **contador persistente** en
un archivo oculto dentro de aralmac, para no repetir IDs entre arranques.

---

## 3. El código común: `src/common/`

Es el código **compartido por los 4 servicios** para no repetirlo. Dos funciones
(`proto.c` / `proto.h`):

- **`msg_send(fd, json)`**: escribe la cadena JSON al FIFO y le añade `'\n'`. Maneja
  escrituras parciales (un `write` puede no escribir todo) reintentando, y reintenta si lo
  interrumpe una señal (`EINTR`).
- **`msg_recv(fd, buf, sz)`**: lee **byte a byte** hasta encontrar `'\n'` (fin de mensaje) y
  lo cambia por `'\0'`. Devuelve longitud, `0` si EOF (el otro extremo se cerró) o `-1` si
  error. Lee de a un byte para **no robarse** parte del siguiente mensaje (un `read` grande
  podría leer dos mensajes pegados).

`vendor/cJSON.c` y `cJSON.h` son la **librería de terceros** para crear/parsear JSON. Están
en `vendor/` (convención: código que no escribimos nosotros).

---

## 4. `gesfich` — Gestor de Ficheros (`src/gesfich/gesfich.c`)

**Para qué:** crear/leer/actualizar/borrar **ficheros de datos** que serán la entrada o
salida de los programas. Los guarda en `aralmac/ficheros/`.

### Operaciones (JSON que recibe → qué hace)
| Operación | Petición | Qué hace | Respuesta OK |
|-----------|----------|----------|--------------|
| **Crear** | `{"servicio":"gesfich","operacion":"Crear"}` | Crea archivo **vacío** `f-XXXX` y devuelve su id | `{"estado":"ok","id-fichero":"f-0001"}` |
| **Leer (uno)** | `{...,"operacion":"Leer","id-fichero":"f-0001"}` | Devuelve el contenido | `{"estado":"ok","contenido":"..."}` |
| **Leer (todos)** | `{...,"operacion":"Leer"}` (sin id) | Lista todos los ids | `{"estado":"ok","ficheros":["f-0001","f-0002"]}` |
| **Actualizar** | `{...,"operacion":"Actualizar","id-fichero":"f-0001","ruta":"/etc/hosts"}` | **Copia** el contenido de `ruta` dentro de f-0001 | `{"estado":"ok"}` |
| **Borrar** | `{...,"operacion":"Borrar","id-fichero":"f-0001"}` | Borra el fichero (`unlink`) | `{"estado":"ok"}` |
| **Suspender/Reasumir/Terminar** | `{...,"operacion":"Suspender"}` etc. | Control del servicio | `{"estado":"ok"}` |

**Implementación (claves):**
- `op_crear`: incrementa el contador, arma `f-%04d`, `open(...O_CREAT...)` crea el archivo
  vacío, lo cierra y guarda el contador.
- `op_leer`: si viene `id-fichero` lee ese archivo (`stat`+`fread`); si no, abre el
  directorio (`opendir`/`readdir`) y lista lo que empieza por `f-`.
- `op_actualizar`: verifica que existan **el fichero destino y la ruta origen** (`stat`) y
  copia byte a byte. Así un fichero se llena con datos reales.
- **Límite de tamaño:** al leer, si el contenido no cabe en `MSG_MAX_LEN/2` devuelve error
  (JSON puede duplicar el tamaño al "escapar" caracteres, y el mensaje no puede pasar de
  4096 bytes).

### Máquina de estados (Figura 3 del PDF)
`Corriendo → (Suspender) → Suspendido → (Reasumir) → Corriendo`; ambos `→ (Terminar) →
Terminado`. **En Suspendido se bloquean TODAS las operaciones de datos** (Crear/Leer/
Actualizar/Borrar dan `"servicio suspendido"`). En `run()`: primero maneja Suspender/
Reasumir/Terminar; si el estado es Suspendido y la op no es de control → error.

---

## 5. `gesprog` — Gestor de Programas (`src/gesprog/gesprog.c`)

**Para qué:** registrar **programas ejecutables** (un binario como `/bin/cat`, o un script)
con sus **argumentos** y **variables de entorno**, para luego poder ejecutarlos.

**Cómo guarda cada programa** en `aralmac/programas/p-XXXX/`:
- `bin` → una **copia** del ejecutable (preservando permisos de ejecución).
- `meta.json` → metadatos: `{id-programa, nombre, ejecutable, args, env}`.

### Operaciones
| Operación | Petición | Respuesta OK |
|-----------|----------|--------------|
| **Guardar** | `{...,"operacion":"Guardar","ejecutable":"/bin/cat","args":["-n"],"env":["K=V"]}` | `{"estado":"ok","id-programa":"p-0001"}` |
| **Leer (uno)** | `{...,"operacion":"Leer","id-programa":"p-0001"}` | `{"estado":"ok","programa":{id-programa,nombre,args,env}}` |
| **Leer (todos)** | `{...,"operacion":"Leer"}` | `{"estado":"ok","programas":["p-0001","p-0002"]}` |
| **Actualizar** | `{...,"operacion":"Actualizar","id-programa":"p-0001","ruta":"/nueva/ruta"}` | `{"estado":"ok"}` |
| **Borrar** | `{...,"operacion":"Borrar","id-programa":"p-0001"}` | `{"estado":"ok"}` |
| Suspender/Reasumir/Terminar | control | `{"estado":"ok"}` |

**Implementación (claves):**
- `op_guardar`: valida que `ejecutable` exista (`stat`). Si **no tiene bits de ejecución**,
  solo lo acepta si es un **script con shebang** (`#!` en los 2 primeros bytes); si no, da
  `"no se pudo guardar el programa"`. Esto cumple "ejecutable válido (binario o guión)" del
  PDF. Crea la carpeta `p-XXXX`, copia el binario y escribe `meta.json`.
- `op_leer` (uno): lee `meta.json` y **borra el campo `ejecutable`** antes de responder,
  porque §3.10.3 dice que el objeto `programa` solo expone `id-programa, nombre, args, env`.
- `args`/`env` son **opcionales**: si no vienen, se guardan como arrays vacíos.

### Máquina de estados (Figura 4) — **el detalle trampa**
Igual que gesfich, **pero** el estado **Suspendido tiene un self-loop `Leer`**: o sea,
**`Leer` SÍ se permite estando Suspendido** (las demás operaciones de datos no). En `run()`,
`Leer` se maneja **antes** del bloqueo por suspensión. *(En el PDF Figura 4; gesfich NO lo
tiene.)*

---

## 6. `ejecutor` — Ejecutor de Procesos (`src/ejecutor/ejecutor.c`)

**Para qué:** tomar un programa registrado y **ejecutarlo de verdad** como proceso del SO,
redirigiendo su stdin/stdout/stderr a ficheros de `gesfich`. Es el servicio más complejo:
maneja procesos reales, señales y concurrencia.

### Operaciones
| Operación | Petición | Respuesta OK |
|-----------|----------|--------------|
| **Ejecutar** | `{...,"operacion":"Ejecutar","id-programa":"p-0001","stdin":"f-0001","stdout":"f-0002","stderr":"f-0003"}` | `{"estado":"ok","id-ejecucion":"e-0001"}` |
| **Estado (uno)** | `{...,"operacion":"Estado","id-ejecucion":"e-0001"}` | `{"estado":"ok","id-ejecucion":"e-0001","id-programa":"p-0001","proceso-estado":"Ejecutando"}` (+`codigo-salida` si Terminado) |
| **Estado (todos)** | `{...,"operacion":"Estado"}` | `{"estado":"ok","procesos":[{...},{...}]}` |
| **Matar** | `{...,"operacion":"Matar","id-ejecucion":"e-0001"}` | `{"estado":"ok"}` |
| Suspender/Reasumir/Parar | control de todos los procesos | `{"estado":"ok"}` |

`stdin/stdout/stderr` son **opcionales**: si no se dan, el hijo hereda los del servicio.
`proceso-estado` ∈ `Ejecutando | Suspendido | Terminado`. `codigo-salida` solo aparece
cuando el proceso ya terminó.

### Cómo se ejecuta un programa (el núcleo, `op_ejecutar`)
1. Lee `meta.json` del programa para sacar `args` y `env`.
2. Arma `argv[]` (argv[0] = ruta al binario copiado `p-XXXX/bin`, luego los args) y `envp[]`.
3. Abre los ficheros de redirección (`f-XXXX` en `aralmac/ficheros/`): stdin solo lectura,
   stdout/stderr en escritura (los crea/trunca).
4. Asigna un `e-XXXX`.
5. **`fork()`** crea un proceso hijo (copia del actual):
   - **En el hijo**: `dup2` redirige los ficheros a 0/1/2 (stdin/stdout/stderr), cierra los
     FIFOs del servicio (para no heredarlos) y llama a **`execve`** (con env) o **`execv`**
     (sin env), que **reemplaza** el hijo por el programa a ejecutar. Si `exec` falla, sale
     con código 127.
   - **En el padre**: cierra los ficheros (ya los tiene el hijo), guarda el proceso en la
     tabla `g_procs[]` con estado `Ejecutando` y responde con el `id-ejecucion`.

**`fork`+`exec` es EL patrón UNIX para lanzar programas:** `fork` duplica el proceso, `exec`
carga el nuevo programa encima. Entre los dos, el hijo prepara las redirecciones.

### ¿Qué es "un proceso se cuelga"? (cuelgue / hang)
Es cuando un proceso queda **bloqueado para siempre** esperando algo que no llega. Casos
aquí:
- **Abrir un FIFO sin el otro extremo bloquea.** Por eso abrimos los FIFOs con `O_RDWR`
  (lectura+escritura): así `open` no se bloquea ni da EOF si un extremo se va.
- Un programa que lee de stdin sin datos puede quedarse esperando. Por eso a `cat` le damos
  un fichero de stdin con contenido; si no, se colgaría.
- En `run.sh stop`: escribir al FIFO de ctrllt **se cuelga si ctrllt ya murió** (no hay
  lector). Lo resolvimos con un subshell que se mata a los 0.5 s (ver §9).

### Señales y recolección de hijos (lo "fino")
Cuando un hijo termina, el SO le manda **`SIGCHLD`** al padre. Si no lo "recoges" con
`waitpid`, queda **zombie**. Implementación:
- `sigchld_handler` solo levanta una bandera `g_sigchld_flag` (un handler debe ser mínimo).
- `reap_children()` usa `waitpid(-1, …, WNOHANG)` (no bloqueante) para recoger todos los
  hijos terminados y marcar `proceso-estado=Terminado` con su `codigo-salida`
  (`WEXITSTATUS`, o `128+señal` si murió por señal).
- Configuramos `SIGCHLD` **sin `SA_RESTART`** a propósito: así el `read` del FIFO se
  **interrumpe** (`EINTR`) al llegar la señal y recogemos hijos al instante.

### Máquina de estados del servicio (Figura 5) + de cada proceso
- **Servicio:** `Ejecutar ↔ Suspendidos`, y `Ejecutar/Suspendidos → Parar → (auto)
  Terminado`. **`Ejecutar/Estado/Matar` son self-loops SOLO en estado `Ejecutar`**: si el
  servicio está Suspendido o Parando dan error (`servicio suspendido` / `servicio parando`).
- **Suspender:** `SIGSTOP` a todos los hijos (los **congela**) → Suspendidos.
- **Reasumir:** `SIGCONT` (los **descongela**) → Ejecutar.
- **Matar:** `SIGKILL` a un proceso concreto.
- **Parar:** deja de aceptar nuevas ejecuciones, descongela los suspendidos (para que
  terminen), y cuando **no quedan procesos activos** pasa solo a `Terminado`
  (`wait_for_parar`, polling cada 10 ms). Es el apagado **ordenado**: no mata, espera.
- **Estado de cada proceso:** `Ejecutando → Suspendido | Terminado`.

---

## 7. `ctrllt` — Controlador / Pasarela (`src/ctrllt/ctrllt.c`)

**Para qué:** único punto de contacto del cliente. **Enruta** por el campo `servicio` y
**no modifica** las respuestas (las reenvía tal cual). Su única operación propia es
`Terminar`.

**Cómo funciona `run()`:**
1. Lee una petición del FIFO del cliente.
2. Mira `"servicio"`:
   - `"ctrllt"` + op `Terminar` → apaga el sistema y responde `{"estado":"ok"}`. Otra op →
     `{"estado":"error","mensaje":"operacion ctrllt desconocida"}`.
   - `gesfich/gesprog/ejecutor` → reenvía el JSON **literal** al FIFO del servicio, lee su
     respuesta y la manda al cliente.
   - Servicio inexistente → `"servicio desconocido"`. Existe pero no conectado →
     `"servicio no conectado"`.

**`Terminar` (do_terminar):** propaga `Terminar` a `gesfich` y `gesprog`, y manda `Parar` a
`ejecutor` (que se auto-detiene al quedarse sin procesos). Luego ctrllt termina.

**Errores propios de ctrllt:** `servicio desconocido`, `operacion ctrllt desconocida`,
`servicio no conectado`, `error enviando solicitud al servicio`, `error leyendo respuesta
del servicio`.

> **Detalle de flags (importante):** el PDF tiene una **errata**: usa `-c` dos veces (request
> de ctrllt y response de gesprog). No se puede repetir una opción, así que en el código la
> respuesta de gesprog usa **`-q`** y por eso `run.sh` lanza ctrllt con `-q`. Si el profe
> lanzara ctrllt con los flags literales del PDF, fallaría; mitigado porque arrancamos con
> `run.sh`.

---

## 8. `Makefile` — qué hace `make`

`make` compila todo:
- `CC=gcc`, `CFLAGS = -std=c11 -Wall -Wextra -g -Ivendor -Isrc/common`.
  - `-std=c11`: estándar C11. `-Wall -Wextra`: **todas las advertencias** (compila sin
    warnings = limpio). `-g`: símbolos de depuración. `-I…`: dónde buscar headers.
- Regla `vendor`: si no existe `vendor/cJSON.c/.h`, lo **descarga** con `curl`.
- Compila cada `.c` a `.o` en `build/` y enlaza cada servicio en `bin/`.
- `make` (= `make all`) genera `bin/{gesfich,gesprog,ejecutor,ctrllt}`. `make clean` borra
  `build/` y `bin/`.

**Por qué un Makefile:** recompila **solo lo que cambió** y documenta la construcción.
`proto.o` y `cJSON.o` son objetos comunes que enlazan los 4 servicios.

---

## 9. `run.sh` — arrancar/parar el sistema a mano

`./run.sh start | stop | status`.

**`start`:**
1. Crea (limpio) `aralmac` en `/tmp/aralmac`.
2. Lanza en segundo plano (`&`) `gesfich`, `gesprog`, `ejecutor` (cada uno con sus FIFOs
   `_req`/`_resp` y `-x aralmac`), guardando sus PIDs.
3. Lanza `ctrllt` conectado a los FIFOs del cliente **y** a los de los 3 servicios.
4. FIFOs del cliente: `/tmp/ctrllt_req` (peticiones) y `/tmp/ctrllt_resp` (respuestas).

**`stop`:** manda `{"servicio":"ctrllt","operacion":"Terminar"}`, luego mata los PIDs
restantes y borra los FIFOs. Como abrir un FIFO para escribir **se cuelga si ya no hay
lector**, ese envío va en un subshell que se mata a los 0.5 s (evita que `stop` se cuelgue).

**`status`:** revisa con `kill -0` si cada PID sigue vivo.

### Mandar peticiones a mano (para la demo)
```bash
./run.sh start
printf '%s\n' '{"servicio":"gesfich","operacion":"Crear"}' > /tmp/ctrllt_req
head -n1 /tmp/ctrllt_resp        # → {"estado":"ok","id-fichero":"f-0001"}
```
`printf … > req` **escribe** la petición; `head -n1 < resp` **lee una línea** (un mensaje).

---

## 10. Los scripts de prueba

### `test.sh` — prueba de integración (flujo completo, end-to-end)
Demuestra el **caso de uso real**. Requiere `./run.sh start`:
1. **Crear** fichero de entrada → captura su `f-XXXX`.
2. **Actualizar** ese fichero con `/etc/hosts` (le mete datos reales).
3. **Crear** fichero de salida.
4. **Listar** ficheros.
5. **Guardar** el programa `cat` (`which cat` da la ruta, p.ej. `/bin/cat`).
6. **Leer** los metadatos del programa.
7. **Ejecutar** `cat` con `stdin`=entrada y `stdout`=salida → `cat` copia entrada a salida.
8. **Polling de Estado** del proceso hasta `Terminado` (reintenta ~3 s).
9. **Leer** el fichero de salida y verificar que tiene contenido (= `cat` funcionó).
10. **Terminar** el sistema. Si al final está `Terminado` y la salida tiene contenido →
    **PASÓ**.

> **¿Qué es `/bin/cat`?** `cat` es un programa de UNIX que lee su stdin y lo vuelca a su
> stdout. `/bin/cat` (o `/usr/bin/cat`) es su **ruta absoluta** en disco. Lo usamos como
> programa de prueba porque su comportamiento es trivial de verificar: lo que entra, sale.
> `/etc/hosts` es un archivo de texto que existe en cualquier máquina, ideal como entrada.

### `test_states.sh` — prueba máquinas de estado y validaciones
No prueba el "happy path", prueba que **se rechace lo que debe rechazarse**:
- gesfich: Suspender → Crear/Actualizar dan error → Reasumir → Crear funciona; Reasumir/
  Suspender repetidos dan `transicion invalida`.
- gesprog: Suspender → Guardar da error **pero Leer SÍ funciona** (self-loop Figura 4).
- Validación: borrar/leer/consultar IDs inexistentes, servicio desconocido, operación
  desconocida, faltar campos obligatorios.
- Terminar el sistema.

---

## 11. Qué pedía el enunciado y cómo lo cumplimos (checklist §por§)

| Sección PDF | Qué pide | Cómo lo abordamos |
|-------------|----------|-------------------|
| §3.1 | Tuberías nombradas, half/full-duplex, nombre único | FIFOs `mkfifo`; 2 por servicio (half-duplex); si omites resp → full-duplex |
| §3.7/3.8 | JSON terminado en `\n`, formato petición/respuesta, `MSG_MAX_LEN` | `proto.c` (send/recv con `\n`), cJSON, `MSG_MAX_LEN=4096` |
| §3.8.3 | IDs `f-/p-/e-XXXX` | Contadores persistentes por servicio |
| §3.4/3.9 | gesfich: Crear/Leer/Actualizar/Borrar + estados | `gesfich.c`, Figura 3 |
| §3.5/3.10 | gesprog: Guardar (ejecutable válido)/Leer/Actualizar/Borrar + Leer en Suspendido | `gesprog.c`, validación shebang, Figura 4 |
| §3.6/3.11 | ejecutor: Ejecutar/Estado/Matar/Suspender/Reasumir/Parar; redirección stdin/out/err | `ejecutor.c`, fork+exec+dup2, SIGSTOP/CONT/KILL, Figura 5 |
| §3.12 | ctrllt: enruta por `servicio`, op propia `Terminar` | `ctrllt.c` |
| §4 | Repo en GitHub, `docs/Diseño.md`, JSON | Entregado; ver `docs/Diseño.md` |

---

## 12. Preguntas que el profe puede hacer (con respuesta corta)

- **¿Por qué FIFOs y no sockets?** El enunciado pide tuberías nombradas; son simples y
  suficientes para procesos en la misma máquina.
- **¿Por qué abren los FIFOs con `O_RDWR`?** Para que `open` no se bloquee esperando al otro
  extremo, y para que el FIFO no dé EOF si un escritor se va.
- **¿Por qué leen byte a byte en `msg_recv`?** Para detenerse exactamente en el `\n` y no
  consumir parte del siguiente mensaje.
- **¿Qué pasa si dos clientes mandan a la vez?** El PDF dice que soporta varios clientes;
  ctrllt atiende **secuencialmente** (un request, una respuesta), así no se mezclan.
- **¿Por qué copiar el ejecutable a aralmac y no guardar solo la ruta?** Para que el programa
  quede "registrado" aunque luego cambien/borren el original (modelo mainframe: la imagen
  del programa vive en el almacén).
- **¿Diferencia entre Matar y Parar?** `Matar` mata **un** proceso (SIGKILL) ya. `Parar` es
  del **servicio**: deja de aceptar trabajos y espera a que los actuales terminen solos.
- **¿Qué es un zombie y cómo lo evitan?** Hijo terminado no recogido; lo recogemos con
  `waitpid` al recibir `SIGCHLD`.
- **¿Por qué `e-XXXX` y no `b-XXXX`?** El PDF §3.8.3 dice `e-XXXX` para ejecución.
- **Programa inexistente al Ejecutar → ¿qué error?** `"no se pudo ejecutar el programa"` (el
  §3.11.3 NO incluye "programa no encontrado"; ese es de gesprog).

---

## 13. Glosario rápido
- **FIFO / named pipe:** canal de comunicación entre procesos con forma de archivo.
- **fork:** crea un proceso hijo (copia del padre).
- **exec (execv/execve):** reemplaza el proceso actual por otro programa.
- **dup2:** redirige un descriptor (p.ej. hace que stdout apunte a un fichero).
- **SIGSTOP/SIGCONT/SIGKILL/SIGCHLD:** congelar / reanudar / matar / "tu hijo terminó".
- **waitpid / WNOHANG:** recoger un hijo terminado; WNOHANG = sin bloquear.
- **stat:** consulta si un archivo existe y sus permisos/tamaño.
- **shebang (`#!`):** primera línea de un script que indica con qué intérprete correrlo.
- **aralmac:** el directorio almacén (`/tmp/aralmac`) con `ficheros/` y `programas/`.
- **zombie:** proceso hijo terminado cuyo estado nadie recogió.

---

# ANEXO — Recorrido línea por línea de CADA función

> Aquí está **cada función** de los 4 servicios + `common`, incluidas las auxiliares.
> Si el profe señala una función en pantalla, busca aquí qué hace bloque a bloque.
> Patrón común a todos: cada servicio tiene `g_estado` (su estado), `g_aralmac` (ruta del
> almacén), `g_fd_in`/`g_fd_out` (FIFOs), un `run()` (bucle principal) y un `main()` (parsea
> flags con `getopt`, abre FIFOs, llama a `run()`).

## A. `src/common/proto.c`

**`msg_send(fd, json_str)`** — envía un mensaje:
- `strlen(json_str)` mide la cadena; si `len+1 > MSG_MAX_LEN` (no cabe el `\n`) → `-1`.
- Copia el JSON a un buffer local y le pone `'\n'` al final (es el **delimitador** de mensaje).
- Bucle `while (sent < total)`: llama a `write()` repetidamente. Un `write` puede escribir
  **menos** bytes de los pedidos (escritura parcial), por eso se reintenta hasta enviar todo.
  Si `write` falla con `EINTR` (lo interrumpió una señal) reintenta; con otro error → `-1`.

**`msg_recv(fd, buf, bufsz)`** — recibe un mensaje:
- Bucle que lee **1 byte por vez** con `read(fd, buf+pos, 1)`.
- Si `read` < 0: si es `EINTR` y aún no leyó nada (`pos==0`) devuelve `-1` (para que el
  llamador revise señales); si ya leía a medias, reintenta (no perder el mensaje).
- Si `read` == 0 (EOF, el otro extremo cerró): cierra la cadena con `'\0'` y devuelve `0` si
  no había nada, o lo leído.
- Si el byte es `'\n'`: lo cambia por `'\0'` y devuelve la longitud → **mensaje completo**.
- Lee de a un byte para parar EXACTO en el `\n` y no "robarse" el inicio del siguiente mensaje.

## B. `src/gesfich/gesfich.c`

**Helpers de ruta** (`dir_ficheros`, `path_for_id`, `path_counter`): arman con `snprintf` las
rutas `aralmac/ficheros`, `aralmac/ficheros/f-XXXX` y `aralmac/ficheros/.gesfich_counter`.
Centralizan las rutas para no escribirlas a mano en cada sitio.

**`load_counter` / `save_counter`**: leen/escriben el último número usado en el archivo
oculto `.gesfich_counter`. Si no existe, `load` devuelve 0. Así los IDs **no se reutilizan**
entre arranques.

**`send_json(obj)`**: convierte el objeto cJSON a texto (`cJSON_PrintUnformatted`). **Guard
de tamaño:** si el texto +`\n` supera `MSG_MAX_LEN`, en vez de bloquear al cliente manda un
error corto. Si no, lo envía con `msg_send`. Siempre libera memoria (`free` + `cJSON_Delete`).
**`send_ok`**: manda `{"estado":"ok"}`. **`send_error(msg)`**: manda `{"estado":"error",
"mensaje":msg}`.

**`op_crear`**: `counter` es `static` (se carga del disco solo la 1ª vez). Si llegó a
`MAX_ID` (9999) → error. Incrementa, forma `f-%04d`, hace `open(..., O_CREAT|O_TRUNC, 0644)`
para crear el archivo **vacío**, lo cierra, guarda el contador y responde con `id-fichero`.
Si `open` falla, deshace el `counter++`.

**`op_leer(req)`**: si viene `id-fichero` → `stat` para ver si existe (si no, `"fichero no
encontrado"`), comprueba que el tamaño quepa en `MSG_MAX_LEN/2` (margen porque JSON puede
escapar caracteres y duplicar tamaño), lee con `fread` y responde `contenido`. Si **no** viene
id → `opendir`/`readdir` lista todo lo que empieza por `f-` y responde el array `ficheros`.

**`op_actualizar(req)`**: exige `id-fichero` y `ruta` (si falta → `"faltan campos…"`).
`stat` al destino (debe existir) y al origen (la ruta). Copia byte a byte con `fread`/`fwrite`
y responde ok. Cualquier fallo → `"no se pudo actualizar el fichero"`.

**`op_borrar(req)`**: exige `id-fichero`, `stat` para ver si existe, `unlink` para borrarlo.

**`run()`**: bucle `while (estado != TERMINADO)`. Lee con `msg_recv`; si ≤0 sale. Parsea JSON
(si inválido → error). Saca `operacion`. **Orden de decisión (clave para los estados):**
1) `Suspender`/`Reasumir`/`Terminar` se atienden siempre (validan transición).
2) Si `estado == SUSPENDIDO` y la op no fue de control → `"servicio suspendido"` (bloquea TODO).
3) Si no, despacha `Crear`/`Leer`/`Actualizar`/`Borrar`. Otro → `"operacion desconocida"`.

**`init_aralmac`**: crea `aralmac` y `aralmac/ficheros` si no existen (ignora "ya existe").

**`main(argc, argv)`**: `getopt(argc,argv,"f:b:x:")` recorre los flags: `-f`=FIFO petición,
`-b`=FIFO respuesta (opcional), `-x`=aralmac. Exige `-f` y `-x`. Llama `init_aralmac`. Crea
el FIFO con `mkfifo` y lo abre con `O_RDWR`. Si hay `-b`, abre también el de respuesta; si no,
usa el **mismo** descriptor para ambos (full-duplex). Llama `run()` y al salir cierra todo.

## C. `src/gesprog/gesprog.c`

**Helpers de ruta** (`dir_programas`, `dir_prog`, `path_bin`, `path_meta`, `path_counter`):
arman `aralmac/programas`, `…/p-XXXX`, `…/p-XXXX/bin`, `…/p-XXXX/meta.json` y el contador.

**`load_counter`/`save_counter`, `send_json`/`send_ok`/`send_error`**: idénticos a gesfich
(el error de truncado dice "no se pudo actualizar el programa").

**`copy_file(src, dst)`**: copia byte a byte con `fread`/`fwrite`. Al final hace `stat(src)`
y `chmod(dst, modo | 0111)` → **preserva/añade los bits de ejecución** para que la copia siga
siendo ejecutable.

**`read_meta(id)`**: abre `meta.json`, lo lee entero (`ftell` para el tamaño) y lo parsea a
cJSON. Devuelve el objeto o `NULL`.

**`op_guardar(req)`**: exige `ejecutable`. `stat` (debe existir). **Validación de ejecutable
válido:** si NO tiene ningún bit de ejecución (`S_IXUSR/GRP/OTH`), solo se acepta si es un
**script con shebang** (lee 2 bytes y comprueba `#!`); si no puede leerlo o no es `#!` →
`"no se pudo guardar el programa"`. Crea la carpeta `p-XXXX` (`mkdir`), copia el binario a
`bin` (`copy_file`), construye `meta.json` con `id-programa`, `nombre` (= `basename` del
ejecutable), `ejecutable`, `args` y `env` (duplica los del request o arrays vacíos), lo
escribe, guarda el contador y responde `id-programa`. Si algo falla, limpia (rmdir) y revierte.

**`op_leer(req)`**: con `id-programa` → `read_meta`, **borra el campo `ejecutable`**
(`cJSON_DeleteItemFromObject`) porque la spec §3.10.3 expone solo `id-programa/nombre/args/env`,
y responde dentro de la clave `programa`. Sin id → lista todo lo que empieza por `p-`.

**`op_actualizar(req)`**: exige `id-programa` y `ruta`. Verifica que el programa exista y la
ruta exista. **Repite la validación de shebang** (igual que Guardar). Copia el nuevo binario
sobre `bin`. Actualiza `meta.json`: cambia `ejecutable` y recalcula `nombre` (`basename`).

**`rmdir_prog(id)`**: borra recursivamente la carpeta `p-XXXX`: recorre con `readdir`,
`unlink` de cada archivo (salta `.`/`..`) y al final `rmdir` de la carpeta. Devuelve -1 si
algún borrado falló.

**`op_borrar(req)`**: exige `id-programa`, verifica que exista y llama `rmdir_prog`.

**`run()`**: igual estructura que gesfich, **con la diferencia clave**: `Leer` se atiende
**antes** del bloqueo por suspensión, por lo que `Leer` funciona también en estado Suspendido
(self-loop de la Figura 4). El resto (`Guardar`/`Actualizar`/`Borrar`) sí se bloquea suspendido.

**`init_aralmac`/`main`**: igual que gesfich pero con flags `-p`(req)`-c`(resp)`-x`(aralmac).

## D. `src/ejecutor/ejecutor.c`

**Estructuras:** `proceso_t` guarda `id` (e-XXXX), `id_prog` (p-XXXX), `pid`, `estado`
(Ejecutando/Suspendido/Terminado), `codigo_salida` y `activo`. `g_procs[MAX_PROCS]` es la
**tabla de procesos**. `g_estado` es el estado del **servicio** (Ejecutar/Suspendidos/Parar/
Terminado).

**`sigchld_handler(sig)`**: handler de la señal `SIGCHLD`. Solo pone `g_sigchld_flag = 1`
(un handler debe hacer lo mínimo y seguro). El trabajo real se hace fuera.

**`reap_children()`**: `while (waitpid(-1, &status, WNOHANG) > 0)` recoge **todos** los hijos
terminados sin bloquear. Por cada uno, busca su slot en `g_procs` por `pid`, lo marca
`Terminado` y calcula `codigo_salida`: `WEXITSTATUS(status)` si terminó normal, o
`128+WTERMSIG` si murió por una señal. Evita procesos **zombie**.

**`count_active()`**: cuenta procesos `activo` y en estado `Ejecutando`. Sirve para saber
cuándo Parar puede pasar a Terminado.

**Helpers de ruta** (`path_fichero`, `path_bin`, `path_meta`) y **`read_meta`**: como en
gesprog, para localizar ficheros de datos, el binario y los metadatos en `aralmac`.

**`send_json`/`send_ok`/`send_error`**: como en los otros (error de truncado: "no se pudo
ejecutar el programa").

**`find_proc(id)`**: busca en `g_procs` un proceso activo cuyo `id` (e-XXXX) coincida.

**`proc_to_json(p)`**: arma el objeto JSON de un proceso: `id-ejecucion`, `id-programa`,
`proceso-estado` y, **solo si está Terminado**, añade `codigo-salida`.

**`op_ejecutar(req)`** (el núcleo): primero recolecta hijos pendientes. Rechaza si el servicio
está Suspendido (`"servicio suspendido"`) o Parando (`"servicio parando"`). Exige
`id-programa`. `read_meta`: si no existe → `"no se pudo ejecutar el programa"` (NO "programa
no encontrado", §3.11.3). Construye `argv[]` (argv[0]=ruta a `bin`, luego los `args` de meta)
y `envp[]` (los `env`). Abre los ficheros de redirección (`stdin` solo lectura; `stdout`/
`stderr` con `O_CREAT|O_TRUNC`); si alguno falla, cierra lo abierto y responde error. Asigna
`e-XXXX`. **`fork()`**:
- **Hijo**: `dup2` conecta los fd de los ficheros a 0/1/2; cierra los FIFOs del servicio (para
  no heredarlos); llama `execve` (si hay env) o `execv`. Si `exec` retorna, falló → `_exit(127)`.
- **Padre**: cierra los fd de ficheros (ya los heredó el hijo), libera `meta`, registra el
  proceso en `g_procs` con estado `Ejecutando`, y responde `id-ejecucion`.

**`op_estado(req)`**: recolecta hijos. Rechaza si Suspendido/Parando. Con `id-ejecucion` →
`find_proc`; si no está → `"proceso no encontrado"`; si está, fusiona los campos de
`proc_to_json` dentro de la respuesta. Sin id → recorre todos los activos y los mete en el
array `procesos`.

**`op_matar(req)`**: recolecta hijos. Rechaza si Suspendido/Parando. Exige `id-ejecucion`.
`find_proc`; si no está → error; si ya está Terminado → `"proceso no encontrado o ya
terminado"`. Manda `SIGKILL` al `pid`, intenta `waitpid(...WNOHANG)` para marcarlo Terminado
de inmediato (`codigo-salida = 128+SIGKILL`) y responde ok.

**`op_suspender()`**: solo válido en estado Ejecutar. Manda `SIGSTOP` (congela) a cada hijo
Ejecutando y lo marca Suspendido. Pasa el servicio a `Suspendidos`.

**`op_reasumir()`**: solo válido en Suspendidos. Manda `SIGCONT` (descongela) a cada hijo
Suspendido, lo marca Ejecutando, y el servicio vuelve a `Ejecutar`.

**`op_parar()`**: válido en Ejecutar o Suspendidos. Descongela (SIGCONT) los suspendidos para
que puedan terminar, pasa el servicio a `Parar` y responde ok. (La transición real a Terminado
ocurre en el bucle/`wait_for_parar`.)

**`wait_for_parar()`**: mientras `count_active() > 0`, recolecta hijos si hay señal pendiente
y duerme 10 ms (`usleep`) — **polling**. Cuando no quedan procesos, pone el servicio en
`Terminado`. Es el apagado **ordenado**: no mata, espera a que terminen solos.

**`run()`**: instala el handler de `SIGCHLD` **sin `SA_RESTART`** (para que `read` se
interrumpa al terminar un hijo). Bucle principal: recolecta hijos; si el estado es `Parar`,
si no hay activos termina, si los hay llama `wait_for_parar` y sale. Lee petición con
`msg_recv`: si fue interrumpida (`EINTR`, n<0) recolecta hijos y reintenta; si EOF (n==0) sale.
Parsea y despacha `Ejecutar/Estado/Matar/Suspender/Reasumir/Parar`. Al salir del bucle,
`while (wait(NULL) > 0)` espera a todos los hijos restantes.

**`main`**: flags `-e`(req)`-d`(resp)`-x`(aralmac); abre FIFOs con `O_RDWR` igual que los demás.

## E. `src/ctrllt/ctrllt.c`

**Estructura `svc_fds_t`**: por cada servicio guarda `req` (fd para escribirle la petición) y
`resp` (fd para leerle la respuesta). Hay tres: `g_gesfich`, `g_gesprog`, `g_ejecutor`, más
los fd del cliente.

**`open_fifo(path, create)`**: si `create`, hace `mkfifo` (ignora si ya existe); abre con
`O_RDWR`. Devuelve el fd (o <0 con mensaje de error).

**`forward(svc, msg, resp_buf, resp_sz)`**: helper usado en el apagado: manda `msg` al FIFO de
petición del servicio y lee su respuesta (usa `resp` si existe, si no `req`). Devuelve 0/-1.

**`do_terminar()`**: envía `Terminar` a `gesfich`, `Terminar` a `gesprog` y `Parar` a
`ejecutor` (que se auto-detiene al quedarse sin procesos). Usa `forward` para cada uno.

**`run()`**: bucle principal de la pasarela:
1. Lee petición del cliente (`msg_recv`); si ≤0 sale.
2. Parsea; si JSON inválido → responde error al cliente.
3. Saca `servicio` y `operacion`.
4. Si `servicio=="ctrllt"`: si op `Terminar` → `do_terminar()`, responde ok y termina el
   bucle (`running=0`); otra op → `"operacion ctrllt desconocida"`.
5. Selecciona el servicio destino (gesfich/gesprog/ejecutor). Si no existe → `"servicio
   desconocido"`; si existe pero no conectado (`req<0`) → `"servicio no conectado"`.
6. Reenvía el **buffer literal** al FIFO del servicio (si falla → `"error enviando solicitud
   al servicio"`), lee su respuesta (si falla → `"error leyendo respuesta del servicio"`) y la
   reenvía **tal cual** al cliente. ctrllt **no modifica** la respuesta.

**`main`**: `getopt(... "c:a:f:b:p:q:e:d:")`. `-c`/`-a`=cliente req/resp; `-f`/`-b`=gesfich;
`-p`/`-q`=gesprog (`-q` por la errata del `-c` duplicado en el PDF); `-e`/`-d`=ejecutor. Exige
`-c -f -p -e`. **ctrllt crea** sus propias tuberías de cliente (`open_fifo(...,1)`) y **abre
sin crear** (`open_fifo(...,0)`) las de los servicios (que ya las creó cada servicio al
arrancar — por eso `run.sh` los lanza antes que ctrllt y mete `sleep 0.2` entre cada uno).
Llama `run()` y al final cierra todos los descriptores.

## F. Detalle de orden de arranque (por qué importa)
`run.sh` arranca **gesfich, gesprog, ejecutor primero** (cada uno hace `mkfifo` de sus FIFOs)
y **ctrllt al final** (que los abre sin crear). Si ctrllt arrancara primero, no encontraría
los FIFOs. Los `sleep 0.2` dan tiempo a que cada servicio cree su FIFO antes de seguir.
