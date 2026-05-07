# Diseño del Sistema — Ejecutor de Lotes

**Asignatura:** ST0257 — Sistemas Operativos
**Estudiante:** Jhon Duque
**Modalidad:** Individual — Linux
**Entrega:** Primera entrega
**Fecha:** Mayo de 2026

---

## 1. Introducción

Este documento presenta el diseño del sistema *Ejecutor de Lotes*
conforme al enunciado de la práctica del profesor Juan Francisco
Cardona Mc'Cormick (ST0257-C2661-4677, 25 de septiembre de 2025).

El sistema simula el modelo de ejecución por lotes presente en los
sistemas operativos de mainframe. En estos sistemas se registran las
imágenes de los programas —con su descripción, ejecutable, argumentos
y ambiente— para luego ser ejecutados directamente a través de un
identificador obtenido en el registro. Cada proceso sigue el modelo
de proceso de lotes: lee los datos de su entrada estándar, los
procesa y retorna el resultado en su salida estándar. El sistema
también registra los ficheros que serán fuente o destino de esos
procesos. Un cliente se conecta al `ctrllt` registrando los programas
y ficheros, y determinando cómo se van a ejecutar los procesos.

Esta primera entrega es exclusivamente un **documento de diseño**: no
contiene implementación. Define la arquitectura, las máquinas de
estado de cada servicio, el formato de los mensajes JSON y la
especificación de cada operación.

---

## 2. Descripción general del sistema

### 2.1. Componentes

El sistema está compuesto por los siguientes componentes, tal como
los describe el enunciado y muestra su Figura 1:

| Componente | Responsabilidad                                                   |
|------------|-------------------------------------------------------------------|
| `cliente`  | CRUD de programas y ficheros. Lanzar, consultar y terminar procesos de lotes. |
| `ctrllt`   | Control de Lotes. Pasarela central del sistema.                   |
| `gesfich`  | Crear, actualizar, borrar y leer ficheros en `aralmac`.           |
| `gesprog`  | Guardar, actualizar, borrar y mostrar programas en `aralmac`.     |
| `ejecutor` | Ejecutar procesos de lotes a partir de programas y ficheros registrados. |
| `aralmac`  | Área de almacenamiento compartida (directorio, base de datos, etc.). |

El `cliente` no hace parte de la implementación de esta práctica. El
profesor entregará una versión del mismo para la segunda entrega.

### 2.2. Diagrama general (Figura 1 del enunciado)

```
                    ┌──► gesprog ──┐
cliente ──► ctrllt ─┤              ├──► aralmac
           ◄──────  ├──► gesfich ──┘
                    └──► ejecutor
```

El enunciado aclara en nota al pie que la tubería del cliente
**también puede ser utilizada para enviar peticiones directamente a
`gesfich`, `gesprog` o `ejecutor`**, sin pasar por `ctrllt`. El flujo
normal, sin embargo, pasa siempre por el controlador.

### 2.3. Tipo de comunicación (§3.1 del enunciado)

Toda la comunicación entre los procesos se lleva a cabo mediante
**tuberías nombradas**. El enunciado especifica:

- En sistemas donde las tuberías nombradas son **full-duplex** basta
  una sola tubería por conexión.
- En sistemas donde son **half-duplex** se requieren **dos tuberías
  nombradas** por conexión.

La implementación es para **Linux**, donde las tuberías nombradas son
half-duplex. Por lo tanto, **cada conexión entre dos procesos usa dos
tuberías**: una para peticiones y otra para respuestas.

Cada tubería utilizada tiene un nombre único, como exige el enunciado.

---

## 3. Formato de los mensajes

El enunciado exige que el formato de los mensajes sea **JSON**. A
continuación se define la estructura que se usará de forma consistente
en todo el sistema.

### 3.1. Petición

```json
{
  "id_peticion": "abc-123",
  "id_cliente":  "c-0001",
  "servicio":    "gesfich",
  "operacion":   "crear",
  "params":      {}
}
```

| Campo         | Tipo   | Descripción                                                              |
|---------------|--------|--------------------------------------------------------------------------|
| `id_peticion` | string | Identificador único de la petición, generado por el cliente.             |
| `id_cliente`  | string | Identificador del cliente que origina la petición. Necesario para que `ctrllt` sepa a quién responder cuando hay varios clientes simultáneos. |
| `servicio`    | string | Servicio destino: `"ctrllt"`, `"gesfich"`, `"gesprog"` o `"ejecutor"`.   |
| `operacion`   | string | Nombre de la operación dentro del servicio.                              |
| `params`      | objeto | Parámetros propios de la operación. Puede ser `{}` si no se requieren.   |

### 3.2. Respuesta exitosa

```json
{
  "id_peticion": "abc-123",
  "status":      "ok",
  "resultado":   {}
}
```

### 3.3. Respuesta de error

```json
{
  "id_peticion": "abc-123",
  "status":      "error",
  "resultado": {
    "codigo":  "NO_ENCONTRADO",
    "mensaje": "El fichero f-0099 no existe."
  }
}
```

| Campo         | Tipo   | Descripción                                                   |
|---------------|--------|---------------------------------------------------------------|
| `id_peticion` | string | El mismo identificador de la petición original.               |
| `status`      | string | `"ok"` si fue exitosa, `"error"` en caso contrario.           |
| `resultado`   | objeto | Datos de la respuesta. En error, contiene `codigo` y `mensaje`. |

### 3.4. Códigos de error

| Código                | Significado                                                          |
|-----------------------|----------------------------------------------------------------------|
| `PARAM_INVALIDO`      | Falta un parámetro obligatorio o su tipo es incorrecto.              |
| `NO_ENCONTRADO`       | El identificador indicado no existe en el sistema.                   |
| `RUTA_INVALIDA`       | La ruta del sistema operativo indicada no existe o no es accesible.  |
| `EJECUTABLE_INVALIDO` | El fichero indicado no es un ejecutable válido (binario o guión).    |
| `ESTADO_INVALIDO`     | La operación no es aplicable al estado actual del servicio.          |
| `IO_ERROR`            | Falla de lectura o escritura sobre el área de almacenamiento.        |
| `ERROR_INTERNO`       | Cualquier otra falla inesperada.                                     |

---

## 4. Identificadores

El sistema utiliza identificadores con formato estricto:

| Recurso              | Formato  | Ejemplo  | Asignado por |
|----------------------|----------|----------|--------------|
| Fichero registrado   | `f-XXXX` | `f-0001` | `gesfich`    |
| Programa registrado  | `p-XXXX` | `p-0001` | `gesprog`    |
| Proceso de lotes     | `b-XXXX` | `b-0001` | `ejecutor`   |

Donde cada `X` es un dígito decimal. Los identificadores son únicos
durante toda la vida del sistema: una vez asignado, un identificador
no se reutiliza aunque el recurso sea borrado.

---

## 5. `cliente`

### 5.1. Responsabilidad (§3.2 del enunciado)

El cliente se encarga de:

- Registrar, consultar, borrar y actualizar (CRUD) programas y
  ficheros.
- Lanzar, consultar y terminar los procesos de lotes.

El sistema soporta **múltiples clientes ejecutándose simultáneamente**
conectando al servidor.

### 5.2. Sinopsis

```
cliente -c <tuberia-nombrada> [-a <tuberia-nombrada>]
```

| Opción | Descripción                                                                       |
|--------|-----------------------------------------------------------------------------------|
| `-c`   | Nombre de la tubería donde se envían las peticiones a `ctrllt`. El enunciado aclara que también puede usarse para enviar peticiones directamente a `gesfich`, `gesprog` o `ejecutor`. |
| `-a`   | Tubería para recibir las respuestas (opcional, requerida en sistemas half-duplex). También puede apuntar a cualquiera de los servicios según el destino de `-c`. |

---

## 6. `ctrllt`

### 6.1. Responsabilidad (§3.3 del enunciado)

`ctrllt` (Control de Lotes) es el **corazón del sistema** y su
función principal es de **pasarela**. Es un servicio que:

1. Recibe las peticiones de los múltiples clientes posibles.
2. Analiza cada petición.
3. La dirige al servicio apropiado (`gesfich`, `gesprog` o
   `ejecutor`).
4. Espera la respuesta de ese servicio.
5. Redirige la respuesta al cliente que la originó.

`ctrllt` también es el encargado de crear las tuberías necesarias
para conectarse con el cliente.

### 6.2. Sinopsis (§3.3.1 del enunciado)

```
ctrllt -c <tuberia-nombrada> [-a <tuberia-nombrada>] \
       -f <tuberia-nombrada> [-b <tuberia-nombrada>] \
       -p <tuberia-nombrada> [-c <tuberia-nombrada>] \
       -e <tuberia-nombrada> [-d <tuberia-nombrada>]
```

Las opciones `-c`, `-f`, `-p`, `-e` corresponden a las tuberías para
**enviar peticiones** a `ctrllt`, `gesfich`, `gesprog` y `ejecutor`
respectivamente.

Las opciones `-a`, `-b`, `-c`, `-d` corresponden a las tuberías para
**recibir respuestas** (en sistemas half-duplex) de `ctrllt`,
`gesfich`, `gesprog` y `ejecutor` respectivamente.

> **Nota:** El enunciado muestra la opción `-c` dos veces en la
> sinopsis de `ctrllt`: una para el pipe de peticiones de clientes, y
> otra para el pipe de respuestas de `gesprog`. Es un error
> tipográfico del enunciado. En este diseño se usa `-q` para la
> tubería de respuestas de `gesprog` con el fin de evitar la colisión:
>
> ```
> ctrllt -c <tuberia-nombrada> [-a <tuberia-nombrada>] \
>        -f <tuberia-nombrada> [-b <tuberia-nombrada>] \
>        -p <tuberia-nombrada> [-q <tuberia-nombrada>] \
>        -e <tuberia-nombrada> [-d <tuberia-nombrada>]
> ```

### 6.3. Ruteo de peticiones

`ctrllt` inspecciona el campo `servicio` de cada petición JSON para
decidir a qué tubería reenviarla:

| Valor de `servicio` | Destino            |
|---------------------|--------------------|
| `"gesfich"`         | servicio `gesfich` |
| `"gesprog"`         | servicio `gesprog` |
| `"ejecutor"`        | servicio `ejecutor`|

### 6.4. Máquina de estados (Figura 2 del enunciado)

```
   inicio ──► Corriendo ──[Terminar]──► Terminado
                ▲    │
                └────┘
         (atiende peticiones en bucle)
```

| Estado     | Descripción                                              |
|------------|----------------------------------------------------------|
| Corriendo  | Atiende peticiones de clientes y las redirige.           |
| Terminado  | El servicio ha finalizado su ejecución.                  |

---

## 7. `gesfich`

### 7.1. Responsabilidad (§3.4 del enunciado)

`gesfich` se encarga de **crear, actualizar, borrar y leer** los
ficheros que se encuentran almacenados en la región `aralmac`.

### 7.2. Sinopsis (§3.4.1 del enunciado)

```
gesfich -f <tuberia-nombrada> [-b <tuberia-nombrada>] -x <info-aralmac>
```

| Opción | Descripción                                                                              |
|--------|------------------------------------------------------------------------------------------|
| `-f`   | Tubería nombrada para recibir peticiones (o también para recibir respuestas si el SO soporta tuberías full-duplex). |
| `-b`   | Tubería nombrada para enviar respuestas en sistemas half-duplex (opcional).              |
| `-x`   | Información de configuración del área de almacenamiento: puede ser la ruta de un directorio, los parámetros de una base de datos, etc. |

### 7.3. Operaciones (§3.4.3 del enunciado)

#### 7.3.1. `crear`

Crea un fichero vacío en el área de almacenamiento. Cada fichero
creado es único y tiene su propio espacio para almacenar o leer los
datos de los procesos de lotes. Retorna un identificador del fichero
(`id_fichero`) con el formato `f-XXXX`, donde X es un dígito.

Petición:

```json
{
  "id_peticion": "u1",
  "id_cliente":  "c-0001",
  "servicio":    "gesfich",
  "operacion":   "crear",
  "params":      {}
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u1",
  "status":      "ok",
  "resultado":   { "id_fichero": "f-0001" }
}
```

#### 7.3.2. `leer`

Tiene **dos formatos**:

- **Con `id_fichero`:** lo verifica; si existe retorna el contenido
  actual del fichero, en caso contrario reporta un error.
- **Sin identificador:** retorna la información de todos los ficheros
  registrados.

Petición (con id):

```json
{
  "id_peticion": "u2",
  "id_cliente":  "c-0001",
  "servicio":    "gesfich",
  "operacion":   "leer",
  "params":      { "id_fichero": "f-0001" }
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u2",
  "status":      "ok",
  "resultado": {
    "id_fichero": "f-0001",
    "tamano":     2048,
    "contenido":  "..."
  }
}
```

Petición (sin id — lista todos):

```json
{
  "id_peticion": "u3",
  "id_cliente":  "c-0001",
  "servicio":    "gesfich",
  "operacion":   "leer",
  "params":      {}
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u3",
  "status":      "ok",
  "resultado": {
    "ficheros": [
      { "id_fichero": "f-0001", "tamano": 2048 },
      { "id_fichero": "f-0002", "tamano": 512  }
    ]
  }
}
```

Respuesta (error — fichero no encontrado):

```json
{
  "id_peticion": "u2",
  "status":      "error",
  "resultado":   { "codigo": "NO_ENCONTRADO", "mensaje": "El fichero f-0099 no existe." }
}
```

#### 7.3.3. `actualizar`

Recibe un `id_fichero` y la ruta de un fichero del sistema operativo.
Verifica la existencia de ambos; si se cumple, copia el contenido del
fichero indicado dentro del `aralmac`. En caso incorrecto reporta un
error.

Petición:

```json
{
  "id_peticion": "u4",
  "id_cliente":  "c-0001",
  "servicio":    "gesfich",
  "operacion":   "actualizar",
  "params": {
    "id_fichero": "f-0001",
    "ruta":       "/home/jhon/datos.txt"
  }
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u4",
  "status":      "ok",
  "resultado":   { "id_fichero": "f-0001", "tamano": 4096 }
}
```

#### 7.3.4. `borrar`

Recibe un `id_fichero`; lo verifica; si existe lo borra del `aralmac`.
En caso contrario, reporta un error.

Petición:

```json
{
  "id_peticion": "u5",
  "id_cliente":  "c-0001",
  "servicio":    "gesfich",
  "operacion":   "borrar",
  "params":      { "id_fichero": "f-0001" }
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u5",
  "status":      "ok",
  "resultado":   { "id_fichero": "f-0001", "borrado": true }
}
```

#### 7.3.5. `suspender`

Sin parámetros. Si es posible, pasa al estado `Suspendido`.

Petición:

```json
{ "id_peticion":"u6","id_cliente":"c-0001","servicio":"gesfich","operacion":"suspender","params":{} }
```

Respuesta (ok):

```json
{ "id_peticion":"u6","status":"ok","resultado":{} }
```

Respuesta (error — transición no válida):

```json
{ "id_peticion":"u6","status":"error","resultado":{"codigo":"ESTADO_INVALIDO","mensaje":"El servicio ya está suspendido."} }
```

#### 7.3.6. `reasumir`

Sin parámetros. Si es posible, pasa al estado `Corriendo`.

```json
{ "id_peticion":"u7","id_cliente":"c-0001","servicio":"gesfich","operacion":"reasumir","params":{} }
```

Respuesta (ok):

```json
{ "id_peticion":"u7","status":"ok","resultado":{} }
```

#### 7.3.7. `terminar`

Sin parámetros. Si es posible, pasa al estado `Terminado` y termina
la ejecución del servicio.

```json
{ "id_peticion":"u8","id_cliente":"c-0001","servicio":"gesfich","operacion":"terminar","params":{} }
```

Respuesta (ok):

```json
{ "id_peticion":"u8","status":"ok","resultado":{} }
```

### 7.4. Máquina de estados (Figura 3 del enunciado)

```
                    ┌─[Crear/Leer/Actualizar/Borrar]─┐
                    ▼                                 │
   inicio ──► Corriendo ──[Suspender]──► Suspendido
                 │    ◄──[Reasumir]──────┘   │
              [Terminar]                 [Terminar]
                 ▼                           ▼
              Terminado ◄───────────────────┘
```

| Estado     | Operaciones válidas                                              |
|------------|------------------------------------------------------------------|
| Corriendo  | `crear`, `leer`, `actualizar`, `borrar`, `suspender`, `terminar` |
| Suspendido | `reasumir`, `terminar`                                           |
| Terminado  | — (el proceso termina)                                           |

> En estado `Suspendido`, `gesfich` **no acepta ninguna operación de
> datos**. Solo `reasumir` y `terminar` son válidas.

---

## 8. `gesprog`

### 8.1. Responsabilidad (§3.5 del enunciado)

`gesprog` se encarga de **guardar, actualizar, borrar y mostrar** los
programas que se encuentran almacenados en la región `aralmac`.

### 8.2. Sinopsis (§3.5.1 del enunciado)

```
gesprog -p <tuberia-nombrada> [-c <tuberia-nombrada>] -x <info-aralmac>
```

| Opción | Descripción                                                                              |
|--------|------------------------------------------------------------------------------------------|
| `-p`   | Tubería nombrada para recibir peticiones (o también para recibir respuestas si el SO soporta tuberías full-duplex). |
| `-c`   | Tubería nombrada para enviar respuestas en sistemas half-duplex (opcional).              |
| `-x`   | Información de configuración del área de almacenamiento.                                 |

### 8.3. Operaciones (§3.5.3 del enunciado)

#### 8.3.1. `guardar`

Recibe el nombre de un ejecutable válido (sea binario o un guión), la
lista de argumentos que se usarán cuando el programa corra, y una
lista de variables de ambiente en la cual el programa debe ser
ejecutado. Lo copia dentro de `aralmac`. Una vez almacenado, retorna
un identificador del programa (`id_programa`) con el formato `p-XXXX`,
donde X es un dígito.

Petición:

```json
{
  "id_peticion": "u9",
  "id_cliente":  "c-0001",
  "servicio":    "gesprog",
  "operacion":   "guardar",
  "params": {
    "ruta_ejecutable": "/usr/local/bin/mi_script.sh",
    "argumentos":      ["--modo", "rapido", "-v"],
    "ambiente": {
      "PATH": "/usr/bin:/bin",
      "LANG": "es_CO.UTF-8"
    }
  }
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u9",
  "status":      "ok",
  "resultado":   { "id_programa": "p-0001" }
}
```

Respuesta (error):

```json
{
  "id_peticion": "u9",
  "status":      "error",
  "resultado":   { "codigo": "EJECUTABLE_INVALIDO", "mensaje": "La ruta indicada no es un ejecutable válido." }
}
```

#### 8.3.2. `leer`

Recibe un `id_programa`, lo verifica; si existe retorna su contenido
actual; en caso contrario reporta un error.

> **Nota sobre el enunciado:** el texto del §3.5.3 usa el término
> `id-fichero` en las operaciones `leer`, `actualizar` y `borrar` de
> `gesprog`. Se trata de un error tipográfico del enunciado (copió el
> texto de `gesfich` sin ajustarlo). El parámetro correcto es
> `id_programa`, que es el identificador que `gesprog` asigna y
> gestiona.

Petición (con id):

```json
{
  "id_peticion": "u10",
  "id_cliente":  "c-0001",
  "servicio":    "gesprog",
  "operacion":   "leer",
  "params":      { "id_programa": "p-0001" }
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u10",
  "status":      "ok",
  "resultado": {
    "id_programa":     "p-0001",
    "ruta_ejecutable": "/usr/local/bin/mi_script.sh",
    "argumentos":      ["--modo", "rapido", "-v"],
    "ambiente":        { "PATH": "/usr/bin:/bin", "LANG": "es_CO.UTF-8" }
  }
}
```

Petición (sin id — lista todos):

```json
{
  "id_peticion": "u11",
  "id_cliente":  "c-0001",
  "servicio":    "gesprog",
  "operacion":   "leer",
  "params":      {}
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u11",
  "status":      "ok",
  "resultado": {
    "programas": [
      { "id_programa": "p-0001", "ruta_ejecutable": "/usr/local/bin/mi_script.sh" },
      { "id_programa": "p-0002", "ruta_ejecutable": "/usr/bin/wc" }
    ]
  }
}
```

#### 8.3.3. `actualizar`

Recibe un `id_programa` y la ruta de un fichero del sistema operativo.
Verifica la existencia de ambos; si se cumple, copia el contenido
dentro del `aralmac`. En caso incorrecto reporta un error.

Petición:

```json
{
  "id_peticion": "u12",
  "id_cliente":  "c-0001",
  "servicio":    "gesprog",
  "operacion":   "actualizar",
  "params": {
    "id_programa": "p-0001",
    "ruta":        "/usr/local/bin/mi_script_v2.sh"
  }
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u12",
  "status":      "ok",
  "resultado":   { "id_programa": "p-0001" }
}
```

#### 8.3.4. `borrar`

Recibe un `id_programa`; lo verifica; si existe lo borra del `aralmac`.
En caso contrario, reporta un error.

Petición:

```json
{
  "id_peticion": "u13",
  "id_cliente":  "c-0001",
  "servicio":    "gesprog",
  "operacion":   "borrar",
  "params":      { "id_programa": "p-0001" }
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u13",
  "status":      "ok",
  "resultado":   { "id_programa": "p-0001", "borrado": true }
}
```

#### 8.3.5. `suspender`, `reasumir`, `terminar`

Siguen el mismo comportamiento que en `gesfich` (sin parámetros,
retornan `ok` o `ESTADO_INVALIDO`).

### 8.4. Máquina de estados (Figura 4 del enunciado)

```
                    ┌─[Guardar/Leer/Actualizar/Borrar]─┐
                    ▼                                   │
   inicio ──► Corriendo ──[Suspender]──► Suspendido ───[Leer]──┐
                 │    ◄──[Reasumir]──────┘   │                 ▲
              [Terminar]                 [Terminar]             │
                 ▼                           ▼                  │
              Terminado ◄───────────────────┘◄──────────────────┘
```

| Estado     | Operaciones válidas                                                 |
|------------|---------------------------------------------------------------------|
| Corriendo  | `guardar`, `leer`, `actualizar`, `borrar`, `suspender`, `terminar`  |
| Suspendido | **`leer`**, `reasumir`, `terminar`                                  |
| Terminado  | — (el proceso termina)                                              |

> A diferencia de `gesfich`, `gesprog` **sí permite la operación
> `leer` cuando está en estado `Suspendido`**. La Figura 4 del
> enunciado muestra explícitamente este *self-loop* desde el estado
> `Suspendido`.

---

## 9. `ejecutor`

### 9.1. Responsabilidad (§3.6 del enunciado)

`ejecutor` se encarga de ejecutar procesos de lotes. Los programas y
los ficheros están almacenados en `aralmac` y a partir de ellos se
crean los procesos de lotes.

### 9.2. Sinopsis (§3.6.1 del enunciado)

```
ejecutor -e <tuberia-nombrada> [-d <tuberia-nombrada>] -x <info-aralmac>
```

| Opción | Descripción                                                                              |
|--------|------------------------------------------------------------------------------------------|
| `-e`   | Tubería nombrada para recibir peticiones (o también para recibir respuestas si el SO soporta tuberías full-duplex). |
| `-d`   | Tubería nombrada para enviar respuestas en sistemas half-duplex (opcional).              |
| `-x`   | Información de configuración del área de almacenamiento.                                 |

### 9.3. Composición de un proceso de lotes

El enunciado indica que la operación `ejecutar` *"recibe un arreglo
conformado por un proceso lote"*. Conforme a la explicación del
profesor en clase, ese arreglo es una **cadena alternada de
identificadores** que comienza y termina con un fichero, con uno o
más programas entre ellos:

```
ejecuta:  f-0001 → p-0001 → p-0002 → f-0003
```

Cada programa de la cadena lee de la entrada que le precede y escribe
en la salida que le sigue. El caso mínimo es una cadena de tres
elementos: `[f-entrada, p-programa, f-salida]`.

Reglas de validación de la cadena:

1. Debe **empezar y terminar con un `id_fichero`** (`f-XXXX`).
2. Debe contener **al menos un `id_programa`** (`p-XXXX`).
3. La salida de cada programa se conecta a la entrada del siguiente.
4. Todos los identificadores de la cadena deben existir previamente
   en `gesfich` y `gesprog` respectivamente.

### 9.4. Operaciones (§3.6.3 del enunciado)

#### 9.4.1. `ejecutar`

Recibe un arreglo conformado por un proceso de lotes. Si el proceso
de lotes es correcto, retorna un identificador del proceso de lotes
en ejecución. Si es incorrecto, retorna un mensaje de error.

Petición:

```json
{
  "id_peticion": "u14",
  "id_cliente":  "c-0001",
  "servicio":    "ejecutor",
  "operacion":   "ejecutar",
  "params": {
    "cadena": ["f-0001", "p-0001", "p-0002", "f-0003"]
  }
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u14",
  "status":      "ok",
  "resultado": {
    "id_proceso_lote": "b-0001",
    "estado":          "corriendo"
  }
}
```

Respuesta (error):

```json
{
  "id_peticion": "u14",
  "status":      "error",
  "resultado":   { "codigo": "PARAM_INVALIDO", "mensaje": "La cadena debe empezar y terminar con un id_fichero." }
}
```

#### 9.4.2. `estado`

Tiene **dos formatos**:

- **Con `id_proceso_lote`:** si el identificador es válido, retorna el
  estado actual del proceso de lotes. Si es inválido, retorna un error.
- **Sin identificador:** lista el estado de todos los procesos de
  lotes.

Petición (con id):

```json
{
  "id_peticion": "u15",
  "id_cliente":  "c-0001",
  "servicio":    "ejecutor",
  "operacion":   "estado",
  "params":      { "id_proceso_lote": "b-0001" }
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u15",
  "status":      "ok",
  "resultado": {
    "id_proceso_lote": "b-0001",
    "estado":          "corriendo",
    "cadena":          ["f-0001", "p-0001", "p-0002", "f-0003"]
  }
}
```

Petición (sin id — lista todos):

```json
{
  "id_peticion": "u16",
  "id_cliente":  "c-0001",
  "servicio":    "ejecutor",
  "operacion":   "estado",
  "params":      {}
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u16",
  "status":      "ok",
  "resultado": {
    "procesos": [
      { "id_proceso_lote": "b-0001", "estado": "corriendo"  },
      { "id_proceso_lote": "b-0002", "estado": "terminado"  },
      { "id_proceso_lote": "b-0003", "estado": "matado"     }
    ]
  }
}
```

Los valores posibles del campo `estado` de un proceso de lotes son:

| Valor        | Significado                                               |
|--------------|-----------------------------------------------------------|
| `corriendo`  | El proceso está ejecutándose.                             |
| `suspendido` | El proceso está pausado (el servicio está suspendido).    |
| `terminado`  | El proceso finalizó normalmente.                          |
| `matado`     | El proceso fue terminado forzosamente con `matar`.        |
| `error`      | El proceso terminó con error o no pudo iniciarse.         |

#### 9.4.3. `matar`

El enunciado incluye esta operación en la máquina de estados del
`ejecutor` (Figura 5) pero no la describe textualmente — el texto
aparece en blanco en el PDF. Se define que recibe un `id_proceso_lote`
y termina forzosamente ese proceso de lotes.

Petición:

```json
{
  "id_peticion": "u17",
  "id_cliente":  "c-0001",
  "servicio":    "ejecutor",
  "operacion":   "matar",
  "params":      { "id_proceso_lote": "b-0001" }
}
```

Respuesta (ok):

```json
{
  "id_peticion": "u17",
  "status":      "ok",
  "resultado":   { "id_proceso_lote": "b-0001", "estado": "matado" }
}
```

#### 9.4.4. `suspender`

Sin parámetros. Si es posible, pasa al estado `Suspendidos`.

#### 9.4.5. `reasumir`

Sin parámetros. Si es posible, vuelve al estado `Ejecutar`.

#### 9.4.6. `parar`

Sin parámetros. Si es posible, pasa al estado `Parar`.

Las tres operaciones anteriores retornan `{"status":"ok","resultado":{}}` en caso de transición válida, o `ESTADO_INVALIDO` si no es posible.

### 9.5. Máquina de estados (Figura 5 del enunciado)

```
                    ┌─[Ejecutar/Estado/Matar]─┐
                    ▼                         │
   inicio ──► Ejecutar ──[Suspender]──► Suspendidos
                 │    ◄──[Reasumir]──────┘
                 │
              [Parar]
                 ▼
               Parar ──[/Proceso=0]──► Terminar ──► Terminado
```

| Estado       | Operaciones válidas                                  |
|--------------|------------------------------------------------------|
| Ejecutar     | `ejecutar`, `estado`, `matar`, `suspender`, `parar`  |
| Suspendidos  | `reasumir`                                           |
| Parar        | — (espera a que no haya procesos activos)            |
| Terminar     | — (transición automática cuando `#procesos == 0`)    |
| Terminado    | — (el servicio ha finalizado)                        |

> **Transición automática `Parar → Terminar`:** cuando el servicio
> recibe `parar`, deja de aceptar nuevas peticiones de `ejecutar` y
> espera a que todos los procesos de lotes activos finalicen. Cuando
> el contador de procesos llega a cero (`/Proceso=0` de la Figura 5),
> el ejecutor transita automáticamente al estado `Terminar` y luego
> a `Terminado`.

---

## 10. Caso de uso ilustrativo

Escenario: contar las líneas de un archivo del sistema y guardar el
resultado en un fichero gestionado por `gesfich`.

| Paso | Operación enviada a `ctrllt`                            | Resultado obtenido         |
|------|---------------------------------------------------------|----------------------------|
| 1    | `gesfich.crear`                                         | `f-0001` (entrada)         |
| 2    | `gesfich.actualizar(f-0001, "/tmp/entrada.txt")`        | ok                         |
| 3    | `gesfich.crear`                                         | `f-0002` (salida)          |
| 4    | `gesprog.guardar("/usr/bin/wc", ["-l"], {LANG:"C"})`    | `p-0001`                   |
| 5    | `ejecutor.ejecutar(["f-0001","p-0001","f-0002"])`       | `b-0001`, estado corriendo |
| 6    | `ejecutor.estado(b-0001)`                               | estado: `terminado`        |
| 7    | `gesfich.leer(f-0002)`                                  | contenido: `"128\n"`       |

---

## 11. Plan de entregas

Conforme a la explicación del profesor en clase:

| Entrega | Contenido                                                              |
|---------|------------------------------------------------------------------------|
| **1**   | API + Diseño + formato de mensajes JSON (este documento).              |
| **2**   | `gesfich` y `gesprog` (registro de fuentes y registro de programas).   |
| **3**   | `ctrllt` y `ejecutor` + integración completa usando tuberías nombradas.|

---

## 12. Decisiones de diseño

### 12.1. Sistema operativo: Linux (half-duplex)

El trabajo es individual y el enunciado permite escoger un sistema
operativo. Se elige **Linux**. En consecuencia, las tuberías nombradas
son half-duplex y cada conexión entre dos procesos usa dos tuberías
con nombres únicos.

### 12.2. Lenguaje de implementación: C

El enunciado establece libre elección de lenguaje, con la restricción
explícita de no usar lenguajes esotéricos. Se elige **C (C11)** por
ser el lenguaje natural para un curso de Sistemas Operativos donde las
APIs de tuberías nombradas, creación de procesos y señales son
interfaces del estándar POSIX que C expone directamente.

### 12.3. Campo `servicio` para enrutamiento en `ctrllt`

Cada petición incluye un campo explícito `servicio`. Esto hace el
ruteo en `ctrllt` directo: una sola comparación de cadena determina
a qué tubería reenviar la petición. El mismo campo es útil cuando el
cliente se conecta directamente a un servicio sin pasar por `ctrllt`.

### 12.4. `id_peticion` en cada mensaje

Permite correlacionar petición y respuesta. Es necesario cuando un
cliente tiene varias peticiones en vuelo simultáneas. `ctrllt` lo
conserva intacto al reenviar tanto la petición al servicio como la
respuesta al cliente.

### 12.5. Errores como objeto con `codigo` y `mensaje`

Tener ambos campos separados permite al cliente reaccionar
programáticamente al `codigo` y mostrar el `mensaje` al usuario. Una
cadena libre no permitiría el primer comportamiento.

### 12.6. Cadena de proceso de lotes como arreglo JSON

El enunciado dice que `ejecutar` recibe *"un arreglo conformado por
un proceso lote"*. Se modela como arreglo plano de identificadores
alternados. Es la representación más directa del modelo lineal de
proceso de lotes del enunciado (stdin → procesamiento → stdout).

### 12.7. Distinción entre estado del servicio y estado del proceso

El servicio `ejecutor` tiene su propia máquina de estados
(`Ejecutar / Suspendidos / Parar / Terminado`). Cada proceso de lotes
individual tiene la suya (`corriendo / suspendido / terminado /
matado / error`). Son cosas distintas que se modelan y reportan por
separado.

### 12.8. Error tipográfico en `gesprog` (`id-fichero` vs `id-programa`)

El §3.5.3 del enunciado usa el término `id-fichero` en las
operaciones `leer`, `actualizar` y `borrar` de `gesprog`. Es un error
tipográfico evidente: el enunciado copió el texto de `gesfich` sin
ajustar el identificador. En este diseño se usa `id_programa`, que
es el identificador que `gesprog` asigna y gestiona.

### 12.9. Error tipográfico en la sinopsis de `ctrllt` (`-c` duplicado)

El enunciado muestra la opción `-c` dos veces en la sinopsis de
`ctrllt`. Se usa `-q` para el pipe de respuestas de `gesprog` con el
fin de evitar la colisión, tal como se explica en §6.2.

### 12.10. Operación `matar` sin descripción en el enunciado

El texto del §3.6.3 lista `Matar:` pero lo deja sin descripción. Se
define su comportamiento a partir de su presencia en la máquina de
estados (Figura 5): termina forzosamente un proceso de lotes activo
dado su `id_proceso_lote`.

### 12.11. Estructura del repositorio

El enunciado exige entregar en un repositorio GitHub o GitLab con el
fichero `Diseño.md` dentro del directorio `docs/`:

```
.
├── docs/
│   └── Diseño.md     ← este documento
└── README.md
```

