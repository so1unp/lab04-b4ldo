#include "asteroide.h"
#include <stddef.h>

int asteroide_minar(ASTEROIDE *ast, int extraido[CANTIDAD_RECURSOS])
{
    if (ast == NULL) {
        return -1;
    }

    pthread_mutex_lock(&ast->mutex);

    bool tiene_minerales = false;
    for (int m = 0; m < CANTIDAD_RECURSOS; m++) {
        if (ast->minerales[m] > 0) {
            tiene_minerales = true;
            // Extraer máximo 20 unidades
            int cant = ast->minerales[m] >= 20 ? 20 : ast->minerales[m];
            ast->minerales[m] -= cant;
            extraido[m] = cant;
        } else {
            extraido[m] = 0;
        }
    }

    if (!tiene_minerales) {
        pthread_mutex_unlock(&ast->mutex);
        return -1; // Ya estaba vacío
    }

    // Verificar si quedó vacío después de la extracción
    bool vacio = true;
    for (int m = 0; m < CANTIDAD_RECURSOS; m++) {
        if (ast->minerales[m] > 0) {
            vacio = false;
            break;
        }
    }

    if (vacio) {
        ast->activo = false;
        pthread_mutex_unlock(&ast->mutex);
        return 0; // Se agotó por completo
    }

    pthread_mutex_unlock(&ast->mutex);
    return 1; // Aún le quedan minerales
}
