# Guía de estudio para la sustentación — Ejecutor de Lotes (ST0257)

> Objetivo: que te aprendas el proyecto en dos días. Lee de arriba abajo, no
> te saltes nada. Lo más importante para el examen está en las secciones
> 1, 2, 3, 6 y 10.

---

## 1. ¿Qué hace este proyecto en una frase?

Es una **simulación de un mainframe** que ejecuta programas por lotes.
Hay cuatro procesos C corriendo a la vez en tu Mac/Linux que se hablan
entre sí mediante **tuberías nombradas (FIFOs)** intercambiando **mensajes
JSON** terminados en `\n`. Un cliente externo le pide cosas al sistema (crear
ficheros, registrar programas, ejecutarlos) y el sistema responde.

Es el modelo clásico cliente↔pasarela↔servicios:

```
   cliente  ──►  ctrllt  ──►  gesfich
                       └──►  gesprog
                       └──►  ejecutor
```

`ctrllt` es la **pasarela única**: el cliente solo le habla a él, y `ctrllt`
reenvía al servicio que toque mirando el campo `"servicio"` del JSON.

---

## 2. Los cuatro componentes (a quién hablan, qué hacen)

| Binario     | Función real                                                     | A quién habla          |
|-------------|------------------------------------------------------------------|------------------------|
| `ctrllt`    | Pasarela: recibe del cliente, enruta al servicio, devuelve resp. | cliente ⇄ los 3 servicios |
| `gesfich`   | CRUD de ficheros guardados en `aralmac/ficheros/`                | solo con `ctrllt`      |
| `gesprog`   | CRUD de programas ejecutables guardados en `aralmac/programas/`  | solo con `ctrllt`      |
| `ejecutor`  | Lanza procesos (`fork`+`exec`), consulta su estado, los mata     | solo con `ctrllt`      |

**`aralmac`** = "área de almacenamiento". Es solo un directorio en disco
(`/tmp/aralmac` por defecto) donde `gesfich` y `gesprog` guardan los
archivos del usuario y los binarios registrados.

El **cliente NO se implementa**. En la sustentación lo simulas tú escribiendo
JSON al FIFO de entrada con `printf` o `echo`.

---

## 3. ¿Cómo se comunican? (FIFOs + JSON + `\n`)

### 3.1 FIFOs (tuberías nombradas)

- Son archivos especiales creados con `mkfifo(path, 0666)`.
- En Linux/Mac son **half-duplex**: solo van en un sentido.
- Por eso **cada servicio usa DOS FIFOs**: uno de petición y uno de respuesta.

Lista completa de FIFOs (todas en `/tmp/`):

| FIFO              | Quién escribe | Quién lee |
|-------------------|---------------|-----------|
| `ctrllt_req`      | cliente       | ctrllt    |
| `ctrllt_resp`     | ctrllt        | cliente   |
| `gesfich_req`     | ctrllt        | gesfich   |
| `gesfich_resp`    | gesfich       | ctrllt    |
| `gesprog_req`     | ctrllt        | gesprog   |
| `gesprog_resp`    | gesprog       | ctrllt    |
| `ejecutor_req`    | ctrllt        | ejecutor  |
| `ejecutor_resp`   | ejecutor      | ctrllt    |

### 3.2 Protocolo de mensajes

Cada mensaje es **un JSON en una línea, terminado en `\n`**. Tamaño máximo
**4096 bytes** (`MSG_MAX_LEN` en `src/common/proto.h`).

**Petición** (cliente → sistema):
```json
{"servicio":"gesfich","operacion":"Crear"}
```

**Respuesta OK**:
```json
{"estado":"ok","id-fichero":"f-0001"}
```

**Respuesta error**:
```json
{"estado":"error","mensaje":"fichero no encontrado"}
```

### 3.3 IDs

Cada cosa que el sistema crea tiene un ID con formato fijo:

- Ficheros: `f-0001`, `f-0002`, … (los crea `gesfich`)
- Programas: `p-0001`, `p-0002`, … (los crea `gesprog`)
- Ejecuciones: `e-0001`, `e-0002`, … (las crea `ejecutor`)

Cada servicio mantiene su propio contador y devuelve el ID al cliente al crear.

---

## 4. Cómo lo compilas, arrancas y paras (cheatsheet para la demo)

> Estás en `/Users/jhonduque/Proyecto_SistemasOperativos`.

### 4.1 Compilar
```bash
make
```
Esto:
1. Descarga `cJSON.c/h` desde GitHub si no están en `vendor/`.
2. Compila los 4 binarios en `bin/`: `gesfich`, `gesprog`, `ejecutor`, `ctrllt`.
3. Flags: `-std=c11 -Wall -Wextra -g`.

### 4.2 Arrancar todo
```bash
./run.sh start
```
Esto crea `/tmp/aralmac`, lanza los 4 procesos en background con sus FIFOs
y guarda los PIDs en `/tmp/ejecutor_lotes.pids`.

### 4.3 Estado del sistema
```bash
./run.sh status
```
Muestra qué PIDs siguen vivos.

### 4.4 Parar todo
```bash
./run.sh stop
```
Envía `{"servicio":"ctrllt","operacion":"Terminar"}\n` al FIFO del cliente,
mata cualquier PID restante y borra los FIFOs.

### 4.5 Probar (importante para la demo)
```bash
./test.sh          # prueba completa: cat /etc/hosts → fichero de salida
./test_states.sh   # prueba de máquinas de estado y errores
```
Estos scripts hablan con `ctrllt` por sus FIFOs y muestran cada
request/response en pantalla.

### 4.6 Enviar un mensaje manualmente (útil para el profe)
```bash
# Terminal A: escuchar respuestas
cat /tmp/ctrllt_resp

# Terminal B: mandar peticiones
echo '{"servicio":"gesfich","operacion":"Crear"}' > /tmp/ctrllt_req
```

---

## 5. Estructura del repo

```
.
├── docs/
│   ├── Diseño.md             ← especificación completa (lee secciones 2, 3, 6)
│   ├── GUIA_SUSTENTACION.md  ← guía larga (referencia, opcional)
│   └── GUIA_ESTUDIO.md       ← este archivo
├── src/
│   ├── common/
│   │   ├── proto.h           ← declara msg_send / msg_recv
│   │   └── proto.c           ← implementación protocolo \n
│   ├── ctrllt/ctrllt.c       ← la pasarela
│   ├── gesfich/gesfich.c     ← servicio de ficheros
│   ├── gesprog/gesprog.c     ← servicio de programas
│   └── ejecutor/ejecutor.c   ← servicio de procesos
├── vendor/cJSON.{c,h}        ← librería JSON (terceros)
├── Makefile                  ← compilación
├── run.sh                    ← start/stop/status
├── test.sh                   ← test de integración (cat)
└── test_states.sh            ← test de máquinas de estado
```

---

## 6. El protocolo común: `proto.h` / `proto.c`

**Lo más importante de todo el código.** Si el profe pregunta cómo se comunican,
explicas esto.

### `src/common/proto.h`
```c
#define MSG_MAX_LEN 4096
int     msg_send(int fd, const char *json_str);
ssize_t msg_recv(int fd, char *buf, size_t bufsz);
```

### `msg_send` — escribe JSON + `\n`

Concatena el `\n`, escribe en un bucle hasta haber mandado todos los bytes
(porque `write()` puede escribir menos de lo pedido), retoma si `EINTR`.

### `msg_recv` — lee byte a byte hasta `\n`

```c
while (pos < bufsz - 1) {
    read(fd, buf + pos, 1);      // ← 1 byte a la vez
    if (buf[pos] == '\n') {
        buf[pos] = '\0';
        return pos;
    }
    pos++;
}
```

**¿Por qué byte a byte?** Porque si leyeras 4096 bytes de golpe podrías
**consumir parte del siguiente mensaje** y perderlo. Es ineficiente para
texto, pero **correcto** y los mensajes son pequeños (< 4 KB).

**Manejo de `EINTR`:** si una señal interrumpe `read()` ANTES de leer
algo (`pos == 0`), retorna `-1` y deja que el llamador decida (el
`ejecutor` lo usa para procesar `SIGCHLD`). Si ya leíste parte del
mensaje, reintenta (no se debe perder).

---

## 7. ctrllt — la pasarela (`src/ctrllt/ctrllt.c`)

### Lo que hace
1. Abre todos los FIFOs (cliente + los 3 servicios) con `O_RDWR`.
2. En bucle: lee del cliente → mira `"servicio"` → reenvía al FIFO de ese
   servicio → lee la respuesta → la devuelve al cliente sin modificar.
3. Operación propia: `{"servicio":"ctrllt","operacion":"Terminar"}` →
   propaga Terminar a `gesfich` y `gesprog`, `Parar` a `ejecutor`, y sale.

### Flags
```
ctrllt -c <cli-req>  -a <cli-resp>
       -f <gf-req>   -b <gf-resp>
       -p <gp-req>   -q <gp-resp>
       -e <ej-req>   -d <ej-resp>
```

> **Detalle picante**: el enunciado tenía un error tipográfico (`-c`
> duplicado para `gesprog`). En este diseño se usa `-q` para la
> respuesta de `gesprog`. Si el profe pregunta, lo mencionas.

### Errores que devuelve ctrllt
- `"servicio desconocido"` — campo `servicio` no es uno de los 3 + ctrllt
- `"operacion ctrllt desconocida"` — solo acepta `Terminar`
- `"servicio no conectado"` — FIFO no disponible
- `"error enviando solicitud al servicio"` / `"error leyendo respuesta del servicio"`

### Máquina de estados
Solo `Corriendo → Terminado` (no tiene Suspender/Reasumir).

---

## 8. gesfich — gestor de ficheros (`src/gesfich/gesfich.c`)

### Estructura en disco
```
/tmp/aralmac/ficheros/
├── f-0001
├── f-0002
└── ...
```
Un fichero por archivo. El nombre **ES** el ID.

### Operaciones

| Operación   | Petición                                               | Respuesta OK                                        |
|-------------|--------------------------------------------------------|-----------------------------------------------------|
| Crear       | `{"servicio":"gesfich","operacion":"Crear"}`           | `{"estado":"ok","id-fichero":"f-0001"}`             |
| Leer (uno)  | `{"...":"Leer","id-fichero":"f-0001"}`                 | `{"estado":"ok","contenido":"<texto>"}`             |
| Leer (todos)| `{"...":"Leer"}` (sin id)                              | `{"estado":"ok","ficheros":["f-0001","f-0002"]}`    |
| Actualizar  | `{"...":"Actualizar","id-fichero":"f-0001","ruta":"/etc/hosts"}` | `{"estado":"ok"}`                         |
| Borrar      | `{"...":"Borrar","id-fichero":"f-0001"}`               | `{"estado":"ok"}`                                   |
| Suspender   | `{"...":"Suspender"}`                                  | `{"estado":"ok"}`                                   |
| Reasumir    | `{"...":"Reasumir"}`                                   | `{"estado":"ok"}`                                   |
| Terminar    | `{"...":"Terminar"}`                                   | `{"estado":"ok"}`                                   |

### Detalles de implementación
- **Actualizar**: copia el contenido del archivo en `ruta` (ej.
  `/etc/hosts`) sobre el fichero `f-XXXX`. Útil para meter datos del
  disco al sistema.
- **Leer**: devuelve el contenido escapado en JSON. Hay un **límite
  de seguridad de `MSG_MAX_LEN/2`** (~2 KB) porque cJSON puede duplicar
  el tamaño al escapar caracteres especiales.
- **Contador persistente**: guarda el último ID usado en
  `aralmac/ficheros/.counter` para que `Crear` siga numerando aunque
  borres ficheros en el medio.

### Máquina de estados
```
Corriendo  ──[Suspender]──►  Suspendido
   ▲             [Reasumir]      │
   └─────────────────────────────┘
[Terminar]                  [Terminar]
   ▼                           ▼
Terminado ◄───────────────────┘
```

| Estado     | Operaciones válidas                              |
|------------|--------------------------------------------------|
| Corriendo  | Crear, Leer, Actualizar, Borrar, Suspender, Terminar |
| Suspendido | **solo** Reasumir, Terminar (lo demás → error `"servicio suspendido"`) |
| Terminado  | nada                                             |

---

## 9. gesprog — gestor de programas (`src/gesprog/gesprog.c`)

### Estructura en disco
```
/tmp/aralmac/programas/
├── p-0001/
│   ├── bin              ← copia del ejecutable
│   └── meta.json        ← {"id-programa":"p-0001","nombre":"cat","ejecutable":"/bin/cat","args":[],"env":[]}
├── p-0002/
└── ...
```

### Operaciones

| Operación   | Petición                                                          | Respuesta OK                                              |
|-------------|-------------------------------------------------------------------|-----------------------------------------------------------|
| Guardar     | `{"...":"Guardar","ejecutable":"/bin/cat","args":[],"env":[]}`    | `{"estado":"ok","id-programa":"p-0001"}`                  |
| Leer (uno)  | `{"...":"Leer","id-programa":"p-0001"}`                           | `{"estado":"ok",<campos del meta>}`                       |
| Leer (todos)| `{"...":"Leer"}` (sin id)                                         | `{"estado":"ok","programas":["p-0001","p-0002"]}`         |
| Actualizar  | `{"...":"Actualizar","id-programa":"p-0001","args":[...],"env":[...]}` | `{"estado":"ok"}`                                    |
| Borrar      | `{"...":"Borrar","id-programa":"p-0001"}`                         | `{"estado":"ok"}`                                         |
| Suspender / Reasumir / Terminar | (sin campos extra)                                | `{"estado":"ok"}`                                         |

### Detalle clave: validación del ejecutable en `Guardar`

```c
// 1. Debe existir (stat).
// 2. Si tiene bit de ejecución → OK.
// 3. Si NO tiene bit de ejecución → leer los primeros 2 bytes:
//    si son "#!" (shebang) → OK, lo aceptamos como script.
//    si no se puede leer o no son "#!" → error "no se pudo guardar el programa".
```
Esto es lo que pidió el profesor en el último commit (`2fef258`).

### Máquina de estados (gesprog tiene una particularidad)
**`Leer` se permite también en estado `Suspendido`** (self-loop en la
Figura 4 del enunciado). Las demás operaciones CRUD no.

---

## 10. ejecutor — el más complejo (`src/ejecutor/ejecutor.c`)

### Operaciones

| Operación   | Petición                                                                    | Respuesta OK                                                                |
|-------------|-----------------------------------------------------------------------------|-----------------------------------------------------------------------------|
| Ejecutar    | `{"...":"Ejecutar","id-programa":"p-0001","stdin":"f-0001","stdout":"f-0002","stderr":"f-0003"}` | `{"estado":"ok","id-ejecucion":"e-0001"}` |
| Estado (uno)| `{"...":"Estado","id-ejecucion":"e-0001"}`                                  | `{"estado":"ok","id-ejecucion":"e-0001","id-programa":"p-0001","proceso-estado":"Terminado","codigo-salida":0}` |
| Estado (todos)| `{"...":"Estado"}` (sin id)                                               | `{"estado":"ok","procesos":[{...},{...}]}`                                  |
| Matar       | `{"...":"Matar","id-ejecucion":"e-0001"}`                                   | `{"estado":"ok"}`                                                           |
| Suspender / Reasumir / Parar | (sin campos extra)                                         | `{"estado":"ok"}`                                                           |

### Cómo se ejecuta un programa (lo más importante de C de Unix)

```c
pid_t pid = fork();          // crea proceso hijo
if (pid == 0) {
    // ── HIJO ──
    dup2(fd_stdin,  STDIN_FILENO);   // redirige stdin desde f-0001
    dup2(fd_stdout, STDOUT_FILENO);  // redirige stdout hacia f-0002
    dup2(fd_stderr, STDERR_FILENO);
    close(g_fd_in); close(g_fd_out); // no heredar FIFOs del servicio
    execve(binpath, argv_buf, envp_buf);
    // si execve falla, no se vuelve aquí
}
// ── PADRE ──
// guarda el pid en la tabla g_procs[] y devuelve "id-ejecucion": "e-XXXX"
```

**`stdin`/`stdout`/`stderr`** son IDs de fichero (`f-XXXX`). El ejecutor abre el
archivo en disco antes del `fork`, y el hijo los redirige con `dup2`.

### SIGCHLD y procesos zombie

Cuando un hijo termina, el kernel envía `SIGCHLD` al padre. Si no se
recolecta, queda **zombie** (ocupa entrada en la tabla de procesos).

El ejecutor instala un handler `sigchld_handler` SIN `SA_RESTART`. Esto hace que:

1. Cuando llega `SIGCHLD`, el `read()` bloqueante en `msg_recv` se
   **interrumpe** (devuelve `-1` con `errno=EINTR`).
2. El bucle principal ve `g_sigchld_flag` activado y llama a
   `reap_children()`, que usa `waitpid(-1, &status, WNOHANG)` para
   recolectar TODOS los hijos terminados sin bloquearse.
3. Marca el proceso como `PROC_TERMINADO` con su `codigo_salida` en `g_procs[]`.

Por eso `proto.c::msg_recv` propaga `EINTR` cuando `pos == 0` (sin perder datos).

### Estados de un proceso individual
`Ejecutando` ──[Suspender]──► `Suspendido` (con `SIGSTOP`)
`Suspendido` ──[Reasumir]──► `Ejecutando` (con `SIGCONT`)
cualquiera ──[hijo termina]──► `Terminado`

### Máquina de estados DEL SERVICIO (distinta de la del proceso)
```
Ejecutar ←──[Reasumir]── Suspendidos
   │   ──[Suspender]──►
   │
[Parar]
   ▼
 Parar ──[/Proceso=0]──► Terminado    (espera a que terminen todos)
```

Cuando llega `Parar`: deja de aceptar nuevas ejecuciones, reactiva
los suspendidos con `SIGCONT` para que terminen, y cuando
`count_active() == 0`, el servicio se termina solo.

### Errores del ejecutor (importante)
- `"no se pudo ejecutar el programa"` — TODOS los errores de `Ejecutar`
  (programa no existe, no se pudo abrir stdin/stdout, fork falló…).
  Esto es así por especificación: §3.11.3 del enunciado dice que la
  lista oficial de errores del ejecutor NO incluye `"programa no
  encontrado"` (ese es exclusivo de `gesprog`). El commit `ef20465`
  corrigió esto.
- `"proceso no encontrado"`, `"transicion invalida"`, `"servicio
  suspendido"`, `"servicio parando"`, `"falta campo: id-programa"`.

---

## 11. Caso de uso completo (paso a paso) — esto sirve para la demo

Escenario: **ejecutar `cat /etc/hosts` y capturar la salida en un fichero**.

| Paso | Petición                                                                                          | Respuesta                                                                  |
|------|---------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------|
| 1    | `{"servicio":"gesfich","operacion":"Crear"}`                                                      | `{"estado":"ok","id-fichero":"f-0001"}`                                    |
| 2    | `{"servicio":"gesfich","operacion":"Actualizar","id-fichero":"f-0001","ruta":"/etc/hosts"}`       | `{"estado":"ok"}`                                                          |
| 3    | `{"servicio":"gesfich","operacion":"Crear"}`                                                      | `{"estado":"ok","id-fichero":"f-0002"}`                                    |
| 4    | `{"servicio":"gesprog","operacion":"Guardar","ejecutable":"/bin/cat","args":[],"env":[]}`         | `{"estado":"ok","id-programa":"p-0001"}`                                   |
| 5    | `{"servicio":"ejecutor","operacion":"Ejecutar","id-programa":"p-0001","stdin":"f-0001","stdout":"f-0002"}` | `{"estado":"ok","id-ejecucion":"e-0001"}`                          |
| 6    | `{"servicio":"ejecutor","operacion":"Estado","id-ejecucion":"e-0001"}`                            | `{"estado":"ok","proceso-estado":"Terminado","codigo-salida":0,...}`       |
| 7    | `{"servicio":"gesfich","operacion":"Leer","id-fichero":"f-0002"}`                                 | `{"estado":"ok","contenido":"<contenido de /etc/hosts>"}`                  |
| 8    | `{"servicio":"ctrllt","operacion":"Terminar"}`                                                    | `{"estado":"ok"}`                                                          |

**Esto es exactamente lo que hace `test.sh`.** En la sustentación:
1. `./run.sh start`
2. `./test.sh` (lo ve todo paso a paso)
3. `./run.sh stop`

Si el profe quiere ver algo manual:
```bash
cat /tmp/ctrllt_resp &                                                  # en background
echo '{"servicio":"gesfich","operacion":"Crear"}' > /tmp/ctrllt_req     # repite con otros JSON
```

---

## 12. Decisiones de implementación clave (preguntas del profe)

Si el profe pregunta "¿por qué hiciste X?", aquí van las respuestas:

### 12.1 `O_RDWR` para abrir los FIFOs
> **Por qué**: Si abres un FIFO en modo solo-lectura (`O_RDONLY`),
> `open()` se **bloquea** hasta que alguien lo abra para escritura.
> Abrirlo en `O_RDWR` no bloquea — el mismo proceso es lector y
> escritor virtual, así que el `open` retorna ya. También evita
> recibir `SIGPIPE` o `EOF` cuando el otro extremo cierra.

### 12.2 Protocolo byte a byte en `msg_recv`
> **Por qué**: Garantiza que no se consume parte del siguiente
> mensaje. Si leyera de a 4 KB y llegaran dos mensajes juntos,
> tendría que bufferear el remanente. Byte a byte es más simple
> y suficiente para mensajes < 4 KB.

### 12.3 `SIGCHLD` sin `SA_RESTART` en el ejecutor
> **Por qué**: Necesito que `read()` en `msg_recv` se **interrumpa**
> cuando un hijo termina, para recolectar zombies con `waitpid(WNOHANG)`
> sin bloquear el servicio. Con `SA_RESTART`, el `read` reanudaría
> solo y nunca limpiaría los zombies.

### 12.4 Contador persistente (`.counter`)
> **Por qué**: Si `Crear`/`Guardar` reusara IDs después de un `Borrar`,
> dos referencias al mismo ID podrían apuntar a cosas distintas en el
> tiempo. Guardar el contador en disco asegura que los IDs son
> monotónicos crecientes incluso si reinicias el servicio.

### 12.5 `args` y `env` por programa, no por ejecución
> **Por qué**: El enunciado dice que cada programa tiene sus argumentos
> y entorno por defecto. `Ejecutar` solo recibe `id-programa` + redirecciones.
> Si quisieras cambiar `args`, primero haces `Actualizar` en `gesprog`.

### 12.6 Aceptar scripts con shebang aunque no tengan bit de ejecución
> **Por qué**: `chmod +x` se vuelve a aplicar al copiar (con
> `chmod(dst, st.st_mode | 0111)`). Aceptar un `.sh` sin permisos pero
> con `#!/bin/bash` es conveniente para el usuario y `exec` lo manejará.

### 12.7 `Terminar` propaga a los servicios
> Cuando `ctrllt` recibe `Terminar`, le manda `Terminar` a `gesfich` y
> `gesprog`, y `Parar` al `ejecutor` (porque el ejecutor primero debe
> esperar a sus hijos). Luego cierra y sale.

---

## 13. Estados y errores comunes (chuletario)

### Errores de cada servicio (lista oficial)

**gesfich**: `"no se pudo crear el fichero"`, `"fichero no encontrado"`,
`"error al listar ficheros"`, `"faltan campos: id-fichero, ruta"`,
`"no se pudo actualizar el fichero"`, `"transicion invalida"`,
`"servicio suspendido"`, `"operacion desconocida"`.

**gesprog**: `"no se pudo guardar el programa"`, `"programa no encontrado"`,
`"falta campo: ejecutable"`, `"no se pudo actualizar el programa"`,
`"transicion invalida"`, `"servicio suspendido"`, `"operacion desconocida"`.

**ejecutor**: `"falta campo: id-programa"`, `"no se pudo ejecutar el programa"`,
`"falta campo: id-ejecucion"`, `"proceso no encontrado"`,
`"proceso no encontrado o ya terminado"`, `"transicion invalida"`,
`"servicio suspendido"`, `"servicio parando"`, `"operacion desconocida"`.

**ctrllt**: `"servicio desconocido"`, `"operacion ctrllt desconocida"`,
`"servicio no conectado"`, `"error enviando solicitud al servicio"`,
`"error leyendo respuesta del servicio"`, `"json invalido"`.

---

## 14. Plan de demostración (lo que harás en el examen)

> **Dos terminales abiertas**. Practícalo hoy y mañana.

### Terminal 1 — sistema
```bash
cd ~/Proyecto_SistemasOperativos
make                  # 1) muestra que compila limpio
./run.sh start        # 2) arranca los 4 servicios
./run.sh status       # 3) (opcional) muestra los PIDs
```

### Terminal 2 — pruebas
```bash
cd ~/Proyecto_SistemasOperativos
./test.sh             # 4) prueba completa (cat /etc/hosts)
./test_states.sh      # 5) prueba de máquinas de estado
```

### Cierre
```bash
./run.sh stop         # 6) baja todo limpiamente
```

### Si el profe pide algo "a mano"

Abre una tercera terminal con `cat /tmp/ctrllt_resp` y desde la 2 le mandas
JSONs uno por uno con `echo '...' > /tmp/ctrllt_req`. Por ejemplo:

```bash
# Listar todos los ficheros
echo '{"servicio":"gesfich","operacion":"Leer"}' > /tmp/ctrllt_req

# Suspender gesfich
echo '{"servicio":"gesfich","operacion":"Suspender"}' > /tmp/ctrllt_req
# Intentar crear en suspendido → debe responder error
echo '{"servicio":"gesfich","operacion":"Crear"}' > /tmp/ctrllt_req
# Reasumir
echo '{"servicio":"gesfich","operacion":"Reasumir"}' > /tmp/ctrllt_req
```

---

## 15. Preguntas típicas + respuestas cortas

**P: ¿Por qué tuberías nombradas en vez de sockets o memoria compartida?**
R: Lo pidió el enunciado. Además son IPC clásico de Unix, muy simples,
y bastan para mensajes pequeños unidireccionales.

**P: ¿Por qué half-duplex (dos FIFOs por servicio)?**
R: Porque los FIFOs en Linux son unidireccionales. Una para petición
y otra para respuesta es la forma estándar de hacer request/response
sobre FIFOs.

**P: ¿Qué pasa si el cliente cierra el FIFO mientras leo?**
R: `msg_recv` recibe `EOF` (read devuelve 0) y el bucle principal sale.
Como `ctrllt` abre todo en `O_RDWR`, el FIFO nunca queda "huérfano".

**P: ¿Cómo evitas procesos zombie?**
R: Handler de `SIGCHLD` + `waitpid(-1, ..., WNOHANG)` en `reap_children()`.
Sin `SA_RESTART` para que el `read` se interrumpa y la limpieza
sea inmediata.

**P: ¿Qué hace `dup2` exactamente?**
R: Duplica un descriptor de archivo a un slot específico. `dup2(fd, 0)`
hace que el descriptor 0 (stdin) apunte a lo mismo que `fd`. Después
el `exec` ve "stdin" leyendo del fichero `f-XXXX` que abrí antes.

**P: ¿Por qué `_POSIX_C_SOURCE 200809L`?**
R: Para activar funciones POSIX no estándar de C11 como `kill`,
`sigaction`, `mkfifo`, etc. Sin esa macro el compilador con `-std=c11`
no las expone.

**P: ¿Por qué cJSON y no escribir el parser?**
R: Es la librería más ligera y de un solo archivo (no requiere build
system). Está en `vendor/`. El Makefile la descarga automáticamente.

**P: ¿Qué pasa si dos clientes mandan a la vez?**
R: El sistema procesa una petición a la vez. El segundo cliente queda
bloqueado en el `write` a `ctrllt_req` hasta que el primero termine.
Esto es un sistema de un solo cliente por diseño.

**P: ¿Por qué el ejecutor copia el binario en `aralmac/programas/p-XXXX/bin`
y no lo invoca directamente por su ruta original?**
R: Para que el sistema sea autocontenido (si borras el original, el
programa sigue funcionando) y para preservar metadatos (`args`, `env`).

---

## 16. Cosas que conviene tener memorizadas

- **4** binarios: `ctrllt`, `gesfich`, `gesprog`, `ejecutor`.
- **8** FIFOs (1 par por servicio + 1 par para el cliente).
- **1** sola pasarela: `ctrllt`. El cliente NUNCA habla directo con los servicios.
- **IDs**: `f-XXXX`, `p-XXXX`, `e-XXXX`.
- **Protocolo**: JSON + `\n`, máximo **4096 bytes**.
- **Operaciones de control**: `Suspender`, `Reasumir`, `Terminar` (en
  ejecutor: `Suspender`, `Reasumir`, `Parar`).
- **Llamadas de sistema clave**: `mkfifo`, `open(O_RDWR)`, `read/write`,
  `fork`, `execve`, `dup2`, `waitpid`, `kill(SIGSTOP/SIGCONT/SIGTERM)`,
  `sigaction(SIGCHLD)`.

---

## 17. Si algo falla durante la sustentación

| Problema                                       | Solución rápida                                             |
|------------------------------------------------|-------------------------------------------------------------|
| `make` da error de cJSON                       | `ls vendor/cJSON.*`; si falta, `make` lo descarga, pero requiere internet. |
| `./run.sh start` dice "no se pudo abrir FIFO"  | `./run.sh stop` y limpia: `rm -f /tmp/ctrllt_* /tmp/ges* /tmp/ejecutor_*` |
| `./test.sh` se cuelga                          | El sistema no está arrancado o un servicio murió. `./run.sh status` y reiniciar. |
| Quedaron procesos colgados                     | `pkill -f 'bin/(ctrllt\|gesfich\|gesprog\|ejecutor)'`       |
| El profe pide ver el directorio aralmac        | `ls -la /tmp/aralmac/` (ficheros y programas)               |

---

¡Eso es todo! Si te sabes esta guía vas a saber sustentar bien. Lo más
importante: secciones **2, 3, 6, 10 y 12**.
