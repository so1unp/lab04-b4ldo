#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <mqueue.h>
#include <time.h>
#include <stdbool.h>

#include "include/mapa.h"
#include "tools/movement.h"

// Estructura de configuración
typedef struct {
    int estaciones;
    int asteroides;
    int precio_deuterio;
    int precio_mutexio;
    int precio_semaforita;
    int precio_kernelio;
    int precio_combustible;
    int precio_oxigeno;
} Configuracion;

// Variable global para controlar la ejecución
volatile sig_atomic_t keep_running = 1;

// Manejador de señal para una salida limpia
void handle_signal(int sig) {
    (void)sig; // Evitar warning de variable no usada
    keep_running = 0;
}

// Función para cargar la configuración desde un archivo
int cargar_configuracion(const char *filename, Configuracion *config) {
    // Valores por defecto razonables primero, por si falla la apertura del archivo
    config->estaciones = 3;
    config->asteroides = 5;
    config->precio_deuterio = 10;
    config->precio_mutexio = 20;
    config->precio_semaforita = 30;
    config->precio_kernelio = 40;
    config->precio_combustible = 5;
    config->precio_oxigeno = 5;

    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        perror("Error al abrir config.txt");
        return -1;
    }

    char linea[128];
    while (fgets(linea, sizeof(linea), fp)) {
        if (linea[0] == '#' || linea[0] == '\n' || linea[0] == '\r') {
            continue;
        }

        char clave[64];
        int valor;
        if (sscanf(linea, "%63[^=]=%d", clave, &valor) == 2) {
            if (strcmp(clave, "estaciones") == 0) {
                config->estaciones = valor;
            } else if (strcmp(clave, "asteroides") == 0) {
                config->asteroides = valor;
            } else if (strcmp(clave, "precio_deuterio") == 0) {
                config->precio_deuterio = valor;
            } else if (strcmp(clave, "precio_mutexio") == 0) {
                config->precio_mutexio = valor;
            } else if (strcmp(clave, "precio_semaforita") == 0) {
                config->precio_semaforita = valor;
            } else if (strcmp(clave, "precio_kernelio") == 0) {
                config->precio_kernelio = valor;
            } else if (strcmp(clave, "precio_combustible") == 0) {
                config->precio_combustible = valor;
            } else if (strcmp(clave, "precio_oxigeno") == 0) {
                config->precio_oxigeno = valor;
            }
        }
    }

    fclose(fp);

    // Limitación exigida por el README (máximo 3 estaciones)
    if (config->estaciones > 3) {
        config->estaciones = 3;
    }
    if (config->estaciones < 1) {
        config->estaciones = 1;
    }
    return 0;
}

// Función para poblar el cuadrante con estaciones y asteroides
void generar_entorno(MapaCompartido *mapa, const Configuracion *config) {
    srand((unsigned int)time(NULL));

    // Las estaciones se posicionan de manera dinámica al lanzar su propio proceso cliente.

    // Posicionar asteroides
    for (int i = 0; i < config->asteroides; i++) {
        int x, y;
        bool exito = false;
        int intentos = 0;
        while (!exito && intentos < 10000) {
            x = rand() % MAP_COLS;
            y = rand() % MAP_ROWS;
            exito = adquirir_posicion_inicial(mapa, x, y, CHAR_ASTEROIDE, false);
            intentos++;
        }
        if (!exito) {
            fprintf(stderr, "[SERVIDOR] No se pudo ubicar asteroide %d: mapa lleno.\n", i + 1);
        } else {
            // Buscar un slot libre para el asteroide en la memoria compartida
            int ast_idx = -1;
            for (int k = 0; k < MAX_ASTEROIDES; k++) {
                if (!mapa->asteroides[k].activo) {
                    ast_idx = k;
                    break;
                }
            }
            if (ast_idx != -1) {
                ASTEROIDE *ast = &mapa->asteroides[ast_idx];
                ast->pos_x = x;
                ast->pos_y = y;
                ast->base.id = ast_idx;
                ast->base.tipo = TIPO_ASTEROIDE;
                ast->base.x = (float)x;
                ast->base.y = (float)y;
                ast->base.velocidad = 0.0f;

                // Inicializar mutex compartido para permitir concurrencia de naves minando
                pthread_mutexattr_t attr;
                pthread_mutexattr_init(&attr);
                pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
                pthread_mutex_init(&ast->mutex, &attr);
                pthread_mutexattr_destroy(&attr);

                // Asignar minerales de forma aleatoria (mínimo 1 mineral)
                bool tiene_mineral = false;
                for (int m = 0; m < CANTIDAD_RECURSOS; m++) {
                    if (rand() % 100 < 75) { // 75% de probabilidad para cada recurso
                        ast->minerales[m] = 100 + (rand() % 401); // Entre 100 y 500 unidades
                        tiene_mineral = true;
                    } else {
                        ast->minerales[m] = 0;
                    }
                }

                // Garantizar al menos Deuterio si todos salieron en 0
                if (!tiene_mineral) {
                    ast->minerales[MINERAL_DEUTERIO] = 200;
                }

                ast->activo = true;
            } else {
                fprintf(stderr, "[SERVIDOR] No hay ranuras de asteroides libres en memoria compartida.\n");
            }
        }
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    // Configurar manejador de señales
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Error al configurar SIGINT");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("Error al configurar SIGTERM");
        exit(EXIT_FAILURE);
    }

    printf("[SERVIDOR] Iniciando servidor del cuadrante espacial...\n");

    // 1. Cargar configuración
    Configuracion config;
    if (cargar_configuracion("config.txt", &config) != 0) {
        fprintf(stderr, "[SERVIDOR] Usando valores de configuración por defecto.\n");
    }

    // 2. Crear el mapa compartido
    MapaCompartido *mapa = mapa_crear_servidor();
    if (mapa == NULL) {
        fprintf(stderr, "[SERVIDOR] Error crítico: no se pudo crear el mapa compartido.\n");
        exit(EXIT_FAILURE);
    }

    // 3. Ubicar asteroides y estaciones
    generar_entorno(mapa, &config);
    printf("[SERVIDOR] Mapa inicializado y poblado con éxito.\n");

    // Bucle principal: Limpiar terminal, renderizar mapa y esperar
    struct timespec req = {1, 0}; // 1 segundo
    while (keep_running) {
        // Limpiar pantalla usando códigos de escape ANSI
        printf("\033[H\033[J");
        printf("=== COSMIKERNEL: SERVIDOR DEL CUADRANTE ESPACIAL ===\n");
        printf("Estaciones: %d | Asteroides: %d\n", config.estaciones, config.asteroides);
        printf("Precios: Deuterio=%d | Mutexio=%d | Semaforita=%d | Kernelio=%d\n", 
               config.precio_deuterio, config.precio_mutexio, config.precio_semaforita, config.precio_kernelio);
        printf("--------------------------------------------------------------------------------\n");
        
        dibujarMapa(mapa);
        
        printf("--------------------------------------------------------------------------------\n");
        printf("Presiona Ctrl+C para apagar el servidor y limpiar los recursos IPC.\n");
        
        nanosleep(&req, NULL);
    }

    printf("\n[SERVIDOR] Apagando el servidor...\n");

    // 6. Destruir el mapa compartido y limpiar semáforos
    mapa_destruir_servidor(mapa);
    printf("[SERVIDOR] Mapa compartido destruido. Salida limpia.\n");

    exit(EXIT_SUCCESS);
}


