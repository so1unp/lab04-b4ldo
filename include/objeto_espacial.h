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
    float x, y;
    float velocidad;
} ObjetoEspacial;


#endif