#ifndef OBJETO_ESPACIAL_H
#define OBJETO_ESPACIAL_H

// Tipos de objetos para identificarlos si es necesario (RTI)
typedef enum {
    TIPO_NAVE,
    TIPO_ESTACION
} TipoObjeto;

// Definición de la interfaz (Tabla de Funciones Virtuales)
typedef struct ObjetoEspacialVTable {
    void (*actualizar)(void* self);
    void (*destruir)(void* self);
} ObjetoEspacialVTable;

// La "Clase Base"
typedef struct {
    const ObjetoEspacialVTable* vtable; // Puntero a las funciones específicas
    TipoObjeto tipo;
    float x, y, z;                      // Propiedades comunes
    float velocidad;
    int vida;
    int combustible;
} ObjetoEspacial;

// Funciones de la interfaz pública
void objeto_espacial_actualizar(ObjetoEspacial* obj);
void objeto_espacial_destruir(ObjetoEspacial* obj);

#endif