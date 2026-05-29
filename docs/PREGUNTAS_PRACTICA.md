# Preguntas de Práctica — Sustentación Ejecutor de Lotes (ST0257)

> Tapa la respuesta, contesta en voz alta y compara. Si fallas una, vuelve a la sección
> indicada de `GUIA_ESTUDIO.md`. Marcadas con ⭐ las que casi seguro caen.

---

## Bloque 1 — Visión general

**P1 ⭐ ¿Qué simula este sistema y qué es un "proceso por lotes"?**
> Simula la ejecución por lotes de un mainframe: se registran ficheros (datos) y programas
> (ejecutables) en un almacén y se piden ejecuciones. Un proceso por lotes es no
> interactivo: lee su stdin, procesa y escribe su stdout, sin pedir nada al usuario.

**P2 ⭐ ¿Cuáles son los 4 procesos y qué hace cada uno?**
> `ctrllt` (pasarela/enrutador), `gesfich` (CRUD de ficheros), `gesprog` (registro de
> programas), `ejecutor` (lanza procesos reales con fork+exec).

**P3 ¿Quién habla con quién? ¿Por qué los servicios no se hablan entre sí?**
> El cliente solo habla con `ctrllt`; ctrllt habla con los 3 servicios. Los servicios nunca
> se hablan entre sí (§3.7). Así hay una sola pasarela y un solo protocolo: más simple.

**P4 ⭐ ¿Qué es `aralmac` y cómo lo implementaste?**
> El área de almacenamiento. El PDF permite que sea un directorio o una BD; elegí **un
> directorio** (`/tmp/aralmac`) con subcarpetas `ficheros/` y `programas/`. Se pasa con `-x`.

---

## Bloque 2 — Comunicación y protocolo

**P5 ⭐ ¿Qué es una tubería nombrada (FIFO) y por qué usas dos por servicio?**
> Un archivo especial (`mkfifo`) que sirve de canal entre procesos no emparentados. En Linux
> son half-duplex (un sentido), así que uso dos: una de peticiones (`_req`) y otra de
> respuestas (`_resp`).

**P6 ⭐ Describe el formato de los 3 tipos de mensaje.**
> Petición: `{"servicio":"...","operacion":"...",...}`. OK: `{"estado":"ok",...}`. Error:
> `{"estado":"error","mensaje":"..."}`. Todos en una línea terminada en `\n`, máx 4096 bytes.

**P7 ¿Por qué `msg_recv` lee byte a byte?**
> Para detenerse exactamente en el `\n` y no consumir parte del siguiente mensaje.

**P8 ¿Por qué abres los FIFOs con `O_RDWR` y no solo lectura o escritura?**
> Para que `open` no se bloquee esperando al otro extremo y para que `read` no devuelva EOF
> cuando temporalmente no hay escritores.

**P9 ¿Qué pasa si una respuesta supera 4096 bytes?**
> El `send_json` lo detecta y manda un error corto en su lugar, para no bloquear al cliente.

**P10 Formato de los IDs y quién los genera.**
> `f-XXXX` (gesfich), `p-XXXX` (gesprog), `e-XXXX` (ejecutor). XXXX = 4 dígitos, nunca se
> reutilizan (contador persistente en archivo oculto dentro de aralmac).

---

## Bloque 3 — gesfich

**P11 Operaciones de gesfich y qué hace Crear.**
> Crear, Leer, Actualizar, Borrar, Suspender, Reasumir, Terminar. Crear hace un archivo
> **vacío** `f-XXXX` y devuelve su id.

**P12 ⭐ Diferencia entre "Leer con id" y "Leer sin id".**
> Con `id-fichero` devuelve el `contenido`; sin id devuelve el array `ficheros` con todos los
> ids existentes.

**P13 ¿Qué hace Actualizar exactamente?**
> Recibe `id-fichero` y `ruta`; verifica que ambos existan y **copia** el contenido del
> archivo en `ruta` dentro del fichero registrado.

**P14 ⭐ En estado Suspendido, ¿qué operaciones funcionan en gesfich?**
> Solo control: Reasumir y Terminar. Crear/Leer/Actualizar/Borrar dan `"servicio suspendido"`.

---

## Bloque 4 — gesprog

**P15 ⭐ ¿Qué guarda gesprog al hacer Guardar y dónde?**
> Copia el ejecutable a `aralmac/programas/p-XXXX/bin` y escribe `meta.json` con
> `id-programa, nombre, ejecutable, args, env`.

**P16 ⭐ ¿Cómo validas que el ejecutable es "válido"?**
> `stat` debe existir. Si tiene bits de ejecución, vale. Si no, solo se acepta si es un
> **script con shebang** (`#!` en los 2 primeros bytes); si no, `"no se pudo guardar el
> programa"`.

**P17 ⭐⭐ TRAMPA: ¿gesprog permite Leer estando Suspendido?**
> **Sí.** La Figura 4 del PDF tiene un self-loop `Leer` en Suspendido. Las demás operaciones
> de datos no. (En gesfich NO se permite nada de datos suspendido — Figura 3.)

**P18 Al Leer un programa, ¿por qué no devuelves el campo `ejecutable`?**
> Porque §3.10.3 dice que el objeto `programa` solo expone `id-programa, nombre, args, env`.
> Lo borro con `cJSON_DeleteItemFromObject` antes de responder.

---

## Bloque 5 — ejecutor (el más preguntado)

**P19 ⭐⭐ Explica fork + exec + dup2 al ejecutar un programa.**
> `fork()` crea un hijo (copia del proceso). En el hijo, `dup2` redirige los ficheros de
> stdin/stdout/stderr a los descriptores 0/1/2, se cierran los FIFOs del servicio, y `execve`
> (o `execv`) reemplaza el hijo por el programa real. El padre registra el proceso y responde.

**P20 ⭐ ¿Qué es un proceso zombie y cómo lo evitas?**
> Un hijo terminado cuyo estado no se ha recogido. Al recibir `SIGCHLD` levanto una bandera y
> en `reap_children` uso `waitpid(-1, WNOHANG)` para recogerlos a todos.

**P21 ⭐ ¿Cómo calculas `codigo-salida`?**
> `WEXITSTATUS(status)` si el proceso terminó normal; `128 + número de señal` si murió por
> una señal (p.ej. matado con SIGKILL → 128+9 = 137).

**P22 ⭐⭐ Diferencia entre Matar, Suspender y Parar.**
> Matar: `SIGKILL` a **un** proceso. Suspender: `SIGSTOP` a **todos** (congela) → servicio
> Suspendido. Parar: deja de aceptar trabajos y **espera** a que los actuales terminen solos;
> cuando no quedan, el servicio pasa a Terminado.

**P23 ⭐ ¿En qué estados están disponibles Ejecutar/Estado/Matar?**
> Solo en estado `Ejecutar` (self-loops de la Figura 5). Suspendido → `"servicio suspendido"`;
> Parando → `"servicio parando"`.

**P24 ⭐⭐ TRAMPA: ¿qué error da Ejecutar con un programa inexistente?**
> `"no se pudo ejecutar el programa"`. Ojo: NO `"programa no encontrado"` (ese es de gesprog;
> §3.11.3 no lo lista para el ejecutor).

**P25 ¿Por qué instalas SIGCHLD sin `SA_RESTART`?**
> Para que el `read` del FIFO se interrumpa (`EINTR`) cuando termina un hijo y pueda
> recolectarlo al instante en vez de quedar bloqueado.

**P26 ¿Cómo sabe el servicio en Parar cuándo terminar?**
> `wait_for_parar` hace polling cada 10 ms; cuando `count_active()==0` pasa a Terminado.

---

## Bloque 6 — ctrllt y apagado

**P27 ⭐ ¿Qué hace ctrllt con una petición normal?**
> Lee `servicio`, reenvía el JSON **literal** al FIFO de ese servicio, lee la respuesta y la
> devuelve al cliente **sin modificarla**.

**P28 ⭐ ¿Qué hace `ctrllt Terminar`?**
> Propaga `Terminar` a gesfich y gesprog, manda `Parar` a ejecutor (que se auto-detiene), y
> termina él mismo.

**P29 ¿Por qué `-q` y no `-c` para la respuesta de gesprog?**
> El PDF repite `-c` (errata). Como una opción no puede repetirse, uso `-q` para la respuesta
> de gesprog. Por eso `run.sh` lanza ctrllt con `-q`.

**P30 Errores propios de ctrllt.**
> `servicio desconocido`, `operacion ctrllt desconocida`, `servicio no conectado`, `error
> enviando solicitud al servicio`, `error leyendo respuesta del servicio`.

---

## Bloque 7 — build, scripts y arquitectura

**P31 ⭐ ¿Qué hace `make`?**
> Descarga cJSON si falta, compila cada `.c` a `.o` con `-std=c11 -Wall -Wextra -g` y enlaza
> los 4 binarios en `bin/`. `make clean` borra build y bin.

**P32 ¿Por qué `run.sh` arranca los servicios antes que ctrllt?**
> Cada servicio crea sus FIFOs al arrancar; ctrllt solo los abre (no los crea). Si ctrllt
> fuera primero no los encontraría. Los `sleep 0.2` dan tiempo a que se creen.

**P33 ¿Qué demuestra `test.sh`?**
> El flujo completo: crear fichero de entrada, llenarlo con /etc/hosts, crear salida, registrar
> `cat`, ejecutarlo (stdin→stdout), hacer polling del estado hasta Terminado y verificar que la
> salida tiene contenido.

**P34 ¿Qué demuestra `test_states.sh`?**
> Que las máquinas de estado y validaciones rechazan lo que deben: operar suspendido, IDs
> inexistentes, servicio/operación desconocidos, campos faltantes — incluido que gesprog SÍ
> deja Leer suspendido.

**P35 ⭐ TRAMPA HONESTA: ¿soporta clientes concurrentes de verdad?**
> Los soporta de forma **serializada**: ctrllt atiende un request y su respuesta antes del
> siguiente. No hay hilos ni `select`/`poll`. Es una limitación consciente del diseño, no un
> bug.

---

## Bloque 8 — relámpago (respuesta de 1 línea)

| # | Pregunta | Respuesta |
|---|----------|-----------|
| R1 | ¿MSG_MAX_LEN? | 4096 bytes |
| R2 | ¿Delimitador de mensaje? | `\n` |
| R3 | ¿Librería JSON? | cJSON (en `vendor/`) |
| R4 | ¿Estándar de C? | C11 |
| R5 | ¿Estado inicial de gesfich/gesprog? | Corriendo |
| R6 | ¿Estado inicial del ejecutor? | Ejecutar |
| R7 | ¿Señal para congelar? ¿Reanudar? ¿Matar? | SIGSTOP / SIGCONT / SIGKILL |
| R8 | ¿Quién genera e-XXXX? | ejecutor |
| R9 | ¿stdin/stdout/stderr son obligatorios al Ejecutar? | No, opcionales |
| R10 | ¿Dónde está el código compartido? | `src/common/proto.c` |
