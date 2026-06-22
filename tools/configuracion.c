#include "configuracion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cargar_configuracion(const char *filename, Configuracion *config)
{
    /* Valores por defecto */
    config->estaciones      = 3;
    config->asteroides      = 5;
    config->precio_deuterio   = 10;
    config->precio_mutexio    = 20;
    config->precio_semaforita = 30;
    config->precio_kernelio   = 40;
    config->precio_combustible = 5;
    config->precio_oxigeno     = 5;

    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        /* Intentar en el directorio padre por si se ejecuta desde bin/ */
        char path[256];
        snprintf(path, sizeof(path), "../%s", filename);
        fp = fopen(path, "r");
    }

    if (fp == NULL) {
        perror("Error al abrir config.txt");
        return -1;
    }

    char linea[128];
    while (fgets(linea, sizeof(linea), fp)) {
        if (linea[0] == '#' || linea[0] == '\n' || linea[0] == '\r')
            continue;

        char clave[64];
        int  valor;
        if (sscanf(linea, "%63[^=]=%d", clave, &valor) != 2)
            continue;

        if      (strcmp(clave, "estaciones")       == 0) config->estaciones       = valor;
        else if (strcmp(clave, "asteroides")        == 0) config->asteroides        = valor;
        else if (strcmp(clave, "precio_deuterio")   == 0) config->precio_deuterio   = valor;
        else if (strcmp(clave, "precio_mutexio")    == 0) config->precio_mutexio    = valor;
        else if (strcmp(clave, "precio_semaforita") == 0) config->precio_semaforita = valor;
        else if (strcmp(clave, "precio_kernelio")   == 0) config->precio_kernelio   = valor;
        else if (strcmp(clave, "precio_combustible")== 0) config->precio_combustible= valor;
        else if (strcmp(clave, "precio_oxigeno")    == 0) config->precio_oxigeno    = valor;
    }
    fclose(fp);

    /* El README establece un máximo de 3 estaciones. */
    if (config->estaciones > 3) config->estaciones = 3;
    if (config->estaciones < 1) config->estaciones = 1;

    return 0;
}
