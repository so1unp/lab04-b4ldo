#ifndef OBJETO_ESPACIAL_H
#define OBJETO_ESPACIAL_H

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
    int vida;
    int x, y;
} ObjetoEspacial;


#endif