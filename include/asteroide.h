#ifndef ASTEROIDE_H
#define ASTEROIDE_H

#include <pthread.h>
#include <stdbool.h>
#include "objeto_espacial.h"
#include "recursos.h"
#include "../tools/movement.h"

typedef struct {
    ObjetoEspacial base;
    int minerales[CANTIDAD_RECURSOS];
    int pos_x;
    int pos_y;
    pthread_mutex_t mutex;
    bool activo;
    bool es_movil;
    Trayectoria trayectoria;
    int velocidad_ms;
    long ultimo_movimiento_ms;
} ASTEROIDE;

#include <signal.h>

struct MapaCompartido;

typedef struct {
    struct MapaCompartido *mapa;
    int max_asteroides;
    volatile sig_atomic_t *keep_running;
} AsteroideThreadArgs;

int asteroide_minar(ASTEROIDE *ast, int extraido[CANTIDAD_RECURSOS]);
void asteroide_spawn(struct MapaCompartido *mapa, int ast_idx);
void asteroide_generar_entorno(struct MapaCompartido *mapa, int cant_asteroides);
void* asteroide_hilo_movimiento(void* arg);

#endif