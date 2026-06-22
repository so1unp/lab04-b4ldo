#ifndef RECURSOS_H
#define RECURSOS_H


// Definimos los indices para los recursos
typedef enum {
    MINERAL_DEUTERIO = 0,
    MINERAL_MUTEXIO,
    MINERAL_SEMAFORITA,
    MINERAL_KERNELIO,
    CANTIDAD_RECURSOS
} TipoRecurso;

typedef struct {
    int precio_deuterio;
    int precio_mutexio;
    int precio_semaforita;
    int precio_kernelio;
    int precio_combustible;
    int precio_oxigeno;
} TarifasComerciales;

#endif