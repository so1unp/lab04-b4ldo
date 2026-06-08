#ifndef NAVE_H
#define NAVE_H

#include "objeto_espacial.h"
#include "recursos.h"

typedef struct {
    ObjetoEspacial base;
    int combustible;
    int oxigeno;
    float velocidad;
    int inventario[CANTIDAD_RECURSOS];
} Nave;

// --- COMPORTAMIENTOS DE LA NAVE ---

// Prepara la nave con sus valores iniciales
void nave_inicializar(Nave* n, int id, int x_inicial, int y_inicial);

// Lógica de consumo vital (para el hilo correspondiente)
void nave_consumir_oxigeno(Nave* n);

// Verifica si la nave sigue viva
bool nave_esta_operativa(Nave* n);

// Lógica de impresión de estado (para debug local)
void nave_imprimir_estado(Nave* n);

#endif