#ifndef NAVE_H
#define NAVE_H

#include "objeto_espacial.h"
#include "recursos.h"
#include "mapa.h"
#include "estacion.h"
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <mqueue.h>
#include <signal.h>

#define FUEL_MAX          100
#define O2_MAX            100

typedef struct {
    ObjetoEspacial base;
    Barra barra_combustible;
    Barra barra_oxigeno;
    int inventario[CANTIDAD_RECURSOS];
    int creditos;
} Nave;

typedef struct {
    int x, y;
    bool activa;
    bool explotada;
    time_t timestamp;
} AlertaEstacion;

/* ─── Estado global de la Nave ─── */
typedef struct {
    Nave              nave;
    MapaCompartido   *mapa;
    int               x, y;
    volatile sig_atomic_t vivo;
    volatile sig_atomic_t extrayendo;
    long long         ultimo_e_press; // Tiempo del último toque al botón E en milisegundos
    volatile int      prog_ext;       // Progreso de extracción (-1 inactivo, 0-100 activo)
    pthread_mutex_t   mx_estado;

    /* Gestión del Hangar e IPC */
    bool              en_hangar;
    int               estacion_x;
    int               estacion_y;
    sem_t            *sem_hangar;
    mqd_t             mq_estacion;
    mqd_t             mq_respuesta;
    char              mq_respuesta_name[64];
    char              pid_file[64];

    /* Sincronización para respuestas IPC */
    pthread_cond_t    cond_respuesta;
    bool              hay_respuesta;
    MensajeNave       ultima_respuesta;

    /* Alertas y errores */
    AlertaEstacion    alertas[3];
    char              hud_error[140];
    time_t            hud_error_recibido;
    int               nave_slot_shm; // Slot de esta nave en la SHM para puntuación
} EstadoNave;

extern EstadoNave g;

#endif /* NAVE_H */