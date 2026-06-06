#ifndef NAVE_H
#define NAVE_H

#include "objeto_espacial.h"

typedef struct {
    ObjetoEspacial base;
    int combustible;
    int oxigeno;
    int inventario[CANTIDAD_RECURSOS];
} Nave;


#endif