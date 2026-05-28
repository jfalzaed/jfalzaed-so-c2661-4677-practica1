#!/usr/bin/env bash
# run.sh — Arrancar y detener el sistema Ejecutor de Lotes
#
# Uso:
#   ./run.sh start   — Lanza todos los servicios en background
#   ./run.sh stop    — Detiene todos los servicios
#   ./run.sh status  — Muestra el estado de los procesos

set -euo pipefail

ARALMAC="${ARALMAC:-/tmp/aralmac}"
BIN="./bin"

# Nombres de los FIFOs
F_CLI_REQ="/tmp/ctrllt_req"
F_CLI_RESP="/tmp/ctrllt_resp"
F_GF_REQ="/tmp/gesfich_req"
F_GF_RESP="/tmp/gesfich_resp"
F_GP_REQ="/tmp/gesprog_req"
F_GP_RESP="/tmp/gesprog_resp"
F_EJ_REQ="/tmp/ejecutor_req"
F_EJ_RESP="/tmp/ejecutor_resp"

PID_FILE="/tmp/ejecutor_lotes.pids"

start() {
    echo "==> Creando directorio aralmac en $ARALMAC"
    rm -rf "$ARALMAC"
    mkdir -p "$ARALMAC"
    # Truncar PID file para evitar acumulación entre arranques sucesivos
    > "$PID_FILE"

    echo "==> Arrancando gesfich..."
    "$BIN/gesfich" -f "$F_GF_REQ" -b "$F_GF_RESP" -x "$ARALMAC" &
    echo $! >> "$PID_FILE"
    sleep 0.2

    echo "==> Arrancando gesprog..."
    "$BIN/gesprog" -p "$F_GP_REQ" -c "$F_GP_RESP" -x "$ARALMAC" &
    echo $! >> "$PID_FILE"
    sleep 0.2

    echo "==> Arrancando ejecutor..."
    "$BIN/ejecutor" -e "$F_EJ_REQ" -d "$F_EJ_RESP" -x "$ARALMAC" &
    echo $! >> "$PID_FILE"
    sleep 0.2

    echo "==> Arrancando ctrllt..."
    "$BIN/ctrllt" \
        -c "$F_CLI_REQ"  -a "$F_CLI_RESP" \
        -f "$F_GF_REQ"   -b "$F_GF_RESP"  \
        -p "$F_GP_REQ"   -q "$F_GP_RESP"  \
        -e "$F_EJ_REQ"   -d "$F_EJ_RESP"  &
    echo $! >> "$PID_FILE"

    echo "==> Sistema arrancado. PID en $PID_FILE"
    echo "    FIFOs cliente: req=$F_CLI_REQ  resp=$F_CLI_RESP"
}

stop() {
    if [ ! -f "$PID_FILE" ]; then
        echo "No hay archivo de PIDs. ¿El sistema está arrancado?"
        return
    fi
    echo "==> Enviando Terminar al sistema..."
    # Abrir un FIFO en modo escritura bloquea si no hay lector (p.ej. ctrllt ya
    # terminó). Lo escribimos en un subshell que matamos tras 0.5 s para que el
    # apagado nunca se cuelgue.
    if [ -p "$F_CLI_REQ" ]; then
        ( printf '{"servicio":"ctrllt","operacion":"Terminar"}\n' > "$F_CLI_REQ" ) 2>/dev/null &
        _wpid=$!
        sleep 0.5
        kill "$_wpid" 2>/dev/null || true
        wait "$_wpid" 2>/dev/null || true
    fi
    echo "==> Matando procesos restantes..."
    while read -r pid; do
        kill "$pid" 2>/dev/null || true
    done < "$PID_FILE"
    rm -f "$PID_FILE"
    # Eliminar FIFOs
    for fifo in "$F_CLI_REQ" "$F_CLI_RESP" "$F_GF_REQ" "$F_GF_RESP" \
                "$F_GP_REQ"  "$F_GP_RESP"  "$F_EJ_REQ" "$F_EJ_RESP"; do
        rm -f "$fifo"
    done
    echo "==> Sistema detenido."
}

status() {
    if [ ! -f "$PID_FILE" ]; then
        echo "Sistema no arrancado."
        return
    fi
    echo "PIDs activos:"
    while read -r pid; do
        if kill -0 "$pid" 2>/dev/null; then
            echo "  PID $pid: corriendo"
        else
            echo "  PID $pid: terminado"
        fi
    done < "$PID_FILE"
}

case "${1:-}" in
    start)  start  ;;
    stop)   stop   ;;
    status) status ;;
    *)
        echo "Uso: $0 {start|stop|status}"
        exit 1
        ;;
esac
