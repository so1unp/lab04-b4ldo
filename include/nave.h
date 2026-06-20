#ifndef NAVE_H
#define NAVE_H

#include "objeto_espacial.h"
#include "recursos.h"

typedef struct {
    ObjetoEspacial base;
    Barra barra_combustible;
    Barra barra_oxigeno;
    int inventario[CANTIDAD_RECURSOS];
} Nave;


#endif