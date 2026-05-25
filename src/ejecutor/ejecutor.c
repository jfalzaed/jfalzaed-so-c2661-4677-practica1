/*
 * ejecutor — Ejecutor de Procesos por Lotes
 *
 * Sinopsis:
 *   ejecutor -e <tuberia-req> [-d <tuberia-resp>] -x <aralmac>
 *
 * Protocolo (PDF §3.11):
 *   Ejecutar : {"servicio":"ejecutor","operacion":"Ejecutar",
 *               "id-programa":"p-0001",
 *               "stdin":"f-0001","stdout":"f-0002","stderr":"f-0003"}
 *   Estado   : {"servicio":"ejecutor","operacion":"Estado"[,"id-ejecucion":"e-0001"]}
 *   Matar    : {"servicio":"ejecutor","operacion":"Matar","id-ejecucion":"e-0001"}
 *   Suspender/Reasumir/Parar: sin parámetros
 *
 * Máquina de estados del servicio:
 *   Ejecutar ↔ Suspendidos → Parar → (auto) Terminado
 *
 * Estado de cada proceso:
 *   Ejecutando → Suspendido | Terminado
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>

#include "cJSON.h"
#include "proto.h"

/* ── Constantes ───────────────────────────────────────────────────────────── */
#define MAX_PROCS 1024
#define MAX_ID    9999
#define MAX_ARGS  256
#define MAX_ENV   256

/* ── Estado del proceso individual ───────────────────────────────────────── */
typedef enum {
    PROC_EJECUTANDO,
    PROC_SUSPENDIDO,
    PROC_TERMINADO
} proc_estado_t;

typedef struct {
    char          id[9];       /* e-XXXX */
    char          id_prog[9];  /* p-XXXX */
    pid_t         pid;
    proc_estado_t estado;
    int           codigo_salida;
    int           activo;      /* 0 = slot libre */
} proceso_t;

/* ── Estado del servicio ─────────────────────────────────────────────────── */
typedef enum {
    SVC_EJECUTAR,
    SVC_SUSPENDIDOS,
    SVC_PARAR,
    SVC_TERMINADO
} svc_estado_t;

/* ── Estado global ────────────────────────────────────────────────────────── */
static proceso_t  g_procs[MAX_PROCS];
static int        g_n_procs   = 0;    /* número de slots usados               */
static int        g_counter   = 0;    /* contador para IDs de ejecución        */
static svc_estado_t g_estado  = SVC_EJECUTAR;
static char       g_aralmac[512];
static int        g_fd_in     = -1;
static int        g_fd_out    = -1;

/* ── SIGCHLD: recolectar hijos terminados ─────────────────────────────────── */
static volatile sig_atomic_t g_sigchld_flag = 0;

static void sigchld_handler(int sig) {
    (void)sig;
    g_sigchld_flag = 1;
}

static void reap_children(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < g_n_procs; i++) {
            if (g_procs[i].activo && g_procs[i].pid == pid) {
                g_procs[i].estado       = PROC_TERMINADO;
                g_procs[i].codigo_salida = WIFEXITED(status)
                                           ? WEXITSTATUS(status)
                                           : 128 + WTERMSIG(status);
            }
        }
    }
}

/* ── Contar procesos activos (no terminados) ─────────────────────────────── */
static int count_active(void) {
    int n = 0;
    for (int i = 0; i < g_n_procs; i++)
        if (g_procs[i].activo && g_procs[i].estado == PROC_EJECUTANDO)
            n++;
    return n;
}

/* ── Rutas en aralmac ────────────────────────────────────────────────────── */
static void path_fichero(const char *id, char *out, size_t sz) {
    snprintf(out, sz, "%s/ficheros/%s", g_aralmac, id);
}

static void path_bin(const char *id_prog, char *out, size_t sz) {
    snprintf(out, sz, "%s/programas/%s/bin", g_aralmac, id_prog);
}

static void path_meta(const char *id_prog, char *out, size_t sz) {
    snprintf(out, sz, "%s/programas/%s/meta.json", g_aralmac, id_prog);
}

/* ── Leer meta.json de un programa ───────────────────────────────────────── */
static cJSON *read_meta(const char *id_prog) {
    char mp[600]; path_meta(id_prog, mp, sizeof mp);
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

/* ── Respuestas ──────────────────────────────────────────────────────────── */
static void send_json(cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) { cJSON_Delete(obj); return; }
    if (strlen(s) + 1 > MSG_MAX_LEN) {
        free(s); cJSON_Delete(obj);
        msg_send(g_fd_out,
                 "{\"estado\":\"error\",\"mensaje\":\"no se pudo ejecutar el programa\"}");
        return;
    }
    if (msg_send(g_fd_out, s) < 0)
        fprintf(stderr, "ejecutor: error escribiendo respuesta\n");
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

/* ── Buscar proceso por id-ejecucion ─────────────────────────────────────── */
static proceso_t *find_proc(const char *id) {
    for (int i = 0; i < g_n_procs; i++)
        if (g_procs[i].activo && strcmp(g_procs[i].id, id) == 0)
            return &g_procs[i];
    return NULL;
}

/* ── Construir objeto JSON de estado de un proceso ───────────────────────── */
static cJSON *proc_to_json(const proceso_t *p) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id-ejecucion", p->id);
    cJSON_AddStringToObject(obj, "id-programa",  p->id_prog);
    const char *est = (p->estado == PROC_EJECUTANDO) ? "Ejecutando"
                    : (p->estado == PROC_SUSPENDIDO) ? "Suspendido"
                    : "Terminado";
    cJSON_AddStringToObject(obj, "proceso-estado", est);
    if (p->estado == PROC_TERMINADO)
        cJSON_AddNumberToObject(obj, "codigo-salida", p->codigo_salida);
    return obj;
}

/* ── Operación: Ejecutar ─────────────────────────────────────────────────── */
static void op_ejecutar(cJSON *req) {
    if (g_sigchld_flag) { reap_children(); g_sigchld_flag = 0; }

    if (g_estado == SVC_SUSPENDIDOS) { send_error("servicio suspendido"); return; }
    if (g_estado == SVC_PARAR)       { send_error("servicio parando");    return; }
    if (g_estado != SVC_EJECUTAR)    { send_error("transicion invalida"); return; }

    cJSON *jid_prog = cJSON_GetObjectItem(req, "id-programa");
    if (!jid_prog || !cJSON_IsString(jid_prog)) {
        send_error("falta campo: id-programa"); return;
    }
    const char *id_prog = jid_prog->valuestring;

    /* Verificar que el programa existe */
    cJSON *meta = read_meta(id_prog);
    if (!meta) { send_error("programa no encontrado"); return; }

    /* Construir argv[] */
    char *argv_buf[MAX_ARGS];
    char  binpath[600];
    path_bin(id_prog, binpath, sizeof binpath);

    argv_buf[0] = binpath;
    int argc = 1;

    cJSON *jargs = cJSON_GetObjectItem(meta, "args");
    if (jargs && cJSON_IsArray(jargs)) {
        int na = cJSON_GetArraySize(jargs);
        for (int i = 0; i < na && argc < MAX_ARGS - 1; i++) {
            cJSON *a = cJSON_GetArrayItem(jargs, i);
            if (cJSON_IsString(a)) argv_buf[argc++] = a->valuestring;
        }
    }
    argv_buf[argc] = NULL;

    /* Construir envp[] */
    char *envp_buf[MAX_ENV + 1];
    int   envc = 0;
    cJSON *jenv = cJSON_GetObjectItem(meta, "env");
    if (jenv && cJSON_IsArray(jenv)) {
        int ne = cJSON_GetArraySize(jenv);
        for (int i = 0; i < ne && envc < MAX_ENV; i++) {
            cJSON *e = cJSON_GetArrayItem(jenv, i);
            if (cJSON_IsString(e)) envp_buf[envc++] = e->valuestring;
        }
    }
    envp_buf[envc] = NULL;

    /* Abrir ficheros de redirección */
    int fd_stdin  = -1, fd_stdout = -1, fd_stderr = -1;
    cJSON *jstdin  = cJSON_GetObjectItem(req, "stdin");
    cJSON *jstdout = cJSON_GetObjectItem(req, "stdout");
    cJSON *jstderr = cJSON_GetObjectItem(req, "stderr");

    if (jstdin && cJSON_IsString(jstdin)) {
        char fp[600]; path_fichero(jstdin->valuestring, fp, sizeof fp);
        fd_stdin = open(fp, O_RDONLY);
        if (fd_stdin < 0) { cJSON_Delete(meta); send_error("no se pudo ejecutar el programa"); return; }
    }
    if (jstdout && cJSON_IsString(jstdout)) {
        char fp[600]; path_fichero(jstdout->valuestring, fp, sizeof fp);
        fd_stdout = open(fp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_stdout < 0) {
            if (fd_stdin >= 0) close(fd_stdin);
            cJSON_Delete(meta); send_error("no se pudo ejecutar el programa"); return;
        }
    }
    if (jstderr && cJSON_IsString(jstderr)) {
        char fp[600]; path_fichero(jstderr->valuestring, fp, sizeof fp);
        fd_stderr = open(fp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_stderr < 0) {
            if (fd_stdin  >= 0) close(fd_stdin);
            if (fd_stdout >= 0) close(fd_stdout);
            cJSON_Delete(meta); send_error("no se pudo ejecutar el programa"); return;
        }
    }

    /* Asignar ID de ejecución */
    if (g_counter >= MAX_ID) {
        if (fd_stdin  >= 0) close(fd_stdin);
        if (fd_stdout >= 0) close(fd_stdout);
        if (fd_stderr >= 0) close(fd_stderr);
        cJSON_Delete(meta); send_error("no se pudo ejecutar el programa"); return;
    }
    g_counter++;
    char eid[9]; snprintf(eid, sizeof eid, "e-%04d", g_counter);

    /* fork */
    pid_t pid = fork();
    if (pid < 0) {
        g_counter--;
        if (fd_stdin  >= 0) close(fd_stdin);
        if (fd_stdout >= 0) close(fd_stdout);
        if (fd_stderr >= 0) close(fd_stderr);
        cJSON_Delete(meta); send_error("no se pudo ejecutar el programa"); return;
    }

    if (pid == 0) {
        /* ── Proceso hijo ── */
        /* Redirigir stdin/stdout/stderr */
        if (fd_stdin >= 0)  { dup2(fd_stdin,  STDIN_FILENO);  close(fd_stdin);  }
        if (fd_stdout >= 0) { dup2(fd_stdout, STDOUT_FILENO); close(fd_stdout); }
        if (fd_stderr >= 0) { dup2(fd_stderr, STDERR_FILENO); close(fd_stderr); }

        /* Cerrar FIFOs del servicio para no heredarlos */
        close(g_fd_in);
        if (g_fd_out != g_fd_in) close(g_fd_out);

        if (envc > 0)
            execve(binpath, argv_buf, envp_buf);
        else
            execv(binpath, argv_buf);

        perror("ejecutor: execv");
        _exit(127);
    }

    /* ── Proceso padre ── */
    if (fd_stdin  >= 0) close(fd_stdin);
    if (fd_stdout >= 0) close(fd_stdout);
    if (fd_stderr >= 0) close(fd_stderr);
    cJSON_Delete(meta);

    /* Registrar proceso */
    if (g_n_procs < MAX_PROCS) {
        proceso_t *p = &g_procs[g_n_procs++];
        strncpy(p->id,      eid,     sizeof p->id - 1);
        strncpy(p->id_prog, id_prog, sizeof p->id_prog - 1);
        p->pid          = pid;
        p->estado       = PROC_EJECUTANDO;
        p->codigo_salida = 0;
        p->activo       = 1;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "estado",      "ok");
    cJSON_AddStringToObject(r, "id-ejecucion", eid);
    send_json(r);
}

/* ── Operación: Estado ───────────────────────────────────────────────────── */
static void op_estado(cJSON *req) {
    if (g_sigchld_flag) { reap_children(); g_sigchld_flag = 0; }
    /* Per Figura 5: Estado solo disponible en estado Ejecutar */
    if (g_estado == SVC_SUSPENDIDOS) { send_error("servicio suspendido"); return; }
    if (g_estado == SVC_PARAR)       { send_error("servicio parando");    return; }

    cJSON *jeid = cJSON_GetObjectItem(req, "id-ejecucion");

    if (jeid && cJSON_IsString(jeid)) {
        proceso_t *p = find_proc(jeid->valuestring);
        if (!p) { send_error("proceso no encontrado"); return; }

        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "estado", "ok");
        /* Fusionar campos de proceso */
        cJSON *pj = proc_to_json(p);
        cJSON *item = pj->child;
        while (item) { cJSON_AddItemToObject(r, item->string, cJSON_Duplicate(item, 1)); item = item->next; }
        cJSON_Delete(pj);
        send_json(r);
    } else {
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < g_n_procs; i++) {
            if (g_procs[i].activo)
                cJSON_AddItemToArray(arr, proc_to_json(&g_procs[i]));
        }
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "estado",   "ok");
        cJSON_AddItemToObject(r, "procesos", arr);
        send_json(r);
    }
}

/* ── Operación: Matar ────────────────────────────────────────────────────── */
static void op_matar(cJSON *req) {
    if (g_sigchld_flag) { reap_children(); g_sigchld_flag = 0; }
    /* Per Figura 5: Matar solo disponible en estado Ejecutar */
    if (g_estado == SVC_SUSPENDIDOS) { send_error("servicio suspendido"); return; }
    if (g_estado == SVC_PARAR)       { send_error("servicio parando");    return; }

    cJSON *jeid = cJSON_GetObjectItem(req, "id-ejecucion");
    if (!jeid || !cJSON_IsString(jeid)) { send_error("falta campo: id-ejecucion"); return; }

    proceso_t *p = find_proc(jeid->valuestring);
    if (!p) { send_error("proceso no encontrado"); return; }
    if (p->estado == PROC_TERMINADO) { send_error("proceso no encontrado o ya terminado"); return; }

    kill(p->pid, SIGKILL);
    /* waitpid sin bloquear; el SIGCHLD lo actualizará luego */
    int status;
    if (waitpid(p->pid, &status, WNOHANG) > 0) {
        p->estado       = PROC_TERMINADO;
        p->codigo_salida = 128 + SIGKILL;
    }
    send_ok();
}

/* ── Operación: Suspender (servicio) ─────────────────────────────────────── */
static void op_suspender(void) {
    if (g_estado != SVC_EJECUTAR) { send_error("transicion invalida"); return; }
    /* Enviar SIGSTOP a todos los hijos activos */
    for (int i = 0; i < g_n_procs; i++) {
        if (g_procs[i].activo && g_procs[i].estado == PROC_EJECUTANDO) {
            kill(g_procs[i].pid, SIGSTOP);
            g_procs[i].estado = PROC_SUSPENDIDO;
        }
    }
    g_estado = SVC_SUSPENDIDOS;
    send_ok();
}

/* ── Operación: Reasumir (servicio) ──────────────────────────────────────── */
static void op_reasumir(void) {
    if (g_estado != SVC_SUSPENDIDOS) { send_error("transicion invalida"); return; }
    for (int i = 0; i < g_n_procs; i++) {
        if (g_procs[i].activo && g_procs[i].estado == PROC_SUSPENDIDO) {
            kill(g_procs[i].pid, SIGCONT);
            g_procs[i].estado = PROC_EJECUTANDO;
        }
    }
    g_estado = SVC_EJECUTAR;
    send_ok();
}

/* ── Operación: Parar ────────────────────────────────────────────────────── */
static void op_parar(void) {
    if (g_estado != SVC_EJECUTAR && g_estado != SVC_SUSPENDIDOS) {
        send_error("transicion invalida"); return;
    }
    /* Reanudar procesos suspendidos para que puedan terminar solos */
    for (int i = 0; i < g_n_procs; i++) {
        if (g_procs[i].activo && g_procs[i].estado == PROC_SUSPENDIDO) {
            kill(g_procs[i].pid, SIGCONT);
            g_procs[i].estado = PROC_EJECUTANDO;
        }
    }
    g_estado = SVC_PARAR;
    send_ok();
    /* La transición a Terminado ocurre en el bucle principal */
}

/* ── Esperar en estado PARAR a que todos los procesos terminen ───────────── */
static void wait_for_parar(void) {
    /* Polling: revisamos cada 10 ms si quedan procesos activos.
     * SIGCHLD actualiza g_sigchld_flag; nosotros recolectamos hijos aquí
     * sin depender de SA_RESTART. */
    while (count_active() > 0) {
        if (g_sigchld_flag) { reap_children(); g_sigchld_flag = 0; }
        usleep(10000);  /* 10 ms */
    }
    g_estado = SVC_TERMINADO;
}

/* ── Bucle principal ─────────────────────────────────────────────────────── */
static void run(void) {
    char buf[MSG_MAX_LEN];

    /* Configurar handler de SIGCHLD sin SA_RESTART para que el read()
     * sea interrumpible por la señal. */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;          /* sin SA_RESTART */
    sigaction(SIGCHLD, &sa, NULL);

    while (g_estado != SVC_TERMINADO) {

        /* Recolectar hijos si hay señales pendientes */
        if (g_sigchld_flag) { reap_children(); g_sigchld_flag = 0; }

        /* Transición automática Parar → Terminado cuando no quedan procesos */
        if (g_estado == SVC_PARAR) {
            if (count_active() == 0) { g_estado = SVC_TERMINADO; break; }
            /* Aún hay procesos activos: esperar bloqueado hasta que terminen */
            wait_for_parar();
            break;
        }

        ssize_t n = msg_recv(g_fd_in, buf, sizeof buf);
        /* EINTR por SIGCHLD: recolectar hijos y reintentar */
        if (n < 0) {
            if (g_sigchld_flag) { reap_children(); g_sigchld_flag = 0; }
            continue;
        }
        if (n == 0) break;   /* EOF */

        if (g_sigchld_flag) { reap_children(); g_sigchld_flag = 0; }

        cJSON *req = cJSON_Parse(buf);
        if (!req) { send_error("json invalido"); continue; }

        cJSON *jop = cJSON_GetObjectItem(req, "operacion");
        if (!jop || !cJSON_IsString(jop)) {
            send_error("operacion desconocida"); cJSON_Delete(req); continue;
        }
        const char *op = jop->valuestring;

        if (strcmp(op, "Ejecutar") == 0) {
            op_ejecutar(req);
        } else if (strcmp(op, "Estado") == 0) {
            op_estado(req);
        } else if (strcmp(op, "Matar") == 0) {
            op_matar(req);
        } else if (strcmp(op, "Suspender") == 0) {
            op_suspender();
        } else if (strcmp(op, "Reasumir") == 0) {
            op_reasumir();
        } else if (strcmp(op, "Parar") == 0) {
            op_parar();
        } else {
            send_error("operacion desconocida");
        }

        cJSON_Delete(req);
    }

    /* Esperar a todos los hijos antes de salir */
    while (wait(NULL) > 0)
        ;
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    char *fifo_req  = NULL;
    char *fifo_resp = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "e:d:x:")) != -1) {
        switch (opt) {
            case 'e': fifo_req  = optarg; break;
            case 'd': fifo_resp = optarg; break;
            case 'x': strncpy(g_aralmac, optarg, sizeof g_aralmac - 1); break;
            default:
                fprintf(stderr, "Uso: ejecutor -e <tuberia-req> [-d <tuberia-resp>] -x <aralmac>\n");
                return 1;
        }
    }
    if (!fifo_req || !g_aralmac[0]) {
        fprintf(stderr, "ejecutor: faltan opciones -e y -x\n"); return 1;
    }

    mkfifo(fifo_req, 0666);
    g_fd_in = open(fifo_req, O_RDWR);
    if (g_fd_in < 0) { perror("ejecutor: open req"); return 1; }

    if (fifo_resp) {
        mkfifo(fifo_resp, 0666);
        g_fd_out = open(fifo_resp, O_RDWR);
        if (g_fd_out < 0) { perror("ejecutor: open resp"); return 1; }
    } else {
        g_fd_out = g_fd_in;
    }

    fprintf(stderr, "ejecutor: listo. aralmac=%s req=%s resp=%s\n",
            g_aralmac, fifo_req, fifo_resp ? fifo_resp : "(same)");

    run();

    if (g_fd_in  >= 0) close(g_fd_in);
    if (g_fd_out >= 0 && g_fd_out != g_fd_in) close(g_fd_out);
    return 0;
}
