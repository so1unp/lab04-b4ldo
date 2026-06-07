#ifndef MAPA_H
#define MAPA_H

#include <stddef.h>
#include <semaphore.h>

#define MAP_ROWS 24
#define MAP_COLS 80
#define MAPA_SHM_NAME "/juego_mapa"

// Representación de los elementos en el mapa
#define CHAR_VACIO ' '
#define CHAR_NAVE 'N'
#define CHAR_ASTEROIDE 'A'
#define CHAR_ESTACION 'E'

typedef struct {
    char celdas[MAP_ROWS][MAP_COLS];
    sem_t semaforos[MAP_ROWS][MAP_COLS];
} MapaCompartido;

MapaCompartido *mapa_crear_servidor(void);
MapaCompartido *mapa_conectar_cliente(void);
void mapa_desconectar(MapaCompartido *mapa);
void mapa_destruir_servidor(MapaCompartido *mapa);
void mapa_limpiar(MapaCompartido *mapa);
void dibujarMapa(const MapaCompartido *mapa);

#endif // MAPA_H
