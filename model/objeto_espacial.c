#include "objeto_espacial.h"
#include <stdlib.h>

void objeto_espacial_actualizar(ObjetoEspacial* obj) {
    if (obj && obj->vtable && obj->vtable->actualizar) {
        obj->vtable->actualizar(obj); // Despacho dinámico
    }
}

void objeto_espacial_destruir(ObjetoEspacial* obj) {
    if (obj && obj->vtable && obj->vtable->destruir) {
        obj->vtable->destruir(obj);
    }
}