#ifndef ASTEROIDE_H
#define ASTEROIDE_H

#include <pthread.h>
#include <stdbool.h>
#include "objeto_espacial.h"
#include "recursos.h"

typedef struct {
    ObjetoEspacial base;
    int minerales[CANTIDAD_RECURSOS];
    int pos_x;
    int pos_y;
    pthread_mutex_t mutex;
    bool activo;
} ASTEROIDE;

int asteroide_minar(ASTEROIDE *ast, int extraido[CANTIDAD_RECURSOS]);

#endif