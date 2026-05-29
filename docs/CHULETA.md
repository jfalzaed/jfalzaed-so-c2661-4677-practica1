# Chuleta — Ejecutor de Lotes (ST0257)

> Todo el JSON y los errores exactos en una página. Para tener al lado en la sustentación.

## Protocolo (todo en 1 línea, termina en `\n`, máx 4096 B)
```
Petición : {"servicio":"<svc>","operacion":"<op>", ...}
OK       : {"estado":"ok", ...}
Error    : {"estado":"error","mensaje":"..."}
```
IDs: fichero `f-XXXX` · programa `p-XXXX` · ejecución `e-XXXX`

## gesfich — Gestor de Ficheros (`aralmac/ficheros/`)
```json
{"servicio":"gesfich","operacion":"Crear"}                                              → {"estado":"ok","id-fichero":"f-0001"}
{"servicio":"gesfich","operacion":"Leer","id-fichero":"f-0001"}                         → {"estado":"ok","contenido":"..."}
{"servicio":"gesfich","operacion":"Leer"}                                               → {"estado":"ok","ficheros":["f-0001","f-0002"]}
{"servicio":"gesfich","operacion":"Actualizar","id-fichero":"f-0001","ruta":"/etc/hosts"} → {"estado":"ok"}
{"servicio":"gesfich","operacion":"Borrar","id-fichero":"f-0001"}                       → {"estado":"ok"}
{"servicio":"gesfich","operacion":"Suspender"}   |   "Reasumir"   |   "Terminar"        → {"estado":"ok"}
```
**Estados:** Corriendo ↔ Suspendido → Terminado. **Suspendido bloquea TODAS las ops de datos.**
**Errores:** `no se pudo crear el fichero` · `fichero no encontrado` · `error al listar ficheros` · `faltan campos: id-fichero, ruta` · `no se pudo actualizar el fichero` · `transicion invalida` · `servicio suspendido` · `operacion desconocida`

## gesprog — Gestor de Programas (`aralmac/programas/p-XXXX/{bin,meta.json}`)
```json
{"servicio":"gesprog","operacion":"Guardar","ejecutable":"/bin/cat","args":["-n"],"env":["K=V"]} → {"estado":"ok","id-programa":"p-0001"}
{"servicio":"gesprog","operacion":"Leer","id-programa":"p-0001"}  → {"estado":"ok","programa":{"id-programa":"p-0001","nombre":"cat","args":[...],"env":[...]}}
{"servicio":"gesprog","operacion":"Leer"}                         → {"estado":"ok","programas":["p-0001","p-0002"]}
{"servicio":"gesprog","operacion":"Actualizar","id-programa":"p-0001","ruta":"/nueva/ruta"} → {"estado":"ok"}
{"servicio":"gesprog","operacion":"Borrar","id-programa":"p-0001"} → {"estado":"ok"}
{"servicio":"gesprog","operacion":"Suspender" | "Reasumir" | "Terminar"}                 → {"estado":"ok"}
```
**Estados:** Corriendo ↔ Suspendido → Terminado. **⭐ Leer SÍ funciona en Suspendido** (self-loop Fig.4).
`args`/`env` opcionales. Ejecutable válido = con bits de ejecución **o** script con shebang `#!`.
**Errores:** `falta campo: ejecutable` · `no se pudo guardar el programa` · `programa no encontrado` · `error al listar programas` · `faltan campos: id-programa, ruta` · `no se pudo actualizar el programa` · `transicion invalida` · `servicio suspendido` · `operacion desconocida`

## ejecutor — Ejecutor de Procesos (fork+exec+dup2)
```json
{"servicio":"ejecutor","operacion":"Ejecutar","id-programa":"p-0001","stdin":"f-0001","stdout":"f-0002","stderr":"f-0003"} → {"estado":"ok","id-ejecucion":"e-0001"}
{"servicio":"ejecutor","operacion":"Estado","id-ejecucion":"e-0001"} → {"estado":"ok","id-ejecucion":"e-0001","id-programa":"p-0001","proceso-estado":"Ejecutando"}
                                                          (terminado) → {...,"proceso-estado":"Terminado","codigo-salida":0}
{"servicio":"ejecutor","operacion":"Estado"}                         → {"estado":"ok","procesos":[ {...}, {...} ]}
{"servicio":"ejecutor","operacion":"Matar","id-ejecucion":"e-0001"}  → {"estado":"ok"}
{"servicio":"ejecutor","operacion":"Suspender" | "Reasumir" | "Parar"} → {"estado":"ok"}
```
`stdin/stdout/stderr` opcionales. `proceso-estado` ∈ Ejecutando | Suspendido | Terminado. `codigo-salida` solo si Terminado.
**Estados servicio:** Ejecutar ↔ Suspendidos → Parar → (auto) Terminado. **Ejecutar/Estado/Matar SOLO en Ejecutar.**
Suspender=SIGSTOP a todos · Reasumir=SIGCONT · Matar=SIGKILL a uno · Parar=espera a que terminen.
**Errores:** `falta campo: id-programa` · `no se pudo ejecutar el programa` *(programa inexistente da ESTE, no "no encontrado")* · `falta campo: id-ejecucion` · `proceso no encontrado` · `proceso no encontrado o ya terminado` · `transicion invalida` · `servicio suspendido` · `servicio parando` · `operacion desconocida`

## ctrllt — Pasarela
```json
{"servicio":"ctrllt","operacion":"Terminar"}  → {"estado":"ok"}   (propaga Terminar a gesfich/gesprog, Parar a ejecutor)
```
Enruta por `servicio`, reenvía respuesta sin modificar. **Estados:** Corriendo → Terminado.
**Errores:** `servicio desconocido` · `operacion ctrllt desconocida` · `servicio no conectado` · `error enviando solicitud al servicio` · `error leyendo respuesta del servicio`

## Arranque y prueba
```bash
make                  # compila a bin/
./run.sh start        # lanza gesfich,gesprog,ejecutor y luego ctrllt
./test.sh             # flujo completo (cat /etc/hosts → fichero salida)
./test_states.sh      # máquinas de estado + errores
./run.sh stop
# A mano:
printf '%s\n' '{"servicio":"gesfich","operacion":"Crear"}' > /tmp/ctrllt_req
head -n1 /tmp/ctrllt_resp
```
FIFOs cliente: `/tmp/ctrllt_req` (pide) · `/tmp/ctrllt_resp` (responde). aralmac: `/tmp/aralmac`.

## 5 frases para no olvidar
1. 4 procesos C que se comunican por **FIFOs** con **JSON** terminado en `\n`.
2. Servicios **nunca** se hablan entre sí; todo pasa por **ctrllt**.
3. **ejecutor** lanza procesos reales con **fork+exec**, redirige con **dup2**, recoge zombies con **waitpid** tras **SIGCHLD**.
4. **gesprog deja Leer suspendido; gesfich no.**
5. Programa inexistente al Ejecutar → **"no se pudo ejecutar el programa"**.
