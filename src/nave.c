#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <ncurses.h>
#include <mqueue.h>
#include <semaphore.h>
#include <fcntl.h>
#include <errno.h>

#include "mapa.h"
#include "movement.h"
#include "nave.h"
#include "recursos.h"
#include "estacion.h"

/* ─── Parámetros de juego ──────────────────────────────────────── */
#define FUEL_MAX          100
#define FUEL_UMBRAL        20
#define FUEL_DECREMENTO     1
#define FUEL_INTERVALO_MS 3000

#define O2_MAX            100
#define O2_UMBRAL          20
#define O2_DECREMENTO       1
#define O2_INTERVALO_MS  2000

#define COSTO_FUEL_MOV    1
#define COSTO_FUEL_EXT    3

/* ─── Layout ncurses ───────────────────────────────────────────── */
#define PANEL_W   24
#define BAR_IN    10

/* ─── Estado global ────────────────────────────────────────────── */
typedef struct {
    Nave              nave;
    MapaCompartido   *mapa;
    int               x, y;
    volatile sig_atomic_t vivo;
    volatile sig_atomic_t extrayendo;
    pthread_mutex_t   mx_estado;

    /* Gestión del Hangar e IPC */
    bool              en_hangar;
    int               estacion_x;
    int               estacion_y;
    sem_t            *sem_hangar;
    mqd_t             mq_estacion;
    mqd_t             mq_respuesta;
    char              mq_respuesta_name[64];
    char              pid_file[64];

    /* Sincronización para respuestas IPC */
    pthread_cond_t    cond_respuesta;
    bool              hay_respuesta;
    MensajeNave       ultima_respuesta;

    /* Alertas y errores */
    char              alerta_str[80];
    time_t            alerta_recibida;
    char              hud_error[80];
    time_t            hud_error_recibido;
} EstadoNave;

static EstadoNave      g;

/* Mutex que serializa todos los accesos a ncurses (no es thread-safe) */
static pthread_mutex_t g_mx_ncurses = PTHREAD_MUTEX_INITIALIZER;

/* Ventanas ncurses */
static WINDOW *win_hud  = NULL;  /* panel izquierdo                */
static WINDOW *win_mapa = NULL;  /* mapa del juego + borde         */

/* ─── Helpers ──────────────────────────────────────────────────── */
static void dormir_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void game_over(const char *motivo)
{
    pthread_mutex_lock(&g.mx_estado);
    g.vivo = 0;
    pthread_mutex_unlock(&g.mx_estado);
    (void)motivo;
}

/* ─── Inicialización y cierre de ncurses ───────────────────────── */
static void ncurses_init(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);           /* ocultar cursor                        */

    /* Colores */
    if (has_colors()) {
        start_color();
        use_default_colors();
        /* Par 1: texto normal HUD                                  */
        init_pair(1, COLOR_CYAN,   -1);
        /* Par 2: barra combustible llena                           */
        init_pair(2, COLOR_GREEN,  -1);
        /* Par 3: barra combustible baja                            */
        init_pair(3, COLOR_RED,    -1);
        /* Par 4: nave en el mapa (resaltada)                       */
        init_pair(4, COLOR_YELLOW, COLOR_BLUE);
        /* Par 5: asteroide                                         */
        init_pair(5, COLOR_WHITE,  -1);
        /* Par 6: estación espacial                                 */
        init_pair(6, COLOR_MAGENTA, -1);
        /* Par 7: título HUD                                        */
        init_pair(7, COLOR_YELLOW, -1);
        /* Par 8: GAME OVER                                         */
        init_pair(8, COLOR_RED,    COLOR_BLACK);
        /* Par 9: barra O2 normal                                   */
        init_pair(9, COLOR_CYAN,   -1);
    }

    /*
     * Ventana HUD (panel izquierdo)
     * Filas: MAP_ROWS + 2 (borde superior e inferior del mapa)
     * Cols: PANEL_W
     */
    win_hud  = newwin(MAP_ROWS + 2, PANEL_W, 0, 0);

    /*
     * Ventana Mapa: MAP_ROWS filas de contenido + 2 de borde ncurses
     * Cols: MAP_COLS + 2 (borde left/right del box)
     * Posición: columna PANEL_W
     */
    win_mapa = newwin(MAP_ROWS + 2, MAP_COLS + 2, 0, PANEL_W);

    nodelay(stdscr, TRUE); /* getch() no bloquea: devuelve ERR si no hay tecla */
    keypad(stdscr, TRUE);  /* habilitar teclas de flecha en stdscr también      */

    refresh();
}

static void ncurses_end(void)
{
    if (win_hud)  { delwin(win_hud);  win_hud  = NULL; }
    if (win_mapa) { delwin(win_mapa); win_mapa = NULL; }
    endwin();
}

/* ─── Dibujo de barra en win_hud ───────────────────────────────── */
static void hud_barra(int row, int val, int maxv, int color_full, int color_low)
{
    int filled = (maxv > 0) ? (val * BAR_IN / maxv) : 0;
    if (filled > BAR_IN) filled = BAR_IN;

    wmove(win_hud, row, 1);
    waddch(win_hud, '[');

    int low = (val * 100 / (maxv > 0 ? maxv : 1)) < 30;
    for (int i = 0; i < BAR_IN; i++) {
        if (i < filled) {
            wattron(win_hud, COLOR_PAIR(low ? color_low : color_full));
            waddch(win_hud, '=');
            wattroff(win_hud, COLOR_PAIR(low ? color_low : color_full));
        } else {
            waddch(win_hud, ' ');
        }
    }
    waddch(win_hud, ']');
    wprintw(win_hud, " %3d", val);
}

/* ─── Render completo ───────────────────────────────────────────── */
static void render_frame(void)
{
    int fuel = barra_get_valor(&g.nave.barra_combustible);
    int o2   = barra_get_valor(&g.nave.barra_oxigeno);
    time_t ahora = time(NULL);

    /* ── Panel HUD ── */
    werase(win_hud);

    /* Título */
    wattron(win_hud, COLOR_PAIR(7) | A_BOLD);
    mvwprintw(win_hud, 0, 1, " COSMIKERNEL");
    wattroff(win_hud, COLOR_PAIR(7) | A_BOLD);

    wattron(win_hud, COLOR_PAIR(1));

    /* Combustible */
    mvwprintw(win_hud, 2, 1, "Combustible");
    hud_barra(3, fuel, FUEL_MAX, 2, 3);

    /* Oxígeno */
    mvwprintw(win_hud, 5, 1, "Oxigeno");
    hud_barra(6, o2, O2_MAX, 9, 3);

    /* Posición */
    mvwprintw(win_hud, 8,  1, "Posicion");
    mvwprintw(win_hud, 9,  1, " X:%-3d  Y:%-3d", g.x, g.y);

    /* Inventario */
    mvwprintw(win_hud, 11, 1, "Inventario");
    mvwprintw(win_hud, 12, 1, " Deu: %d", g.nave.inventario[MINERAL_DEUTERIO]);
    mvwprintw(win_hud, 13, 1, " Mut: %d", g.nave.inventario[MINERAL_MUTEXIO]);
    mvwprintw(win_hud, 14, 1, " Sem: %d", g.nave.inventario[MINERAL_SEMAFORITA]);
    mvwprintw(win_hud, 15, 1, " Ker: %d", g.nave.inventario[MINERAL_KERNELIO]);

    /* Controles dinámicos */
    mvwprintw(win_hud, 17, 1, "Controles:");
    mvwprintw(win_hud, 18, 1, " WASD  mover");
    mvwprintw(win_hud, 19, 1, " E     extraer");

    if (g.en_hangar) {
        mvwprintw(win_hud, 20, 1, " V     vender");
        mvwprintw(win_hud, 21, 1, " C/O   fuel/O2");
        mvwprintw(win_hud, 22, 1, " H     salir hangar");
    } else {
        bool estacion_cerca = false;
        int dx_dirs[] = {0, 0, -1, 1};
        int dy_dirs[] = {-1, 1, 0, 0};
        for (int i = 0; i < 4; i++) {
            int nx = g.x + dx_dirs[i];
            int ny = g.y + dy_dirs[i];
            if (nx >= 0 && nx < MAP_COLS && ny >= 0 && ny < MAP_ROWS) {
                if (g.mapa->celdas[ny][nx] == CHAR_ESTACION) {
                    estacion_cerca = true;
                    break;
                }
            }
        }
        if (estacion_cerca) {
            wattron(win_hud, COLOR_PAIR(7) | A_BOLD);
            mvwprintw(win_hud, 20, 1, " H     entrar hangar");
            wattroff(win_hud, COLOR_PAIR(7) | A_BOLD);
        }
        mvwprintw(win_hud, 21, 1, " Q     salir");
    }

    wattroff(win_hud, COLOR_PAIR(1));

    /* Alertas del radar (últimos 6 segundos) */
    if (ahora - g.alerta_recibida < 6) {
        wattron(win_hud, COLOR_PAIR(3) | A_BOLD);
        mvwprintw(win_hud, 23, 1, "%s", g.alerta_str);
        wattroff(win_hud, COLOR_PAIR(3) | A_BOLD);
    }

    /* Mensajes de error (últimos 3 segundos) */
    if (ahora - g.hud_error_recibido < 3) {
        wattron(win_hud, COLOR_PAIR(3));
        mvwprintw(win_hud, 24, 1, "%s", g.hud_error);
        wattroff(win_hud, COLOR_PAIR(3));
    }

    /* GAME OVER */
    if (!g.vivo) {
        wattron(win_hud, COLOR_PAIR(8) | A_BOLD | A_BLINK);
        mvwprintw(win_hud, 22, 1, "*** GAME OVER ***");
        wattroff(win_hud, COLOR_PAIR(8) | A_BOLD | A_BLINK);
    }

    wnoutrefresh(win_hud);

    /* ── Ventana Mapa ── */
    werase(win_mapa);
    box(win_mapa, 0, 0);  /* borde con caracteres de línea ncurses  */

    for (int r = 0; r < MAP_ROWS; r++) {
        for (int c = 0; c < MAP_COLS; c++) {
            char cell = g.mapa->celdas[r][c];
            /* +1 por el borde box() */
            wmove(win_mapa, r + 1, c + 1);

            if (cell == CHAR_NAVE) {
                wattron(win_mapa, COLOR_PAIR(4) | A_BOLD);
                waddch(win_mapa, 'N');
                wattroff(win_mapa, COLOR_PAIR(4) | A_BOLD);
            } else if (cell == CHAR_ASTEROIDE) {
                wattron(win_mapa, COLOR_PAIR(5));
                waddch(win_mapa, 'A');
                wattroff(win_mapa, COLOR_PAIR(5));
            } else if (cell == CHAR_ESTACION) {
                wattron(win_mapa, COLOR_PAIR(6) | A_BOLD);
                waddch(win_mapa, 'E');
                wattroff(win_mapa, COLOR_PAIR(6) | A_BOLD);
            } else {
                waddch(win_mapa, cell == CHAR_VACIO ? ' ' : (chtype)(unsigned char)cell);
            }
        }
    }

    wnoutrefresh(win_mapa);

    /* Envía todo al terminal en un único paso (reduce flicker) */
    doupdate();
}

/* ─── Gestión de ingreso y egreso de Hangar ─────────────────────── */
static void salir_de_hangar(void)
{
    if (!g.en_hangar) return;

    if (g.sem_hangar != SEM_FAILED) {
        sem_post(g.sem_hangar);
        sem_close(g.sem_hangar);
        g.sem_hangar = SEM_FAILED;
    }

    if (g.mq_estacion != (mqd_t)-1) {
        mq_close(g.mq_estacion);
        g.mq_estacion = (mqd_t)-1;
    }

    g.en_hangar = false;
}

static void intentar_ingresar_hangar(void)
{
    int dx_dirs[] = {0, 0, -1, 1};
    int dy_dirs[] = {-1, 1, 0, 0};
    int ex = -1, ey = -1;
    bool encontrada = false;

    for (int i = 0; i < 4; i++) {
        int nx = g.x + dx_dirs[i];
        int ny = g.y + dy_dirs[i];
        if (nx >= 0 && nx < MAP_COLS && ny >= 0 && ny < MAP_ROWS) {
            if (g.mapa->celdas[ny][nx] == CHAR_ESTACION) {
                ex = nx;
                ey = ny;
                encontrada = true;
                break;
            }
        }
    }

    if (!encontrada) {
        strncpy(g.hud_error, "Estacion no encontrada", sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
        return;
    }

    char sem_name[64];
    snprintf(sem_name, sizeof(sem_name), "/estacion_sem_%d_%d", ex, ey);

    sem_t *sem = sem_open(sem_name, 0);
    if (sem == SEM_FAILED) {
        strncpy(g.hud_error, "Estacion inactiva", sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
        return;
    }

    if (sem_trywait(sem) != 0) {
        sem_close(sem);
        strncpy(g.hud_error, "Hangar lleno (max 3)", sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
        return;
    }

    char mq_est_name[64];
    snprintf(mq_est_name, sizeof(mq_est_name), "/estacion_mq_%d_%d", ex, ey);

    mqd_t mq_est = mq_open(mq_est_name, O_WRONLY);
    if (mq_est == (mqd_t)-1) {
        sem_post(sem);
        sem_close(sem);
        strncpy(g.hud_error, "Error de conexion IPC", sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
        return;
    }

    g.sem_hangar = sem;
    g.mq_estacion = mq_est;
    g.en_hangar = true;
    g.estacion_x = ex;
    g.estacion_y = ey;
}

/* ─── Transacciones Comerciales ─────────────────────────────────── */
static bool realizar_transaccion_sincrona(MensajeEstacion *req, MensajeNave *resp)
{
    pthread_mutex_lock(&g.mx_estado);
    g.hay_respuesta = false;
    pthread_mutex_unlock(&g.mx_estado);

    if (mq_send(g.mq_estacion, (const char *)req, sizeof(*req), 0) == -1) {
        return false;
    }

    /* Esperar respuesta del hilo receptor */
    pthread_mutex_lock(&g.mx_estado);
    while (!g.hay_respuesta && g.vivo) {
        pthread_cond_wait(&g.cond_respuesta, &g.mx_estado);
    }
    if (!g.vivo) {
        pthread_mutex_unlock(&g.mx_estado);
        return false;
    }
    *resp = g.ultima_respuesta;
    g.hay_respuesta = false;
    pthread_mutex_unlock(&g.mx_estado);

    return true;
}

static void realizar_venta(void)
{
    /* Vender minerales */
    for (int t = MINERAL_MUTEXIO; t < CANTIDAD_RECURSOS; t++) {
        int cant = g.nave.inventario[t];
        if (cant > 0) {
            MensajeEstacion req;
            req.pid_origen = getpid();
            req.tipo = REQ_VENDER_MINERAL;
            req.recurso = t;
            req.cantidad = cant;

            MensajeNave resp;
            if (realizar_transaccion_sincrona(&req, &resp) && resp.exito) {
                g.nave.inventario[t] = 0;
            }
        }
    }

    /* Vender Deuterio */
    int cant_deuterio = g.nave.inventario[MINERAL_DEUTERIO];
    if (cant_deuterio > 0) {
        MensajeEstacion req;
        req.pid_origen = getpid();
        req.tipo = REQ_VENDER_MINERAL;
        req.recurso = MINERAL_DEUTERIO;
        req.cantidad = cant_deuterio;

        MensajeNave resp;
        if (realizar_transaccion_sincrona(&req, &resp) && resp.exito) {
            g.nave.inventario[MINERAL_DEUTERIO] = 0;
        }
    }
}

static void realizar_compra_combustible(void)
{
    MensajeEstacion req;
    req.pid_origen = getpid();
    req.tipo = REQ_COMPRAR_COMBUSTIBLE;
    req.recurso = MINERAL_DEUTERIO;
    req.cantidad = 40;

    MensajeNave resp;
    if (realizar_transaccion_sincrona(&req, &resp) && resp.exito) {
        barra_modificar(&g.nave.barra_combustible, resp.cantidad);
    } else {
        strncpy(g.hud_error, resp.mensaje, sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
    }
}

static void realizar_compra_oxigeno(void)
{
    MensajeEstacion req;
    req.pid_origen = getpid();
    req.tipo = REQ_COMPRAR_OXIGENO;
    req.recurso = 0;
    req.cantidad = 40;

    MensajeNave resp;
    if (realizar_transaccion_sincrona(&req, &resp) && resp.exito) {
        barra_modificar(&g.nave.barra_oxigeno, resp.cantidad);
    }
}

/* ─── Hilos ────────────────────────────────────────────────────── */

/* Hilo receptor de respuestas y de alertas de la cola de la nave */
static void *hilo_receptor_nave(void *arg)
{
    (void)arg;
    struct mq_attr attr;
    mq_getattr(g.mq_respuesta, &attr);
    char *buf = malloc((size_t)attr.mq_msgsize);
    if (!buf) return NULL;

    while (g.vivo) {
        ssize_t bytes = mq_receive(g.mq_respuesta, buf, (size_t)attr.mq_msgsize, NULL);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!g.vivo) break;

        MensajeNave *msg = (MensajeNave *)buf;
        if (msg->es_alerta) {
            pthread_mutex_lock(&g.mx_estado);
            snprintf(g.alerta_str, sizeof(g.alerta_str), "Alerta: Est (%d,%d)", 
                     msg->estacion_x, msg->estacion_y);
            g.alerta_recibida = time(NULL);
            pthread_mutex_unlock(&g.mx_estado);
        } else {
            pthread_mutex_lock(&g.mx_estado);
            g.ultima_respuesta = *msg;
            g.hay_respuesta = true;
            pthread_cond_signal(&g.cond_respuesta);
            pthread_mutex_unlock(&g.mx_estado);
        }
    }
    free(buf);
    return NULL;
}

static void *hilo_soporte_vital(void *arg)
{
    (void)arg;
    while (g.vivo) {
        barra_esperar_notificacion(&g.nave.barra_oxigeno);
        if (!g.vivo) break;
        if (barra_get_valor(&g.nave.barra_oxigeno) <= 0)
            game_over("oxigeno");
    }
    return NULL;
}

static void *hilo_propulsion(void *arg)
{
    (void)arg;

    while (g.vivo) {
        /*
         * Leer teclado desde stdscr con nodelay=TRUE.
         * NO tomamos g_mx_ncurses aquí: getch() sobre stdscr es seguro
         * en un único hilo lector, y evitamos bloquear hilo_radar.
         */
        int ch = getch();
        if (ch == ERR) { dormir_ms(10); continue; }

        int dx = 0, dy = 0;
        switch (ch) {
            case 'w': case 'W': case KEY_UP:    dy = -1; break;
            case 's': case 'S': case KEY_DOWN:  dy =  1; break;
            case 'a': case 'A': case KEY_LEFT:  dx = -1; break;
            case 'd': case 'D': case KEY_RIGHT: dx =  1; break;
            case 'e': case 'E': g.extrayendo = 1; continue;
            case 'h': case 'H':
                if (g.en_hangar) {
                    salir_de_hangar();
                } else {
                    intentar_ingresar_hangar();
                }
                continue;
            case 'v': case 'V':
                if (g.en_hangar) realizar_venta();
                continue;
            case 'c': case 'C':
                if (g.en_hangar) realizar_compra_combustible();
                continue;
            case 'o': case 'O':
                if (g.en_hangar) realizar_compra_oxigeno();
                continue;
            case 'q': case 'Q':
                pthread_mutex_lock(&g.mx_estado);
                g.vivo = 0;
                pthread_mutex_unlock(&g.mx_estado);
                continue;
            default: continue;
        }

        if (!g.vivo) break;
        if (barra_get_valor(&g.nave.barra_combustible) <= 0) continue;

        /* Salir de hangar automáticamente si decide moverse */
        if (g.en_hangar && (dx != 0 || dy != 0)) {
            salir_de_hangar();
        }

        pthread_mutex_lock(&g.mx_estado);
        int xn = g.x + dx, yn = g.y + dy;
        bool ok = intentar_mover_objeto(g.mapa, &g.x, &g.y, xn, yn, CHAR_NAVE, false);
        pthread_mutex_unlock(&g.mx_estado);

        if (ok) {
            barra_modificar(&g.nave.barra_combustible, -COSTO_FUEL_MOV);
            if (barra_get_valor(&g.nave.barra_combustible) <= 0)
                game_over("combustible");
        }
    }
    return NULL;
}

static void *hilo_extraccion(void *arg)
{
    (void)arg;
    struct timespec ciclo = {1, 0};
    while (g.vivo) {
        if (!g.extrayendo) { dormir_ms(100); continue; }
        if (barra_get_valor(&g.nave.barra_combustible) <= 0) { g.extrayendo = 0; continue; }
        nanosleep(&ciclo, NULL);
        barra_modificar(&g.nave.barra_combustible, -COSTO_FUEL_EXT);
        
        /* Simular minado: añadimos mineral aleatorio al inventario */
        int recurso_minado = rand() % CANTIDAD_RECURSOS;
        pthread_mutex_lock(&g.mx_estado);
        g.nave.inventario[recurso_minado] += 5;
        pthread_mutex_unlock(&g.mx_estado);

        if (barra_get_valor(&g.nave.barra_combustible) <= 0) game_over("combustible");
        g.extrayendo = 0;
    }
    return NULL;
}

static void *hilo_radar(void *arg)
{
    (void)arg;
    while (g.vivo) {
        pthread_mutex_lock(&g_mx_ncurses);
        render_frame();
        pthread_mutex_unlock(&g_mx_ncurses);
        dormir_ms(120);
    }
    /* Último frame mostrando GAME OVER */
    pthread_mutex_lock(&g_mx_ncurses);
    render_frame();
    pthread_mutex_unlock(&g_mx_ncurses);
    return NULL;
}

/* ─── Spawn ────────────────────────────────────────────────────── */
static bool spawnear_nave(void)
{
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (int i = 0; i < 1000; i++) {
        int x = rand() % MAP_COLS;
        int y = rand() % MAP_ROWS;
        if (adquirir_posicion_inicial(g.mapa, x, y, CHAR_NAVE, false)) {
            g.x = x; g.y = y;
            return true;
        }
    }
    return false;
}

/* ─── main ─────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    memset(&g, 0, sizeof(g));
    g.mq_respuesta = (mqd_t)-1;
    g.mq_estacion = (mqd_t)-1;
    g.sem_hangar = SEM_FAILED;

    /* 1. Conectar al mapa del servidor */
    g.mapa = mapa_conectar_cliente();
    if (!g.mapa) {
        fprintf(stderr, "[NAVE] No se pudo conectar al servidor.\n");
        exit(EXIT_FAILURE);
    }

    /* 2. Crear archivo indicador PID y cola de respuesta de la Nave */
    snprintf(g.pid_file, sizeof(g.pid_file), "bin/nave_%d.pid", getpid());
    int pfd = open(g.pid_file, O_WRONLY | O_CREAT | O_TRUNC, 0660);
    if (pfd != -1) {
        char pid_str[16];
        int len = snprintf(pid_str, sizeof(pid_str), "%d", getpid());
        if (write(pfd, pid_str, (size_t)len) == -1) {
            perror("write pid nave falló");
        }
        close(pfd);
    }

    snprintf(g.mq_respuesta_name, sizeof(g.mq_respuesta_name), "/nave_mq_%d", getpid());
    mq_unlink(g.mq_respuesta_name);

    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(MensajeNave);
    attr.mq_curmsgs = 0;

    g.mq_respuesta = mq_open(g.mq_respuesta_name, O_RDONLY | O_CREAT | O_EXCL, 0660, &attr);
    if (g.mq_respuesta == (mqd_t)-1) {
        perror("mq_open respuesta nave falló");
        unlink(g.pid_file);
        mapa_desconectar(g.mapa);
        exit(EXIT_FAILURE);
    }

    /* 3. Inicializar barras */
    barra_init(&g.nave.barra_combustible,
               FUEL_MAX, FUEL_UMBRAL, FUEL_DECREMENTO, FUEL_INTERVALO_MS);
    barra_init(&g.nave.barra_oxigeno,
               O2_MAX, O2_UMBRAL, O2_DECREMENTO, O2_INTERVALO_MS);

    pthread_mutex_init(&g.mx_estado, NULL);
    pthread_cond_init(&g.cond_respuesta, NULL);

    g.vivo = 1;
    g.extrayendo = 0;
    g.en_hangar = false;
    memset(g.nave.inventario, 0, sizeof(g.nave.inventario));
    g.nave.base.id        = getpid();
    g.nave.base.tipo      = TIPO_NAVE;
    g.nave.base.velocidad = 1.0f;

    /* 4. Spawn en el mapa */
    if (!spawnear_nave()) {
        fprintf(stderr, "[NAVE] No hay lugar libre en el mapa.\n");
        mq_close(g.mq_respuesta);
        mq_unlink(g.mq_respuesta_name);
        unlink(g.pid_file);
        mapa_desconectar(g.mapa);
        exit(EXIT_FAILURE);
    }
    g.nave.base.x = (float)g.x;
    g.nave.base.y = (float)g.y;

    /* 5. Inicializar ncurses */
    ncurses_init();

    /* 6. Lanzar hilos */
    pthread_t t_vital, t_prop, t_ext, t_radar, t_receptor;
    pthread_create(&t_receptor, NULL, hilo_receptor_nave, NULL);
    pthread_create(&t_vital, NULL, hilo_soporte_vital, NULL);
    pthread_create(&t_prop,  NULL, hilo_propulsion,    NULL);
    pthread_create(&t_ext,   NULL, hilo_extraccion,    NULL);
    pthread_create(&t_radar, NULL, hilo_radar,         NULL);

    /* Loop principal: espera a que el juego termine */
    while (g.vivo) dormir_ms(100);
    sleep(2);   /* pausa para que el jugador lea el GAME OVER       */

    /* 7. Limpieza */
    pthread_cancel(t_receptor);
    pthread_cancel(t_vital);
    pthread_join(t_receptor, NULL);
    pthread_join(t_vital, NULL);
    pthread_join(t_prop,  NULL);
    pthread_join(t_ext,   NULL);
    pthread_join(t_radar, NULL);

    ncurses_end();
    salir_de_hangar();

    liberar_posicion(g.mapa, g.x, g.y);
    barra_destroy(&g.nave.barra_combustible);
    barra_destroy(&g.nave.barra_oxigeno);
    
    pthread_cond_destroy(&g.cond_respuesta);
    pthread_mutex_destroy(&g.mx_estado);
    pthread_mutex_destroy(&g_mx_ncurses);
    mapa_desconectar(g.mapa);

    printf("[NAVE] Desconectada.\n");
    exit(EXIT_SUCCESS);
}
