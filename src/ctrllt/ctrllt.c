/*
 * ctrllt — Controlador / Pasarela
 *
 * Sinopsis:
 *   ctrllt -c <tub-cliente-req> [-a <tub-cliente-resp>]
 *          -f <tub-gesfich-req> [-b <tub-gesfich-resp>]
 *          -p <tub-gesprog-req> [-q <tub-gesprog-resp>]
 *          -e <tub-ejecutor-req>[-d <tub-ejecutor-resp>]
 *
 * Nota: el enunciado tiene un error tipográfico (-c duplicado para gesprog).
 * En este diseño se usa -q para la respuesta de gesprog.
 *
 * Protocolo (PDF §3.12):
 *   - Enruta peticiones por el campo "servicio".
 *   - Operación propia: {"servicio":"ctrllt","operacion":"Terminar"}
 *     → propaga Terminar a gesfich y gesprog, Parar a ejecutor.
 *
 * Máquina de estados: Corriendo → Terminado
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#include "cJSON.h"
#include "proto.h"

/* ── Descriptores de FIFOs ───────────────────────────────────────────────── */
typedef struct {
    int req;   /* escribir petición al servicio  */
    int resp;  /* leer respuesta del servicio    */
} svc_fds_t;

static int      g_fd_cli_req  = -1;  /* leer peticiones del cliente  */
static int      g_fd_cli_resp = -1;  /* escribir respuestas al cliente */
static svc_fds_t g_gesfich   = {-1, -1};
static svc_fds_t g_gesprog   = {-1, -1};
static svc_fds_t g_ejecutor  = {-1, -1};

/* ── Abrir un FIFO con O_RDWR (no bloquea) ───────────────────────────────── */
static int open_fifo(const char *path, int create) {
    if (create) mkfifo(path, 0666); /* ignorar EEXIST */
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ctrllt: no se pudo abrir FIFO '%s': %s\n",
                path, strerror(errno));
    }
    return fd;
}

/* ── Enviar petición a servicio y leer respuesta ─────────────────────────── */
static int forward(svc_fds_t svc, const char *msg, char *resp_buf, size_t resp_sz) {
    if (svc.req < 0) return -1;
    if (msg_send(svc.req, msg) < 0) return -1;
    ssize_t n = msg_recv(svc.resp < 0 ? svc.req : svc.resp, resp_buf, resp_sz);
    return (n >= 0) ? 0 : -1;
}

/* ── Operación propia: Terminar el sistema ───────────────────────────────── */
static void do_terminar(void) {
    char resp[MSG_MAX_LEN];

    /* Terminar gesfich */
    if (g_gesfich.req >= 0) {
        const char *msg = "{\"servicio\":\"gesfich\",\"operacion\":\"Terminar\"}";
        forward(g_gesfich, msg, resp, sizeof resp);
    }
    /* Terminar gesprog */
    if (g_gesprog.req >= 0) {
        const char *msg = "{\"servicio\":\"gesprog\",\"operacion\":\"Terminar\"}";
        forward(g_gesprog, msg, resp, sizeof resp);
    }
    /* Parar ejecutor (se auto-detiene al quedarse sin procesos) */
    if (g_ejecutor.req >= 0) {
        const char *msg = "{\"servicio\":\"ejecutor\",\"operacion\":\"Parar\"}";
        forward(g_ejecutor, msg, resp, sizeof resp);
    }
}

/* ── Bucle principal ─────────────────────────────────────────────────────── */
static void run(void) {
    char buf[MSG_MAX_LEN];
    char resp[MSG_MAX_LEN];
    int  running = 1;

    while (running) {
        ssize_t n = msg_recv(g_fd_cli_req, buf, sizeof buf);
        if (n <= 0) break;

        cJSON *req = cJSON_Parse(buf);
        if (!req) {
            msg_send(g_fd_cli_resp, "{\"estado\":\"error\",\"mensaje\":\"json invalido\"}");
            continue;
        }

        cJSON *jsvc = cJSON_GetObjectItem(req, "servicio");
        cJSON *jop  = cJSON_GetObjectItem(req, "operacion");

        if (!jsvc || !cJSON_IsString(jsvc)) {
            msg_send(g_fd_cli_resp, "{\"estado\":\"error\",\"mensaje\":\"servicio desconocido\"}");
            cJSON_Delete(req); continue;
        }

        const char *svc = jsvc->valuestring;
        const char *op  = (jop && cJSON_IsString(jop)) ? jop->valuestring : "";

        /* ── Operación propia del controlador ── */
        if (strcmp(svc, "ctrllt") == 0) {
            if (strcmp(op, "Terminar") == 0) {
                do_terminar();
                msg_send(g_fd_cli_resp, "{\"estado\":\"ok\"}");
                running = 0;
            } else {
                msg_send(g_fd_cli_resp,
                         "{\"estado\":\"error\",\"mensaje\":\"operacion ctrllt desconocida\"}");
            }
            cJSON_Delete(req); continue;
        }

        /* ── Seleccionar servicio destino ── */
        svc_fds_t *dest = NULL;
        if      (strcmp(svc, "gesfich")  == 0) dest = &g_gesfich;
        else if (strcmp(svc, "gesprog")  == 0) dest = &g_gesprog;
        else if (strcmp(svc, "ejecutor") == 0) dest = &g_ejecutor;

        if (!dest) {
            msg_send(g_fd_cli_resp, "{\"estado\":\"error\",\"mensaje\":\"servicio desconocido\"}");
            cJSON_Delete(req); continue;
        }
        if (dest->req < 0) {
            msg_send(g_fd_cli_resp, "{\"estado\":\"error\",\"mensaje\":\"servicio no conectado\"}");
            cJSON_Delete(req); continue;
        }

        /* Reenviar mensaje al servicio */
        if (msg_send(dest->req, buf) < 0) {
            msg_send(g_fd_cli_resp,
                     "{\"estado\":\"error\",\"mensaje\":\"error enviando solicitud al servicio\"}");
            cJSON_Delete(req); continue;
        }

        /* Leer respuesta del servicio */
        int resp_fd = (dest->resp >= 0) ? dest->resp : dest->req;
        ssize_t rn = msg_recv(resp_fd, resp, sizeof resp);
        if (rn < 0) {
            msg_send(g_fd_cli_resp,
                     "{\"estado\":\"error\",\"mensaje\":\"error leyendo respuesta del servicio\"}");
            cJSON_Delete(req); continue;
        }

        /* Reenviar respuesta al cliente */
        msg_send(g_fd_cli_resp, resp);

        cJSON_Delete(req);
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    char *fifo_cli_req  = NULL;
    char *fifo_cli_resp = NULL;
    char *fifo_gf_req   = NULL, *fifo_gf_resp  = NULL;
    char *fifo_gp_req   = NULL, *fifo_gp_resp  = NULL;
    char *fifo_ej_req   = NULL, *fifo_ej_resp  = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "c:a:f:b:p:q:e:d:")) != -1) {
        switch (opt) {
            case 'c': fifo_cli_req  = optarg; break;
            case 'a': fifo_cli_resp = optarg; break;
            case 'f': fifo_gf_req   = optarg; break;
            case 'b': fifo_gf_resp  = optarg; break;
            case 'p': fifo_gp_req   = optarg; break;
            case 'q': fifo_gp_resp  = optarg; break;
            case 'e': fifo_ej_req   = optarg; break;
            case 'd': fifo_ej_resp  = optarg; break;
            default:
                fprintf(stderr,
                    "Uso: ctrllt -c <cli-req> [-a <cli-resp>]\n"
                    "            -f <gf-req>  [-b <gf-resp>]\n"
                    "            -p <gp-req>  [-q <gp-resp>]\n"
                    "            -e <ej-req>  [-d <ej-resp>]\n");
                return 1;
        }
    }
    if (!fifo_cli_req || !fifo_gf_req || !fifo_gp_req || !fifo_ej_req) {
        fprintf(stderr, "ctrllt: faltan opciones obligatorias -c -f -p -e\n");
        return 1;
    }

    /* ctrllt crea sus propias tuberías de cliente */
    g_fd_cli_req = open_fifo(fifo_cli_req, 1);
    if (g_fd_cli_req < 0) return 1;

    if (fifo_cli_resp) {
        g_fd_cli_resp = open_fifo(fifo_cli_resp, 1);
        if (g_fd_cli_resp < 0) return 1;
    } else {
        g_fd_cli_resp = g_fd_cli_req;
    }

    /* Abrir tuberías hacia los servicios (ya creadas por cada servicio) */
    g_gesfich.req  = open_fifo(fifo_gf_req,  0);
    g_gesfich.resp = fifo_gf_resp ? open_fifo(fifo_gf_resp, 0) : g_gesfich.req;

    g_gesprog.req  = open_fifo(fifo_gp_req,  0);
    g_gesprog.resp = fifo_gp_resp ? open_fifo(fifo_gp_resp, 0) : g_gesprog.req;

    g_ejecutor.req  = open_fifo(fifo_ej_req, 0);
    g_ejecutor.resp = fifo_ej_resp ? open_fifo(fifo_ej_resp, 0) : g_ejecutor.req;

    fprintf(stderr,
            "ctrllt: listo.\n"
            "  cliente req=%s resp=%s\n"
            "  gesfich req=%s resp=%s\n"
            "  gesprog req=%s resp=%s\n"
            "  ejecutor req=%s resp=%s\n",
            fifo_cli_req,  fifo_cli_resp ? fifo_cli_resp : "(same)",
            fifo_gf_req,   fifo_gf_resp  ? fifo_gf_resp  : "(same)",
            fifo_gp_req,   fifo_gp_resp  ? fifo_gp_resp  : "(same)",
            fifo_ej_req,   fifo_ej_resp  ? fifo_ej_resp  : "(same)");

    run();

    /* Cerrar descriptores */
    if (g_fd_cli_req  >= 0) close(g_fd_cli_req);
    if (g_fd_cli_resp >= 0 && g_fd_cli_resp != g_fd_cli_req) close(g_fd_cli_resp);
    if (g_gesfich.req  >= 0) close(g_gesfich.req);
    if (g_gesfich.resp >= 0 && g_gesfich.resp != g_gesfich.req) close(g_gesfich.resp);
    if (g_gesprog.req  >= 0) close(g_gesprog.req);
    if (g_gesprog.resp >= 0 && g_gesprog.resp != g_gesprog.req) close(g_gesprog.resp);
    if (g_ejecutor.req  >= 0) close(g_ejecutor.req);
    if (g_ejecutor.resp >= 0 && g_ejecutor.resp != g_ejecutor.req) close(g_ejecutor.resp);

    return 0;
}
