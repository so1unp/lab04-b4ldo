#define _POSIX_C_SOURCE 200809L

#include <ncurses.h>
#include <unistd.h>
#include "hud.h"
#include "mapa.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define PANEL_W 24

static WINDOW *win_hud   = NULL;  /* panel izquierdo                */
static WINDOW *win_mapa  = NULL;  /* mapa del juego + borde         */
static WINDOW *win_score = NULL;  /* panel derecho de puntuaciones  */

/* Mutex privado que serializa los accesos a ncurses */
static pthread_mutex_t g_mx_ncurses = PTHREAD_MUTEX_INITIALIZER;

/* ─── Inicialización de ncurses ─── */
void init_ncurses(void)
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
        /* Par 3: barra combustible baja/alerta                      */
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

    win_hud   = newwin(MAP_ROWS + 4, PANEL_W, 0, 0);
    win_mapa  = newwin(MAP_ROWS + 2, MAP_COLS + 2, 0, PANEL_W);
    win_score = newwin(MAP_ROWS + 4, PANEL_W, 0, PANEL_W + MAP_COLS + 2);

    nodelay(stdscr, TRUE); /* getch() no bloquea */
    keypad(stdscr, TRUE);

    refresh();
}

/* ─── Cierre de ncurses ─── */
void cleanup_ncurses(void)
{
    if (win_hud)   { delwin(win_hud);   win_hud   = NULL; }
    if (win_mapa)  { delwin(win_mapa);  win_mapa  = NULL; }
    if (win_score) { delwin(win_score); win_score = NULL; }
    endwin();
    pthread_mutex_destroy(&g_mx_ncurses);
}

/* ─── Dibujo de barra de progreso en win_hud ─── */
static void hud_barra(int row, int val, int maxv, int color_full, int color_low)
{
    if (!win_hud) return;

    int cols = 10;
    int fill = (int)(((float)val / (float)maxv) * (float)cols);
    bool low = (val <= (maxv * 20 / 100)); // 20% umbral

    wmove(win_hud, row, 1);
    waddch(win_hud, '[');
    for (int i = 0; i < cols; i++) {
        if (i < fill) {
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

/* ─── Renderizado del frame (privado de ncurses) ─── */
static void render_frame(void)
{
    if (!win_hud || !win_mapa) return;

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
    mvwprintw(win_hud, 8,  1, "Posicion: X:%-2d Y:%-2d", g.x, g.y);
    mvwprintw(win_hud, 9,  1, "PID Nave: %-5d", getpid());

    /* Inventario (Compactado en dos columnas para ahorrar espacio) */
    mvwprintw(win_hud, 11, 1, "Inventario");
    mvwprintw(win_hud, 12, 1, " Deu:%-3d Mut:%-3d", g.nave.inventario[MINERAL_DEUTERIO], g.nave.inventario[MINERAL_MUTEXIO]);
    mvwprintw(win_hud, 13, 1, " Sem:%-3d Ker:%-3d", g.nave.inventario[MINERAL_SEMAFORITA], g.nave.inventario[MINERAL_KERNELIO]);

    /* Extracción */
    mvwprintw(win_hud, 15, 1, "Extraccion");
    int prog = g.prog_ext;
    if (prog < 0) prog = 0;
    hud_barra(16, prog, 100, 7, 7); // Color 7 (cyan) para carga

    /* Controles dinámicos */
    mvwprintw(win_hud, 18, 1, "Controles:");
    mvwprintw(win_hud, 19, 1, " WASD  mover");

    // Iluminar 'E extraer' si hay un asteroide adyacente en el mapa
    bool asteroide_cerca = false;
    {
        int dx_dirs[] = {0, 0, -1, 1};
        int dy_dirs[] = {-1, 1, 0, 0};
        for (int i = 0; i < 4; i++) {
            int nx = g.x + dx_dirs[i];
            int ny = g.y + dy_dirs[i];
            if (nx >= 0 && nx < MAP_COLS && ny >= 0 && ny < MAP_ROWS) {
                if (g.mapa->celdas[ny][nx] == CHAR_ASTEROIDE) {
                    asteroide_cerca = true;
                    break;
                }
            }
        }
    }

    if (asteroide_cerca) {
        wattron(win_hud, COLOR_PAIR(7) | A_BOLD);
        mvwprintw(win_hud, 20, 1, " E     extraer");
        wattroff(win_hud, COLOR_PAIR(7) | A_BOLD);
    } else {
        mvwprintw(win_hud, 20, 1, " E     extraer");
    }

    if (g.en_hangar) {
        mvwprintw(win_hud, 21, 1, " V     vender");
        mvwprintw(win_hud, 22, 1, " C/O   fuel/O2");
        mvwprintw(win_hud, 23, 1, " H     salir hangar");
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
            mvwprintw(win_hud, 21, 1, " H     entrar hangar");
            wattroff(win_hud, COLOR_PAIR(7) | A_BOLD);
        } else {
            mvwprintw(win_hud, 21, 1, "                   "); // limpiar si ya no está cerca
        }
        mvwprintw(win_hud, 22, 1, " Q     salir");
        mvwprintw(win_hud, 23, 1, "                   "); // limpiar línea extra
    }

    wattroff(win_hud, COLOR_PAIR(1));

    /* Mensajes de error/notificación (últimos 3 segundos) - Fila 25 */
    if (ahora - g.hud_error_recibido < 3) {
        wattron(win_hud, COLOR_PAIR(3));
        mvwprintw(win_hud, 25, 1, "%s", g.hud_error);
        wattroff(win_hud, COLOR_PAIR(3));
    }

    /* Alertas de estaciones - Fila 26 y 27 */
    int alert_row = 26;
    for (int i = 0; i < 3; i++) {
        if (g.alertas[i].activa && (ahora - g.alertas[i].timestamp < 8)) {
            if (g.alertas[i].explotada) {
                wattron(win_hud, COLOR_PAIR(3) | A_BOLD | A_BLINK);
                mvwprintw(win_hud, alert_row++, 1, "Est(%d,%d) EXPLOTO!", g.alertas[i].x, g.alertas[i].y);
                wattroff(win_hud, COLOR_PAIR(3) | A_BOLD | A_BLINK);
            } else {
                wattron(win_hud, COLOR_PAIR(3) | A_BOLD);
                mvwprintw(win_hud, alert_row++, 1, "Est(%d,%d) pide H2", g.alertas[i].x, g.alertas[i].y);
                wattroff(win_hud, COLOR_PAIR(3) | A_BOLD);
            }
            if (alert_row > 27) break;
        }
    }

    /* GAME OVER */
    if (!g.vivo) {
        wattron(win_hud, COLOR_PAIR(8) | A_BOLD | A_BLINK);
        mvwprintw(win_hud, 25, 1, "*** GAME OVER ***");
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

    /* ── Ventana Scoreboard ── */
    if (win_score) {
        werase(win_score);
        box(win_score, 0, 0);

        /* Título del Scoreboard */
        wattron(win_score, COLOR_PAIR(7) | A_BOLD);
        mvwprintw(win_score, 0, 4, " LEADERBOARD ");
        wattroff(win_score, COLOR_PAIR(7) | A_BOLD);

        wattron(win_score, COLOR_PAIR(1));
        mvwprintw(win_score, 2, 2, "Nave PID    Creditos");
        mvwprintw(win_score, 3, 2, "----------  --------");

        // Encontrar y listar todas las naves activas
        RegistroNave sorted_naves[MAX_NAVES];
        int count = 0;
        for (int i = 0; i < MAX_NAVES; i++) {
            if (g.mapa->naves[i].activo) {
                sorted_naves[count++] = g.mapa->naves[i];
            }
        }

        // Ordenar por créditos descendente
        for (int i = 0; i < count - 1; i++) {
            for (int j = i + 1; j < count; j++) {
                if (sorted_naves[j].creditos > sorted_naves[i].creditos) {
                    RegistroNave temp = sorted_naves[i];
                    sorted_naves[i] = sorted_naves[j];
                    sorted_naves[j] = temp;
                }
            }
        }

        // Dibujar ranking
        int my_pid = getpid();
        for (int i = 0; i < count; i++) {
            int y_row = 4 + i;
            if (y_row > MAP_ROWS) break; // Evitar salir de la ventana

            // Resaltar a nuestra propia nave
            if (sorted_naves[i].pid == my_pid) {
                wattron(win_score, COLOR_PAIR(7) | A_BOLD); // Amarillo
                mvwprintw(win_score, y_row, 2, "%-10d  %-8d", sorted_naves[i].pid, sorted_naves[i].creditos);
                wattroff(win_score, COLOR_PAIR(7) | A_BOLD);
            } else {
                mvwprintw(win_score, y_row, 2, "%-10d  %-8d", sorted_naves[i].pid, sorted_naves[i].creditos);
            }
        }
        wattroff(win_score, COLOR_PAIR(1));

        wnoutrefresh(win_score);
    }

    /* Envía todo al terminal en un único paso */
    doupdate();
}

/* ─── Renderizado seguro de un frame (exposición pública) ─── */
void hud_render(void)
{
    pthread_mutex_lock(&g_mx_ncurses);
    render_frame();
    pthread_mutex_unlock(&g_mx_ncurses);
}
