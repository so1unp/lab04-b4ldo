#ifndef CONFIGURACION_H
#define CONFIGURACION_H

typedef struct {
    int estaciones;       /* Nro máximo de estaciones (informativo, max 3)   */
    int asteroides;       /* Cantidad de asteroides a generar al inicio       */
    int precio_deuterio;
    int precio_mutexio;
    int precio_semaforita;
    int precio_kernelio;
    int precio_combustible;
    int precio_oxigeno;
} Configuracion;

/*
 * Carga parámetros desde un archivo de texto con formato "clave=valor".
 * Busca en la ruta especificada y, si falla, intenta buscar en la carpeta superior.
 * Si no encuentra el archivo, asigna valores por defecto.
 */
int cargar_configuracion(const char *filename, Configuracion *config);

#endif /* CONFIGURACION_H */
