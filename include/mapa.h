/* =============================================================================
 * mapa.h — Memoria compartida POSIX del cuadrante espacial
 *
 * Este header define la estructura MapaCompartido que vive en una región de
 * memoria compartida POSIX (shm_open / mmap).  El proceso servidor la crea
 * y la inicializa; los clientes (naves y estaciones) se conectan a ella para
 * leer el estado del mapa y modificar sus propias entradas.
 *
 * Sincronización de celdas:
 *   Cada celda [fila][columna] tiene un semáforo binario POSIX asociado
 *   (sem_t semaforos[MAP_ROWS][MAP_COLS]) con valor inicial 1 (libre).
 *   Para ocupar una celda se hace sem_wait / sem_trywait; para liberarla
 *   sem_post.  Esto garantiza exclusión mutua a nivel de celda y evita que
 *   dos naves pisen la misma posición.
 * ============================================================================= */

#ifndef MAPA_H
#define MAPA_H

#include <stddef.h>
#include <semaphore.h>
#include <stdbool.h>
#include <pthread.h>
#include "asteroide.h"
#include "recursos.h"

/* ── Dimensiones del mapa ─────────────────────────────────────────────── */
#define MAP_ROWS        24
#define MAP_COLS        80
#define MAPA_SHM_NAME   "/juego_mapa"   /* Nombre del objeto de SHM POSIX   */
#define MAX_ASTEROIDES  50
#define MAX_NAVES       10

/* ── Caracteres que representan cada entidad en el mapa de texto ───────── */
#define CHAR_VACIO    ' '
#define CHAR_NAVE     'N'
#define CHAR_ASTEROIDE 'A'
#define CHAR_ESTACION 'E'

/* ── Registro de cada nave en la SHM ────────────────────────────────────
 *
 * Permite al servidor y a otras naves conocer el estado de cada nave:
 *   - creditos: saldo actual (actualizado por las estaciones).
 *   - incapacitada: true cuando la nave se quedó sin O2 o combustible.
 *     El proceso nave termina pero deja 'activo = true' e 'incapacitada = true'
 *     para que su cuerpo siga en el mapa y pueda ser saqueado.
 *   - pos_x / pos_y: posición en el mapa (para que otras naves la encuentren).
 *   - inventario / combustible / oxigeno: recursos disponibles para el loot.
 *   - mutex: PTHREAD_PROCESS_SHARED, protege el saqueo concurrente.
 * ──────────────────────────────────────────────────────────────────────── */
typedef struct {
    int  pid;
    int  creditos;
    bool activo;
    bool incapacitada;
    int  pos_x;
    int  pos_y;
    int  inventario[CANTIDAD_RECURSOS];
    int  combustible;
    int  oxigeno;
    pthread_mutex_t mutex;   /* PTHREAD_PROCESS_SHARED */
} RegistroNave;

/* ── Estructura principal de la memoria compartida ──────────────────────
 *
 * Reside en SHM POSIX.  El servidor la crea con mapa_crear_servidor() y
 * los clientes la mapean con mapa_conectar_cliente().
 *
 *   celdas:          Grilla de caracteres visible por todos los procesos.
 *   semaforos:       Un semáforo binario por celda (exclusión mutua posicional).
 *   asteroides:      Datos de cada asteroide (minerales, mutex, estado).
 *   naves:           Registro de cada nave activa/incapacitada (ver RegistroNave).
 *   tarifas:         Precios publicados por el servidor al inicio.
 *   servidor_activo: El servidor lo pone en false al cerrarse. Los clientes
 *                    pollan este flag para detectar la desconexión y terminar.
 *   game_over_global: true si todas las estaciones explotaron.
 * ──────────────────────────────────────────────────────────────────────── */
typedef struct MapaCompartido {
    char         celdas[MAP_ROWS][MAP_COLS];
    sem_t        semaforos[MAP_ROWS][MAP_COLS];
    ASTEROIDE    asteroides[MAX_ASTEROIDES];
    RegistroNave naves[MAX_NAVES];
    TarifasComerciales tarifas;
    bool         servidor_activo;
    bool         game_over_global;
} MapaCompartido;

/* ── Prototipos ─────────────────────────────────────────────────────────── */
MapaCompartido *mapa_crear_servidor(void);    /* Crea y limpia la SHM        */
MapaCompartido *mapa_conectar_cliente(void);  /* Mapea la SHM existente      */
void mapa_desconectar(MapaCompartido *mapa);  /* munmap + cierra fd          */
void mapa_destruir_servidor(MapaCompartido *mapa); /* munmap + shm_unlink    */
void mapa_limpiar(MapaCompartido *mapa);      /* Pone todo en estado inicial */
void dibujarMapa(const MapaCompartido *mapa); /* Imprime el mapa en stdout   */

#endif /* MAPA_H */
