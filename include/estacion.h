#ifndef ESTACION_H
#define ESTACION_H

#include "objeto_espacial.h"
#include "recursos.h"
#include <sys/types.h>
#include <stdbool.h>

/* Estructuras de mensajes IPC para la comunicación por colas de mensajes POSIX */

typedef enum {
    REQ_VENDER_MINERAL,
    REQ_COMPRAR_COMBUSTIBLE,
    REQ_COMPRAR_OXIGENO
} TipoPeticion;

typedef struct {
    pid_t pid_origen;
    TipoPeticion tipo;
    int recurso;       /* TipoRecurso (MINERAL_DEUTERIO, MINERAL_MUTEXIO, etc.) */
    int cantidad;      /* Cantidad a comprar o vender */
} MensajeEstacion;

typedef struct {
    bool es_alerta;    /* true si es una alerta de deuterio de una estación, false si es respuesta a transacción */
    int estacion_x;    /* Posición X de la estación (para alertas) */
    int estacion_y;    /* Posición Y de la estación (para alertas) */
    bool exito;        /* Indica si la transacción fue exitosa */
    int cantidad;      /* Cantidad de recurso transferido */
    char mensaje[128]; /* Mensaje descriptivo */
} MensajeNave;

typedef struct {
    ObjetoEspacial base;
    Barra barra_combustible;
    int capacidad;
    int ocupacion;
    int creditos;
} ESTACION;

#endif /* ESTACION_H */