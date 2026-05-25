#!/usr/bin/env bash
# test_states.sh — Prueba de máquinas de estado y errores de validación

F_REQ="/tmp/ctrllt_req"
F_RESP="/tmp/ctrllt_resp"

sr() {
    local msg="$1" label="$2"
    printf '%s\n' "$msg" > "$F_REQ"
    local r; r=$(timeout 2 head -n1 < "$F_RESP" 2>/dev/null || echo "(timeout)")
    printf "  %-55s → %s\n" "$label" "$r"
}

echo "═══ Máquina de estados gesfich ═══════════════════════════════"
sr '{"servicio":"gesfich","operacion":"Suspender"}'          "Suspender (Corriendo→Suspendido)"
sr '{"servicio":"gesfich","operacion":"Crear"}'              "Crear en Suspendido (debe: error)"
sr '{"servicio":"gesfich","operacion":"Actualizar","id-fichero":"f-0001","ruta":"/etc/hosts"}' \
                                                             "Actualizar en Suspendido (debe: error)"
sr '{"servicio":"gesfich","operacion":"Suspender"}'          "Suspender en Suspendido (debe: error)"
sr '{"servicio":"gesfich","operacion":"Reasumir"}'           "Reasumir (Suspendido→Corriendo)"
sr '{"servicio":"gesfich","operacion":"Reasumir"}'           "Reasumir en Corriendo (debe: error)"
sr '{"servicio":"gesfich","operacion":"Crear"}'              "Crear tras Reasumir (debe: ok)"

echo ""
echo "═══ Máquina de estados gesprog ════════════════════════════════"
sr '{"servicio":"gesprog","operacion":"Suspender"}'          "Suspender (Corriendo→Suspendido)"
sr '{"servicio":"gesprog","operacion":"Guardar","ejecutable":"/bin/echo","args":["test"],"env":[]}' \
                                                             "Guardar en Suspendido (debe: error)"
sr '{"servicio":"gesprog","operacion":"Leer"}'               "Leer en Suspendido (debe: ok!)"
sr '{"servicio":"gesprog","operacion":"Reasumir"}'           "Reasumir (Suspendido→Corriendo)"

echo ""
echo "═══ Errores de validación ══════════════════════════════════════"
sr '{"servicio":"gesfich","operacion":"Borrar","id-fichero":"f-9999"}' \
                                                             "Borrar ID inexistente"
sr '{"servicio":"gesprog","operacion":"Leer","id-programa":"p-9999"}' \
                                                             "Leer programa inexistente"
sr '{"servicio":"ejecutor","operacion":"Estado","id-ejecucion":"e-9999"}' \
                                                             "Estado ejecución inexistente"
sr '{"servicio":"ninguno","operacion":"Crear"}'              "Servicio desconocido"
sr '{"servicio":"gesfich","operacion":"XYZ"}'                "Operación desconocida"
sr '{"servicio":"gesprog","operacion":"Actualizar","id-programa":"p-0001"}' \
                                                             "Actualizar sin ruta"
sr '{"servicio":"ejecutor","operacion":"Ejecutar"}'          "Ejecutar sin id-programa"

echo ""
echo "═══ Terminar sistema ═══════════════════════════════════════════"
sr '{"servicio":"ctrllt","operacion":"Terminar"}'            "Terminar"
echo "  Listo."
