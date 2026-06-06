#ifndef ASTEROIDE_H
#define ASTEROIDE_H

#include "objeto_espacial.h"
#include "recursos.h"

typedef struct {
    ObjetoEspacial base;
    int minerales[CANTIDAD_RECURSOS];
} ASTEROIDE;


#endif