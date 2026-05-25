# Diseño del Sistema — Ejecutor de Lotes

**Asignatura:** ST0257 — Sistemas Operativos
**Estudiante:** Jhon Duque
**Modalidad:** Individual — Linux
**Entrega:** Entrega final (segunda y tercera entrega)
**Fecha:** Mayo de 2026

---

## 1. Introducción

Este documento presenta el diseño e implementación del sistema *Ejecutor de Lotes*
conforme al enunciado de la práctica del profesor Juan Francisco
Cardona Mc'Cormick (ST0257-C2661-4677).

El sistema simula el modelo de ejecución por lotes presente en los
sistemas operativos de mainframe. Se registran imágenes de programas
—con su descripción, ejecutable, argumentos y ambiente— para luego ser
ejecutados a través de un identificador obtenido en el registro. Cada
proceso sigue el modelo de proceso de lotes: lee los datos de su
entrada estándar, los procesa y retorna el resultado en su salida
estándar. El sistema también registra los ficheros que serán fuente o
destino de esos procesos.

---

## 2. Descripción general del sistema

### 2.1. Componentes

| Componente | Responsabilidad                                                   |
|------------|-------------------------------------------------------------------|
| `cliente`  | CRUD de programas y ficheros. Lanzar, consultar y terminar procesos de lotes. |
| `ctrllt`   | Control de Lotes. Pasarela central del sistema.                   |
| `gesfich`  | Crear, actualizar, borrar y leer ficheros en `aralmac`.           |
| `gesprog`  | Guardar, actualizar, borrar y mostrar programas en `aralmac`.     |
| `ejecutor` | Ejecutar procesos de lotes a partir de programas y ficheros registrados. |
| `aralmac`  | Área de almacenamiento compartida (directorio del sistema de archivos). |

El `cliente` no hace parte de la implementación de esta práctica.

### 2.2. Diagrama general

```
                    ┌──► gesprog ──┐
cliente ──► ctrllt ─┤              ├──► aralmac
           ◄──────  ├──► gesfich ──┘
                    └──► ejecutor
```

### 2.3. Tipo de comunicación

Toda la comunicación entre los procesos se lleva a cabo mediante
**tuberías nombradas (FIFOs)**. En Linux las tuberías nombradas son
**half-duplex**, por lo que **cada conexión usa dos tuberías**: una
para peticiones y otra para respuestas.

Cada tubería tiene un nombre único.

---

## 3. Formato de los mensajes

El sistema utiliza **JSON delimitado por salto de línea** (`\n`).
Cada mensaje es un objeto JSON en una sola línea terminada en `\n`.
El tamaño máximo es `MSG_MAX_LEN = 4096` bytes (incluyendo el `\n`).

### 3.1. Petición (cliente → ctrllt → servicio)

```json
{"servicio":"<svc>","operacion":"<op>"[, campos adicionales...]}
```

El campo `servicio` puede ser `"gesfich"`, `"gesprog"`, `"ejecutor"` o
`"ctrllt"`. El campo `operacion` identifica la acción solicitada. Los
campos adicionales dependen de la operación.

### 3.2. Respuesta exitosa (servicio → ctrllt → cliente)

```json
{"estado":"ok"[, campos adicionales...]}
```

### 3.3. Respuesta de error

```json
{"estado":"error","mensaje":"<descripcion del error>"}
```

---

## 4. Identificadores

| Recurso    | Formato  | Ejemplo  | Asignado por |
|------------|----------|----------|--------------|
| Fichero    | `f-XXXX` | `f-0001` | `gesfich`    |
| Programa   | `p-XXXX` | `p-0001` | `gesprog`    |
| Ejecución  | `e-XXXX` | `e-0001` | `ejecutor`   |

Cada `X` es un dígito decimal (0–9). Los identificadores son únicos
durante toda la vida del sistema; nunca se reutilizan.

---

## 5. `cliente`

### 5.1. Responsabilidad

El cliente se encarga de registrar, consultar, borrar y actualizar
(CRUD) programas y ficheros, y de lanzar, consultar y terminar los
procesos de lotes.

El sistema soporta **múltiples clientes ejecutándose simultáneamente**.

### 5.2. Sinopsis

```
cliente -c <tuberia-nombrada> [-a <tuberia-nombrada>]
```

| Opción | Descripción |
|--------|-------------|
| `-c`   | Tubería donde se envían las peticiones a `ctrllt`. |
| `-a`   | Tubería para recibir respuestas (half-duplex, opcional). |

---

## 6. `ctrllt`

### 6.1. Responsabilidad

`ctrllt` (Control de Lotes) es la pasarela central. Recibe peticiones
de los clientes, las dirige al servicio apropiado, espera la respuesta
y la reenvía al cliente.

`ctrllt` crea las tuberías del lado del cliente.

### 6.2. Sinopsis

```
ctrllt -c <tuberia-nombrada> [-a <tuberia-nombrada>] \
       -f <tuberia-nombrada> [-b <tuberia-nombrada>] \
       -p <tuberia-nombrada> [-q <tuberia-nombrada>] \
       -e <tuberia-nombrada> [-d <tuberia-nombrada>]
```

Las opciones `-c`, `-f`, `-p`, `-e` corresponden a las tuberías de
peticiones hacia `ctrllt`, `gesfich`, `gesprog` y `ejecutor`
respectivamente. Las opciones `-a`, `-b`, `-q`, `-d` son las tuberías
de respuesta (half-duplex).

> **Nota sobre el enunciado:** La sinopsis original usa `-c` dos veces.
> Es un error tipográfico; este diseño usa `-q` para respuestas de
> `gesprog` con el fin de evitar la colisión.

### 6.3. Ruteo de peticiones

`ctrllt` inspecciona el campo `servicio` del JSON y reenvía la petición
a la tubería del servicio correspondiente, luego devuelve la respuesta
al cliente sin modificarla. La única operación propia del controlador
es `Terminar`.

### 6.4. Operación propia: Terminar

```json
{"servicio":"ctrllt","operacion":"Terminar"}
```

Respuesta:
```json
{"estado":"ok"}
```

Propaga `Terminar` a `gesfich` y `gesprog`, envía `Parar` a `ejecutor`,
y finaliza el propio controlador.

### 6.5. Máquina de estados

```
inicio ──► Corriendo ──[Terminar]──► Terminado
```

### 6.6. Mensajes de error del controlador

```json
{"estado":"error","mensaje":"servicio desconocido"}
{"estado":"error","mensaje":"operacion ctrllt desconocida"}
{"estado":"error","mensaje":"servicio no conectado"}
{"estado":"error","mensaje":"error enviando solicitud al servicio"}
{"estado":"error","mensaje":"error leyendo respuesta del servicio"}
```

---

## 7. `gesfich`

### 7.1. Responsabilidad

`gesfich` crea, actualiza, borra y lee los ficheros almacenados en
`aralmac`.

### 7.2. Sinopsis

```
gesfich -f <tuberia-req> [-b <tuberia-resp>] -x <aralmac>
```

### 7.3. Estructura de almacenamiento

```
<aralmac>/
└── ficheros/
    ├── .gesfich_counter
    ├── f-0001
    ├── f-0002
    └── ...
```

### 7.4. Máquina de estados

```
                    ┌─[Crear/Leer/Actualizar/Borrar]─┐
                    ▼                                 │
   inicio ──► Corriendo ──[Suspender]──► Suspendido
                 │    ◄──[Reasumir]───────┘  │
              [Terminar]                  [Terminar]
                 ▼                           ▼
              Terminado ◄───────────────────┘
```

| Estado     | Operaciones válidas |
|------------|---------------------|
| Corriendo  | Crear, Leer, Actualizar, Borrar, Suspender, Terminar |
| Suspendido | Reasumir, Terminar |
| Terminado  | — |

### 7.5. Operaciones

#### Crear

Crea un fichero vacío en el repositorio y devuelve su identificador.

Petición:
```json
{"servicio":"gesfich","operacion":"Crear"}
```

Respuesta (éxito):
```json
{"estado":"ok","id-fichero":"f-0001"}
```

#### Leer (por identificador)

Devuelve el contenido del fichero indicado.

Petición:
```json
{"servicio":"gesfich","operacion":"Leer","id-fichero":"f-0001"}
```

Respuesta (éxito):
```json
{"estado":"ok","contenido":"<contenido del fichero>"}
```

#### Leer (listar todos)

Devuelve la lista de todos los identificadores de fichero existentes.

Petición:
```json
{"servicio":"gesfich","operacion":"Leer"}
```

Respuesta (éxito):
```json
{"estado":"ok","ficheros":["f-0001","f-0002"]}
```

#### Actualizar

Reemplaza el contenido de un fichero con el del archivo indicado en `ruta`.

Petición:
```json
{"servicio":"gesfich","operacion":"Actualizar","id-fichero":"f-0001","ruta":"/ruta/al/archivo"}
```

Respuesta (éxito):
```json
{"estado":"ok"}
```

#### Borrar

Elimina un fichero del repositorio.

Petición:
```json
{"servicio":"gesfich","operacion":"Borrar","id-fichero":"f-0001"}
```

Respuesta (éxito):
```json
{"estado":"ok"}
```

#### Suspender / Reasumir / Terminar

Sin parámetros adicionales.

```json
{"servicio":"gesfich","operacion":"Suspender"}
{"servicio":"gesfich","operacion":"Reasumir"}
{"servicio":"gesfich","operacion":"Terminar"}
```

Respuesta genérica de éxito:
```json
{"estado":"ok"}
```

### 7.6. Mensajes de error posibles

`"no se pudo crear el fichero"`, `"fichero no encontrado"`,
`"error al listar ficheros"`, `"faltan campos: id-fichero, ruta"`,
`"no se pudo actualizar el fichero"`, `"transicion invalida"`,
`"servicio suspendido"`, `"operacion desconocida"`.

---

## 8. `gesprog`

### 8.1. Responsabilidad

`gesprog` guarda, actualiza, borra y muestra los programas almacenados
en `aralmac`.

### 8.2. Sinopsis

```
gesprog -p <tuberia-req> [-c <tuberia-resp>] -x <aralmac>
```

### 8.3. Estructura de almacenamiento

```
<aralmac>/
└── programas/
    ├── .gesprog_counter
    └── p-0001/
        ├── bin        ← copia del ejecutable
        └── meta.json  ← metadatos (nombre, args, env, ejecutable)
```

### 8.4. Máquina de estados

```
                    ┌─[Guardar/Leer/Actualizar/Borrar]─┐
                    ▼                                   │
   inicio ──► Corriendo ──[Suspender]──► Suspendido ──[Leer]──┐
                 │    ◄──[Reasumir]───────┘  │                 ▲
              [Terminar]                  [Terminar]            │
                 ▼                           ▼                  │
              Terminado ◄───────────────────┘◄─────────────────┘
```

| Estado     | Operaciones válidas |
|------------|---------------------|
| Corriendo  | Guardar, Leer, Actualizar, Borrar, Suspender, Terminar |
| Suspendido | **Leer**, Reasumir, Terminar |
| Terminado  | — |

> `Leer` es válido también en estado `Suspendido` (self-loop en la
> Figura 4 del enunciado).

### 8.5. Operaciones

#### Guardar

Registra un programa ejecutable con sus argumentos y variables de
entorno. Lo copia dentro de `aralmac`. `args` y `env` son opcionales.

Petición:
```json
{"servicio":"gesprog","operacion":"Guardar",
 "ejecutable":"/ruta/al/ejecutable",
 "args":["arg1","arg2"],
 "env":["CLAVE=VALOR"]}
```

Respuesta (éxito):
```json
{"estado":"ok","id-programa":"p-0001"}
```

#### Leer (por identificador)

Devuelve los metadatos del programa. El objeto `programa` expone
`id-programa`, `nombre`, `args` y `env` (no la ruta del ejecutable).

Petición:
```json
{"servicio":"gesprog","operacion":"Leer","id-programa":"p-0001"}
```

Respuesta (éxito):
```json
{"estado":"ok","programa":{
  "id-programa":"p-0001",
  "nombre":"nombre_ejecutable",
  "args":["arg1","arg2"],
  "env":["CLAVE=VALOR"]
}}
```

#### Leer (listar todos)

```json
{"servicio":"gesprog","operacion":"Leer"}
```

Respuesta (éxito):
```json
{"estado":"ok","programas":["p-0001","p-0002"]}
```

#### Actualizar

Reemplaza el binario almacenado con el archivo en la nueva `ruta`.

Petición:
```json
{"servicio":"gesprog","operacion":"Actualizar","id-programa":"p-0001","ruta":"/nueva/ruta"}
```

Respuesta (éxito):
```json
{"estado":"ok"}
```

#### Borrar

```json
{"servicio":"gesprog","operacion":"Borrar","id-programa":"p-0001"}
```

Respuesta (éxito):
```json
{"estado":"ok"}
```

#### Suspender / Reasumir / Terminar

```json
{"servicio":"gesprog","operacion":"Suspender"}
{"servicio":"gesprog","operacion":"Reasumir"}
{"servicio":"gesprog","operacion":"Terminar"}
```

Respuesta:
```json
{"estado":"ok"}
```

### 8.6. Mensajes de error posibles

`"falta campo: ejecutable"`, `"no se pudo guardar el programa"`,
`"programa no encontrado"`, `"error al listar programas"`,
`"faltan campos: id-programa, ruta"`, `"no se pudo actualizar el programa"`,
`"transicion invalida"`, `"servicio suspendido"`, `"operacion desconocida"`.

---

## 9. `ejecutor`

### 9.1. Responsabilidad

`ejecutor` lanza procesos de lotes. Los programas y ficheros están
almacenados en `aralmac`; a partir de ellos se crean los procesos.

### 9.2. Sinopsis

```
ejecutor -e <tuberia-req> [-d <tuberia-resp>] -x <aralmac>
```

### 9.3. Máquina de estados del servicio

```
                    ┌─[Ejecutar/Estado/Matar]─┐
                    ▼                         │
   inicio ──► Ejecutar ──[Suspender]──► Suspendidos
                 │    ◄──[Reasumir]──────┘
                 │
              [Parar]
                 ▼
               Parar ──[/Proceso=0]──► Terminado
```

| Estado      | Operaciones válidas |
|-------------|---------------------|
| Ejecutar    | Ejecutar, Estado, Matar, Suspender, Parar |
| Suspendidos | Reasumir |
| Parar       | — (espera a que los procesos terminen) |
| Terminado   | — |

Cuando `Parar` es recibido, el servicio deja de aceptar nuevas
ejecuciones y espera a que todos los procesos activos finalicen.
Cuando el contador llega a cero, el servicio termina automáticamente.

### 9.4. Estado de cada proceso individual

| Valor         | Significado |
|---------------|-------------|
| `Ejecutando`  | El proceso está en ejecución. |
| `Suspendido`  | El proceso está detenido (SIGSTOP). |
| `Terminado`   | El proceso finalizó; campo `codigo-salida` presente. |

### 9.5. Operaciones

#### Ejecutar

Lanza el programa indicado. `stdin`, `stdout` y `stderr` son
identificadores de fichero opcionales; si se omiten, el proceso hereda
los descriptores del servicio.

Petición:
```json
{"servicio":"ejecutor","operacion":"Ejecutar",
 "id-programa":"p-0001",
 "stdin":"f-0001",
 "stdout":"f-0002",
 "stderr":"f-0003"}
```

Respuesta (éxito):
```json
{"estado":"ok","id-ejecucion":"e-0001"}
```

#### Estado (por identificador)

Consulta el estado de un proceso en ejecución. El campo `codigo-salida`
solo aparece cuando `proceso-estado` es `"Terminado"`.

Petición:
```json
{"servicio":"ejecutor","operacion":"Estado","id-ejecucion":"e-0001"}
```

Respuesta (proceso ejecutando):
```json
{"estado":"ok","id-ejecucion":"e-0001","id-programa":"p-0001","proceso-estado":"Ejecutando"}
```

Respuesta (proceso terminado):
```json
{"estado":"ok","id-ejecucion":"e-0001","id-programa":"p-0001","proceso-estado":"Terminado","codigo-salida":0}
```

#### Estado (todos los procesos)

```json
{"servicio":"ejecutor","operacion":"Estado"}
```

Respuesta:
```json
{"estado":"ok","procesos":[
  {"id-ejecucion":"e-0001","id-programa":"p-0001","proceso-estado":"Ejecutando"},
  {"id-ejecucion":"e-0002","id-programa":"p-0002","proceso-estado":"Terminado","codigo-salida":1}
]}
```

#### Matar

Termina la ejecución del proceso indicado.

Petición:
```json
{"servicio":"ejecutor","operacion":"Matar","id-ejecucion":"e-0001"}
```

Respuesta:
```json
{"estado":"ok"}
```

#### Suspender / Reasumir / Parar

Sin parámetros adicionales.

```json
{"servicio":"ejecutor","operacion":"Suspender"}
{"servicio":"ejecutor","operacion":"Reasumir"}
{"servicio":"ejecutor","operacion":"Parar"}
```

Respuesta genérica de éxito:
```json
{"estado":"ok"}
```

### 9.6. Mensajes de error posibles

`"falta campo: id-programa"`, `"no se pudo ejecutar el programa"`,
`"programa no encontrado"`, `"falta campo: id-ejecucion"`,
`"proceso no encontrado"`, `"proceso no encontrado o ya terminado"`,
`"transicion invalida"`, `"servicio suspendido"`, `"servicio parando"`,
`"operacion desconocida"`.

---

## 10. Implementación

### 10.1. Lenguaje y compilación

- Lenguaje: **C11**
- Compilador: `gcc -std=c11 -Wall -Wextra`
- Biblioteca JSON: **cJSON** (vendor/)
- Build: `make` genera `bin/gesfich`, `bin/gesprog`, `bin/ejecutor`, `bin/ctrllt`

### 10.2. Estructura del repositorio

```
.
├── docs/
│   ├── Diseño.md           ← este documento
│   └── GUIA_SUSTENTACION.md
├── src/
│   ├── common/
│   │   ├── proto.c         ← msg_send / msg_recv
│   │   └── proto.h
│   ├── gesfich/gesfich.c
│   ├── gesprog/gesprog.c
│   ├── ejecutor/ejecutor.c
│   └── ctrllt/ctrllt.c
├── vendor/
│   ├── cJSON.c
│   └── cJSON.h
├── Makefile
├── run.sh                  ← arrancar/detener el sistema
├── test.sh                 ← prueba de integración
└── test_states.sh          ← prueba de máquinas de estado
```

### 10.3. Arranque del sistema

```bash
./run.sh start   # lanza todos los servicios
./run.sh stop    # detiene todos los servicios
./run.sh status  # muestra el estado de los procesos
```

Comandos directos:
```bash
./bin/gesfich -f /tmp/gesfich_req -b /tmp/gesfich_resp -x /tmp/aralmac
./bin/gesprog -p /tmp/gesprog_req -c /tmp/gesprog_resp -x /tmp/aralmac
./bin/ejecutor -e /tmp/ejecutor_req -d /tmp/ejecutor_resp -x /tmp/aralmac
./bin/ctrllt -c /tmp/ctrllt_req -a /tmp/ctrllt_resp \
             -f /tmp/gesfich_req -b /tmp/gesfich_resp \
             -p /tmp/gesprog_req -q /tmp/gesprog_resp \
             -e /tmp/ejecutor_req -d /tmp/ejecutor_resp
```

### 10.4. Decisiones de implementación clave

#### O_RDWR en FIFOs

Todos los FIFOs se abren con `O_RDWR` en lugar de `O_RDONLY`/`O_WRONLY`.
Esto evita que `open()` bloquee (el mismo proceso es lector y escritor
simultáneo) y evita que `read()` devuelva EOF cuando no hay escritores
externos temporalmente.

#### Protocolo byte a byte

`msg_recv` lee un byte a la vez hasta encontrar `\n`. Esto garantiza
que no se consuma parte del siguiente mensaje, siendo correcto aunque
no máximamente eficiente para mensajes pequeños (< 4 KB).

#### SIGCHLD en ejecutor

`ejecutor` instala un handler de `SIGCHLD` sin `SA_RESTART`. Cuando
`msg_recv` es interrumpido por `SIGCHLD` (retorna -1 con `errno=EINTR`
y `pos==0`), el bucle principal verifica el flag `g_sigchld_flag` y
llama a `reap_children()` (que usa `waitpid(-1, WNOHANG)`) para
recolectar todos los hijos terminados.

#### Contador persistente

Tanto `gesfich` como `gesprog` guardan el último ID asignado en un
archivo `.gesfich_counter` / `.gesprog_counter` dentro de `aralmac`.
Esto garantiza que los IDs no se reutilicen incluso tras un reinicio
del servicio.

#### Error tipográfico en el enunciado (-c duplicado)

La sinopsis de `ctrllt` en el enunciado usa `-c` dos veces. Este
diseño usa `-q` para la tubería de respuesta de `gesprog` con el fin
de evitar la colisión.

---

## 11. Caso de uso ilustrativo

Escenario: ejecutar `cat` con un fichero de entrada y capturar la salida.

| Paso | Petición enviada a `ctrllt` | Respuesta |
|------|-----------------------------|-----------|
| 1 | `{"servicio":"gesfich","operacion":"Crear"}` | `{"estado":"ok","id-fichero":"f-0001"}` |
| 2 | `{"servicio":"gesfich","operacion":"Actualizar","id-fichero":"f-0001","ruta":"/etc/hosts"}` | `{"estado":"ok"}` |
| 3 | `{"servicio":"gesfich","operacion":"Crear"}` | `{"estado":"ok","id-fichero":"f-0002"}` |
| 4 | `{"servicio":"gesprog","operacion":"Guardar","ejecutable":"/bin/cat","args":[],"env":[]}` | `{"estado":"ok","id-programa":"p-0001"}` |
| 5 | `{"servicio":"ejecutor","operacion":"Ejecutar","id-programa":"p-0001","stdin":"f-0001","stdout":"f-0002"}` | `{"estado":"ok","id-ejecucion":"e-0001"}` |
| 6 | `{"servicio":"ejecutor","operacion":"Estado","id-ejecucion":"e-0001"}` | `{"estado":"ok","id-ejecucion":"e-0001","id-programa":"p-0001","proceso-estado":"Terminado","codigo-salida":0}` |
| 7 | `{"servicio":"gesfich","operacion":"Leer","id-fichero":"f-0002"}` | `{"estado":"ok","contenido":"<contenido de /etc/hosts>"}` |
| 8 | `{"servicio":"ctrllt","operacion":"Terminar"}` | `{"estado":"ok"}` |

---

## 12. Referencias

- Cardona Mc'Cormick, J. F. *Ejecutor lotes*, enunciado de la
  práctica ST0257-C2661-4677, Universidad EAFIT, Mayo de 2026.
- RFC 8259 — *The JavaScript Object Notation (JSON) Data Interchange
  Format*, IETF, 2017.
- cJSON — https://github.com/DaveGamble/cJSON
