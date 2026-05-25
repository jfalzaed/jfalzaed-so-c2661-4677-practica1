/*
 * gesfich — Gestor de Ficheros
 *
 * Sinopsis:
 *   gesfich -f <tuberia-req> [-b <tuberia-resp>] -x <aralmac>
 *
 * Protocolo (PDF §3.9):
 *   Petición : {"servicio":"gesfich","operacion":"<Op>"[,...]}
 *   Respuesta: {"estado":"ok"[,...]} | {"estado":"error","mensaje":"..."}
 *
 * Máquina de estados: Corriendo ↔ Suspendido → Terminado
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <dirent.h>

#include "cJSON.h"
#include "proto.h"

/* ── Constantes ───────────────────────────────────────────────────────────── */
#define MAX_ID 9999

typedef enum { ST_CORRIENDO, ST_SUSPENDIDO, ST_TERMINADO } estado_t;

/* ── Estado global ────────────────────────────────────────────────────────── */
static estado_t g_estado  = ST_CORRIENDO;
static char     g_aralmac[512];   /* directorio raíz de almacenamiento        */
static int      g_fd_in   = -1;   /* FIFO de peticiones                       */
static int      g_fd_out  = -1;   /* FIFO de respuestas                       */

/* ── Utilidades de ruta ───────────────────────────────────────────────────── */
static void dir_ficheros(char *out, size_t sz) {
    snprintf(out, sz, "%s/ficheros", g_aralmac);
}

static void path_for_id(const char *id, char *out, size_t sz) {
    snprintf(out, sz, "%s/ficheros/%s", g_aralmac, id);
}

static void path_counter(char *out, size_t sz) {
    snprintf(out, sz, "%s/ficheros/.gesfich_counter", g_aralmac);
}

/* ── Contador persistente ────────────────────────────────────────────────── */
static int load_counter(void) {
    char p[600]; path_counter(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int n = 0; fscanf(f, "%d", &n); fclose(f);
    return n;
}

static void save_counter(int n) {
    char p[600]; path_counter(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%d\n", n); fclose(f);
}

/* ── Envío de respuestas ──────────────────────────────────────────────────── */
static void send_json(cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) { cJSON_Delete(obj); return; }
    /* Si la respuesta excede MSG_MAX_LEN el cliente quedaría bloqueado para
     * siempre esperando. Mandamos un error corto en su lugar. */
    if (strlen(s) + 1 > MSG_MAX_LEN) {
        free(s); cJSON_Delete(obj);
        msg_send(g_fd_out,
                 "{\"estado\":\"error\",\"mensaje\":\"no se pudo actualizar el fichero\"}");
        return;
    }
    if (msg_send(g_fd_out, s) < 0)
        fprintf(stderr, "gesfich: error escribiendo respuesta\n");
    free(s);
    cJSON_Delete(obj);
}

static void send_ok(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "estado", "ok");
    send_json(r);
}

static void send_error(const char *msg) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "estado", "error");
    cJSON_AddStringToObject(r, "mensaje", msg);
    send_json(r);
}

/* ── Operación: Crear ────────────────────────────────────────────────────── */
static void op_crear(void) {
    static int counter = -1;
    if (counter < 0) counter = load_counter();

    if (counter >= MAX_ID) { send_error("no se pudo crear el fichero"); return; }

    counter++;
    char id[9]; snprintf(id, sizeof id, "f-%04d", counter);

    char path[600]; path_for_id(id, path, sizeof path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { send_error("no se pudo crear el fichero"); counter--; return; }
    close(fd);
    save_counter(counter);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "estado",     "ok");
    cJSON_AddStringToObject(r, "id-fichero", id);
    send_json(r);
}

/* ── Operación: Leer ─────────────────────────────────────────────────────── */
static void op_leer(cJSON *req) {
    cJSON *jid = cJSON_GetObjectItem(req, "id-fichero");

    if (jid && cJSON_IsString(jid)) {
        /* Leer contenido de un fichero concreto */
        const char *id = jid->valuestring;
        char path[600]; path_for_id(id, path, sizeof path);

        struct stat st;
        if (stat(path, &st) < 0) { send_error("fichero no encontrado"); return; }

        /* Leer contenido completo */
        FILE *f = fopen(path, "r");
        if (!f) { send_error("fichero no encontrado"); return; }

        size_t sz = (size_t)st.st_size;
        /* El contenido + overhead JSON debe caber en MSG_MAX_LEN.
         * En el peor caso cJSON escapa cada byte (ej. todos son '\'), duplicando
         * el tamaño. Usamos MSG_MAX_LEN/2 como límite seguro. */
        if (sz > (MSG_MAX_LEN / 2)) {
            fclose(f);
            send_error("no se pudo actualizar el fichero");
            return;
        }
        char *buf = calloc(sz + 1, 1);
        if (!buf) { fclose(f); send_error("no se pudo crear el fichero"); return; }
        size_t got = fread(buf, 1, sz, f);
        buf[got] = '\0';
        fclose(f);

        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "estado",    "ok");
        cJSON_AddStringToObject(r, "contenido", buf);
        free(buf);
        send_json(r);
    } else {
        /* Listar todos los ficheros */
        char dirp[600]; dir_ficheros(dirp, sizeof dirp);
        DIR *d = opendir(dirp);
        if (!d) { send_error("error al listar ficheros"); return; }

        cJSON *arr = cJSON_CreateArray();
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == 'f' && ent->d_name[1] == '-')
                cJSON_AddItemToArray(arr, cJSON_CreateString(ent->d_name));
        }
        closedir(d);

        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "estado",   "ok");
        cJSON_AddItemToObject(r, "ficheros", arr);
        send_json(r);
    }
}

/* ── Operación: Actualizar ───────────────────────────────────────────────── */
static void op_actualizar(cJSON *req) {
    cJSON *jid   = cJSON_GetObjectItem(req, "id-fichero");
    cJSON *jruta = cJSON_GetObjectItem(req, "ruta");

    if (!jid || !cJSON_IsString(jid) || !jruta || !cJSON_IsString(jruta)) {
        send_error("faltan campos: id-fichero, ruta"); return;
    }
    const char *id   = jid->valuestring;
    const char *ruta = jruta->valuestring;

    char dst[600]; path_for_id(id, dst, sizeof dst);
    struct stat st;
    if (stat(dst, &st) < 0) { send_error("fichero no encontrado"); return; }
    if (stat(ruta, &st) < 0) { send_error("no se pudo actualizar el fichero"); return; }

    /* Copiar contenido */
    FILE *fsrc = fopen(ruta, "rb");
    if (!fsrc) { send_error("no se pudo actualizar el fichero"); return; }
    FILE *fdst = fopen(dst, "wb");
    if (!fdst) { fclose(fsrc); send_error("no se pudo actualizar el fichero"); return; }

    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, fsrc)) > 0)
        fwrite(tmp, 1, n, fdst);
    fclose(fsrc); fclose(fdst);

    send_ok();
}

/* ── Operación: Borrar ───────────────────────────────────────────────────── */
static void op_borrar(cJSON *req) {
    cJSON *jid = cJSON_GetObjectItem(req, "id-fichero");
    if (!jid || !cJSON_IsString(jid)) { send_error("fichero no encontrado"); return; }

    char path[600]; path_for_id(jid->valuestring, path, sizeof path);
    struct stat st;
    if (stat(path, &st) < 0) { send_error("fichero no encontrado"); return; }
    if (unlink(path) < 0)    { send_error("no se pudo actualizar el fichero"); return; }

    send_ok();
}

/* ── Bucle principal ─────────────────────────────────────────────────────── */
static void run(void) {
    char buf[MSG_MAX_LEN];

    while (g_estado != ST_TERMINADO) {
        ssize_t n = msg_recv(g_fd_in, buf, sizeof buf);
        if (n <= 0) break;

        cJSON *req = cJSON_Parse(buf);
        if (!req) { send_error("json invalido"); continue; }

        cJSON *jop = cJSON_GetObjectItem(req, "operacion");
        if (!jop || !cJSON_IsString(jop)) {
            send_error("operacion desconocida"); cJSON_Delete(req); continue;
        }
        const char *op = jop->valuestring;

        /* Operaciones de control de estado — siempre disponibles */
        if (strcmp(op, "Suspender") == 0) {
            if (g_estado != ST_CORRIENDO) send_error("transicion invalida");
            else { g_estado = ST_SUSPENDIDO; send_ok(); }
        } else if (strcmp(op, "Reasumir") == 0) {
            if (g_estado != ST_SUSPENDIDO) send_error("transicion invalida");
            else { g_estado = ST_CORRIENDO; send_ok(); }
        } else if (strcmp(op, "Terminar") == 0) {
            g_estado = ST_TERMINADO; send_ok();
        } else if (g_estado == ST_SUSPENDIDO) {
            /* En suspendido, solo se aceptan operaciones de control */
            send_error("servicio suspendido");
        } else if (strcmp(op, "Crear") == 0) {
            op_crear();
        } else if (strcmp(op, "Leer") == 0) {
            op_leer(req);
        } else if (strcmp(op, "Actualizar") == 0) {
            op_actualizar(req);
        } else if (strcmp(op, "Borrar") == 0) {
            op_borrar(req);
        } else {
            send_error("operacion desconocida");
        }

        cJSON_Delete(req);
    }
}

/* ── Inicialización del área de almacenamiento ───────────────────────────── */
static int init_aralmac(void) {
    char dirp[600]; dir_ficheros(dirp, sizeof dirp);
    struct stat st;
    if (stat(dirp, &st) < 0) {
        if (mkdir(g_aralmac, 0755) < 0 && errno != EEXIST) return -1;
        if (mkdir(dirp, 0755)      < 0 && errno != EEXIST) return -1;
    }
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    char *fifo_req  = NULL;
    char *fifo_resp = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "f:b:x:")) != -1) {
        switch (opt) {
            case 'f': fifo_req  = optarg; break;
            case 'b': fifo_resp = optarg; break;
            case 'x': strncpy(g_aralmac, optarg, sizeof g_aralmac - 1); break;
            default:
                fprintf(stderr, "Uso: gesfich -f <tuberia-req> [-b <tuberia-resp>] -x <aralmac>\n");
                return 1;
        }
    }
    if (!fifo_req || !g_aralmac[0]) {
        fprintf(stderr, "gesfich: faltan opciones -f y -x\n"); return 1;
    }

    /* Crear directorio aralmac si no existe */
    if (init_aralmac() < 0) {
        perror("gesfich: init_aralmac"); return 1;
    }

    /* Crear y abrir FIFOs */
    mkfifo(fifo_req, 0666); /* ignorar EEXIST */
    g_fd_in = open(fifo_req, O_RDWR);
    if (g_fd_in < 0) { perror("gesfich: open req"); return 1; }

    if (fifo_resp) {
        mkfifo(fifo_resp, 0666);
        g_fd_out = open(fifo_resp, O_RDWR);
        if (g_fd_out < 0) { perror("gesfich: open resp"); return 1; }
    } else {
        /* Full-duplex: misma tubería para entrada y salida */
        g_fd_out = g_fd_in;
    }

    fprintf(stderr, "gesfich: listo. aralmac=%s req=%s resp=%s\n",
            g_aralmac, fifo_req, fifo_resp ? fifo_resp : "(same)");

    run();

    if (g_fd_in  >= 0) close(g_fd_in);
    if (g_fd_out >= 0 && g_fd_out != g_fd_in) close(g_fd_out);
    return 0;
}
