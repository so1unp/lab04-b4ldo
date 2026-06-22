/* =============================================================================
 * servidor.c — Proceso servidor del cuadrante espacial (CosmiKernel)
 *
 * Responsabilidades:
 *   1. Crear y gestionar la memoria compartida POSIX que contiene el mapa.
 *   2. Poblar el mapa inicial con asteroides (las estaciones se ubican solas
 *      cuando sus propios procesos arrrancan).
 *   3. Publicar las tarifas comerciales en la SHM para que todos los clientes
 *      las lean sin comunicación adicional.
 *   4. Detectar el Game Over global (todas las estaciones destruidas).
 *   5. Al cerrarse (Ctrl+C o Game Over), guardar el estado en disco y
 *      señalizar a todos los clientes poniendo servidor_activo = false en la SHM.
 *
 * Concurrencia:
 *   - El servidor NO es multihilo; sólo el hilo principal corre el loop.
 *   - Los semáforos POSIX de cada celda del mapa son inicializados aquí con
 *     PTHREAD_PROCESS_SHARED para que los clientes puedan usarlos.
 *   - Los mutexes de las naves (para el loot) también son PROCESS_SHARED.
 * ============================================================================= */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <mqueue.h>
#include <time.h>
#include <stdbool.h>
#include <pthread.h>

#include "include/mapa.h"
#include "tools/movement.h"

/* ── Configuración inicial leída de config.txt ───────────────────────────── */
typedef struct {
    int estaciones;       /* Nro máximo de estaciones (informativo, max 3)   */
    int asteroides;       /* Cantidad de asteroides a generar al inicio       */
    int precio_deuterio;
    int precio_mutexio;
    int precio_semaforita;
    int precio_kernelio;
    int precio_combustible;
    int precio_oxigeno;
} Configuracion;

/* ── Control de señales ─────────────────────────────────────────────────── */
/* Usamos sig_atomic_t para que la escritura sea atómica desde el handler.  */
volatile sig_atomic_t keep_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    keep_running = 0;
}

/* ── Carga de configuración ─────────────────────────────────────────────── */
/*
 * Carga parámetros desde un archivo de texto con formato "clave=valor".
 * Líneas que empiezan con '#' o están vacías son ignoradas.
 * Si el archivo no existe, se usan los valores por defecto ya asignados.
 */
static int cargar_configuracion(const char *filename, Configuracion *config)
{
    /* Valores por defecto */
    config->estaciones      = 3;
    config->asteroides      = 5;
    config->precio_deuterio   = 10;
    config->precio_mutexio    = 20;
    config->precio_semaforita = 30;
    config->precio_kernelio   = 40;
    config->precio_combustible = 5;
    config->precio_oxigeno     = 5;

    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        perror("Error al abrir config.txt");
        return -1;
    }

    char linea[128];
    while (fgets(linea, sizeof(linea), fp)) {
        if (linea[0] == '#' || linea[0] == '\n' || linea[0] == '\r')
            continue;

        char clave[64];
        int  valor;
        if (sscanf(linea, "%63[^=]=%d", clave, &valor) != 2)
            continue;

        if      (strcmp(clave, "estaciones")       == 0) config->estaciones       = valor;
        else if (strcmp(clave, "asteroides")        == 0) config->asteroides        = valor;
        else if (strcmp(clave, "precio_deuterio")   == 0) config->precio_deuterio   = valor;
        else if (strcmp(clave, "precio_mutexio")    == 0) config->precio_mutexio    = valor;
        else if (strcmp(clave, "precio_semaforita") == 0) config->precio_semaforita = valor;
        else if (strcmp(clave, "precio_kernelio")   == 0) config->precio_kernelio   = valor;
        else if (strcmp(clave, "precio_combustible")== 0) config->precio_combustible= valor;
        else if (strcmp(clave, "precio_oxigeno")    == 0) config->precio_oxigeno    = valor;
    }
    fclose(fp);

    /* El README establece un máximo de 3 estaciones. */
    if (config->estaciones > 3) config->estaciones = 3;
    if (config->estaciones < 1) config->estaciones = 1;

    return 0;
}

/* ── Inicialización de mutexes de naves ─────────────────────────────────── */
/*
 * Los mutexes de RegistroNave viven en la SHM y deben ser accesibles por
 * múltiples procesos (PTHREAD_PROCESS_SHARED).  Los inicializa el servidor
 * porque es quien crea la SHM primero.
 */
static void inicializar_mutexes_naves(MapaCompartido *mapa)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    for (int i = 0; i < MAX_NAVES; i++) {
        pthread_mutex_init(&mapa->naves[i].mutex, &attr);
    }
    pthread_mutexattr_destroy(&attr);
}

/* ── Publicación de tarifas en la SHM ──────────────────────────────────── */
/*
 * Las tarifas se almacenan en la SHM para evitar que cada cliente deba
 * leer su propia copia del config.txt.  Así se garantiza consistencia.
 */
static void publicar_tarifas(MapaCompartido *mapa, const Configuracion *config)
{
    mapa->tarifas.precio_deuterio    = config->precio_deuterio;
    mapa->tarifas.precio_mutexio     = config->precio_mutexio;
    mapa->tarifas.precio_semaforita  = config->precio_semaforita;
    mapa->tarifas.precio_kernelio    = config->precio_kernelio;
    mapa->tarifas.precio_combustible = config->precio_combustible;
    mapa->tarifas.precio_oxigeno     = config->precio_oxigeno;
}

static long obtener_tiempo_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ── Generación de asteroides ───────────────────────────────────────────── */
static void spawn_asteroide(MapaCompartido *mapa, int ast_idx)
{
    ASTEROIDE *ast = &mapa->asteroides[ast_idx];
    ast->es_movil = (rand() % 100 < 50); // 50% de probabilidad

    int x = 0, y = 0;
    bool exito = false;
    int intentos = 0;

    if (ast->es_movil) {
        int x1, y1, x2, y2;
        if (rand() % 2 == 0) { // Borde vertical
            x1 = (rand() % 2 == 0) ? 0 : MAP_COLS - 1;
            y1 = rand() % MAP_ROWS;
            x2 = (x1 == 0) ? MAP_COLS - 1 : 0;
            y2 = rand() % MAP_ROWS;
        } else { // Borde horizontal
            x1 = rand() % MAP_COLS;
            y1 = (rand() % 2 == 0) ? 0 : MAP_ROWS - 1;
            x2 = rand() % MAP_COLS;
            y2 = (y1 == 0) ? MAP_ROWS - 1 : 0;
        }
        
        generar_trayectoria_bresenham(x1, y1, x2, y2, &ast->trayectoria);
        
        // Buscar la primera celda libre en la trayectoria
        for (int i = 0; i < ast->trayectoria.cantidad; i++) {
            if (adquirir_posicion_inicial(mapa, ast->trayectoria.puntos[i].x, ast->trayectoria.puntos[i].y, CHAR_ASTEROIDE, false)) {
                ast->trayectoria.indice_actual = i;
                x = ast->trayectoria.puntos[i].x;
                y = ast->trayectoria.puntos[i].y;
                exito = true;
                break;
            }
        }
        ast->velocidad_ms = 500 + (rand() % 1500); // 500ms a 2000ms
        ast->ultimo_movimiento_ms = obtener_tiempo_ms();
    } else {
        while (!exito && intentos < 10000) {
            x = rand() % MAP_COLS;
            y = rand() % MAP_ROWS;
            exito = adquirir_posicion_inicial(mapa, x, y, CHAR_ASTEROIDE, false);
            intentos++;
        }
    }

    if (!exito) {
        ast->activo = false;
        return;
    }

    ast->pos_x = x;
    ast->pos_y = y;
    ast->base.id = ast_idx;
    ast->base.tipo = TIPO_ASTEROIDE;
    ast->base.x = (float)x;
    ast->base.y = (float)y;
    ast->base.velocidad = 0.0f;

    bool tiene_mineral = false;
    for (int m = 0; m < CANTIDAD_RECURSOS; m++) {
        if (rand() % 100 < 75) {
            ast->minerales[m] = 100 + (rand() % 401);
            tiene_mineral = true;
        } else {
            ast->minerales[m] = 0;
        }
    }
    if (!tiene_mineral) ast->minerales[MINERAL_DEUTERIO] = 200;

    ast->activo = true;
}

static void generar_entorno(MapaCompartido *mapa, const Configuracion *config)
{
    srand((unsigned int)time(NULL));

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

    for (int i = 0; i < MAX_ASTEROIDES; i++) {
        pthread_mutex_init(&mapa->asteroides[i].mutex, &attr);
        mapa->asteroides[i].activo = false;
    }
    pthread_mutexattr_destroy(&attr);

    for (int i = 0; i < config->asteroides; i++) {
        spawn_asteroide(mapa, i);
    }
}

static MapaCompartido *mapa_global = NULL;
static int max_asteroides_config = 0;

static void* hilo_movimiento_asteroides(void* arg) {
    (void)arg;
    while (keep_running) {
        long ahora = obtener_tiempo_ms();
        int activos = 0;

        for (int i = 0; i < MAX_ASTEROIDES; i++) {
            ASTEROIDE *ast = &mapa_global->asteroides[i];
            if (ast->activo) {
                activos++;
                if (ast->es_movil) {
                    if (ahora - ast->ultimo_movimiento_ms >= ast->velocidad_ms) {
                        // Anclaje Magnético: trylock
                        if (pthread_mutex_trylock(&ast->mutex) == 0) {
                            int next_idx = ast->trayectoria.indice_actual + 1;
                            if (next_idx < ast->trayectoria.cantidad) {
                                int nx = ast->trayectoria.puntos[next_idx].x;
                                int ny = ast->trayectoria.puntos[next_idx].y;
                                
                                if (intentar_mover_objeto(mapa_global, &ast->pos_x, &ast->pos_y, nx, ny, CHAR_ASTEROIDE, false)) {
                                    ast->trayectoria.indice_actual = next_idx;
                                    ast->base.x = (float)ast->pos_x;
                                    ast->base.y = (float)ast->pos_y;
                                }
                            } else {
                                // Final de trayectoria
                                liberar_posicion(mapa_global, ast->pos_x, ast->pos_y);
                                ast->activo = false;
                                activos--;
                            }
                            ast->ultimo_movimiento_ms = ahora;
                            pthread_mutex_unlock(&ast->mutex);
                        } else {
                            // Está siendo minado, anclado temporalmente
                            ast->ultimo_movimiento_ms = ahora;
                        }
                    }
                }
            }
        }
        
        if (activos < max_asteroides_config) {
            for (int i = 0; i < MAX_ASTEROIDES; i++) {
                if (!mapa_global->asteroides[i].activo) {
                    spawn_asteroide(mapa_global, i);
                    break;
                }
            }
        }

        struct timespec req_ast = {0, 50000000}; // 50ms
        nanosleep(&req_ast, NULL);
    }
    return NULL;
}

/* ── Contar estaciones activas en el mapa ───────────────────────────────── */
static int contar_estaciones(const MapaCompartido *mapa)
{
    int total = 0;
    for (int r = 0; r < MAP_ROWS; r++)
        for (int c = 0; c < MAP_COLS; c++)
            if (mapa->celdas[r][c] == CHAR_ESTACION)
                total++;
    return total;
}

/* ── Guardado del estado en disco ───────────────────────────────────────── */
/*
 * Vuelca la estructura MapaCompartido completa en un archivo binario.
 * Esto permite recuperar posiciones, minerales y créditos si el servidor
 * se reinicia (siempre que los semáforos/mutexes sean reinicializados).
 */
static void guardar_estado(const MapaCompartido *mapa)
{
    FILE *fp = fopen("estado_servidor.dat", "wb");
    if (fp) {
        fwrite(mapa, sizeof(MapaCompartido), 1, fp);
        fclose(fp);
        printf("[SERVIDOR] Estado guardado en estado_servidor.dat.\n");
    } else {
        perror("[SERVIDOR] Error al guardar el estado");
    }
}

/* ── Renderizado del mapa en la terminal del servidor ───────────────────── */
static void render_servidor(const MapaCompartido *mapa, const Configuracion *cfg,
                             int estaciones_vivas)
{
    printf("\033[H\033[J"); /* Limpiar terminal (ANSI) */
    printf("=== COSMIKERNEL: SERVIDOR DEL CUADRANTE ===\n");
    printf("Estaciones activas: %d | Asteroides conf.: %d\n",
           estaciones_vivas, cfg->asteroides);
    printf("Precios — Deu:%d Mut:%d Sem:%d Ker:%d Fuel:%d O2:%d\n",
           cfg->precio_deuterio, cfg->precio_mutexio,
           cfg->precio_semaforita, cfg->precio_kernelio,
           cfg->precio_combustible, cfg->precio_oxigeno);
    printf("%.80s\n", "--------------------------------------------------------------------------------");
    dibujarMapa(mapa);
    printf("%.80s\n", "--------------------------------------------------------------------------------");
    printf("Ctrl+C para apagar el servidor limpiamente.\n");
}

/* ── main ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* 1. Registrar manejadores de señal para salida limpia */
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT,  &sa, NULL) == -1) { perror("sigaction SIGINT");  exit(EXIT_FAILURE); }
    if (sigaction(SIGTERM, &sa, NULL) == -1) { perror("sigaction SIGTERM"); exit(EXIT_FAILURE); }

    printf("[SERVIDOR] Iniciando...\n");

    /* 2. Cargar configuración */
    Configuracion config;
    if (cargar_configuracion("config.txt", &config) != 0)
        fprintf(stderr, "[SERVIDOR] Usando configuración por defecto.\n");

    /* 3. Crear la memoria compartida POSIX e inicializar semáforos de celdas */
    MapaCompartido *mapa = mapa_crear_servidor();
    if (mapa == NULL) {
        fprintf(stderr, "[SERVIDOR] Error crítico: no se pudo crear la SHM.\n");
        exit(EXIT_FAILURE);
    }

    /* 4. Inicializar mutexes de naves (PROCESS_SHARED) para el loot */
    inicializar_mutexes_naves(mapa);

    /* 5. Publicar tarifas en la SHM */
    publicar_tarifas(mapa, &config);

    /* 6. Poblar el mapa con asteroides */
    generar_entorno(mapa, &config);
    printf("[SERVIDOR] Mapa inicializado. Lanzando hilo de asteroides...\n");

    mapa_global = mapa;
    max_asteroides_config = config.asteroides;
    pthread_t t_asteroides;
    pthread_create(&t_asteroides, NULL, hilo_movimiento_asteroides, NULL);

    /* 7. Loop principal: renderizar mapa y vigilar condición de Game Over.
     *    El Game Over global ocurre cuando todas las estaciones desaparecen
     *    (se quedan sin combustible y explotan), DESPUÉS de que al menos una
     *    había estado activa (para no dispararlo al inicio, antes de que
     *    los procesos estación arranquen).                                    */
    struct timespec req = {1, 0};
    bool juego_iniciado = false;

    while (keep_running) {
        int estaciones_vivas = contar_estaciones(mapa);

        if (estaciones_vivas > 0) juego_iniciado = true;

        if (juego_iniciado && estaciones_vivas == 0) {
            printf("\033[H\033[J");
            printf("\n===================================================================\n");
            printf("[SERVIDOR] !TODAS LAS ESTACIONES DESTRUIDAS! — GAME OVER GLOBAL\n");
            printf("===================================================================\n");
            mapa->game_over_global = true;
            keep_running = 0;
            continue;
        }

        render_servidor(mapa, &config, estaciones_vivas);
        nanosleep(&req, NULL);
    }

    /* 8. Cierre ordenado */
    printf("\n[SERVIDOR] %s\n",
           mapa->game_over_global
               ? "Apagando por GAME OVER global."
               : "Apagando por señal del operador.");

    /* Guardar estado en disco antes de liberar la SHM */
    guardar_estado(mapa);

    /* Esperar que termine el hilo de asteroides */
    pthread_join(t_asteroides, NULL);

    /* Notificar a los clientes poniendo el flag en false.
     * Los clientes pollan este flag y reaccionan al detectarlo.
     * Esperamos 2 segundos para darles tiempo de terminar limpiamente. */
    mapa->servidor_activo = false;
    printf("[SERVIDOR] Flag servidor_activo desactivado. Esperando clientes...\n");
    sleep(2);

    /* Destruir la SHM y los semáforos internos */
    mapa_destruir_servidor(mapa);
    printf("[SERVIDOR] SHM destruida. Salida limpia.\n");

    exit(EXIT_SUCCESS);
}
