#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>

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

/* ─── Layout de pantalla ───────────────────────────────────────── */
/* Total de filas mostradas: MAP_ROWS + 2 (bordes superior/inferior) */
#define PANEL_W   22          /* ancho del panel izquierdo           */
#define BAR_IN    10          /* ancho interior de la barra [======] */
/* ancho total de pantalla: PANEL_W + 1(|) + MAP_COLS + 1(|) = 104  */

/* ─── ANSI helpers ─────────────────────────────────────────────── */
#define ESC           "\033"
#define ANSI_HOME     ESC "[H"
#define ANSI_HIDE_CUR ESC "[?25l"
#define ANSI_SHOW_CUR ESC "[?25h"
#define ANSI_CLEAR    ESC "[2J"
#define ANSI_BOLD     ESC "[1m"
#define ANSI_REV      ESC "[7m"
#define ANSI_RST      ESC "[0m"
#define ANSI_CLR_LINE ESC "[K"

/* ─── Frame buffer ─────────────────────────────────────────────── */
#define FBUF 131072
static char  g_fb[FBUF];
static int   g_fb_pos;

static void fb_reset(void)  { g_fb_pos = 0; }
static void fb_flush(void)  { write(STDOUT_FILENO, g_fb, (size_t)g_fb_pos); }

static void fb_raw(const char *s, int n)
{
    if (g_fb_pos + n >= FBUF) return;
    memcpy(g_fb + g_fb_pos, s, (size_t)n);
    g_fb_pos += n;
}
static void fb_str(const char *s) { fb_raw(s, (int)strlen(s)); }
static void fb_ch(char c)         { if (g_fb_pos < FBUF - 1) g_fb[g_fb_pos++] = c; }



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
static pthread_mutex_t g_mx_out = PTHREAD_MUTEX_INITIALIZER; /* serializa writes */

/* ─── Terminal raw mode ────────────────────────────────────────── */
static struct termios g_term_orig;

static void term_raw(void)
{
    struct termios t;
    tcgetattr(STDIN_FILENO, &g_term_orig);
    t = g_term_orig;
    t.c_lflag &= (unsigned)~(ICANON | ECHO | ISIG);
    t.c_iflag &= (unsigned)~(IXON | ICRNL);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 1; /* 100 ms de timeout por read() */
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void term_restore(void)
{
    static const char seq[] = ANSI_SHOW_CUR ANSI_CLEAR ANSI_HOME;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_term_orig);
    write(STDOUT_FILENO, seq, sizeof(seq) - 1);
}

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

/* ─── Render ───────────────────────────────────────────────────── */

/* Escribe una línea del panel, exactamente PANEL_W chars, sin newline */
static void fb_panel_line(const char *fmt, ...)
{
    char tmp[PANEL_W + 2];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    int len = (int)strlen(tmp);
    fb_raw(tmp, len < PANEL_W ? len : PANEL_W);
    for (int i = len; i < PANEL_W; i++) fb_ch(' ');
}

static void fb_bar(int val, int maxv)
{
    int filled = (maxv > 0) ? (val * BAR_IN / maxv) : 0;
    if (filled > BAR_IN) filled = BAR_IN;
    char tmp[PANEL_W + 2];
    tmp[0] = ' '; tmp[1] = '[';
    for (int i = 0; i < BAR_IN; i++) tmp[2 + i] = (i < filled) ? '=' : ' ';
    tmp[2 + BAR_IN] = ']';
    snprintf(tmp + 3 + BAR_IN, sizeof(tmp) - 3 - BAR_IN, " %3d", val);
    int len = (int)strlen(tmp);
    fb_raw(tmp, len < PANEL_W ? len : PANEL_W);
    for (int i = len; i < PANEL_W; i++) fb_ch(' ');
}

/*
 * Genera el frame completo en el buffer y hace un único write().
 * Layout:
 *   col 0..PANEL_W-1  : panel izquierdo
 *   col PANEL_W        : borde vertical |
 *   col PANEL_W+1 ..   : celdas del mapa
 *   col PANEL_W+1+MAP_COLS : borde vertical |
 *
 * Filas:
 *   fila 0           : panel[0]  + borde superior del mapa
 *   fila 1..MAP_ROWS : panel[r]  + | + celdas + |
 *   fila MAP_ROWS+1  : panel[last] + borde inferior del mapa
 */
static void render_frame(void)
{
    int fuel = barra_get_valor(&g.nave.barra_combustible);
    int o2   = barra_get_valor(&g.nave.barra_oxigeno);

    /* ── Construir array de líneas del panel ── */
    /* Máximo MAP_ROWS+2 líneas (una por fila de pantalla)            */
    /* Las almacenamos como funciones inline al emitir el frame       */

    /* ── Emitir frame ── */
    fb_reset();
    fb_str(ANSI_HOME);   /* sin CLEAR: sobreescribimos in-place → sin flicker */

    /* Fila 0: título del panel + borde superior del mapa */
    fb_str(ANSI_BOLD);
    fb_panel_line(" COSMIKERNEL");
    fb_str(ANSI_RST);
    fb_ch('+');
    for (int c = 0; c < MAP_COLS; c++) fb_ch('-');
    fb_str("+\r\n");

    /* Filas 1..MAP_ROWS: panel + mapa */
    /* Definimos las líneas del panel según el índice de fila (1-based) */
    for (int r = 1; r <= MAP_ROWS; r++) {
        /* Panel izquierdo */
        switch (r) {
            case  1: fb_panel_line(" ----------------------"); break;
            case  2: fb_panel_line(""); break;
            case  3: fb_panel_line(" Combustible"); break;
            case  4: fb_bar(fuel, FUEL_MAX); break;
            case  5: fb_panel_line(""); break;
            case  6: fb_panel_line(" Oxigeno"); break;
            case  7: fb_bar(o2, O2_MAX); break;
            case  8: fb_panel_line(""); break;
            case  9: fb_panel_line(" Posicion"); break;
            case 10: fb_panel_line(" X:%-3d  Y:%-3d", g.x, g.y); break;
            case 11: fb_panel_line(""); break;
            case 12: fb_panel_line(" Inventario"); break;
            case 13: fb_panel_line(" Deu: %d", g.nave.inventario[MINERAL_DEUTERIO]); break;
            case 14: fb_panel_line(" Mut: %d", g.nave.inventario[MINERAL_MUTEXIO]); break;
            case 15: fb_panel_line(" Sem: %d", g.nave.inventario[MINERAL_SEMAFORITA]); break;
            case 16: fb_panel_line(" Ker: %d", g.nave.inventario[MINERAL_KERNELIO]); break;
            case 17: fb_panel_line(""); break;
            case 18: fb_panel_line(" Controles:"); break;
            case 19: fb_panel_line(" WASD  mover"); break;
            case 20: fb_panel_line(" E     extraer"); break;
            case 21: fb_panel_line(" Q     salir"); break;
            case 22: fb_panel_line(""); break;
            case 23:
                if (!g.vivo) { fb_str(ANSI_BOLD ANSI_REV); fb_panel_line(" *** GAME OVER ***"); fb_str(ANSI_RST); }
                else          { fb_panel_line(""); }
                break;
            default: fb_panel_line(""); break;
        }

        /* Borde + celdas del mapa */
        fb_ch('|');
        for (int c = 0; c < MAP_COLS; c++) {
            char cell = g.mapa->celdas[r - 1][c];
            if (cell == CHAR_NAVE) {
                fb_str(ANSI_BOLD ANSI_REV);
                fb_ch('N');
                fb_str(ANSI_RST);
            } else {
                fb_ch(cell == CHAR_VACIO ? ' ' : cell);
            }
        }
        fb_str("|\r\n");
    }

    /* Última fila: borde inferior del mapa */
    fb_panel_line("");
    fb_ch('+');
    for (int c = 0; c < MAP_COLS; c++) fb_ch('-');
    fb_str("+\r\n");

    fb_flush();
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
    char ch;

    while (g.vivo) {
        /* read() con VTIME=1 → timeout de 100 ms, no bloquea para siempre */
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) continue;

        int dx = 0, dy = 0;
        switch (ch) {
            case 'w': case 'W': dy = -1; break;
            case 's': case 'S': dy =  1; break;
            case 'a': case 'A': dx = -1; break;
            case 'd': case 'D': dx =  1; break;
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
        pthread_mutex_lock(&g_mx_out);
        render_frame();
        pthread_mutex_unlock(&g_mx_out);
        dormir_ms(120);
    }
    /* Último frame con GAME OVER */
    pthread_mutex_lock(&g_mx_out);
    render_frame();
    pthread_mutex_unlock(&g_mx_out);
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
    g.nave.base.id       = getpid();
    g.nave.base.tipo     = TIPO_NAVE;
    g.nave.base.velocidad = 1.0f;

    /* 3. Spawn en el mapa */
    if (!spawnear_nave()) {
        fprintf(stderr, "[NAVE] No hay lugar libre en el mapa.\n");
        mapa_desconectar(g.mapa);
        exit(EXIT_FAILURE);
    }
    g.nave.base.x = (float)g.x;
    g.nave.base.y = (float)g.y;

    /* 4. Preparar terminal: raw mode + ocultar cursor */
    term_raw();
    { static const char seq[] = ANSI_HIDE_CUR ANSI_CLEAR ANSI_HOME;
      write(STDOUT_FILENO, seq, sizeof(seq) - 1); }

    /* 5. Lanzar hilos */
    pthread_t t_vital, t_prop, t_ext, t_radar;
    pthread_create(&t_vital, NULL, hilo_soporte_vital, NULL);
    pthread_create(&t_prop,  NULL, hilo_propulsion,    NULL);
    pthread_create(&t_ext,   NULL, hilo_extraccion,    NULL);
    pthread_create(&t_radar, NULL, hilo_radar,         NULL);

    while (g.vivo) dormir_ms(100);
    sleep(2);

    /* 6. Limpieza */
    pthread_cancel(t_vital);
    pthread_join(t_vital, NULL);
    pthread_join(t_prop,  NULL);
    pthread_join(t_ext,   NULL);
    pthread_join(t_radar, NULL);

    term_restore();

    liberar_posicion(g.mapa, g.x, g.y);
    barra_destroy(&g.nave.barra_combustible);
    barra_destroy(&g.nave.barra_oxigeno);
    pthread_mutex_destroy(&g.mx_estado);
    pthread_mutex_destroy(&g_mx_out);
    mapa_desconectar(g.mapa);

    printf("[NAVE] Desconectada.\n");
    exit(EXIT_SUCCESS);
}
