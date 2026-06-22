#include "asteroide.h"
#include "mapa.h"
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

int asteroide_minar(ASTEROIDE *ast, int extraido[CANTIDAD_RECURSOS])
{
    if (ast == NULL) {
        return -1;
    }

    /* Inicializar extraido a 0 */
    for (int m = 0; m < CANTIDAD_RECURSOS; m++) {
        extraido[m] = 0;
    }

    /* Identificar qué recursos están disponibles */
    int disponibles[CANTIDAD_RECURSOS];
    int count = 0;
    for (int m = 0; m < CANTIDAD_RECURSOS; m++) {
        if (ast->minerales[m] > 0) {
            disponibles[count++] = m;
        }
    }

    if (count == 0) {
        return -1; /* Ya estaba vacío */
    }

    /* Seleccionar aleatoriamente uno de los recursos disponibles */
    int idx_recurso = disponibles[rand() % count];

    /* Extraer una cantidad aleatoria (entre 10 y 25 unidades) */
    int cant_max = 10 + (rand() % 16);
    int cant = ast->minerales[idx_recurso] >= cant_max ? cant_max : ast->minerales[idx_recurso];
    
    ast->minerales[idx_recurso] -= cant;
    extraido[idx_recurso] = cant;

    /* Verificar si el asteroide quedó completamente vacío */
    bool vacio = true;
    for (int m = 0; m < CANTIDAD_RECURSOS; m++) {
        if (ast->minerales[m] > 0) {
            vacio = false;
            break;
        }
    }

    if (vacio) {
        ast->activo = false;
        return 0; /* Se agotó por completo */
    }

    return 1; /* Aún le quedan minerales */
}

static long obtener_tiempo_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void asteroide_spawn(struct MapaCompartido *mapa, int ast_idx)
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

void asteroide_generar_entorno(struct MapaCompartido *mapa, int cant_asteroides)
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

    for (int i = 0; i < cant_asteroides; i++) {
        asteroide_spawn(mapa, i);
    }
}

void* asteroide_hilo_movimiento(void* arg)
{
    AsteroideThreadArgs *args = (AsteroideThreadArgs *)arg;
    struct MapaCompartido *mapa = args->mapa;
    int max_asteroides_config = args->max_asteroides;
    volatile sig_atomic_t *keep_running = args->keep_running;

    while (*keep_running) {
        long ahora = obtener_tiempo_ms();
        int activos = 0;

        for (int i = 0; i < MAX_ASTEROIDES; i++) {
            ASTEROIDE *ast = &mapa->asteroides[i];
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
                                
                                if (intentar_mover_objeto(mapa, &ast->pos_x, &ast->pos_y, nx, ny, CHAR_ASTEROIDE, false)) {
                                    ast->trayectoria.indice_actual = next_idx;
                                    ast->base.x = (float)ast->pos_x;
                                    ast->base.y = (float)ast->pos_y;
                                }
                            } else {
                                // Final de trayectoria
                                liberar_posicion(mapa, ast->pos_x, ast->pos_y);
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
                if (!mapa->asteroides[i].activo) {
                    asteroide_spawn(mapa, i);
                    break;
                }
            }
        }

        struct timespec req_ast = {0, 50000000}; // 50ms
        nanosleep(&req_ast, NULL);
    }
    return NULL;
}
