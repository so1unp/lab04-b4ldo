#ifndef MOVEMENT_H
#define MOVEMENT_H

#include "../model/mapa.h"
#include <stdbool.h>

#define MAX_TRAYECTORIA 256

typedef struct {
    int x;
    int y;
} Punto;

typedef struct {
    Punto puntos[MAX_TRAYECTORIA];
    int cantidad;
    int indice_actual;
} Trayectoria;

// Inicialización de semáforos del mapa (lado del Servidor)
void map_init_semaphores(MapaCompartido* mapa);

// Destrucción de semáforos del mapa (lado del Servidor)
void map_destroy_semaphores(MapaCompartido* mapa);

// Adquirir una celda inicial al spawnear un objeto
bool adquirir_posicion_inicial(MapaCompartido* mapa, int x, int y, char token, bool bloquear);

// Liberar una celda al destruir un objeto o sacarlo del mapa
void liberar_posicion(MapaCompartido* mapa, int x, int y);

// Mover un objeto de (x_actual, y_actual) a (x_nuevo, y_nuevo)
// Retorna true si el movimiento fue exitoso, false en caso contrario
bool intentar_mover_objeto(MapaCompartido* mapa, int* x_actual, int* y_actual, int x_nuevo, int y_nuevo, char token, bool bloquear);

// Generación de trayectoria lineal usando el algoritmo de Bresenham
void generar_trayectoria_bresenham(int x1, int y1, int x2, int y2, Trayectoria* tray);

// Generación de trayectoria lineal usando la ecuación de la recta y = mx + c
void generar_trayectoria_lineal_ecuacion(float m, float c, int x_inicio, int x_fin, Trayectoria* tray);

#endif // MOVEMENT_H
