#ifndef MAPA_H
#define MAPA_H

#include <semaphore.h>

#define MAP_ROWS 24
#define MAP_COLS 80

// Representación de los elementos en el mapa
#define CHAR_VACIO ' '
#define CHAR_NAVE 'N'
#define CHAR_ASTEROIDE 'A'
#define CHAR_ESTACION 'E'

typedef struct {
    char celdas[MAP_ROWS][MAP_COLS];
    sem_t semaforos[MAP_ROWS][MAP_COLS];
} MapaCompartido;

#endif // MAPA_H
