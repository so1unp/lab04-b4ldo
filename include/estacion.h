#ifndef ESTACION_H
#define ESTACION_H

#include "objeto_espacial.h"

typedef struct {
    ObjetoEspacial base;
    Barra barra_combustible;
    int capacidad;
    int ocupacion;
} ESTACION;


#endif