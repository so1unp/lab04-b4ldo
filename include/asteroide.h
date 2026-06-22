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

int asteroide_minar(ASTEROIDE *ast, int extraido[CANTIDAD_RECURSOS]);

#endif