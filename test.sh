#!/usr/bin/env bash
# test.sh — Prueba de integración básica del sistema
#
# Requiere que el sistema esté arrancado con: ./run.sh start
# Usa las mismas rutas de FIFOs que run.sh

set -euo pipefail

F_CLI_REQ="/tmp/ctrllt_req"
F_CLI_RESP="/tmp/ctrllt_resp"

# Envía msg al sistema, imprime req/resp al terminal (stderr) y devuelve JSON en stdout
send_recv() {
    local msg="$1"
    local desc="$2"
    echo "" >&2
    echo "── $desc" >&2
    echo "  REQ : $msg" >&2
    printf '%s\n' "$msg" > "$F_CLI_REQ"
    local resp
    resp=$(head -n1 < "$F_CLI_RESP")
    echo "  RESP: $resp" >&2
    echo "$resp"
}

# Extraer campo JSON por nombre (sin jq)
json_get() {
    local json="$1"
    local key="$2"
    echo "$json" | grep -o "\"${key}\":\"[^\"]*\"" | head -1 | cut -d'"' -f4
}

echo "========================================" >&2
echo " Test de integración — Ejecutor de Lotes" >&2
echo "========================================" >&2

# 1. Crear fichero de entrada — capturar id asignado
resp=$(send_recv '{"servicio":"gesfich","operacion":"Crear"}' \
                 "gesfich: Crear fichero de entrada")
FID_ENTRADA=$(json_get "$resp" "id-fichero")
echo "  → $FID_ENTRADA (entrada)" >&2

# 2. Actualizar fichero de entrada con /etc/hosts
send_recv "{\"servicio\":\"gesfich\",\"operacion\":\"Actualizar\",\"id-fichero\":\"$FID_ENTRADA\",\"ruta\":\"/etc/hosts\"}" \
          "gesfich: Actualizar $FID_ENTRADA con /etc/hosts" > /dev/null

# 3. Crear fichero de salida — capturar id asignado
resp=$(send_recv '{"servicio":"gesfich","operacion":"Crear"}' \
                 "gesfich: Crear fichero de salida")
FID_SALIDA=$(json_get "$resp" "id-fichero")
echo "  → $FID_SALIDA (salida)" >&2

# 4. Listar ficheros
send_recv '{"servicio":"gesfich","operacion":"Leer"}' \
          "gesfich: Leer (listar todos)" > /dev/null

# 5. Registrar programa cat — capturar id asignado
CAT_PATH=$(which cat)
resp=$(send_recv "{\"servicio\":\"gesprog\",\"operacion\":\"Guardar\",\"ejecutable\":\"$CAT_PATH\",\"args\":[],\"env\":[]}" \
                 "gesprog: Guardar $CAT_PATH")
PID_PROG=$(json_get "$resp" "id-programa")
echo "  → $PID_PROG (cat)" >&2

# 6. Leer metadatos del programa recién registrado
send_recv "{\"servicio\":\"gesprog\",\"operacion\":\"Leer\",\"id-programa\":\"$PID_PROG\"}" \
          "gesprog: Leer $PID_PROG" > /dev/null

# 7. Ejecutar cat con stdin=entrada, stdout=salida — capturar id-ejecucion
resp=$(send_recv "{\"servicio\":\"ejecutor\",\"operacion\":\"Ejecutar\",\"id-programa\":\"$PID_PROG\",\"stdin\":\"$FID_ENTRADA\",\"stdout\":\"$FID_SALIDA\"}" \
                 "ejecutor: Ejecutar cat $FID_ENTRADA → $FID_SALIDA")
EID=$(json_get "$resp" "id-ejecucion")
echo "  → $EID" >&2

# 8. Polling estado hasta Terminado (hasta 3 s)
echo "" >&2
echo "── ejecutor: Polling $EID ..." >&2
ESTADO_PROC="Ejecutando"
for i in $(seq 1 30); do
    sleep 0.1
    resp=$(send_recv "{\"servicio\":\"ejecutor\",\"operacion\":\"Estado\",\"id-ejecucion\":\"$EID\"}" \
                     "ejecutor: Estado $EID (intento $i)")
    ESTADO_PROC=$(json_get "$resp" "proceso-estado")
    [ "$ESTADO_PROC" = "Terminado" ] && break
done
echo "  → Estado final: $ESTADO_PROC" >&2

# 9. Leer contenido del fichero de salida
resp=$(send_recv "{\"servicio\":\"gesfich\",\"operacion\":\"Leer\",\"id-fichero\":\"$FID_SALIDA\"}" \
                 "gesfich: Leer $FID_SALIDA (salida de cat)")
CONTENIDO=$(json_get "$resp" "contenido")
if [ -n "$CONTENIDO" ]; then
    echo "  ✓ Fichero de salida tiene contenido (${#CONTENIDO} chars)" >&2
else
    echo "  ✗ ADVERTENCIA: fichero de salida vacío" >&2
fi

# 10. Terminar el sistema
send_recv '{"servicio":"ctrllt","operacion":"Terminar"}' \
          "ctrllt: Terminar sistema" > /dev/null

echo "" >&2
echo "========================================" >&2
echo " Tests completados." >&2
if [ "$ESTADO_PROC" = "Terminado" ] && [ -n "$CONTENIDO" ]; then
    echo " Resultado: PASÓ ✓" >&2
else
    echo " Resultado: FALLÓ ✗  (estado=$ESTADO_PROC contenido=$([ -n "$CONTENIDO" ] && echo 'ok' || echo 'vacío'))" >&2
fi
echo "========================================" >&2
