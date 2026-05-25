# Ejecutor de Lotes — ST0257

Práctica del curso **Sistemas Operativos (ST0257)** — Universidad EAFIT.

Sistema que simula la ejecución por lotes de un mainframe usando cuatro
microservicios que se comunican mediante **tuberías nombradas (FIFOs)**
y un protocolo JSON delimitado por `\n`.

## Componentes

| Binario    | Función |
|------------|---------|
| `gesfich`  | Gestor de ficheros — crea, lee, actualiza y borra archivos en `aralmac` |
| `gesprog`  | Gestor de programas — registra, lee, actualiza y borra ejecutables en `aralmac` |
| `ejecutor` | Ejecutor de procesos — lanza, consulta y mata procesos de lotes |
| `ctrllt`   | Controlador/pasarela — enruta peticiones del cliente al servicio correcto |

## Compilación

```bash
make
```

Genera los binarios en `bin/`.

## Uso rápido

```bash
./run.sh start   # lanza todos los servicios en background
./run.sh stop    # detiene todos los servicios
./run.sh status  # muestra el estado de los procesos
```

## Pruebas

```bash
# Primero arrancar el sistema:
./run.sh start

# Prueba de integración completa (cat /etc/hosts → fichero de salida):
./test.sh

# Prueba de máquinas de estado y errores:
./test_states.sh
```

## Protocolo

Mensajes JSON terminados en `\n`, tamaño máximo `MSG_MAX_LEN = 4096` bytes.

**Petición:**
```json
{"servicio":"gesfich","operacion":"Crear"}
```

**Respuesta OK:**
```json
{"estado":"ok","id-fichero":"f-0001"}
```

**Respuesta error:**
```json
{"estado":"error","mensaje":"descripcion del error"}
```

## Estructura del repositorio

```
.
├── docs/
│   ├── Diseño.md              # Documento de diseño y especificación
│   └── GUIA_SUSTENTACION.md   # Guía de sustentación oral
├── src/
│   ├── common/proto.c         # msg_send / msg_recv
│   ├── gesfich/gesfich.c
│   ├── gesprog/gesprog.c
│   ├── ejecutor/ejecutor.c
│   └── ctrllt/ctrllt.c
├── vendor/cJSON.{c,h}
├── Makefile
├── run.sh
├── test.sh
└── test_states.sh
```

## Autor

- Jhon Fredy Alzate Duque — Individual (Linux)
