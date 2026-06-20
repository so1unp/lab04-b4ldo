#ifndef OBJETO_ESPACIAL_H
#define OBJETO_ESPACIAL_H

#include "barra.h"

// Tipos de objetos para identificarlos
typedef enum {
    TIPO_NAVE,
    TIPO_ESTACION,
    TIPO_ASTEROIDE
} TipoObjeto;

// Clase Base
typedef struct {
    int id;
    TipoObjeto tipo;
    Barra barra_vida;
    float x, y;
    float velocidad;
} ObjetoEspacial;


#endif