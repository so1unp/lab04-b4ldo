#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <ncurses.h>

#include "mapa.h"
#include "movement.h"
#include "nave.h"
#include "recursos.h"

/* ─── Parámetros de juego ──────────────────────────────────────── */
#define FUEL_MAX          100
#define FUEL_UMBRAL        20
#define FUEL_DECREMENTO     1
#define FUEL_INTERVALO_MS 3000

#define O2_MAX            100
#define O2_UMBRAL          20
#define O2_DECREMENTO       1
#define O2_INTERVALO_MS  2000

#define COSTO_FUEL_MOV    2
#define COSTO_FUEL_EXT    3

/* ─── Layout ncurses ───────────────────────────────────────────── */
/* Ventana HUD: columna 0, ancho PANEL_W                           */
/* Ventana Mapa: columna PANEL_W, MAP_COLS+2 de ancho              */
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

    /* Controles */
    mvwprintw(win_hud, 17, 1, "Controles:");
    mvwprintw(win_hud, 18, 1, " WASD  mover");
    mvwprintw(win_hud, 19, 1, " E     extraer");
    mvwprintw(win_hud, 20, 1, " Q     salir");

    wattroff(win_hud, COLOR_PAIR(1));

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

/* ─── Hilos ────────────────────────────────────────────────────── */

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
            case 'q': case 'Q':
                pthread_mutex_lock(&g.mx_estado);
                g.vivo = 0;
                pthread_mutex_unlock(&g.mx_estado);
                continue;
            default: continue;
        }

        if (!g.vivo) break;
        if (barra_get_valor(&g.nave.barra_combustible) <= 0) continue;

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

    /* 1. Conectar al mapa del servidor */
    g.mapa = mapa_conectar_cliente();
    if (!g.mapa) {
        fprintf(stderr, "[NAVE] No se pudo conectar al servidor.\n");
        exit(EXIT_FAILURE);
    }

    /* 2. Inicializar barras */
    barra_init(&g.nave.barra_combustible,
               FUEL_MAX, FUEL_UMBRAL, FUEL_DECREMENTO, FUEL_INTERVALO_MS);
    barra_init(&g.nave.barra_oxigeno,
               O2_MAX, O2_UMBRAL, O2_DECREMENTO, O2_INTERVALO_MS);

    pthread_mutex_init(&g.mx_estado, NULL);
    g.vivo = 1;
    g.extrayendo = 0;
    memset(g.nave.inventario, 0, sizeof(g.nave.inventario));
    g.nave.base.id        = getpid();
    g.nave.base.tipo      = TIPO_NAVE;
    g.nave.base.velocidad = 1.0f;

    /* 3. Spawn en el mapa */
    if (!spawnear_nave()) {
        fprintf(stderr, "[NAVE] No hay lugar libre en el mapa.\n");
        mapa_desconectar(g.mapa);
        exit(EXIT_FAILURE);
    }
    g.nave.base.x = (float)g.x;
    g.nave.base.y = (float)g.y;

    /* 4. Inicializar ncurses */
    ncurses_init();

    /* 5. Lanzar hilos */
    pthread_t t_vital, t_prop, t_ext, t_radar;
    pthread_create(&t_vital, NULL, hilo_soporte_vital, NULL);
    pthread_create(&t_prop,  NULL, hilo_propulsion,    NULL);
    pthread_create(&t_ext,   NULL, hilo_extraccion,    NULL);
    pthread_create(&t_radar, NULL, hilo_radar,         NULL);

    /* Loop principal: espera a que el juego termine */
    while (g.vivo) dormir_ms(100);
    sleep(2);   /* pausa para que el jugador lea el GAME OVER       */

    /* 6. Limpieza */
    pthread_cancel(t_vital);
    pthread_join(t_vital, NULL);
    pthread_join(t_prop,  NULL);
    pthread_join(t_ext,   NULL);
    pthread_join(t_radar, NULL);

    ncurses_end();

    liberar_posicion(g.mapa, g.x, g.y);
    barra_destroy(&g.nave.barra_combustible);
    barra_destroy(&g.nave.barra_oxigeno);
    pthread_mutex_destroy(&g.mx_estado);
    pthread_mutex_destroy(&g_mx_ncurses);
    mapa_desconectar(g.mapa);

    printf("[NAVE] Desconectada.\n");
    exit(EXIT_SUCCESS);
}
