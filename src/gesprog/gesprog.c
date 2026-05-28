/*
 * gesprog — Gestor de Programas
 *
 * Sinopsis:
 *   gesprog -p <tuberia-req> [-c <tuberia-resp>] -x <aralmac>
 *
 * Protocolo (PDF §3.10):
 *   Operaciones: Guardar, Leer, Actualizar, Borrar, Suspender, Reasumir, Terminar
 *
 * Almacenamiento en aralmac:
 *   <aralmac>/programas/p-XXXX/bin      — copia del ejecutable
 *   <aralmac>/programas/p-XXXX/meta.json — metadatos (nombre, args, env)
 *
 * Máquina de estados: Corriendo ↔ Suspendido → Terminado
 *   (Leer es válido en Suspendido — self-loop "Leer" de la Figura 4 del PDF)
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
#include <libgen.h>

#include "cJSON.h"
#include "proto.h"

#define MAX_ID 9999

typedef enum { ST_CORRIENDO, ST_SUSPENDIDO, ST_TERMINADO } estado_t;

static estado_t g_estado  = ST_CORRIENDO;
static char     g_aralmac[512];
static int      g_fd_in   = -1;
static int      g_fd_out  = -1;

/* ── Rutas ────────────────────────────────────────────────────────────────── */
static void dir_programas(char *out, size_t sz) {
    snprintf(out, sz, "%s/programas", g_aralmac);
}

static void dir_prog(const char *id, char *out, size_t sz) {
    snprintf(out, sz, "%s/programas/%s", g_aralmac, id);
}

static void path_bin(const char *id, char *out, size_t sz) {
    snprintf(out, sz, "%s/programas/%s/bin", g_aralmac, id);
}

static void path_meta(const char *id, char *out, size_t sz) {
    snprintf(out, sz, "%s/programas/%s/meta.json", g_aralmac, id);
}

static void path_counter(char *out, size_t sz) {
    snprintf(out, sz, "%s/programas/.gesprog_counter", g_aralmac);
}

/* ── Contador persistente ─────────────────────────────────────────────────── */
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

/* ── Respuestas ───────────────────────────────────────────────────────────── */
static void send_json(cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) { cJSON_Delete(obj); return; }
    if (strlen(s) + 1 > MSG_MAX_LEN) {
        free(s); cJSON_Delete(obj);
        msg_send(g_fd_out,
                 "{\"estado\":\"error\",\"mensaje\":\"no se pudo actualizar el programa\"}");
        return;
    }
    if (msg_send(g_fd_out, s) < 0)
        fprintf(stderr, "gesprog: error escribiendo respuesta\n");
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

/* ── Copiar archivo en el sistema de ficheros ─────────────────────────────── */
static int copy_file(const char *src, const char *dst) {
    FILE *fsrc = fopen(src, "rb");
    if (!fsrc) return -1;
    FILE *fdst = fopen(dst, "wb");
    if (!fdst) { fclose(fsrc); return -1; }

    char tmp[4096]; size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, fsrc)) > 0)
        fwrite(tmp, 1, n, fdst);
    fclose(fsrc); fclose(fdst);

    /* Preservar bits de ejecución */
    struct stat st;
    if (stat(src, &st) == 0)
        chmod(dst, st.st_mode | 0111);
    return 0;
}

/* ── Leer meta.json de un programa ───────────────────────────────────────── */
static cJSON *read_meta(const char *id) {
    char mp[600]; path_meta(id, mp, sizeof mp);
    FILE *f = fopen(mp, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

/* ── Operación: Guardar ───────────────────────────────────────────────────── */
static void op_guardar(cJSON *req) {
    static int counter = -1;
    if (counter < 0) counter = load_counter();

    cJSON *jexe = cJSON_GetObjectItem(req, "ejecutable");
    if (!jexe || !cJSON_IsString(jexe)) { send_error("falta campo: ejecutable"); return; }
    const char *exe = jexe->valuestring;

    struct stat st;
    if (stat(exe, &st) < 0) { send_error("no se pudo guardar el programa"); return; }
    if (!(st.st_mode & S_IXUSR) && !(st.st_mode & S_IXGRP) && !(st.st_mode & S_IXOTH)) {
        /* Sin bits de ejecución: solo se acepta si es un guión con shebang (#!).
         * Si no se puede leer para verificarlo, se rechaza. */
        FILE *f = fopen(exe, "r");
        if (!f) { send_error("no se pudo guardar el programa"); return; }
        char shebang[3] = {0};
        size_t nr = fread(shebang, 1, 2, f); fclose(f);
        if (nr < 2 || shebang[0] != '#' || shebang[1] != '!') {
            send_error("no se pudo guardar el programa"); return;
        }
    }

    if (counter >= MAX_ID) { send_error("no se pudo guardar el programa"); return; }
    counter++;
    char id[9]; snprintf(id, sizeof id, "p-%04d", counter);

    /* Crear directorio p-XXXX */
    char dpath[600]; dir_prog(id, dpath, sizeof dpath);
    if (mkdir(dpath, 0755) < 0) { send_error("no se pudo guardar el programa"); counter--; return; }

    /* Copiar ejecutable */
    char bpath[600]; path_bin(id, bpath, sizeof bpath);
    if (copy_file(exe, bpath) < 0) {
        rmdir(dpath); send_error("no se pudo guardar el programa"); counter--; return;
    }

    /* Guardar metadatos */
    cJSON *meta = cJSON_CreateObject();
    /* nombre = basename del ejecutable (sin modificar la cadena original) */
    char exe_copy[512]; strncpy(exe_copy, exe, sizeof exe_copy - 1);
    cJSON_AddStringToObject(meta, "id-programa", id);
    cJSON_AddStringToObject(meta, "nombre", basename(exe_copy));
    cJSON_AddStringToObject(meta, "ejecutable", exe);

    cJSON *jargs = cJSON_GetObjectItem(req, "args");
    cJSON *jenv  = cJSON_GetObjectItem(req, "env");
    cJSON_AddItemToObject(meta, "args", jargs ? cJSON_Duplicate(jargs, 1) : cJSON_CreateArray());
    cJSON_AddItemToObject(meta, "env",  jenv  ? cJSON_Duplicate(jenv,  1) : cJSON_CreateArray());

    char mp[600]; path_meta(id, mp, sizeof mp);
    char *ms = cJSON_PrintUnformatted(meta);
    FILE *mf = fopen(mp, "w");
    if (mf) { fputs(ms, mf); fclose(mf); }
    free(ms); cJSON_Delete(meta);

    save_counter(counter);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "estado",     "ok");
    cJSON_AddStringToObject(r, "id-programa", id);
    send_json(r);
}

/* ── Operación: Leer ──────────────────────────────────────────────────────── */
static void op_leer(cJSON *req) {
    cJSON *jid = cJSON_GetObjectItem(req, "id-programa");

    if (jid && cJSON_IsString(jid)) {
        const char *id = jid->valuestring;
        char dp[600]; dir_prog(id, dp, sizeof dp);
        struct stat st;
        if (stat(dp, &st) < 0) { send_error("programa no encontrado"); return; }

        cJSON *meta = read_meta(id);
        if (!meta) { send_error("programa no encontrado"); return; }

        /* §3.10.3: el objeto programa expone solo id-programa, nombre, args, env */
        cJSON_DeleteItemFromObject(meta, "ejecutable");

        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "estado", "ok");
        cJSON_AddItemToObject(r, "programa", meta);
        send_json(r);
    } else {
        /* Listar todos */
        char dp[600]; dir_programas(dp, sizeof dp);
        DIR *d = opendir(dp);
        if (!d) { send_error("error al listar programas"); return; }

        cJSON *arr = cJSON_CreateArray();
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == 'p' && ent->d_name[1] == '-')
                cJSON_AddItemToArray(arr, cJSON_CreateString(ent->d_name));
        }
        closedir(d);

        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "estado",    "ok");
        cJSON_AddItemToObject(r, "programas", arr);
        send_json(r);
    }
}

/* ── Operación: Actualizar ────────────────────────────────────────────────── */
static void op_actualizar(cJSON *req) {
    cJSON *jid   = cJSON_GetObjectItem(req, "id-programa");
    cJSON *jruta = cJSON_GetObjectItem(req, "ruta");
    if (!jid || !cJSON_IsString(jid) || !jruta || !cJSON_IsString(jruta)) {
        send_error("faltan campos: id-programa, ruta"); return;
    }
    const char *id   = jid->valuestring;
    const char *ruta = jruta->valuestring;

    char dp[600]; dir_prog(id, dp, sizeof dp);
    struct stat st;
    if (stat(dp, &st) < 0) { send_error("programa no encontrado"); return; }
    if (stat(ruta, &st) < 0) { send_error("no se pudo actualizar el programa"); return; }

    /* Validar que la nueva ruta es ejecutable (igual que en Guardar) */
    if (!(st.st_mode & S_IXUSR) && !(st.st_mode & S_IXGRP) && !(st.st_mode & S_IXOTH)) {
        FILE *fchk = fopen(ruta, "r");
        if (fchk) {
            char sh[3] = {0};
            size_t nr = fread(sh, 1, 2, fchk); fclose(fchk);
            if (nr < 2 || sh[0] != '#' || sh[1] != '!') {
                send_error("no se pudo actualizar el programa"); return;
            }
        } else {
            send_error("no se pudo actualizar el programa"); return;
        }
    }

    char bpath[600]; path_bin(id, bpath, sizeof bpath);
    if (copy_file(ruta, bpath) < 0) { send_error("no se pudo actualizar el programa"); return; }

    /* Actualizar ruta en meta.json */
    cJSON *meta = read_meta(id);
    if (meta) {
        cJSON *je = cJSON_GetObjectItem(meta, "ejecutable");
        if (je && cJSON_IsString(je)) {
            cJSON_SetValuestring(je, ruta);
            /* actualizar nombre */
            char rc[512]; strncpy(rc, ruta, sizeof rc - 1);
            cJSON *jn = cJSON_GetObjectItem(meta, "nombre");
            if (jn && cJSON_IsString(jn)) cJSON_SetValuestring(jn, basename(rc));
        }
        char mp[600]; path_meta(id, mp, sizeof mp);
        char *ms = cJSON_PrintUnformatted(meta);
        FILE *mf = fopen(mp, "w");
        if (mf) { fputs(ms, mf); fclose(mf); }
        free(ms); cJSON_Delete(meta);
    }

    send_ok();
}

/* ── Borrar directorio recursivamente (solo 1 nivel) ─────────────────────── */
/* Retorna 0 si el directorio quedó completamente eliminado, -1 si algún
 * unlink() o el rmdir() final falló (deja constancia para el caller). */
static int rmdir_prog(const char *id) {
    char dp[600]; dir_prog(id, dp, sizeof dp);
    DIR *d = opendir(dp);
    if (!d) return -1;
    struct dirent *ent;
    int rc = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char fp[700]; snprintf(fp, sizeof fp, "%s/%s", dp, ent->d_name);
        if (unlink(fp) < 0) rc = -1;
    }
    closedir(d);
    if (rmdir(dp) < 0) rc = -1;
    return rc;
}

/* ── Operación: Borrar ────────────────────────────────────────────────────── */
static void op_borrar(cJSON *req) {
    cJSON *jid = cJSON_GetObjectItem(req, "id-programa");
    if (!jid || !cJSON_IsString(jid)) { send_error("programa no encontrado"); return; }
    const char *id = jid->valuestring;
    char dp[600]; dir_prog(id, dp, sizeof dp);
    struct stat st;
    if (stat(dp, &st) < 0) { send_error("programa no encontrado"); return; }
    if (rmdir_prog(id) < 0) { send_error("no se pudo actualizar el programa"); return; }
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

        if (strcmp(op, "Suspender") == 0) {
            if (g_estado != ST_CORRIENDO) send_error("transicion invalida");
            else { g_estado = ST_SUSPENDIDO; send_ok(); }
        } else if (strcmp(op, "Reasumir") == 0) {
            if (g_estado != ST_SUSPENDIDO) send_error("transicion invalida");
            else { g_estado = ST_CORRIENDO; send_ok(); }
        } else if (strcmp(op, "Terminar") == 0) {
            g_estado = ST_TERMINADO; send_ok();
        } else if (strcmp(op, "Leer") == 0) {
            /* Leer es válido también en estado Suspendido (self-loop Figura 4) */
            op_leer(req);
        } else if (g_estado == ST_SUSPENDIDO) {
            send_error("servicio suspendido");
        } else if (strcmp(op, "Guardar") == 0) {
            op_guardar(req);
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

/* ── Inicialización ───────────────────────────────────────────────────────── */
static int init_aralmac(void) {
    char dp[600]; dir_programas(dp, sizeof dp);
    struct stat st;
    if (stat(dp, &st) < 0) {
        if (mkdir(g_aralmac, 0755) < 0 && errno != EEXIST) return -1;
        if (mkdir(dp, 0755)        < 0 && errno != EEXIST) return -1;
    }
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    char *fifo_req  = NULL;
    char *fifo_resp = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:c:x:")) != -1) {
        switch (opt) {
            case 'p': fifo_req  = optarg; break;
            case 'c': fifo_resp = optarg; break;
            case 'x': strncpy(g_aralmac, optarg, sizeof g_aralmac - 1); break;
            default:
                fprintf(stderr, "Uso: gesprog -p <tuberia-req> [-c <tuberia-resp>] -x <aralmac>\n");
                return 1;
        }
    }
    if (!fifo_req || !g_aralmac[0]) {
        fprintf(stderr, "gesprog: faltan opciones -p y -x\n"); return 1;
    }

    if (init_aralmac() < 0) { perror("gesprog: init_aralmac"); return 1; }

    mkfifo(fifo_req, 0666);
    g_fd_in = open(fifo_req, O_RDWR);
    if (g_fd_in < 0) { perror("gesprog: open req"); return 1; }

    if (fifo_resp) {
        mkfifo(fifo_resp, 0666);
        g_fd_out = open(fifo_resp, O_RDWR);
        if (g_fd_out < 0) { perror("gesprog: open resp"); return 1; }
    } else {
        g_fd_out = g_fd_in;
    }

    fprintf(stderr, "gesprog: listo. aralmac=%s req=%s resp=%s\n",
            g_aralmac, fifo_req, fifo_resp ? fifo_resp : "(same)");

    run();

    if (g_fd_in  >= 0) close(g_fd_in);
    if (g_fd_out >= 0 && g_fd_out != g_fd_in) close(g_fd_out);
    return 0;
}
