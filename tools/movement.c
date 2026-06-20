#include "movement.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// Helper para verificar límites del mapa
static bool en_limites(int x, int y) {
    return (x >= 0 && x < MAP_COLS && y >= 0 && y < MAP_ROWS);
}

// NOTA: la inicialización/destrucción de semáforos de la shm la maneja
// mapa.c (mapa_crear_servidor / mapa_destruir_servidor). No la dupliquemos
// acá para evitar doble sem_init/sem_destroy sobre la misma memoria.

// Adquirir una celda inicial al spawnear un objeto
bool adquirir_posicion_inicial(MapaCompartido* mapa, int x, int y, char token, bool bloquear) {
    if (!mapa || !en_limites(x, y)) return false;

    if (bloquear) {
        if (sem_wait(&mapa->semaforos[y][x]) != 0) {
            return false;
        }
    } else {
        if (sem_trywait(&mapa->semaforos[y][x]) != 0) {
            return false;
        }
    }

    // Actualizar la celda con el token del objeto
    mapa->celdas[y][x] = token;
    return true;
}

// Liberar una celda al destruir un objeto o sacarlo del mapa
void liberar_posicion(MapaCompartido* mapa, int x, int y) {
    if (!mapa || !en_limites(x, y)) return;

    // Ponemos la celda vacía y liberamos el semáforo
    mapa->celdas[y][x] = CHAR_VACIO;
    if (sem_post(&mapa->semaforos[y][x]) != 0) {
        perror("sem_post falló");
    }
}

// Mover un objeto de (x_actual, y_actual) a (x_nuevo, y_nuevo)
// Retorna true si el movimiento fue exitoso, false en caso contrario
bool intentar_mover_objeto(MapaCompartido* mapa, int* x_actual, int* y_actual, int x_nuevo, int y_nuevo, char token, bool bloquear) {
    if (!mapa || !x_actual || !y_actual) return false;
    
    // Si no cambió de posición, es exitoso (ya está ahí y tiene el semáforo)
    if (*x_actual == x_nuevo && *y_actual == y_nuevo) {
        return true;
    }

    // Validar límites de la nueva posición
    if (!en_limites(x_nuevo, y_nuevo)) {
        return false;
    }

    // Intentar reservar la nueva posición
    if (bloquear) {
        if (sem_wait(&mapa->semaforos[y_nuevo][x_nuevo]) != 0) {
            return false;
        }
    } else {
        if (sem_trywait(&mapa->semaforos[y_nuevo][x_nuevo]) != 0) {
            return false;
        }
    }

    // Si la reserva de la nueva celda fue exitosa, liberamos la celda vieja
    int x_viejo = *x_actual;
    int y_viejo = *y_actual;

    // Actualizar celdas en el mapa
    mapa->celdas[y_nuevo][x_nuevo] = token;
    mapa->celdas[y_viejo][x_viejo] = CHAR_VACIO;

    // Liberar el semáforo de la celda vieja
    if (sem_post(&mapa->semaforos[y_viejo][x_viejo]) != 0) {
        perror("sem_post de celda vieja falló");
    }

    // Actualizar las coordenadas del objeto
    *x_actual = x_nuevo;
    *y_actual = y_nuevo;

    return true;
}

// Generación de trayectoria lineal usando el algoritmo de Bresenham
void generar_trayectoria_bresenham(int x1, int y1, int x2, int y2, Trayectoria* tray) {
    if (!tray) return;

    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;

    int x = x1;
    int y = y1;
    int idx = 0;

    while (idx < MAX_TRAYECTORIA - 1) {
        tray->puntos[idx].x = x;
        tray->puntos[idx].y = y;
        idx++;

        if (x == x2 && y == y2) break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }

    tray->cantidad = idx;
    tray->indice_actual = 0;
}

// Generación de trayectoria lineal usando la ecuación de la recta y = mx + c
// Nota: Para pendientes m > 1 o m < -1, iteramos sobre Y para evitar huecos en la grilla.
void generar_trayectoria_lineal_ecuacion(float m, float c, int x_inicio, int x_fin, Trayectoria* tray) {
    if (!tray) return;

    int idx = 0;
    
    // Si la pendiente es suave (|m| <= 1), iteramos en X
    if (m >= -1.0f && m <= 1.0f) {
        int dir = (x_inicio <= x_fin) ? 1 : -1;
        for (int x = x_inicio; (dir > 0 ? x <= x_fin : x >= x_fin) && idx < MAX_TRAYECTORIA; x += dir) {
            int y = (int)roundf(m * (float)x + c);
            if (en_limites(x, y)) {
                tray->puntos[idx].x = x;
                tray->puntos[idx].y = y;
                idx++;
            }
        }
    } 
    // Si la pendiente es empinada (|m| > 1), iteramos en Y usando la inversa: x = (y - c) / m
    else {
        int y_inicio = (int)roundf(m * (float)x_inicio + c);
        int y_fin    = (int)roundf(m * (float)x_fin    + c);
        
        int dir = (y_inicio <= y_fin) ? 1 : -1;
        for (int y = y_inicio; (dir > 0 ? y <= y_fin : y >= y_fin) && idx < MAX_TRAYECTORIA; y += dir) {
            int x = (int)roundf(((float)y - c) / m);
            if (en_limites(x, y)) {
                tray->puntos[idx].x = x;
                tray->puntos[idx].y = y;
                idx++;
            }
        }
    }

    tray->cantidad = idx;
    tray->indice_actual = 0;
}
