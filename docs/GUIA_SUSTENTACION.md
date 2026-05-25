# GUÍA EXHAUSTIVA DE SUSTENTACIÓN ORAL
## Proyecto: Ejecutor de Lotes — ST0257 EAFIT
### Sustentación: 25 de Mayo de 2026

---

# ÍNDICE

1. [VISIÓN GENERAL DEL SISTEMA](#1-visión-general-del-sistema)
2. [IPC CON NAMED PIPES (FIFOs)](#2-ipc-con-named-pipes-fifos)
3. [PROTOCOLO DE MENSAJES](#3-protocolo-de-mensajes)
4. [GESFICH — Gestor de Ficheros](#4-gesfich--gestor-de-ficheros)
5. [GESPROG — Gestor de Programas](#5-gesprog--gestor-de-programas)
6. [EJECUTOR — Ejecutor de Procesos](#6-ejecutor--ejecutor-de-procesos)
7. [CTRLLT — Controlador/Pasarela](#7-ctrllt--controladorpasarela)
8. [SEÑALES Y CONCURRENCIA](#8-señales-y-concurrencia)
9. [ALMACENAMIENTO (aralmac)](#9-almacenamiento-aralmac)
10. [DECISIONES DE DISEÑO Y SUS JUSTIFICACIONES](#10-decisiones-de-diseño-y-sus-justificaciones)
11. [FLUJOS COMPLETOS PASO A PASO](#11-flujos-completos-paso-a-paso)
12. [PREGUNTAS DE SUSTENTACIÓN](#12-preguntas-de-sustentación)
13. [ERRORES COMUNES Y CÓMO DEFENDERLOS](#13-errores-comunes-y-cómo-defenderlos)

---

# 1. VISIÓN GENERAL DEL SISTEMA

## 1.1 Diagrama ASCII de Arquitectura

```
┌─────────────────────────────────────────────────────────────────────┐
│                         CLIENTE (CLI / script)                      │
│                     (lee/escribe JSON por FIFO)                     │
└───────────────────┬─────────────────────────────┬───────────────────┘
                    │ /tmp/ctrllt_req              │ /tmp/ctrllt_resp
                    ▼                             ▲
┌───────────────────────────────────────────────────────────────────┐
│                   CTRLLT  (Controlador/Pasarela)                  │
│   Parsea "servicio" del JSON y enruta al servicio correspondiente │
└──────┬──────────┬────────────────┬─────────────────┬─────────────┘
       │          │                │                 │
  /tmp/gesfich_req  /tmp/gesprog_req   /tmp/ejecutor_req
  /tmp/gesfich_resp /tmp/gesprog_resp  /tmp/ejecutor_resp
       ▼          ▼                ▼
┌──────────┐ ┌──────────┐  ┌────────────────────────────────────────┐
│ GESFICH  │ │ GESPROG  │  │              EJECUTOR                  │
│          │ │          │  │   fork/exec procesos del usuario       │
│aralmac/  │ │aralmac/  │  │   SIGCHLD → reap_children()           │
│ficheros/ │ │programas/│  │   g_procs[1024]                        │
│f-XXXX    │ │p-XXXX/   │  │                                        │
└──────────┘ └──────────┘  └────────────────────────────────────────┘
                                         │ fork/exec
                                         ▼
                               ┌─────────────────┐
                               │  Proceso hijo   │
                               │  (programa      │
                               │   del usuario)  │
                               └─────────────────┘
```

## 1.2 Los 4 Procesos del Sistema

| Proceso   | Función                                      | FIFO entrada            | FIFO salida              |
|-----------|----------------------------------------------|-------------------------|--------------------------|
| gesfich   | Almacena/recupera archivos de datos          | /tmp/gesfich_req        | /tmp/gesfich_resp        |
| gesprog   | Almacena/recupera programas ejecutables      | /tmp/gesprog_req        | /tmp/gesprog_resp        |
| ejecutor  | Lanza, suspende, mata procesos del usuario   | /tmp/ejecutor_req       | /tmp/ejecutor_resp       |
| ctrllt    | Pasarela: enruta cliente → servicio correcto | /tmp/ctrllt_req         | /tmp/ctrllt_resp         |

## 1.3 Flujo de Datos de una Petición Típica

```
Cliente                   ctrllt                  gesfich
  │                         │                        │
  │── JSON (op:Crear) ─────▶│                        │
  │   por ctrllt_req        │                        │
  │                         │── mismo JSON ─────────▶│
  │                         │   por gesfich_req      │
  │                         │                        │ (procesa, crea f-0001)
  │                         │◀── JSON respuesta ─────│
  │                         │   por gesfich_resp     │
  │◀── misma respuesta ─────│                        │
  │   por ctrllt_resp       │                        │
```

## 1.4 Secuencia de Arranque (run.sh)

```bash
rm -rf aralmac          # limpia estado previo
mkdir -p aralmac        # crea directorio base
./gesfich -f gesfich_req -b gesfich_resp -x aralmac &
sleep 0.2
./gesprog -p gesprog_req -c gesprog_resp -x aralmac &
sleep 0.2
./ejecutor -e ejecutor_req -d ejecutor_resp -x aralmac &
sleep 0.2
./ctrllt -c ctrllt_req -a ctrllt_resp \
         -f gesfich_req -b gesfich_resp \
         -p gesprog_req -q gesprog_resp \
         -e ejecutor_req -d ejecutor_resp &
```

Los `sleep 0.2` garantizan que cada servicio haya ejecutado `mkfifo` y abierto sus FIFOs antes de que el siguiente proceso intente abrirlos.

---

# 2. IPC CON NAMED PIPES (FIFOs)

## 2.1 ¿Qué es un FIFO (Named Pipe)?

Un FIFO es un archivo especial en el sistema de archivos que implementa comunicación IPC tipo pipe pero con nombre visible. A diferencia de los pipes anónimos (creados con `pipe(2)`), los FIFOs persisten en el filesystem y permiten que procesos sin relación padre-hijo se comuniquen.

Características clave:
- Semántica FIFO: el primer byte que entra es el primero que sale
- El kernel mantiene un buffer interno (típicamente 64 KB en Linux)
- **`open()` bloquea** hasta que haya un proceso abriendo el otro extremo (salvo O_NONBLOCK)
- Cuando todos los escritores cierran el FIFO, el lector ve EOF (read retorna 0)

## 2.2 Creación de FIFOs

```c
// En ctrllt y en cada servicio:
mkfifo(path, 0666);  // ignora error EEXIST
// Luego:
int fd = open(path, O_RDWR);
```

`mkfifo(2)` crea el archivo especial. Si ya existe (EEXIST), se ignora el error — esto es intencional para tolerancia a reinicios.

## 2.3 El Truco O_RDWR en FIFOs

### Por qué NO usar O_RDONLY / O_WRONLY

Si un proceso abre el FIFO con `O_RDONLY`, el `open()` **bloquea** hasta que otro proceso abra el mismo FIFO con `O_WRONLY`. Esto crea una dependencia de orden de arranque muy frágil.

Peor aún: si el escritor cierra su extremo (por ejemplo, entre peticiones), el lector en `read()` recibe **EOF inmediato** (retorna 0) aunque el FIFO siga existiendo. El servicio interpretaría esto como "fin de comunicación" y terminaría inadecuadamente.

### Por qué O_RDWR resuelve ambos problemas

```c
int fd = open(path, O_RDWR);
```

1. **No bloquea en open**: Como el mismo proceso es simultáneamente lector Y escritor, el kernel ve que hay al menos un extremo de lectura Y uno de escritura, así que `open()` retorna inmediatamente.

2. **Nunca recibe EOF**: El proceso tiene el FIFO abierto para escritura, por lo que nunca se cierra el extremo de escritura. `read()` nunca retorna 0 por "no hay escritores" — solo bloquea esperando datos.

3. **Simplicidad**: Un solo fd por FIFO en lugar de dos (uno read-only, uno write-only).

### Análisis detallado del comportamiento del kernel

El kernel POSIX mantiene contadores de `open_count_read` y `open_count_write` por FIFO. Con O_RDWR, el proceso incrementa **ambos** contadores. Cuando el cliente externo abre el FIFO con O_WRONLY para enviar una petición y luego lo cierra, `open_count_write` cae de 2 a 1 (el servicio aún tiene su O_RDWR). Por eso el lector nunca ve EOF.

## 2.4 FIFOs vs Otras Alternativas de IPC

| Mecanismo         | Pros                              | Contras                                      |
|-------------------|-----------------------------------|----------------------------------------------|
| Named Pipes (FIFO)| Simple, visible en filesystem, sin servidor dedicado | Unidireccional por canal, sin broadcast |
| Sockets Unix      | Bidireccional, conexiones múltiples | Más complejo (bind/listen/accept)           |
| Memoria compartida| Muy rápido                        | Requiere sincronización (semáforos/mutex)    |
| Colas de mensajes | Tipado, persistente               | Límite de tamaño de mensaje, API más compleja|
| Sockets TCP       | Distribuido, red                  | Overhead de red, complejidad                 |

**Justificación de FIFOs para este proyecto**: Los servicios son procesos independientes sin relación padre-hijo, la comunicación es estrictamente request-response (no broadcast), los mensajes son pequeños (JSON < 4 KB), y la visibilidad en el filesystem facilita debugging. FIFOs son la herramienta POSIX estándar exacta para este patrón.

---

# 3. PROTOCOLO DE MENSAJES

## 3.1 Diseño del Protocolo

El protocolo usa JSON delimitado por newline (`\n`). Cada mensaje es una línea JSON terminada en `\n`.

```
[JSON string...]\n
```

Ejemplo:
```
{"servicio":"gesfich","operacion":"Crear","contenido":"Hola mundo"}\n
```

## 3.2 Constante MSG_MAX_LEN = 4096

```c
#define MSG_MAX_LEN 4096
```

Esta constante define el tamaño máximo de un mensaje JSON incluyendo el terminador `\n`. Es 4096 bytes (4 KB) por varias razones:
- Es un múltiplo de la página de memoria del sistema (4 KB en x86/ARM)
- Es suficiente para metadatos JSON y contenidos pequeños de archivos
- Evita buffers dinámicos en el stack, reduciendo complejidad

## 3.3 Función msg_send — Análisis Línea por Línea

```c
int msg_send(int fd, const char *json_str) {
    size_t len = strlen(json_str);
    if (len + 1 > MSG_MAX_LEN) return -1;   // [1]
    
    char buf[MSG_MAX_LEN];                   // [2]
    memcpy(buf, json_str, len);              // [3]
    buf[len] = '\n';                         // [4]
    
    size_t total = len + 1;                  // [5]
    size_t sent  = 0;
    while (sent < total) {                   // [6]
        ssize_t n = write(fd, buf + sent, total - sent);
        if (n < 0) {
            if (errno == EINTR) continue;    // [7]
            return -1;
        }
        sent += (size_t)n;                  // [8]
    }
    return 0;
}
```

**Anotaciones:**
- `[1]`: `len + 1` porque el `\n` ocupa un byte extra. Si el JSON ya tiene 4095 bytes, agregar `\n` lo haría de 4096 bytes — el límite exacto. Si no cabe, retorna error sin escribir nada.
- `[2]`: Buffer en stack, no en heap. Rápido, sin malloc/free, sin fragmentación.
- `[3]`: Copia el JSON al buffer local para poder agregar `\n` contiguo.
- `[4]`: El delimitador de mensaje. `msg_recv` leerá hasta encontrar este byte.
- `[5]`: Total de bytes a escribir = JSON + newline.
- `[6]`: Bucle de escritura parcial. `write(2)` puede escribir menos bytes de los pedidos (especialmente en pipes si el buffer del kernel está casi lleno).
- `[7]`: Si `write` fue interrumpido por una señal (EINTR), reintentar. Esto es crítico cuando el proceso maneja señales (como SIGCHLD en ejecutor).
- `[8]`: Avanza el puntero de escritura en exactamente los bytes que write() confirmó haber escrito.

## 3.4 Función msg_recv — Análisis Línea por Línea

```c
ssize_t msg_recv(int fd, char *buf, size_t bufsz) {
    size_t pos = 0;
    while (pos < bufsz - 1) {                        // [1]
        ssize_t n = read(fd, buf + pos, 1);           // [2]
        if (n < 0) {
            if (errno == EINTR && pos == 0) return -1; // [3]
            if (errno == EINTR) continue;              // [4]
            return -1;
        }
        if (n == 0) {                                  // [5]
            buf[pos] = '\0';
            return (pos == 0) ? 0 : (ssize_t)pos;
        }
        if (buf[pos] == '\n') {                        // [6]
            buf[pos] = '\0';
            return (ssize_t)pos;
        }
        pos++;
    }
    buf[pos] = '\0';
    return (ssize_t)pos;                              // [7]
}
```

**Anotaciones:**
- `[1]`: Condición `bufsz - 1` deja espacio para el `\0` terminador de C-string.
- `[2]`: Lee **1 byte a la vez**. Ineficiente en throughput pero garantiza que no se consuma parte del siguiente mensaje. En FIFOs con mensajes pequeños de control, esto es aceptable.
- `[3]`: Si EINTR ocurre cuando aún no hemos leído ningún byte (pos==0), retorna -1 para que el llamador pueda verificar el flag de señal y actuar.
- `[4]`: Si EINTR ocurre en medio de un mensaje (pos>0), continuar leyendo para no perder datos parciales.
- `[5]`: EOF: el otro extremo cerró. Retorna 0 si no había datos, o los bytes leídos hasta ahora.
- `[6]`: El delimitador `\n` encontrado. Lo reemplaza con `\0` y retorna la longitud del mensaje (sin incluir el `\n`).
- `[7]`: Buffer lleno sin encontrar `\n` — mensaje truncado. Retorna lo que hay.

## 3.5 Formato de Mensajes JSON

### Petición del cliente:
```json
{
  "servicio": "gesfich",
  "operacion": "Crear",
  "contenido": "texto del archivo"
}
```

### Respuesta exitosa:
```json
{
  "estado": "ok",
  "id-fichero": "f-0001"
}
```

### Respuesta de error:
```json
{
  "estado": "error",
  "mensaje": "descripción del error"
}
```

## 3.6 Por qué JSON y no un protocolo binario

- **Legibilidad**: Facilita debugging con `cat /tmp/gesfich_req`
- **Flexibilidad**: Campos opcionales sin versionar el protocolo
- **Ecosystem**: cJSON es una biblioteca C ligera y bien testeada
- **Interoperabilidad**: Cualquier cliente (Python, bash, C) puede generar/parsear JSON

---

# 4. GESFICH — Gestor de Ficheros

## 4.1 Responsabilidad

Gesfich es el servicio de almacenamiento de **archivos de datos** (ficheros). No ejecuta código, solo almacena y recupera contenidos de archivos que serán usados como entrada/salida por los procesos del ejecutor.

## 4.2 Estructura de Almacenamiento

```
aralmac/
└── ficheros/
    ├── .gesfich_counter    ← número del último ID asignado
    ├── f-0001              ← contenido del fichero 1
    ├── f-0002              ← contenido del fichero 2
    └── f-XXXX              ← contenido del fichero N
```

## 4.3 Máquina de Estados

```
         ┌─────────────────┐
         │    CORRIENDO    │◄──────────────┐
         └────────┬────────┘               │
                  │ op:Suspender           │ op:Reasumir
                  ▼                        │
         ┌─────────────────┐               │
         │   SUSPENDIDO    │───────────────┘
         └────────┬────────┘
                  │ op:Terminar (desde cualquier estado)
                  ▼
         ┌─────────────────┐
         │    TERMINADO    │
         └─────────────────┘
```

**Operaciones permitidas por estado:**

| Operación   | Corriendo | Suspendido |
|-------------|-----------|------------|
| Crear       | ✓         | ✗          |
| Leer        | ✓         | ✗          |
| Actualizar  | ✓         | ✗          |
| Borrar      | ✓         | ✗          |
| Suspender   | ✓         | ✗          |
| Reasumir    | ✗         | ✓          |
| Terminar    | ✓         | ✓          |

## 4.4 Operaciones en Detalle

### op_crear

```c
static int next_id = 0;  // contador estático en memoria
// ...
if (next_id == 0) load_counter();  // lazy init: lee .gesfich_counter
next_id++;
// genera path "aralmac/ficheros/f-XXXX"
// open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644)
// write contenido
// save_counter(next_id)
// responde {"estado":"ok","id-fichero":"f-XXXX"}
```

**Por qué O_CREAT|O_TRUNC**: `O_CREAT` crea el archivo si no existe. `O_TRUNC` trunca a 0 bytes si ya existe (para Actualizar que también lo usa). Para Crear, el archivo no debería existir, pero si por alguna razón existiera (por ejemplo, reinicio parcial), se sobreescribe limpiamente.

**Por qué load_counter() lazy**: No se llama en la inicialización del proceso sino en la primera operación Crear. Esto evita fallos de inicialización si el directorio aún no existe al arrancar.

### op_leer con ID específico

```c
// stat(path) para verificar existencia → error si no existe
// stat.st_size > MSG_MAX_LEN/2 → error (el contenido podría no caber)
// fopen(path, "r")
// fread(buf, 1, sizeof(buf)-1, f)
// buf[got] = '\0'  ← importante: null-terminate
// responde {"estado":"ok","id-fichero":"f-XXXX","contenido":"..."}
```

**Por qué `MSG_MAX_LEN/2` y no `MSG_MAX_LEN`**: Cuando el contenido se serializa como JSON, caracteres especiales son escapados (ej: `"` → `\"`, `\n` → `\\n`, caracteres no-ASCII → `\uXXXX`). En el peor caso, cada byte se convierte en 6 caracteres (`\uXXXX`), sextuplicando el tamaño. El límite de `MSG_MAX_LEN/2` es un heurístico conservador que previene desbordamientos de buffer en la serialización JSON.

### op_leer sin ID (listar todos)

```c
// opendir("aralmac/ficheros")
// readdir loop
// filtra entradas que comienzan con "f-"
// construye array JSON con todos los IDs
// responde {"estado":"ok","ficheros":["f-0001","f-0002",...]}
```

**Por qué filtrar "f-"**: El directorio también contiene `.gesfich_counter` y las entradas especiales `.` y `..`. El prefijo "f-" garantiza que solo se listan ficheros de usuario reales.

### op_actualizar

```c
// verifica que dst (f-XXXX) existe con stat
// copia src (ruta del filesystem externo) → dst
// copy: fread/fwrite en chunks de 4096 bytes
```

**El paso "verificar existencia"** es fundamental: no se puede actualizar algo que no fue previamente creado. Retorna error claro en lugar de crear silenciosamente un nuevo fichero.

### op_borrar

```c
// stat(path) → error si no existe
// unlink(path) → elimina el archivo
// NO actualiza el contador (los IDs son permanentes, no se reutilizan)
```

**Por qué no reutilizar IDs**: Si se borra f-0001 y se crea uno nuevo con ID f-0001, podría confundir a clientes que guardaron referencias al ID original. Los IDs son únicos y permanentes.

## 4.5 Manejo del Bucle Principal

```c
while (state != ST_TERMINADO) {
    msg_recv(fd_in, buf, sizeof(buf));
    parse JSON → op
    
    if (op == "Suspender") { state = ST_SUSPENDIDO; send_ok; continue; }
    if (op == "Reasumir")  { state = ST_CORRIENDO;  send_ok; continue; }
    if (op == "Terminar")  { state = ST_TERMINADO;  send_ok; break; }
    
    if (state == ST_SUSPENDIDO) { send_error("servicio suspendido"); continue; }
    
    // operaciones normales (Crear, Leer, Actualizar, Borrar)
    dispatch(op);
}
```

**Orden crítico**: Suspender/Reasumir/Terminar se verifican **antes** del check de estado. Esto permite Terminar desde cualquier estado (incluyendo Suspendido) y Reasumir cuando está Suspendido.

## 4.6 send_json con verificación de longitud

```c
void send_json(int fd, const char *s) {
    if (strlen(s) + 1 > MSG_MAX_LEN) {
        // envía error corto: {"estado":"error","mensaje":"no se pudo actualizar el fichero"}
        msg_send(fd, SHORT_ERROR);
        return;
    }
    msg_send(fd, s);
}
```

Esta verificación extra protege contra respuestas JSON que la biblioteca cJSON construye y que podrían exceder MSG_MAX_LEN. Es una línea de defensa adicional.

## 4.7 init_aralmac

```c
void init_aralmac(const char *aralmac) {
    mkdir(aralmac, 0755);                           // crea aralmac/
    char path[512];
    snprintf(path, sizeof(path), "%s/ficheros", aralmac);
    mkdir(path, 0755);                              // crea aralmac/ficheros/
}
```

`mkdir` retorna -1 si el directorio ya existe (errno=EEXIST), lo cual se ignora. Esto hace que init_aralmac sea idempotente — se puede llamar múltiples veces sin error.

---

# 5. GESPROG — Gestor de Programas

## 5.1 Responsabilidad

Gesprog almacena **programas ejecutables** que el ejecutor podrá lanzar. Guarda una copia del binario más metadatos (nombre, argumentos por defecto, variables de entorno).

## 5.2 Estructura de Almacenamiento

```
aralmac/
└── programas/
    ├── .gesprog_counter    ← número del último ID asignado
    └── p-0001/
        ├── bin             ← copia del ejecutable (con bits +x)
        └── meta.json       ← metadatos del programa
```

### Formato de meta.json:

```json
{
  "id-programa": "p-0001",
  "nombre": "mi_programa",
  "ejecutable": "/ruta/original/mi_programa",
  "args": ["--verbose", "--output", "resultado.txt"],
  "env": ["PATH=/usr/bin", "HOME=/tmp"]
}
```

## 5.3 Máquina de Estados

```
         ┌─────────────────┐
         │    CORRIENDO    │◄──────────────┐
         └────────┬────────┘               │
                  │ op:Suspender           │ op:Reasumir
                  ▼                        │
         ┌─────────────────┐               │
         │   SUSPENDIDO    │───────────────┘
         └────────┬────────┘
                  │ op:Terminar
                  ▼
         ┌─────────────────┐
         │    TERMINADO    │
         └─────────────────┘
```

**DIFERENCIA CLAVE con gesfich**: `Leer` es válido en estado `SUSPENDIDO`.

**Operaciones permitidas por estado:**

| Operación   | Corriendo | Suspendido |
|-------------|-----------|------------|
| Guardar     | ✓         | ✗          |
| Leer        | ✓         | **✓**      |
| Actualizar  | ✓         | ✗          |
| Borrar      | ✓         | ✗          |
| Suspender   | ✓         | ✗          |
| Reasumir    | ✗         | ✓          |
| Terminar    | ✓         | ✓          |

## 5.4 Por qué Leer es válido en Suspendido

Esta es una decisión de diseño deliberada y justificada. Cuando gesprog está suspendido, el ejecutor puede necesitar leer los metadatos de un programa para lanzarlo (si el ejecutor recibe un comando Ejecutar). Bloquear Leer en Suspendido crearía un deadlock: ejecutor no puede ejecutar sin leer metadatos, pero gesprog está suspendido.

**Justificación formal (§3.10.3 del enunciado)**: La operación Leer es de solo lectura y no modifica el estado del almacenamiento. Suspender un servicio tiene el objetivo de pausar operaciones que modifican estado. Leer no modifica nada, por lo tanto es seguro permitirlo en Suspendido.

## 5.5 Operaciones en Detalle

### op_guardar

```c
// 1. stat(src_path) → verifica que el ejecutable origen existe
// 2. verifica bits de ejecución: (st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH)) != 0
// 3. Si no tiene execute bits, lee 2 bytes y verifica shebang "#!"
// 4. next_id++; mkdir("aralmac/programas/p-XXXX", 0755)
// 5. copy_file(src, "aralmac/programas/p-XXXX/bin")
// 6. escribe meta.json
// 7. save_counter()
// 8. responde {"estado":"ok","id-programa":"p-XXXX"}
```

**Verificación de ejecutabilidad**: Primero revisa los bits de ejecución POSIX (S_IXUSR etc.). Si no los tiene pero empieza con `#!` (shebang), se acepta como script. Si no tiene bits de ejecución NI shebang, se rechaza con error — no sería ejecutable.

**Por qué verificar el shebang**: Un script bash `#!/bin/bash` puede tener permisos `644` (sin execute bit) y aun así ser válido si se ejecuta con `bash script.sh`. El sistema lo acepta para ser flexible.

### copy_file con preservación de permisos

```c
void copy_file(const char *src, const char *dst) {
    struct stat st;
    stat(src, &st);
    // fopen rb/wb
    // fread/fwrite en chunks de 4096
    // chmod(dst, st.st_mode | 0111)  ← garantiza bits +x
}
```

`st.st_mode | 0111` garantiza que la copia tiene bits de ejecución para usuario, grupo y otros, incluso si el original solo tenía `u+x`. Esto es necesario porque el ejecutor usará `execv()` sobre este archivo.

### op_leer con ID — ocultamiento de "ejecutable"

```c
// Lee meta.json completo
cJSON *meta = cJSON_Parse(meta_json_content);
cJSON_DeleteItemFromObject(meta, "ejecutable");  // ← IMPORTANTE
// Serializa el meta sin el campo "ejecutable"
// Responde con el JSON sin ruta original
```

**Por qué se oculta "ejecutable"**: El enunciado §3.10.3 especifica que la respuesta a Leer debe contener solo: `id-programa`, `nombre`, `args`, `env`. La ruta del ejecutable original es un detalle de implementación interna (puede apuntar a un path que ya no existe si el archivo original fue movido/borrado). El ejecutor usa la copia en `bin`, no la ruta original.

### op_actualizar

```c
// 1. Verifica que p-XXXX existe (stat meta.json)
// 2. Verifica que nueva ruta ejecutable existe y es ejecutable (misma validación que Guardar)
// 3. copy_file(nueva_ruta, "p-XXXX/bin")  ← sobreescribe la copia
// 4. Lee meta.json, actualiza "ejecutable" y "nombre" con nueva ruta y basename
// 5. Reescribe meta.json
```

### op_borrar con rmdir_prog

```c
void rmdir_prog(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            unlink(path);  // elimina bin y meta.json
        }
    }
    closedir(d);
    rmdir(dir);  // elimina el directorio p-XXXX ahora vacío
}
```

`rmdir(2)` solo funciona en directorios vacíos. Por eso primero se eliminan todos los archivos dentro, luego el directorio.

## 5.6 Bucle Principal con Leer en Suspendido

```c
while (state != ST_TERMINADO) {
    msg_recv(fd_in, buf, sizeof(buf));
    parse JSON → op
    
    // Controles de estado siempre disponibles
    if (op == "Suspender") { state = ST_SUSPENDIDO; send_ok; continue; }
    if (op == "Reasumir")  { state = ST_CORRIENDO;  send_ok; continue; }
    if (op == "Terminar")  { state = ST_TERMINADO;  send_ok; break; }
    
    // LEER es válido incluso en Suspendido — verificar ANTES del check de estado
    if (op == "Leer") { op_leer(params); continue; }
    
    // Resto de operaciones bloqueadas en Suspendido
    if (state == ST_SUSPENDIDO) { send_error("servicio suspendido"); continue; }
    
    // Guardar, Actualizar, Borrar
    dispatch(op);
}
```

**El orden de los checks importa**: `Leer` se evalúa **antes** del `if (state == ST_SUSPENDIDO)`. Si se pusiera después, Leer también quedaría bloqueado en Suspendido, lo cual violaría el diseño.

---

# 6. EJECUTOR — Ejecutor de Procesos

## 6.1 Responsabilidad

El ejecutor es el componente más complejo del sistema. Lanza programas del usuario como procesos independientes usando `fork`/`exec`, gestiona su ciclo de vida (suspender, matar, esperar terminación), y reporta su estado.

## 6.2 Estructuras de Datos

```c
typedef enum {
    PROC_EJECUTANDO,
    PROC_SUSPENDIDO,
    PROC_TERMINADO
} proc_estado_t;

typedef struct {
    char id[16];          // "e-0001"
    char id_prog[16];     // "p-0001"
    pid_t pid;            // PID del proceso hijo
    proc_estado_t estado;
    int codigo_salida;    // código de retorno (WEXITSTATUS o 128+signal)
    int activo;           // 1 si el proceso existe, 0 si fue removido
} proceso_t;

proceso_t g_procs[MAX_PROCS];  // MAX_PROCS = 1024
int g_n_procs = 0;
int g_counter = 0;  // NO persiste en disco (diferencia con gesfich/gesprog)
```

**Por qué g_counter no persiste**: Los procesos son entidades volátiles que solo existen mientras el ejecutor está vivo. Al reiniciar el sistema, todos los procesos del usuario habrían terminado (o fueron matados). No tiene sentido persistir contadores de ejecución.

## 6.3 Máquina de Estados del SERVICIO

```
         ┌─────────────────┐
         │   SVC_EJECUTAR  │◄──────────────┐
         └────────┬────────┘               │
                  │ op:Suspender           │ op:Reasumir
                  ▼                        │
         ┌─────────────────┐               │
         │ SVC_SUSPENDIDOS │───────────────┘
         └────────┬────────┘
                  │ op:Parar (desde cualquier estado no-TERMINADO)
                  ▼
         ┌─────────────────┐
         │   SVC_PARAR     │
         └────────┬────────┘
                  │ (automático cuando count_active()==0)
                  ▼
         ┌─────────────────┐
         │ SVC_TERMINADO   │
         └─────────────────┘
```

**Diferencia con gesfich/gesprog**: Ejecutor tiene un estado extra `SVC_PARAR` que es un estado transitorio. En `SVC_PARAR`, el servicio ya no acepta nuevas operaciones pero espera a que los procesos activos terminen antes de finalizar.

## 6.4 Máquina de Estados de cada PROCESO

```
PROC_EJECUTANDO ──[op:Suspender/SIGSTOP]──► PROC_SUSPENDIDO
PROC_EJECUTANDO ──[SIGCHLD recibido]──────► PROC_TERMINADO
PROC_SUSPENDIDO ──[op:Reasumir/SIGCONT]──► PROC_EJECUTANDO
PROC_SUSPENDIDO ──[op:Parar/SIGCONT]─────► PROC_EJECUTANDO (para que termine)
PROC_EJECUTANDO ──[op:Matar/SIGKILL]─────► PROC_TERMINADO (vía SIGCHLD)
```

## 6.5 fork/exec — Análisis Detallado

### Antes del fork: preparación de redirección

```c
int fd_stdin  = -1, fd_stdout = -1, fd_stderr = -1;

if (archivo_entrada)
    fd_stdin  = open(archivo_entrada,  O_RDONLY);
if (archivo_salida)
    fd_stdout = open(archivo_salida, O_WRONLY|O_CREAT|O_TRUNC, 0644);
if (archivo_error)
    fd_stderr = open(archivo_error,  O_WRONLY|O_CREAT|O_TRUNC, 0644);
```

Los archivos se abren **en el proceso padre** antes del fork. ¿Por qué? Porque después del fork, el hijo puede necesitar abrir archivos que requieren permisos del usuario, y hacerlo en el padre es más seguro y eficiente.

### El fork()

```c
pid_t pid = fork();
if (pid < 0) {
    // error: no se pudo crear proceso
    send_error("fork failed");
    return;
}
```

`fork()` crea una copia exacta del proceso padre. El hijo tiene:
- Copia del espacio de direcciones (copy-on-write)
- Los mismos file descriptors abiertos (incluyendo los FIFOs del servicio)
- El mismo código, pero `fork()` retorna 0 en el hijo y el PID del hijo en el padre

### En el hijo: dup2 y cierre de FDs

```c
if (pid == 0) {
    // HIJO
    
    // Redirigir stdin/stdout/stderr
    if (fd_stdin >= 0) {
        dup2(fd_stdin, STDIN_FILENO);   // fd_stdin → fd 0
        close(fd_stdin);                // cierra original
    }
    if (fd_stdout >= 0) {
        dup2(fd_stdout, STDOUT_FILENO); // fd_stdout → fd 1
        close(fd_stdout);
    }
    if (fd_stderr >= 0) {
        dup2(fd_stderr, STDERR_FILENO); // fd_stderr → fd 2
        close(fd_stderr);
    }
    
    // CRÍTICO: cerrar FDs del servicio que el hijo heredó
    close(g_fd_in);    // cierra el FIFO de peticiones
    close(g_fd_out);   // cierra el FIFO de respuestas
    
    // Ejecutar el programa
    if (envp != NULL)
        execve(binpath, argv, envp);
    else
        execv(binpath, argv);
    
    // Si llegamos aquí, exec falló
    _exit(127);  // usar _exit, no exit (no llama atexit handlers)
}
```

### ¿Por qué dup2 y no solo asignar?

`dup2(oldfd, newfd)` duplica `oldfd` en la posición `newfd` de la tabla de FDs. Si `newfd` ya estaba abierto, lo cierra primero. Esto garantiza que el proceso ejecutado con `execv` tiene exactamente los FDs 0, 1, 2 apuntando a los archivos deseados.

No se puede "asignar" directamente porque los FDs son índices en una tabla del kernel — solo el kernel puede modificarlos a través de llamadas del sistema como `dup2`.

### ¿Por qué cerrar g_fd_in y g_fd_out en el hijo?

Los FIFOs del servicio son heredados por el hijo. Si el hijo los deja abiertos:
1. El FIFO de respuesta tendría dos escritores (padre e hijo), potencialmente mezclando respuestas
2. El FIFO de peticiones tendría dos lectores (padre e hijo), pudiendo el hijo "robar" peticiones del padre
3. Si el padre muere, el hijo mantendría el FIFO abierto, evitando que el sistema detecte la caída

### ¿Por qué _exit y no exit?

`exit(3)` llama a los handlers registrados con `atexit()` y hace flush de buffers de stdio. En un proceso hijo que nunca llamó a las funciones de inicialización, esto puede causar comportamiento inesperado (doble flush de buffers del padre copiados por fork). `_exit(2)` termina inmediatamente sin estos efectos secundarios.

### En el padre: registro del proceso

```c
// PADRE
close(fd_stdin);   // cierra FDs que ya no necesita el padre
close(fd_stdout);
close(fd_stderr);

cJSON_Delete(meta);  // libera memoria del meta.json parseado

// Registrar en la tabla
g_counter++;
g_procs[g_n_procs].pid = pid;
snprintf(g_procs[g_n_procs].id, 16, "e-%04d", g_counter);
strcpy(g_procs[g_n_procs].id_prog, id_prog);
g_procs[g_n_procs].estado = PROC_EJECUTANDO;
g_procs[g_n_procs].activo = 1;
g_n_procs++;

send_ok_con_id(g_procs[g_n_procs-1].id);
```

## 6.6 Operaciones Bloqueadas según Estado

| Operación   | SVC_EJECUTAR | SVC_SUSPENDIDOS | SVC_PARAR |
|-------------|--------------|-----------------|-----------|
| Ejecutar    | ✓            | ✗               | ✗         |
| Estado      | ✓            | ✗               | ✗         |
| Matar       | ✓            | ✗               | ✗         |
| Suspender   | ✓            | ✗               | ✗         |
| Reasumir    | ✗            | ✓               | ✗         |
| Parar       | ✓            | ✓               | ✗         |

**Por qué Estado y Matar están bloqueados en SVC_SUSPENDIDOS**: Cuando el servicio está suspendido, la semántica es que está "pausado" y no debería realizar operaciones activas sobre los procesos. Si se permitiera Matar en Suspendido, se podría matar un proceso que el servicio no puede reportar correctamente porque él mismo está suspendido.

**Por qué Parar es válido desde SVC_SUSPENDIDOS**: Esto es necesario para poder terminar limpiamente el sistema. Si no se pudiera Parar desde Suspendido, primero habría que Reasumir y luego Parar — dos operaciones cuando solo debería ser una. El enunciado explícitamente permite esta transición.

## 6.7 op_suspender

```c
void op_suspender() {
    for (int i = 0; i < g_n_procs; i++) {
        if (g_procs[i].activo && g_procs[i].estado == PROC_EJECUTANDO) {
            kill(g_procs[i].pid, SIGSTOP);
            g_procs[i].estado = PROC_SUSPENDIDO;
        }
    }
    g_estado = SVC_SUSPENDIDOS;
    send_ok();
}
```

`SIGSTOP` suspende el proceso sin posibilidad de ser ignorado (a diferencia de SIGTSTP). El proceso queda congelado hasta recibir SIGCONT.

## 6.8 op_reasumir

```c
void op_reasumir() {
    for (int i = 0; i < g_n_procs; i++) {
        if (g_procs[i].activo && g_procs[i].estado == PROC_SUSPENDIDO) {
            kill(g_procs[i].pid, SIGCONT);
            g_procs[i].estado = PROC_EJECUTANDO;
        }
    }
    g_estado = SVC_EJECUTAR;
    send_ok();
}
```

## 6.9 op_parar

```c
void op_parar() {
    // Primero: SIGCONT a los suspendidos para que puedan terminar
    for (int i = 0; i < g_n_procs; i++) {
        if (g_procs[i].activo && g_procs[i].estado == PROC_SUSPENDIDO) {
            kill(g_procs[i].pid, SIGCONT);
            g_procs[i].estado = PROC_EJECUTANDO;
        }
    }
    g_estado = SVC_PARAR;
    send_ok();
    // El bucle principal detectará SVC_PARAR y llamará wait_for_parar()
}
```

**Por qué SIGCONT antes de Parar**: Un proceso suspendido con SIGSTOP no puede terminar porque está congelado. Si se pone el sistema en modo Parar sin enviar SIGCONT primero, los procesos suspendidos nunca terminarían y el sistema quedaría en SVC_PARAR para siempre.

## 6.10 wait_for_parar — Polling Loop

```c
static void wait_for_parar(void) {
    while (count_active() > 0) {
        usleep(10000);  // 10 ms
        if (g_sigchld_flag) {
            reap_children();
            g_sigchld_flag = 0;
        }
    }
    g_estado = SVC_TERMINADO;
}
```

**Por qué polling**: El proceso necesita periódicamente verificar `g_sigchld_flag` para recolectar procesos terminados. No puede bloquearse en `wait()` sin hacer nada más, porque podría perderse el flag o no actualizar la tabla de procesos. El polling con `usleep(10ms)` es un compromiso razonable entre latencia (10 ms máximo de retraso en detectar terminación) y uso de CPU.

---

# 7. CTRLLT — Controlador/Pasarela

## 7.1 Responsabilidad

Ctrllt es la pasarela única de entrada al sistema. Los clientes solo conocen los FIFOs de ctrllt. Ctrllt enruta cada petición al servicio correcto y retorna la respuesta al cliente.

## 7.2 FIFOs de ctrllt

```
Lado cliente:
  /tmp/ctrllt_req   ← ctrllt lee peticiones de clientes
  /tmp/ctrllt_resp  ← ctrllt escribe respuestas a clientes

Lado servicios (ctrllt es el cliente de estos):
  /tmp/gesfich_req   ← ctrllt escribe, gesfich lee
  /tmp/gesfich_resp  ← gesfich escribe, ctrllt lee
  /tmp/gesprog_req   ← ctrllt escribe, gesprog lee
  /tmp/gesprog_resp  ← gesprog escribe, ctrllt lee
  /tmp/ejecutor_req  ← ctrllt escribe, ejecutor lee
  /tmp/ejecutor_resp ← ejecutor escribe, ctrllt lee
```

## 7.3 Máquina de Estados de ctrllt

```
CORRIENDO → (op:Terminar) → TERMINADO
```

Ctrllt es el más simple: solo tiene dos estados. La única operación especial que maneja directamente es Terminar dirigido a sí mismo.

## 7.4 Flujo de Enrutamiento

```c
// Bucle principal
while (running) {
    msg_recv(g_fd_cli_req, buf, sizeof(buf));
    
    cJSON *req = cJSON_Parse(buf);
    const char *servicio = cJSON_GetString(req, "servicio");
    
    if (strcmp(servicio, "ctrllt") == 0) {
        const char *op = cJSON_GetString(req, "operacion");
        if (strcmp(op, "Terminar") == 0) {
            do_terminar();  // propaga Terminar a todos los servicios
            send_ok(g_fd_cli_resp);
            running = 0;
        }
        // otras ops de ctrllt si existieran
    } else {
        // Seleccionar destino
        int fd_req, fd_resp;
        if (strcmp(servicio, "gesfich") == 0) {
            fd_req = g_fd_gf_req; fd_resp = g_fd_gf_resp;
        } else if (strcmp(servicio, "gesprog") == 0) {
            fd_req = g_fd_gp_req; fd_resp = g_fd_gp_resp;
        } else if (strcmp(servicio, "ejecutor") == 0) {
            fd_req = g_fd_ej_req; fd_resp = g_fd_ej_resp;
        }
        
        msg_send(fd_req, buf);    // reenvía el JSON original
        msg_recv(fd_resp, resp, sizeof(resp));
        msg_send(g_fd_cli_resp, resp);  // retorna respuesta al cliente
    }
    cJSON_Delete(req);
}
```

**Transparencia**: Ctrllt NO parsea ni modifica el contenido de las peticiones (excepto para leer el campo "servicio"). El JSON original se reenvía tal cual al servicio correspondiente.

## 7.5 do_terminar — Terminación en Cascada

```c
void do_terminar() {
    // Terminar gesfich
    msg_send(g_fd_gf_req, "{\"servicio\":\"gesfich\",\"operacion\":\"Terminar\"}");
    msg_recv(g_fd_gf_resp, resp, sizeof(resp));
    
    // Terminar gesprog
    msg_send(g_fd_gp_req, "{\"servicio\":\"gesprog\",\"operacion\":\"Terminar\"}");
    msg_recv(g_fd_gp_resp, resp, sizeof(resp));
    
    // Parar ejecutor (no Terminar — ejecutor usa Parar para esperar hijos)
    msg_send(g_fd_ej_req, "{\"servicio\":\"ejecutor\",\"operacion\":\"Parar\"}");
    msg_recv(g_fd_ej_resp, resp, sizeof(resp));
}
```

**Por qué "Parar" y no "Terminar" para ejecutor**: Ejecutor necesita tiempo para esperar que sus procesos hijos terminen (o ya habrán terminado). "Parar" indica esta intención. Si se enviara "Terminar" directamente, el ejecutor podría dejar procesos huérfanos.

## 7.6 open_fifo — Creación y Apertura

```c
int open_fifo(const char *path) {
    mkfifo(path, 0666);  // ignora EEXIST
    return open(path, O_RDWR);
}
```

Ctrllt crea y abre sus propios FIFOs de cliente (`ctrllt_req`, `ctrllt_resp`). Para los FIFOs de los servicios, estos ya fueron creados por los propios servicios (que arrancan primero). Ctrllt solo los abre con O_RDWR.

---

# 8. SEÑALES Y CONCURRENCIA

## 8.1 El Problema de SIGCHLD

Cuando un proceso hijo termina, el kernel envía SIGCHLD al proceso padre. Si el padre no llama a `wait()` o `waitpid()`, el hijo queda como **proceso zombie** — ocupa una entrada en la tabla de procesos del kernel aunque ya haya terminado.

En el ejecutor, puede haber hasta 1024 procesos hijos. Si no se recolectan correctamente, el sistema puede quedarse sin entradas en la tabla de procesos.

## 8.2 Diseño del Manejador de SIGCHLD

```c
static volatile sig_atomic_t g_sigchld_flag = 0;

static void sigchld_handler(int sig) {
    (void)sig;           // [1]
    g_sigchld_flag = 1;  // [2]
}
```

**Anotaciones:**
- `[1]`: Casting explícito a void para suprimir warning de "unused parameter". El parámetro `sig` existe por la firma estándar de signal handlers pero no se usa porque solo manejamos SIGCHLD.
- `[2]`: La ÚNICA operación segura en un signal handler es escribir en una variable `volatile sig_atomic_t`. No se puede llamar a funciones no reentrantes (malloc, printf, etc.) desde un signal handler.

## 8.3 Por qué NO usar SA_RESTART

```c
struct sigaction sa;
sa.sa_handler = sigchld_handler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;  // ← SIN SA_RESTART
sigaction(SIGCHLD, &sa, NULL);
```

**Con SA_RESTART**: Cuando SIGCHLD interrumpe una syscall bloqueante (como `read` en `msg_recv`), el kernel reinicia automáticamente la syscall. El proceso nunca vería el EINTR — pero tampoco tendría oportunidad de verificar `g_sigchld_flag` y recolectar los hijos.

**Sin SA_RESTART**: Cuando SIGCHLD llega mientras `msg_recv` está bloqueada en `read()`, la syscall retorna -1 con errno=EINTR. El código en el bucle principal detecta EINTR, salta a verificar `g_sigchld_flag`, llama `reap_children()`, y luego vuelve a `msg_recv`.

```c
ssize_t n = msg_recv(g_fd_in, buf, sizeof buf);
if (n < 0) {
    if (g_sigchld_flag) { reap_children(); g_sigchld_flag = 0; }
    continue;  // ← vuelve al inicio del bucle, intenta recv de nuevo
}
```

Este es el mecanismo que permite al ejecutor ser responsivo tanto a peticiones (en el FIFO) como a terminaciones de hijos (vía SIGCHLD), siendo single-threaded.

## 8.4 reap_children — Análisis

```c
static void reap_children(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {  // [1]
        for (int i = 0; i < g_n_procs; i++) {
            if (g_procs[i].activo && g_procs[i].pid == pid) {
                g_procs[i].estado = PROC_TERMINADO;
                g_procs[i].codigo_salida = WIFEXITED(status)   // [2]
                    ? WEXITSTATUS(status)
                    : 128 + WTERMSIG(status);                  // [3]
            }
        }
    }
}
```

**Anotaciones:**
- `[1]`: `waitpid(-1, ...)` espera a CUALQUIER hijo. `WNOHANG` retorna inmediatamente si ningún hijo ha terminado (retorna 0). El bucle `while` recolecta TODOS los hijos que hayan terminado, no solo uno — porque múltiples hijos pueden terminar entre dos llamadas a reap_children.
- `[2]`: `WIFEXITED(status)` verifica si el proceso terminó normalmente (con exit()). `WEXITSTATUS(status)` extrae el código de salida (0-255).
- `[3]`: Si el proceso fue matado por una señal, `WTERMSIG(status)` da el número de señal. La convención `128 + signal_number` es el estándar de shells POSIX (bash, sh) para indicar "terminado por señal N". Por ejemplo, SIGKILL=9 → código 137.

## 8.5 volatile sig_atomic_t — Por qué importa

```c
static volatile sig_atomic_t g_sigchld_flag = 0;
```

- **`volatile`**: Le dice al compilador que esta variable puede cambiar en cualquier momento (por el signal handler), por lo que no debe cachearla en un registro. Sin `volatile`, el compilador podría optimizar el bucle `while(g_sigchld_flag == 0)` a `while(true)` si cree que el valor nunca cambia en el bucle.

- **`sig_atomic_t`**: Es el único tipo garantizado por POSIX a ser leído/escrito atómicamente en un contexto de señal. En la mayoría de plataformas es `int`, pero el estándar garantiza que la asignación `g_sigchld_flag = 1` es atómica (no puede ser interrumpida a la mitad por otra señal).

## 8.6 Race Condition y cómo se evita

**¿Qué pasaría sin el flag?** Si el ejecutor llamara `waitpid` directamente desde el signal handler:
- El signal handler podría ser llamado en cualquier momento, incluyendo mientras el bucle principal está modificando `g_procs[]`
- Se tendría acceso concurrente a estructuras de datos sin protección
- Potencial race condition: el handler modifica `g_procs[i]` mientras el bucle principal lo lee

**Con el flag**: El signal handler solo hace `g_sigchld_flag = 1` (una operación atómica). La recolección real (`reap_children`) se hace en el contexto del bucle principal, que es single-threaded. No hay acceso concurrente a `g_procs`.

## 8.7 while(wait(NULL) > 0) al finalizar

```c
// Al final del bucle principal, antes de exit():
while (wait(NULL) > 0);
```

Esta línea recolecta cualquier hijo que pudiera haber terminado después de la última llamada a `reap_children()`. Garantiza que no quedan zombies cuando el ejecutor sale.

---

# 9. ALMACENAMIENTO (aralmac)

## 9.1 Estructura Completa de Directorios

```
aralmac/                           ← directorio raíz (pasado con -x)
├── ficheros/                      ← espacio de gesfich
│   ├── .gesfich_counter           ← "42\n" (texto, número del último ID)
│   ├── f-0001                     ← contenido del fichero 1
│   ├── f-0002                     ← contenido del fichero 2
│   └── ...
└── programas/                     ← espacio de gesprog
    ├── .gesprog_counter           ← "17\n" (texto, número del último ID)
    └── p-0001/
        ├── bin                    ← copia del ejecutable (chmod +x)
        └── meta.json             ← metadatos del programa
```

## 9.2 Contadores Persistentes

### Formato

Los contadores son archivos de texto plano con un número decimal seguido de `\n`:
```
42\n
```

### Por qué persistir los contadores

**Problema sin persistencia**: Si el servicio se reinicia después de haber creado 42 ficheros, el contador en memoria volvería a 0. El próximo fichero creado sería "f-0001", pero ya existe un "f-0001" del ciclo anterior. La operación Crear sobreescribiría silenciosamente el fichero existente, corrompiendo datos.

**Con persistencia**: Al reiniciar, `load_counter()` lee el archivo y sabe que el próximo ID es 43. Los ficheros existentes nunca se sobreescriben por reutilización de ID.

### Implementación

```c
void load_counter(void) {
    FILE *f = fopen(counter_path, "r");
    if (f) {
        fscanf(f, "%d", &next_id);
        fclose(f);
    }
    // Si no existe el archivo, next_id queda en 0 (primera ejecución)
}

void save_counter(int val) {
    FILE *f = fopen(counter_path, "w");
    if (f) {
        fprintf(f, "%d\n", val);
        fclose(f);
    }
}
```

### Cuándo se guarda

`save_counter()` se llama **después** de crear el fichero/programa exitosamente. Si la creación falla, el contador no se incrementa — no se desperdician IDs.

## 9.3 Aislamiento de Almacenamiento

Cada servicio tiene su propio subdirectorio bajo aralmac:
- gesfich → `aralmac/ficheros/`
- gesprog → `aralmac/programas/`

Esto previene colisiones de nombres y permite borrar el almacenamiento de un servicio sin afectar al otro.

## 9.4 run.sh: rm -rf aralmac al inicio

```bash
rm -rf aralmac
mkdir -p aralmac
```

Esto garantiza un estado limpio al arrancar el sistema. Los contadores se resetean, todos los ficheros y programas previos se eliminan. Es el comportamiento esperado para un sistema de batch por lotes — cada ejecución del sistema comienza de cero.

---

# 10. DECISIONES DE DISEÑO Y SUS JUSTIFICACIONES

## 10.1 Por qué O_RDWR en lugar de O_RDONLY/O_WRONLY

**Pregunta**: ¿Por qué abrir los FIFOs con O_RDWR si son unidireccionales?

**Respuesta técnica**: POSIX no garantiza que O_RDWR funcione en FIFOs, pero Linux sí lo soporta. Los beneficios son:

1. **No bloquea en open**: Con O_RDONLY, open() bloquea hasta que haya un escritor. Con O_RDWR, el proceso actúa como su propio escritor, así que open() retorna inmediatamente.

2. **No genera EOF falso**: Con O_RDONLY, si todos los escritores externos cierran el FIFO entre peticiones, read() retorna 0 (EOF). Con O_RDWR, el proceso mantiene el extremo de escritura abierto, así read() solo bloquea esperando datos — nunca retorna EOF falso.

3. **Código más simple**: Un FD por FIFO en lugar de dos.

## 10.2 Por qué polling en wait_for_parar

**Pregunta**: ¿Por qué usar polling con usleep(10ms) en lugar de un enfoque basado en eventos?

**Respuesta**: Las alternativas serían:
- `wait(NULL)` bloqueante: bloquearía el proceso, sin poder verificar `g_sigchld_flag`
- `sigsuspend()`: Suspende hasta que llegue una señal — posible pero más complejo
- `signalfd()`: Linux-específico, no portable

El polling con 10ms de intervalo es la solución más simple y correcta para el alcance del proyecto. La latencia de 10ms es aceptable (el proceso ya está en modo "parando"). La verificación de `g_sigchld_flag` dentro del bucle garantiza que los hijos terminados sean recolectados durante la espera.

## 10.3 Por qué Leer es válido en Suspendido para gesprog pero no gesfich

**Pregunta**: ¿Inconsistencia de diseño o decisión deliberada?

**Respuesta**: Decisión deliberada basada en el uso real:
- El **ejecutor** necesita leer metadatos de gesprog para ejecutar programas
- El ejecutor puede recibir una petición Ejecutar mientras gesprog está suspendido
- Si Leer estuviera bloqueado en gesprog suspendido, el ejecutor no podría lanzar el programa

Para gesfich, no hay ese requerimiento operativo. Los ficheros son solo almacenamiento de datos de entrada/salida — si gesfich está suspendido, esperamos a que se reanude antes de acceder a datos.

Además, Leer de gesprog es especialmente seguro en Suspendido porque solo lee el meta.json, que es inmutable durante la vida del programa registrado.

## 10.4 Por qué Parar desde Suspendido está permitido en ejecutor

**Pregunta**: ¿No debería el ejecutor necesitar reanudarse antes de poder Parar?

**Respuesta**: La transición Suspendido→Parar es necesaria para la terminación limpia del sistema:
- ctrllt envía Parar al ejecutor como parte de `do_terminar()`
- Si el ejecutor estuviera suspendido en ese momento, ctrllt no podría terminarlo
- El sistema quedaría en un deadlock: ctrllt esperando respuesta, ejecutor suspendido sin leer peticiones

La solución: el ejecutor en estado SVC_SUSPENDIDOS sigue procesando mensajes del FIFO (solo bloquea operaciones como Ejecutar/Matar). Cuando llega Parar, lo procesa correctamente.

**Implementación**: Parar se verifica antes del check de SVC_SUSPENDIDOS, igual que Reasumir y Terminar en los otros servicios.

## 10.5 Por qué leer byte a byte en msg_recv

**Pregunta**: ¿Por qué leer 1 byte a la vez? ¿No es muy ineficiente?

**Respuesta**: Sí, es ineficiente para mensajes grandes. Pero:
1. Los mensajes son pequeños (< 4 KB) y poco frecuentes (peticiones de usuario, no streaming de alta velocidad)
2. Leer múltiples bytes requeriría un buffer de prefetch complejo para no consumir parte del siguiente mensaje
3. La alternativa (leer en bloques y buscar `\n`) requiere manejar el caso de múltiples mensajes en un buffer
4. La simplicidad de "leer hasta encontrar `\n`" supera la penalidad de rendimiento para el alcance del proyecto

Una implementación producción usaría un buffer circular de lectura con `read()` de N bytes, buscando `\n` en el buffer.

## 10.6 Por qué no usar threads en lugar de fork

**Pregunta**: ¿Por qué fork/exec y no crear threads para ejecutar programas del usuario?

**Respuesta**: 
1. **Aislamiento**: Con fork, cada proceso tiene su propio espacio de direcciones. Un crash en el programa del usuario no afecta al ejecutor. Con threads, un crash mataría todo el proceso.
2. **Señales**: Threads complican enormemente el manejo de señales (SIGCHLD no existe para threads).
3. **exec**: No se puede hacer exec desde un thread para reemplazar el código — exec reemplaza todo el proceso.
4. **Requisito del proyecto**: El enunciado requiere fork/exec explícitamente.

## 10.7 Formato de IDs: por qué "f-XXXX", "p-XXXX", "e-XXXX"

- El prefijo (`f-`, `p-`, `e-`) identifica el tipo de entidad de un vistazo
- Previene confusión entre IDs de diferentes servicios
- `XXXX` es un número con 0-padding de 4 dígitos: soporta hasta 9999 entidades
- Los IDs son strings (no enteros) en JSON: consistente con el modelo REST

---

# 11. FLUJOS COMPLETOS PASO A PASO

## 11.1 Flujo: Guardar un programa y ejecutarlo

```
1. CLIENTE envía a ctrllt_req:
   {"servicio":"gesprog","operacion":"Guardar",
    "ejecutable":"/usr/bin/python3",
    "args":["-c","print('hola')"],"env":[]}

2. CTRLLT recibe, lee "servicio"="gesprog"
   CTRLLT reenvía mismo JSON a gesprog_req

3. GESPROG recibe la petición:
   - stat("/usr/bin/python3") → existe
   - verifica S_IXUSR bit → tiene execute permission
   - g_counter = 1; mkdir("aralmac/programas/p-0001")
   - copy_file("/usr/bin/python3", "aralmac/programas/p-0001/bin")
   - chmod("aralmac/programas/p-0001/bin", mode | 0111)
   - escribe meta.json con id-programa="p-0001", nombre="python3", etc.
   - save_counter(1)
   - responde a gesprog_resp: {"estado":"ok","id-programa":"p-0001"}

4. CTRLLT lee respuesta de gesprog_resp
   CTRLLT reenvía respuesta a ctrllt_resp

5. CLIENTE recibe: {"estado":"ok","id-programa":"p-0001"}

--- (más tarde) ---

6. CLIENTE envía a ctrllt_req:
   {"servicio":"ejecutor","operacion":"Ejecutar",
    "id-programa":"p-0001",
    "stdin":"","stdout":"salida.txt","stderr":""}

7. CTRLLT reenvía a ejecutor_req

8. EJECUTOR recibe:
   - Lee meta.json de "aralmac/programas/p-0001/meta.json"
   - binpath = "aralmac/programas/p-0001/bin"
   - argv = ["aralmac/programas/p-0001/bin", "-c", "print('hola')"]
   - envp = NULL (env vacío)
   - abre fd_stdout = open("salida.txt", O_WRONLY|O_CREAT|O_TRUNC)
   - fork()
   
   [HIJO]:
   - dup2(fd_stdout, STDOUT_FILENO)  → stdout → salida.txt
   - close(fd_stdout)
   - close(g_fd_in)  → cierra ejecutor_req
   - close(g_fd_out) → cierra ejecutor_resp
   - execv("aralmac/programas/p-0001/bin", argv)
   → Python ejecuta print('hola'), escribe "hola\n" a salida.txt, sale con código 0
   
   [PADRE]:
   - close(fd_stdout)
   - registra: g_procs[0] = {id="e-0001", pid=HIJO_PID, estado=PROC_EJECUTANDO}
   - responde: {"estado":"ok","id-ejecucion":"e-0001"}

9. CTRLLT retorna respuesta a cliente

10. (Momento después) SIGCHLD llega al EJECUTOR:
    - g_sigchld_flag = 1
    - Siguiente iteración del bucle: reap_children()
    - waitpid(-1, &status, WNOHANG) → retorna HIJO_PID, status=0
    - g_procs[0].estado = PROC_TERMINADO; g_procs[0].codigo_salida = 0
```

## 11.2 Flujo: Suspender y Reasumir el sistema completo

```
1. CLIENTE envía: {"servicio":"ejecutor","operacion":"Suspender"}
   CTRLLT → ejecutor_req
   EJECUTOR: SIGSTOP a todos los PROC_EJECUTANDO; g_estado=SVC_SUSPENDIDOS
   EJECUTOR responde: {"estado":"ok"}
   (todos los procesos del usuario congelados)

2. CLIENTE envía: {"servicio":"gesfich","operacion":"Suspender"}
   CTRLLT → gesfich_req
   GESFICH: state=ST_SUSPENDIDO
   Responde: {"estado":"ok"}

3. CLIENTE envía: {"servicio":"gesprog","operacion":"Suspender"}
   CTRLLT → gesprog_req
   GESPROG: state=ST_SUSPENDIDO
   Responde: {"estado":"ok"}

--- Sistema suspendido ---

4. CLIENTE envía: {"servicio":"gesprog","operacion":"Leer"}
   CTRLLT → gesprog_req
   GESPROG: estado suspendido pero Leer es válido → op_leer()
   Responde con lista de programas ✓

5. CLIENTE envía: {"servicio":"gesfich","operacion":"Crear",...}
   CTRLLT → gesfich_req
   GESFICH: estado suspendido, Crear no es válido
   Responde: {"estado":"error","mensaje":"servicio suspendido"}

6. CLIENTE envía: {"servicio":"ejecutor","operacion":"Reasumir"}
   CTRLLT → ejecutor_req
   EJECUTOR: SIGCONT a todos PROC_SUSPENDIDO; g_estado=SVC_EJECUTAR
   Procesos del usuario se reanudan
   Responde: {"estado":"ok"}
```

## 11.3 Flujo: Terminación del sistema

```
1. CLIENTE envía: {"servicio":"ctrllt","operacion":"Terminar"}
   CTRLLT parsea: servicio="ctrllt", op="Terminar"
   CTRLLT llama do_terminar():
   
   a. msg_send(gf_req, {"servicio":"gesfich","operacion":"Terminar"})
      GESFICH recibe → state=ST_TERMINADO → sale del bucle → termina
      msg_recv(gf_resp) → {"estado":"ok"}
   
   b. msg_send(gp_req, {"servicio":"gesprog","operacion":"Terminar"})
      GESPROG recibe → state=ST_TERMINADO → sale del bucle → termina
      msg_recv(gp_resp) → {"estado":"ok"}
   
   c. msg_send(ej_req, {"servicio":"ejecutor","operacion":"Parar"})
      EJECUTOR recibe → op_parar():
        - SIGCONT a todos PROC_SUSPENDIDO
        - g_estado = SVC_PARAR
        - Responde: {"estado":"ok"}
      EJECUTOR bucle: detecta SVC_PARAR → wait_for_parar()
        - polling hasta count_active()==0
        - reap_children en cada ciclo
        - g_estado = SVC_TERMINADO → break
      EJECUTOR: while(wait(NULL)>0) → limpia zombies
      EJECUTOR sale
   
   do_terminar() retorna
   CTRLLT responde al cliente: {"estado":"ok"}
   CTRLLT: running=0 → sale del bucle → termina

2. run.sh: kill a todos los PIDs (por si alguno no terminó)
   run.sh: rm FIFOs
```

---

# 12. PREGUNTAS DE SUSTENTACIÓN

## BLOQUE A: Arquitectura y IPC

**P1: ¿Por qué usar named pipes (FIFOs) y no sockets Unix?**

Los FIFOs son la elección correcta para este patrón de comunicación por varios motivos. Primero, la comunicación es estrictamente unidireccional por canal y sigue un patrón request-response simple, que los FIFOs modelan naturalmente. Segundo, los FIFOs son visibles como archivos en /tmp, lo que facilita enormemente el debugging — se puede hacer `cat /tmp/ejecutor_req` para ver qué peticiones llegan. Tercero, no requieren un servidor dedicado con `listen/accept`; simplemente se abren y se leen/escriben. Los sockets Unix serían más apropiados si necesitáramos conexiones múltiples simultáneas o comunicación bidireccional en un solo FD, pero para un sistema request-response con un cliente a la vez, los FIFOs son más simples y suficientes.

---

**P2: Explica en detalle el truco O_RDWR en FIFOs. ¿Por qué no usar O_RDONLY?**

Un FIFO abierto con O_RDONLY tiene dos problemas críticos. Primero, el `open()` bloquea hasta que otro proceso abra el mismo FIFO con O_WRONLY — esto crea una dependencia de orden de arranque que puede causar deadlocks si los procesos arrancan en el orden incorrecto. Segundo, y más importante: si todos los procesos que tienen el FIFO abierto para escritura cierran su extremo (por ejemplo, entre peticiones), el proceso lector recibe EOF (`read()` retorna 0) aunque el FIFO siga existiendo. Esto haría que el servicio interpretara EOF como "fin de comunicación" y terminara.

Con O_RDWR, el proceso mismo es simultáneamente lector Y escritor. Esto significa: (a) `open()` retorna inmediatamente sin esperar ningún otro proceso, y (b) el proceso siempre tiene el extremo de escritura abierto, así que nunca recibirá EOF espurio — `read()` solo bloquea esperando datos reales. El protocolo POSIX técnicamente dice que O_RDWR en FIFOs tiene comportamiento no especificado, pero en Linux está bien definido y documentado en el manual de pipe(7).

---

**P3: ¿Cuántos FIFOs hay en el sistema y para qué sirve cada uno?**

Hay 8 FIFOs en total, organizados en 4 pares (req/resp):
- `/tmp/ctrllt_req` y `/tmp/ctrllt_resp`: comunicación entre clientes externos y el controlador ctrllt
- `/tmp/gesfich_req` y `/tmp/gesfich_resp`: ctrllt envía peticiones a gesfich, gesfich responde
- `/tmp/gesprog_req` y `/tmp/gesprog_resp`: ctrllt envía peticiones a gesprog, gesprog responde
- `/tmp/ejecutor_req` y `/tmp/ejecutor_resp`: ctrllt envía peticiones a ejecutor, ejecutor responde

Cada servicio abre sus FIFOs de req con O_RDWR para recibir (leer) peticiones, y sus FIFOs de resp con O_RDWR para enviar (escribir) respuestas. Ctrllt tiene 8 FDs abiertos: 2 para el cliente y 6 para los tres servicios.

---

**P4: ¿Qué pasaría si dos clientes enviaran peticiones simultáneamente a ctrllt?**

Este es un escenario problemático que el diseño actual no maneja completamente. Ctrllt es single-threaded y procesa peticiones secuencialmente: recibe una petición, la reenvía, espera la respuesta, responde al cliente. Si dos clientes escriben simultáneamente en `ctrllt_req`, sus mensajes JSON podrían entrelazarse en el buffer del FIFO. Si los mensajes se "rompen" a nivel de bytes, msg_recv leería datos corruptos (un fragmento del mensaje de A mezclado con un fragmento del mensaje de B).

En la práctica, si los mensajes son suficientemente pequeños y se escriben con una sola llamada a `write()`, el kernel de Linux garantiza atomicidad para escrituras menores a PIPE_BUF (4096 bytes en Linux). Como nuestros mensajes son de máximo MSG_MAX_LEN=4096 bytes, y msg_send usa un bucle de escritura, teóricamente podría haber entrelazamiento. El sistema está diseñado para un cliente a la vez (arquitectura de lotes).

---

## BLOQUE B: Protocolo de Mensajes

**P5: ¿Por qué MSG_MAX_LEN=4096 y no más grande?**

4096 bytes es un número que tiene múltiples justificaciones. Es igual al tamaño de una página de memoria en sistemas x86/ARM, lo que significa que el buffer encaja exactamente en una o dos páginas físicas sin fragmentación. Es suficiente para cualquier mensaje de control JSON (que contiene IDs cortos, nombres de operaciones, y metadatos pequeños). También es igual a PIPE_BUF en Linux, lo que implica que la mayoría de los mensajes son lo suficientemente pequeños como para beneficiarse de las garantías de atomicidad del kernel en pipes. Aumentar el límite requeriría buffers más grandes en stack y mayor memoria por proceso, sin beneficio real dado el tipo de datos que maneja el sistema (metadatos e IDs, no contenidos de archivos grandes).

---

**P6: ¿Por qué msg_recv lee byte a byte y no en bloques?**

La razón fundamental es que los FIFOs son un stream de bytes sin delimitadores de mensaje a nivel del kernel. Si msg_recv leyera en bloques de, digamos, 512 bytes, podría leer el final de un mensaje Y el principio del siguiente en la misma llamada. Entonces necesitaríamos un buffer de "lookahead" para almacenar los bytes excedentes y devolverlos en la siguiente llamada a msg_recv — esto es substancialmente más complejo. Al leer byte a byte y parar al encontrar `\n`, nunca consumimos más de lo necesario. La penalidad de rendimiento (muchas syscalls read) es aceptable porque los mensajes son pequeños (< 4 KB) y poco frecuentes (peticiones de usuario, no streaming de video).

---

**P7: ¿Por qué msg_send tiene un bucle de escritura?**

`write(2)` puede retornar con menos bytes escritos de los solicitados. Esto sucede en varias circunstancias: si el buffer del kernel en el FIFO está casi lleno, write puede aceptar solo los bytes que quepan; si write es interrumpido por una señal (EINTR), puede haber escrito 0 bytes; en teoría, write puede retornar cualquier cantidad entre 1 y el solicitado. Sin el bucle, se perderían bytes y el receptor recibiría mensajes incompletos o corruptos. El bucle garantiza que TODOS los bytes del mensaje (incluyendo el `\n` final) son escritos antes de retornar, manteniendo la integridad del protocolo.

---

**P8: ¿Qué significa que msg_recv retorna 0 vs retornar -1?**

Retornar 0 significa EOF real: el otro extremo del FIFO cerró todos sus descriptores de escritura y no quedaron más datos por leer. En el contexto del sistema, esto significaría que ctrllt terminó sin enviar el `\n` final, o que el FIFO fue eliminado. El servicio debería interpretar esto como señal de terminación y salir limpiamente. Retornar -1 significa error: errno indicará la causa (EINTR si fue interrumpido por una señal, EBADF si el FD es inválido, etc.). En el ejecutor, un retorno de -1 con EINTR es la señal de que hay hijos esperando ser recolectados. En gesfich y gesprog, un error irrecuperable en msg_recv debería hacer que el servicio termine.

---

## BLOQUE C: SIGCHLD y Concurrencia

**P9: ¿Por qué no llamar waitpid directamente desde el signal handler?**

Los signal handlers deben ser funciones asíncronamente seguras (async-signal-safe). Las funciones async-signal-safe son un subconjunto muy limitado de las syscalls y funciones de biblioteca. `waitpid` es async-signal-safe, pero acceder a la estructura `g_procs[]` (buscar el PID terminado y actualizar su estado) implica leer y escribir múltiples campos de una struct. Si el signal handler hace esto mientras el bucle principal también está leyendo `g_procs[]` (por ejemplo, en `count_active()`), hay una race condition: los datos podrían estar en un estado inconsistente a medio actualizar.

La solución del flag es el patrón estándar: el handler solo hace la operación mínima y atómica (`g_sigchld_flag = 1`), y la lógica real se ejecuta en el contexto del bucle principal, que es single-threaded y no tiene problemas de concurrencia.

---

**P10: ¿Qué pasa si llegan múltiples SIGCHLD mientras el proceso está en reap_children?**

Linux (y POSIX en general) no garantiza que se entregue una señal por cada evento. Si 5 procesos terminan "simultáneamente" (en el mismo quantum de CPU), el proceso podría recibir solo 1 o 2 SIGCHLD. Por eso reap_children usa un bucle `while((pid = waitpid(-1, &status, WNOHANG)) > 0)`. En cada llamada a reap_children, el bucle continúa llamando a waitpid hasta que retorna 0 (no hay más hijos terminados) o -1 (error). Esto garantiza que en una sola invocación de reap_children se recolectan TODOS los hijos terminados disponibles, no solo uno.

Si llega SIGCHLD mientras estamos en reap_children, el flag se pone en 1 de nuevo. En la próxima iteración del bucle principal, se detectará y se llamará reap_children de nuevo (aunque probablemente waitpid retorne 0 inmediatamente si ya los recolectamos todos).

---

**P11: ¿Por qué 128 + WTERMSIG(status) para el código de salida cuando el proceso muere por señal?**

Esta es la convención estándar de shells POSIX. Cuando un proceso es terminado por una señal N, su "código de salida virtual" es 128+N. Los valores 0-127 son reservados para códigos de salida normales (exit codes), y 128 está reservado para indicar "terminado por señal". Esta convención permite al observador determinar si el proceso salió normalmente (código 0-127) o fue matado por una señal (código >= 129 → señal = código - 128). Por ejemplo: SIGKILL=9 → 137, SIGSEGV=11 → 139, SIGTERM=15 → 143.

---

**P12: ¿Qué hace dup2 exactamente y por qué no simplemente usar el FD del archivo como stdin?**

`dup2(oldfd, newfd)` duplica el file descriptor `oldfd` en la posición `newfd` de la tabla de file descriptors del proceso. Si `newfd` ya estaba abierto, lo cierra primero. Después de `dup2(fd_entrada, STDIN_FILENO)`, tanto `fd_entrada` como STDIN_FILENO (0) apuntan al mismo archivo. Cuando el proceso llama a `read(0, ...)` (stdin), en realidad lee del archivo de entrada.

No se puede simplemente "usar el FD del archivo" porque los programas del usuario (Python, bash, compilados en C) leen de stdin usando la convención de que stdin es FD 0. No saben nada sobre el FD que nosotros abrimos. La única forma de hacer que su stdin venga de nuestro archivo es mover ese archivo al FD 0, lo cual es exactamente lo que hace dup2.

---

**P13: ¿Por qué el hijo debe cerrar g_fd_in y g_fd_out?**

Al hacer fork(), el hijo hereda todos los file descriptors del padre, incluyendo los FIFOs del ejecutor (g_fd_in = ejecutor_req, g_fd_out = ejecutor_resp). Si el hijo no los cierra:

1. **FIFO de respuesta (g_fd_out)**: El hijo y el padre comparten el extremo de escritura de ejecutor_resp. Si el hijo (programa del usuario) escribe accidentalmente a g_fd_out, el cliente recibiría datos corruptos. Si el padre muere y el hijo sigue vivo, el FIFO sigue abierto y ctrllt no sabría que el ejecutor terminó.

2. **FIFO de petición (g_fd_in)**: El hijo podría leer peticiones destinadas al padre, "robando" mensajes del ejecutor.

3. **FD leak**: El hijo no necesita estos FDs para nada. Es buena práctica cerrar todo lo que no se necesita.

4. **Tras exec**: Los FDs abiertos sobreviven a exec (a menos que tengan la flag FD_CLOEXEC). El programa del usuario (Python, bash) no debería tener acceso a los FDs del sistema del ejecutor.

---

## BLOQUE D: Servicios en Detalle

**P14: ¿Por qué gesfich verifica que el tamaño del fichero sea < MSG_MAX_LEN/2 antes de leerlo?**

Cuando el contenido del fichero se incluye en la respuesta JSON, la biblioteca cJSON lo serializa como un string JSON. La serialización JSON puede expandir el contenido: cada carácter `"` se convierte en `\"` (2 caracteres), cada `\n` se convierte en `\\n` (2 caracteres), y caracteres no-ASCII se codifican como `\uXXXX` (6 caracteres). En el peor caso teórico, cada byte del contenido original produce 6 bytes en JSON, pero en la práctica el factor de expansión es mucho menor. Sin embargo, para ser seguros contra desbordamientos de buffer en la serialización, se usa el factor conservador de 2 (MSG_MAX_LEN/2). Si el archivo tiene más de 2048 bytes, no intentamos incluirlo en la respuesta y enviamos un error apropiado. Una implementación más robusta calcularía el tamaño real del JSON serializado.

---

**P15: ¿Por qué gesprog guarda una COPIA del ejecutable en lugar de guardar solo la ruta?**

Hay varias razones críticas. Primera: el archivo original podría ser movido, renombrado o eliminado después de registrarlo. Si solo guardáramos la ruta y el archivo desaparece, el ejecutor fallaría al intentar lanzarlo — un error en tiempo de ejecución difícil de diagnosticar. Segunda: si el archivo original es modificado (por ejemplo, el usuario actualiza su programa), el sistema de lotes debería ejecutar la versión que se registró, no una versión posterior. Tercera: al guardar la copia con `chmod +0111`, garantizamos los bits de ejecución independientemente de los permisos del archivo original. Cuarta: centralizar todas las copias bajo aralmac/programas/ simplifica la gestión y respaldo del estado del sistema.

---

**P16: ¿Por qué gesprog oculta el campo "ejecutable" al responder a Leer?**

El campo "ejecutable" en meta.json contiene la ruta del archivo original en el filesystem del usuario. Esta ruta es un detalle de implementación interna: puede apuntar a una ubicación que ya no existe (si el usuario movió su archivo después de registrarlo), o puede revelar la estructura de directorios del usuario. El enunciado §3.10.3 especifica exactamente qué campos debe retornar Leer: `id-programa`, `nombre`, `args`, `env`. El ejecutor (que es quien usa esta información) no necesita saber la ruta original — usa la copia en `aralmac/programas/p-XXXX/bin` directamente. Ocultar "ejecutable" cumple el principio de mínima exposición de información y la especificación del enunciado.

---

**P17: ¿Cómo funciona la verificación del shebang en gesprog?**

```c
// Si el archivo no tiene bits de ejecución (S_IXUSR|S_IXGRP|S_IXOTH):
FILE *f = fopen(path, "rb");
char magic[2];
size_t n = fread(magic, 1, 2, f);
fclose(f);
if (n == 2 && magic[0] == '#' && magic[1] == '!')
    // aceptar como script
else
    // rechazar: no es ejecutable
```

Un shebang `#!` en los primeros 2 bytes indica que el archivo es un script que será interpretado. El kernel Linux, al hacer execve() sobre un archivo que empieza con `#!`, automáticamente lee el resto de la primera línea para encontrar el intérprete (por ejemplo `#!/bin/bash`) y lanza el intérprete pasando el script como argumento. Por lo tanto, un archivo con shebang es ejecutable aunque sus bits de permiso no lo sean — siempre que el proceso que llama execve tenga permiso de lectura del archivo. La verificación permite registrar scripts sin bits de ejecución explícitos.

---

**P18: Explica la diferencia entre execv y execve y cuándo se usa cada uno en el ejecutor.**

La familia exec tiene múltiples variantes que difieren en cómo pasan argumentos y entorno:
- `execv(path, argv)`: Ejecuta `path` con argumentos `argv`. El proceso hijo hereda el entorno del padre (la variable global `environ`).
- `execve(path, argv, envp)`: Ejecuta `path` con argumentos `argv` y entorno explícito `envp`. El proceso hijo tiene EXACTAMENTE el entorno especificado en `envp`.

En el ejecutor, si el meta.json del programa tiene un array `env` no vacío, se construye `envp[]` con esas variables y se llama `execve`. Si `env` está vacío, `envp` es NULL y se llama `execv` (para que el hijo herede el entorno del padre). Esto da flexibilidad: programas con entorno personalizado o programas que simplemente heredan el entorno del sistema.

---

## BLOQUE E: Flujos y Transiciones de Estado

**P19: ¿Qué pasa si se envía Parar al ejecutor cuando todos los procesos ya terminaron?**

Si `count_active()` retorna 0 al entrar en el bucle de `SVC_PARAR`, la función `wait_for_parar()` verifica la condición inicialmente y sale inmediatamente del while. `g_estado` se pone en `SVC_TERMINADO` y el bucle principal hace `break`. No hay espera innecesaria. La secuencia es:

1. op_parar(): SIGCONT a suspendidos (ninguno en este caso), g_estado=SVC_PARAR, send_ok
2. Bucle principal: detecta SVC_PARAR → `if (count_active() == 0) { g_estado = SVC_TERMINADO; break; }` → termina directamente

Esto hace que Parar sea O(1) cuando no hay procesos activos, en lugar de esperar innecesariamente.

---

**P20: ¿Por qué ctrllt envía primero Terminar a gesfich/gesprog y luego Parar a ejecutor, y no al revés?**

El orden es importante por semántica de limpieza. Gesfich y gesprog son servicios de almacenamiento — no tienen procesos hijos que esperar. Cuando reciben Terminar, pueden salir inmediatamente. El ejecutor, en cambio, necesita tiempo para que sus procesos hijos terminen. Si enviáramos Parar al ejecutor primero y esperáramos (wait_for_parar), gesfich y gesprog seguirían vivos consumiendo recursos durante esa espera, sin ningún beneficio. Al terminar primero los servicios de almacenamiento (que terminan instantáneamente) y luego el ejecutor (que puede tardar en esperar hijos), el sistema se limpia de manera más eficiente. Además, una vez que el ejecutor está en SVC_PARAR, ya no acepta más peticiones de Ejecutar que podrían necesitar acceder a gesfich o gesprog.

---

**P21: ¿Qué sucede si un proceso hijo del ejecutor hace fork() y genera nietos?**

Este es un caso límite interesante. Si el programa del usuario (hijo) hace fork() internamente y crea nietos, el ejecutor no tiene registro de esos nietos — solo tiene el PID del hijo directo. Los nietos son hijos del hijo, no del ejecutor. Cuando el hijo termina, sus nietos (si los hay) se convierten en huérfanos y son adoptados por el proceso init (PID 1), que se encarga de recolectarlos con wait(). El ejecutor solo verá la terminación del hijo directo vía SIGCHLD y actualizará su tabla de procesos correctamente. Los nietos corren de forma independiente. El sistema no los rastrea ni puede suspenderlos/matarlos directamente — una limitación de diseño aceptable para el alcance del proyecto.

---

**P22: ¿Qué pasa si el ejecutable en bin está corrupto o tiene permisos incorrectos cuando se intenta ejecutar?**

Si execv falla, el proceso hijo llama a `_exit(127)`. El código 127 es la convención POSIX para "command not found" o "cannot execute" — el mismo código que usa el shell cuando no puede ejecutar un comando. El padre no sabe inmediatamente que execv falló porque el hijo sale vía SIGCHLD. Cuando reap_children() procesa ese SIGCHLD, ve que el código de salida es 127 (WIFEXITED y WEXITSTATUS=127) y actualiza g_procs con PROC_TERMINADO y codigo_salida=127. Si el cliente luego consulta el estado con op_estado, recibirá estado="terminado" y codigo_salida=127, lo que indica que el lanzamiento falló.

---

**P23: ¿Por qué el ejecutor no persiste g_counter en disco?**

Los procesos son entidades efímeras. A diferencia de los ficheros y programas (que tienen significado persistente entre ejecuciones del sistema), una ejecución (e-XXXX) solo existe mientras el ejecutor está vivo. Cuando el ejecutor termina, todos sus procesos hijos han terminado o son matados. Al reiniciar el sistema, no hay procesos de "la sesión anterior" que referencia — sería confuso tener IDs e-0042 hasta e-0099 del ciclo anterior que ya no existen. Empezar desde e-0001 en cada ejecución del sistema es más limpio y coherente. Si se persistiera el contador, el cliente podría recibir un ID "e-0100" en la primera ejecución de una nueva sesión, lo que podría confundir a sistemas que monitorean rangos de IDs.

---

**P24: ¿Qué hace sigemptyset(&sa.sa_mask) en la configuración de sigaction?**

`sa.sa_mask` especifica qué señales adicionales deben ser bloqueadas durante la ejecución del signal handler. `sigemptyset()` inicializa este conjunto a vacío — no se bloquea ninguna señal adicional. Esto significa que si llega otra señal (por ejemplo, otro SIGCHLD) mientras el handler de SIGCHLD está ejecutando, el handler puede ser interrumpido por esa segunda señal. Sin embargo, como nuestro handler solo hace `g_sigchld_flag = 1` (una operación atómica), esta re-entrada es segura. La señal SIGCHLD misma es bloqueada automáticamente mientras el handler está ejecutando (comportamiento estándar de sigaction), lo que previene re-entradas del mismo tipo de señal.

---

## BLOQUE F: Preguntas de Código Específico

**P25: En msg_recv, ¿qué diferencia hay entre los dos casos de EINTR?**

```c
if (errno == EINTR && pos == 0) return -1;  // Caso A
if (errno == EINTR) continue;               // Caso B
```

**Caso A** (`pos == 0`): EINTR llegó antes de leer cualquier byte del mensaje. El llamador aún no ha recibido datos parciales. Retornar -1 le permite al llamador (el bucle del ejecutor) verificar `g_sigchld_flag` y recolectar hijos terminados antes de reintentar msg_recv.

**Caso B** (`pos > 0`): EINTR llegó después de leer algunos bytes del mensaje. Si retornáramos -1, esos bytes parciales se perderían y el protocolo quedaría desincronizado — el servicio habría consumido parte de un mensaje del FIFO sin procesarlo. En este caso, continuar (reintentar el `read`) es la única opción correcta para mantener la integridad del protocolo.

---

**P26: ¿Por qué se usa snprintf en lugar de sprintf para construir paths?**

`sprintf` no tiene límite de longitud — si el string resultante es más largo que el buffer destino, escribe más allá del buffer causando un stack buffer overflow. `snprintf(buf, sizeof(buf), ...)` garantiza que nunca se escriben más de `sizeof(buf)` bytes incluyendo el `\0` terminador. En sistemas que manejan rutas de archivos del usuario, que podrían ser arbitrariamente largas, esta protección es esencial. Los paths en el sistema incluyen `aralmac` + `/ficheros/` + `f-XXXX` — relativamente cortos, pero la defensa en profundidad es buena práctica y requerida en código seguro.

---

**P27: ¿Qué garantiza `while (wait(NULL) > 0)` al final del ejecutor?**

Al salir del bucle principal, el ejecutor pudo haber tenido procesos hijos que terminaron después de la última llamada a `reap_children()`. `wait(NULL)` bloquea hasta que un hijo cualquiera termina. El bucle continúa hasta que no haya más hijos (`wait` retorna -1 con errno=ECHILD). Esto garantiza que:
1. No quedan procesos zombie cuando el ejecutor termina
2. El kernel puede liberar todas las entradas de la tabla de procesos
3. El sistema de archivos no queda con descriptores de pipes del ejecutor accesibles desde procesos huérfanos

Sin esta línea, si el ejecutor termina mientras aún hay hijos en ejecución, esos hijos se convierten en huérfanos y son adoptados por init. Aunque esto no es catastrófico, es un recurso del sistema que no fue liberado limpiamente.

---

# 13. ERRORES COMUNES Y CÓMO DEFENDERLOS

## Error 1: "¿No hay race condition entre g_sigchld_flag y el bucle principal?"

**Defensa**: La única operación en el signal handler es `g_sigchld_flag = 1`. Como `sig_atomic_t` garantiza escrituras atómicas, no puede haber un estado intermedio donde el flag esté "a medias". El bucle principal lee el flag y lo resetea a 0 SOLO en el contexto del bucle (no desde el handler). El peor caso es que el handler llegue justo cuando el bucle principal está reseteando el flag — en ese caso, el flag queda en 1 de nuevo y reap_children se llama una vez extra en la próxima iteración, donde waitpid retornará 0 inmediatamente (no hay hijos para recolectar). Esto es correcto y no causa ningún problema.

## Error 2: "¿Por qué no se usa select() o poll() en lugar de blocking read?"

**Defensa**: select/poll permitirían monitorear múltiples FDs simultáneamente con timeout. En el ejecutor, esto sería útil para no bloquearse en msg_recv mientras hay señales pendientes. Sin embargo, como usamos SA_flags=0 (sin SA_RESTART), SIGCHLD interrumpe el read() bloqueante con EINTR, lo que logra el mismo efecto: el bucle no se queda "atascado" esperando una petición cuando hay hijos que recolectar. Para el alcance del proyecto, el enfoque con EINTR es más simple que select/poll y correcto.

## Error 3: "¿El O_RDWR en FIFOs viola POSIX?"

**Defensa**: POSIX dice que O_RDWR en FIFOs tiene "resultado no especificado" (unspecified behavior). Sin embargo, el sistema está documentado para correr en Linux, donde el comportamiento de O_RDWR en FIFOs está bien definido: el open no bloquea, el proceso puede leer y escribir, y no genera EOF espurio. Las notas del man page de Linux pipe(7) confirman este comportamiento. Para máxima portabilidad POSIX, se usaría un enfoque con dos FDs (uno read-only, uno write-only) y gestión cuidadosa del orden de apertura, pero esto complica innecesariamente el código para un proyecto que explícitamente corre en Linux.

## Error 4: "¿Qué pasa si cJSON falla al parsear el JSON?"

**Defensa**: `cJSON_Parse()` retorna NULL si el JSON está malformado. Todos los puntos donde se llama cJSON_Parse deben verificar el resultado: si es NULL, se responde con un error JSON estándar (`{"estado":"error","mensaje":"JSON inválido"}`) y se continúa el bucle. Si se llamara a `cJSON_GetObjectItem()` sobre un NULL, habría un segfault. La verificación de NULL es la primera validación después de parsear cualquier petición.

## Error 5: "¿El contador persistente podría desincronizarse?"

**Defensa**: Podría desincronizarse si el proceso muere justo después de crear el archivo pero antes de llamar save_counter(). En ese caso, el siguiente inicio leería el contador N-1 y podría crear un archivo "f-N" que ya existe. Hay dos protecciones: primero, Crear usa `O_WRONLY|O_CREAT|O_TRUNC`, así que sobreescribiría el archivo existente (no silenciosamente — el cliente recibe un nuevo ID). Segundo, este escenario requiere un crash exactamente entre esas dos operaciones, lo cual es estadísticamente poco probable. Una implementación producción usaría una transacción (escribir a archivo temporal, renombrar) para garantizar atomicidad, pero esto está fuera del alcance del proyecto.

## Error 6: "¿Por qué no usar SA_NOCLDWAIT para evitar zombies automáticamente?"

**Defensa**: SA_NOCLDWAIT hace que el kernel no cree zombies — los hijos son recolectados automáticamente. Sin embargo, esto tiene una consecuencia importante: si el padre llama `wait()` o `waitpid()`, esas llamadas fallan con ECHILD (no hay hijos que esperar). En nuestro sistema, el ejecutor necesita mapear PIDs a sus estructuras `proceso_t` para actualizar `estado` y `codigo_salida`. Si los hijos son recolectados automáticamente por el kernel, el padre no puede obtener el código de salida. Por eso necesitamos manejar SIGCHLD manualmente: queremos recolectar los hijos nosotros mismos para poder registrar los códigos de salida.

---

## TABLA RESUMEN DE ESTADOS

| Servicio | Estado           | Descripción                                              |
|----------|------------------|----------------------------------------------------------|
| gesfich  | ST_CORRIENDO     | Acepta todas las operaciones                             |
| gesfich  | ST_SUSPENDIDO    | Solo Reasumir y Terminar                                 |
| gesfich  | ST_TERMINADO     | Proceso sale del bucle y termina                         |
| gesprog  | ST_CORRIENDO     | Acepta todas las operaciones                             |
| gesprog  | ST_SUSPENDIDO    | Leer + Reasumir + Terminar                               |
| gesprog  | ST_TERMINADO     | Proceso sale del bucle y termina                         |
| ejecutor | SVC_EJECUTAR     | Acepta Ejecutar/Estado/Matar/Suspender/Parar             |
| ejecutor | SVC_SUSPENDIDOS  | Solo Reasumir y Parar (procesos del usuario congelados)  |
| ejecutor | SVC_PARAR        | Espera que hijos terminen, no acepta nuevas peticiones   |
| ejecutor | SVC_TERMINADO    | Sale del bucle y termina                                 |
| ctrllt   | CORRIENDO        | Enruta peticiones de clientes a servicios                |
| ctrllt   | TERMINADO        | Sale del bucle y termina                                 |

---

## GLOSARIO TÉCNICO

| Término        | Definición                                                                                 |
|----------------|-------------------------------------------------------------------------------------------|
| FIFO           | First In First Out — archivo especial IPC con nombre en el filesystem                      |
| IPC            | Inter-Process Communication — mecanismo para comunicar procesos                           |
| fork()         | Syscall que crea una copia del proceso actual                                              |
| exec()         | Familia de syscalls que reemplaza el código del proceso con un nuevo ejecutable           |
| dup2()         | Duplica un file descriptor en una posición específica de la tabla de FDs                  |
| SIGCHLD        | Señal enviada al padre cuando un hijo termina o se suspende                               |
| SIGSTOP        | Señal que suspende un proceso (no puede ser ignorada)                                     |
| SIGCONT        | Señal que reanuda un proceso suspendido                                                   |
| SIGKILL        | Señal que mata un proceso (no puede ser ignorada)                                         |
| waitpid        | Syscall para esperar la terminación de procesos hijos                                     |
| WNOHANG        | Flag de waitpid: retorna inmediatamente si ningún hijo ha terminado                       |
| zombie         | Proceso que terminó pero cuya entrada en la tabla del kernel no ha sido recolectada       |
| sig_atomic_t   | Tipo entero garantizado de lectura/escritura atómica en signal handlers                   |
| volatile       | Calificador C que previene optimizaciones del compilador sobre una variable               |
| EINTR          | Error: syscall interrumpida por una señal                                                 |
| EEXIST         | Error: el archivo/directorio ya existe                                                    |
| ECHILD         | Error: no hay hijos que esperar                                                           |
| O_RDWR         | Flag open(): abrir para lectura Y escritura                                               |
| O_CREAT        | Flag open(): crear el archivo si no existe                                                |
| O_TRUNC        | Flag open(): truncar el archivo a 0 bytes al abrir                                       |
| PIPE_BUF       | Tamaño máximo de escritura atómica en un pipe (4096 bytes en Linux)                      |
| shebang        | `#!` al inicio de un script que indica el intérprete a usar                              |
| _exit()        | Termina el proceso sin llamar handlers de atexit ni hacer flush de stdio                 |
| aralmac        | Almacenamiento del sistema (directorio raíz de datos persistentes)                       |
| cJSON          | Biblioteca C ligera para parsear/generar JSON                                             |

---

*Guía generada para ST0257 — EAFIT — Sustentación 25 de Mayo de 2026*
*Versión completa con 27 preguntas de sustentación y análisis exhaustivo de código*
