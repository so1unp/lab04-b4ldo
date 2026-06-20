#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <ncurses.h>
#include <unistd.h>
#include <time.h>

#include "mapa.h"
#include "movement.h"
#include "nave.h"
#include "recursos.h"

// ------------------------------------------------------------------
// Parámetros de juego
// ------------------------------------------------------------------
#define FUEL_MAX           100
#define FUEL_UMBRAL         20
#define FUEL_DECREMENTO      1
#define FUEL_INTERVALO_MS 3000

#define O2_MAX             100
#define O2_UMBRAL           20
#define O2_DECREMENTO        1
#define O2_INTERVALO_MS   2000

#define COSTO_FUEL_MOV     2
#define COSTO_FUEL_EXT     3

// ------------------------------------------------------------------
// Layout de pantalla
// LEFT_W: ancho del panel izquierdo (sin borde del mapa)
// El mapa empieza en la columna LEFT_W:
//   col LEFT_W        -> borde izquierdo |
//   col LEFT_W+1 ..   -> celdas del mapa (MAP_COLS)
//   col LEFT_W+1+MAP_COLS -> borde derecho |
// ------------------------------------------------------------------
#define LEFT_W   22
#define BAR_W    12    // ancho interior de la barra de progreso

// ------------------------------------------------------------------
// Estado global
// ------------------------------------------------------------------
typedef struct {
    Nave               nave;
    MapaCompartido    *mapa;
    int                x, y;
    volatile sig_atomic_t vivo;
    volatile sig_atomic_t extrayendo;
    pthread_mutex_t    mutex_estado;
} EstadoNave;

static EstadoNave       g_estado;
static pthread_mutex_t  g_ncurses_mutex = PTHREAD_MUTEX_INITIALIZER;

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------
static void dormir_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void finalizar_por_falta_de_recurso(const char *motivo)
{
    pthread_mutex_lock(&g_estado.mutex_estado);
    g_estado.vivo = 0;
    pthread_mutex_unlock(&g_estado.mutex_estado);
    (void)motivo;
}

// ------------------------------------------------------------------
// Dibujado del HUD + mapa (llamar con g_ncurses_mutex tomado)
// ------------------------------------------------------------------
static void dibujar_barra_progreso(int row, int col, int val, int max_val)
{
    int filled = (max_val > 0) ? (val * BAR_W / max_val) : 0;
    if (filled > BAR_W) filled = BAR_W;
    mvaddch(row, col, '[');
    for (int i = 0; i < BAR_W; i++)
        mvaddch(row, col + 1 + i, (i < filled) ? '=' : ' ');
    mvaddch(row, col + 1 + BAR_W, ']');
    mvprintw(row, col + BAR_W + 3, "%3d", val);
}

static void dibujar_todo(void)
{
    erase();

    int fuel = barra_get_valor(&g_estado.nave.barra_combustible);
    int o2   = barra_get_valor(&g_estado.nave.barra_oxigeno);

    // ── Panel izquierdo ───────────────────────────────────────────
    // Las filas del panel van de 0 a MAP_ROWS+1 (misma altura que la caja)
    int r = 0;
    attron(A_BOLD);
    mvprintw(r++, 1, "COSMIKERNEL");
    attroff(A_BOLD);
    mvprintw(r++, 1, "───────────────────");

    r++;
    mvprintw(r++, 1, "Combustible");
    dibujar_barra_progreso(r++, 1, fuel, FUEL_MAX);

    r++;
    mvprintw(r++, 1, "Oxigeno");
    dibujar_barra_progreso(r++, 1, o2, O2_MAX);

    r++;
    mvprintw(r++, 1, "Posicion");
    mvprintw(r++, 1, "X:%-3d  Y:%-3d", g_estado.x, g_estado.y);

    r++;
    mvprintw(r++, 1, "Inventario");
    mvprintw(r++, 1, "Deu: %d", g_estado.nave.inventario[MINERAL_DEUTERIO]);
    mvprintw(r++, 1, "Mut: %d", g_estado.nave.inventario[MINERAL_MUTEXIO]);
    mvprintw(r++, 1, "Sem: %d", g_estado.nave.inventario[MINERAL_SEMAFORITA]);
    mvprintw(r++, 1, "Ker: %d", g_estado.nave.inventario[MINERAL_KERNELIO]);

    r++;
    mvprintw(r++, 1, "Controles:");
    mvprintw(r++, 1, "WASD mover");
    mvprintw(r++, 1, "E    extraer");
    mvprintw(r++, 1, "Q    salir");

    if (!g_estado.vivo) {
        r++;
        attron(A_BOLD | A_REVERSE);
        mvprintw(r, 1, "  GAME OVER!  ");
        attroff(A_BOLD | A_REVERSE);
    }

    // ── Caja del mapa ────────────────────────────────────────────
    int mc = LEFT_W; // columna del borde izquierdo de la caja

    // Borde superior
    mvaddch(0, mc, ACS_ULCORNER);
    for (int c = 0; c < MAP_COLS; c++)
        mvaddch(0, mc + 1 + c, ACS_HLINE);
    mvaddch(0, mc + 1 + MAP_COLS, ACS_URCORNER);

    // Filas de contenido
    for (int row = 0; row < MAP_ROWS; row++) {
        mvaddch(row + 1, mc, ACS_VLINE);
        for (int col = 0; col < MAP_COLS; col++) {
            char cell = g_estado.mapa->celdas[row][col];
            if (cell == CHAR_NAVE) {
                attron(A_BOLD | A_REVERSE);
                mvaddch(row + 1, mc + 1 + col, (unsigned char)cell);
                attroff(A_BOLD | A_REVERSE);
            } else {
                mvaddch(row + 1, mc + 1 + col, (unsigned char)cell);
            }
        }
        mvaddch(row + 1, mc + 1 + MAP_COLS, ACS_VLINE);
    }

    // Borde inferior
    mvaddch(MAP_ROWS + 1, mc, ACS_LLCORNER);
    for (int c = 0; c < MAP_COLS; c++)
        mvaddch(MAP_ROWS + 1, mc + 1 + c, ACS_HLINE);
    mvaddch(MAP_ROWS + 1, mc + 1 + MAP_COLS, ACS_LRCORNER);

    refresh();
}

// ------------------------------------------------------------------
// Hilo: Soporte vital
// ------------------------------------------------------------------
static void *hilo_soporte_vital(void *arg)
{
    (void)arg;
    while (g_estado.vivo) {
        barra_esperar_notificacion(&g_estado.nave.barra_oxigeno);
        if (!g_estado.vivo) break;
        if (barra_get_valor(&g_estado.nave.barra_oxigeno) <= 0) {
            finalizar_por_falta_de_recurso("oxigeno");
            break;
        }
    }
    return NULL;
}

// ------------------------------------------------------------------
// Hilo: Propulsión (lectura de teclado + movimiento)
// Con halfdelay(1) activo, getch() devuelve ERR después de ~100 ms
// si no hay tecla, lo que permite liberar el mutex de ncurses
// regularmente para que el hilo radar pueda redibujar.
// ------------------------------------------------------------------
static void *hilo_propulsion(void *arg)
{
    (void)arg;
    while (g_estado.vivo) {
        pthread_mutex_lock(&g_ncurses_mutex);
        int ch = getch();
        pthread_mutex_unlock(&g_ncurses_mutex);

        if (ch == ERR) continue;

        int dx = 0, dy = 0;
        switch (ch) {
            case 'w': case 'W': dy = -1; break;
            case 's': case 'S': dy =  1; break;
            case 'a': case 'A': dx = -1; break;
            case 'd': case 'D': dx =  1; break;
            case 'e': case 'E':
                g_estado.extrayendo = 1;
                continue;
            case 'q': case 'Q':
                pthread_mutex_lock(&g_estado.mutex_estado);
                g_estado.vivo = 0;
                pthread_mutex_unlock(&g_estado.mutex_estado);
                continue;
            default:
                continue;
        }

        if (!g_estado.vivo) break;

        if (barra_get_valor(&g_estado.nave.barra_combustible) <= 0)
            continue;

        pthread_mutex_lock(&g_estado.mutex_estado);
        int x_nuevo = g_estado.x + dx;
        int y_nuevo = g_estado.y + dy;
        bool movido = intentar_mover_objeto(g_estado.mapa,
                                            &g_estado.x, &g_estado.y,
                                            x_nuevo, y_nuevo,
                                            CHAR_NAVE, false);
        pthread_mutex_unlock(&g_estado.mutex_estado);

        if (movido) {
            barra_modificar(&g_estado.nave.barra_combustible, -COSTO_FUEL_MOV);
            if (barra_get_valor(&g_estado.nave.barra_combustible) <= 0)
                finalizar_por_falta_de_recurso("combustible");
        }
    }
    return NULL;
}

// ------------------------------------------------------------------
// Hilo: Extracción
// ------------------------------------------------------------------
static void *hilo_extraccion(void *arg)
{
    (void)arg;
    struct timespec ciclo = {1, 0};
    while (g_estado.vivo) {
        if (!g_estado.extrayendo) { dormir_ms(100); continue; }
        if (barra_get_valor(&g_estado.nave.barra_combustible) <= 0) {
            g_estado.extrayendo = 0; continue;
        }
        nanosleep(&ciclo, NULL);
        barra_modificar(&g_estado.nave.barra_combustible, -COSTO_FUEL_EXT);
        if (barra_get_valor(&g_estado.nave.barra_combustible) <= 0)
            finalizar_por_falta_de_recurso("combustible");
        g_estado.extrayendo = 0;
    }
    return NULL;
}

// ------------------------------------------------------------------
// Hilo: Radar (redibuja cada 150 ms)
// ------------------------------------------------------------------
static void *hilo_radar(void *arg)
{
    (void)arg;
    while (g_estado.vivo) {
        dormir_ms(150);
        pthread_mutex_lock(&g_ncurses_mutex);
        dibujar_todo();
        pthread_mutex_unlock(&g_ncurses_mutex);
    }
    // Último frame mostrando GAME OVER
    pthread_mutex_lock(&g_ncurses_mutex);
    dibujar_todo();
    pthread_mutex_unlock(&g_ncurses_mutex);
    return NULL;
}

// ------------------------------------------------------------------
// Spawn aleatorio
// ------------------------------------------------------------------
static bool spawnear_nave(EstadoNave *e)
{
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    for (int i = 0; i < 1000; i++) {
        int x = rand() % MAP_COLS;
        int y = rand() % MAP_ROWS;
        if (adquirir_posicion_inicial(e->mapa, x, y, CHAR_NAVE, false)) {
            e->x = x; e->y = y;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------
// main
// ------------------------------------------------------------------
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    memset(&g_estado, 0, sizeof(g_estado));

    // 1. Conectar al mapa del servidor
    g_estado.mapa = mapa_conectar_cliente();
    if (!g_estado.mapa) {
        fprintf(stderr, "[NAVE] No se pudo conectar al servidor.\n");
        exit(EXIT_FAILURE);
    }

    // 2. Inicializar barras
    barra_init(&g_estado.nave.barra_combustible,
               FUEL_MAX, FUEL_UMBRAL, FUEL_DECREMENTO, FUEL_INTERVALO_MS);
    barra_init(&g_estado.nave.barra_oxigeno,
               O2_MAX, O2_UMBRAL, O2_DECREMENTO, O2_INTERVALO_MS);

    pthread_mutex_init(&g_estado.mutex_estado, NULL);
    g_estado.vivo = 1;
    g_estado.extrayendo = 0;
    memset(g_estado.nave.inventario, 0, sizeof(g_estado.nave.inventario));

    g_estado.nave.base.id       = getpid();
    g_estado.nave.base.tipo     = TIPO_NAVE;
    g_estado.nave.base.velocidad = 1.0f;

    // 3. Spawn en el mapa
    if (!spawnear_nave(&g_estado)) {
        fprintf(stderr, "[NAVE] No hay lugar libre en el mapa.\n");
        mapa_desconectar(g_estado.mapa);
        exit(EXIT_FAILURE);
    }
    g_estado.nave.base.x = (float)g_estado.x;
    g_estado.nave.base.y = (float)g_estado.y;

    // 4. Inicializar ncurses
    initscr();
    halfdelay(1);   // getch() devuelve ERR tras ~100 ms sin tecla
    noecho();
    curs_set(0);

    // 5. Lanzar hilos
    pthread_t t_vital, t_propulsion, t_extraccion, t_radar;
    pthread_create(&t_vital,     NULL, hilo_soporte_vital, NULL);
    pthread_create(&t_propulsion,NULL, hilo_propulsion,    NULL);
    pthread_create(&t_extraccion,NULL, hilo_extraccion,    NULL);
    pthread_create(&t_radar,     NULL, hilo_radar,         NULL);

    // Hilo principal: espera fin de partida
    while (g_estado.vivo)
        dormir_ms(100);

    sleep(2); // tiempo para ver el GAME OVER

    // 6. Limpieza
    pthread_cancel(t_vital);
    pthread_join(t_vital,      NULL);
    pthread_join(t_propulsion, NULL);
    pthread_join(t_extraccion, NULL);
    pthread_join(t_radar,      NULL);

    endwin();

    liberar_posicion(g_estado.mapa, g_estado.x, g_estado.y);
    barra_destroy(&g_estado.nave.barra_combustible);
    barra_destroy(&g_estado.nave.barra_oxigeno);
    pthread_mutex_destroy(&g_estado.mutex_estado);
    pthread_mutex_destroy(&g_ncurses_mutex);

    mapa_desconectar(g_estado.mapa);
    printf("[NAVE] Desconectada.\n");
    exit(EXIT_SUCCESS);
}
