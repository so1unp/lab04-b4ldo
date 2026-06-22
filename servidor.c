/* =============================================================================
 * servidor.c — Proceso servidor del cuadrante espacial (CosmiKernel)
 *
 * Responsabilidades:
 *   1. Crear y gestionar la memoria compartida POSIX que contiene el mapa.
 *   2. Poblar el mapa inicial con asteroides llamando al módulo asteroide.
 *   3. Publicar las tarifas comerciales en la SHM para que todos los clientes
 *      las lean sin comunicación adicional.
 *   4. Detectar el Game Over global (todas las estaciones destruidas).
 *   5. Guardar el estado lógico persistente al cerrarse y notificar a los clientes.
 *
 * DOCUMENTACIÓN DE CONCURRENCIA:
 * 
 *   A. Escritores y Lectores de Flags de Control:
 *      - `servidor_activo` (en MapaCompartido):
 *        * Escritor: Únicamente el SERVIDOR (lo activa a true en inicialización,
 *          y lo apaga a false en el shutdown).
 *        * Lectores: Todos los procesos CLIENTE (naves y estaciones). Pollan este
 *          flag para enterarse si el servidor cayó y salir limpiamente.
 *      - `game_over_global` (en MapaCompartido):
 *        * Escritor: Únicamente el SERVIDOR (bucle principal). Se pone en true
 *          cuando se detecta que no quedan estaciones vivas en el mapa.
 *        * Lectores: Las naves espaciales, que al verlo abortan su ejecución.
 * 
 *   B. Recursos Process-Shared (Memoria Compartida):
 *      - `semaforos` (en MapaCompartido):
 *        * Grilla de semáforos anónimos POSIX (sem_t) compartidos entre procesos.
 *        * Sincronización: Protegen la exclusión mutua de posición física en la 
 *          celda [fila][columna]. Cada celda es adquirida por naves, asteroides 
 *          o estaciones usando `sem_wait`/`sem_trywait` y liberada por `sem_post`.
 *      - `mutex` en `RegistroNave` (en MapaCompartido):
 *        * Mutexes de tipo PTHREAD_PROCESS_SHARED.
 *        * Sincronización: Protege los recursos y la integridad de la nave (créditos,
 *          combustible, oxígeno, inventario) durante el proceso de comercio o saqueo (loot).
 *      - `mutex` en `ASTEROIDE` (en MapaCompartido):
 *        * Mutexes de tipo PTHREAD_PROCESS_SHARED.
 *        * Sincronización: Evita condiciones de carrera entre la simulación de
 *          movimiento del asteroide y la minería concurrente por parte de naves.
 *          El servidor usa `pthread_mutex_trylock` para moverlo; si falla (porque
 *          una nave lo está minando), el asteroide se mantiene estático (anclaje magnético).
 * ============================================================================= */

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
#include <pthread.h>

#include "include/mapa.h"
#include "include/configuracion.h"

/* ── Control de señales ─────────────────────────────────────────────────── */
/* Usamos sig_atomic_t para que la escritura sea atómica desde el handler.  */
volatile sig_atomic_t keep_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    keep_running = 0;
}

/* ── Estructuras para Guardado de Estado Lógico (Compacto) ───────── */
typedef struct {
    int id;
    int pos_x;
    int pos_y;
    bool es_movil;
    int minerales[CANTIDAD_RECURSOS];
} AsteroidePersistente;

typedef struct {
    int  pid;
    int  creditos;
    int  pos_x;
    int  pos_y;
    int  inventario[CANTIDAD_RECURSOS];
    int  combustible;
    int  oxigeno;
    int  escudo;
    bool incapacitada;
} NavePersistente;

/* ── Estructura de Estado del Servidor ────────────────────── */
typedef struct {
    Configuracion config;
    MapaCompartido *mapa;
    pthread_t t_asteroides;
    bool asteroide_thread_created;
    AsteroideThreadArgs ast_args;
} ServidorEstado;

/* ── Inicialización de mutexes de naves ───────────────────── */
static int inicializar_mutexes_naves(MapaCompartido *mapa)
{
    pthread_mutexattr_t attr;
    int ret;

    ret = pthread_mutexattr_init(&attr);
    if (ret != 0) {
        fprintf(stderr, "[SERVIDOR] Error inicializando atributos de mutex: %s\n", strerror(ret));
        return -1;
    }

    ret = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if (ret != 0) {
        fprintf(stderr, "[SERVIDOR] Error estableciendo pshared en mutex: %s\n", strerror(ret));
        pthread_mutexattr_destroy(&attr);
        return -1;
    }

    for (int i = 0; i < MAX_NAVES; i++) {
        ret = pthread_mutex_init(&mapa->naves[i].mutex, &attr);
        if (ret != 0) {
            fprintf(stderr, "[SERVIDOR] Error inicializando mutex de nave %d: %s\n", i, strerror(ret));
            /* Revertir mutexes inicializados previamente */
            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(&mapa->naves[j].mutex);
            }
            pthread_mutexattr_destroy(&attr);
            return -1;
        }
    }

    ret = pthread_mutexattr_destroy(&attr);
    if (ret != 0) {
        fprintf(stderr, "[SERVIDOR] Error destruyendo atributos de mutex: %s\n", strerror(ret));
        return -1;
    }

    return 0;
}

/* ── Publicación de tarifas en la SHM ──────────────────────────────────── */
static void publicar_tarifas(MapaCompartido *mapa, const Configuracion *config)
{
    mapa->tarifas.precio_deuterio    = config->precio_deuterio;
    mapa->tarifas.precio_mutexio     = config->precio_mutexio;
    mapa->tarifas.precio_semaforita  = config->precio_semaforita;
    mapa->tarifas.precio_kernelio    = config->precio_kernelio;
    mapa->tarifas.precio_combustible = config->precio_combustible;
    mapa->tarifas.precio_oxigeno     = config->precio_oxigeno;
}


/* ── Guardado del estado lógico en disco ──────────────────── */
static void guardar_estado(const MapaCompartido *mapa)
{
    FILE *fp = fopen("estado_servidor.dat", "wb");
    if (!fp) {
        perror("[SERVIDOR] Error al abrir estado_servidor.dat para guardar");
        return;
    }

    /* 1. Escribir las celdas del mapa */
    if (fwrite(mapa->celdas, sizeof(mapa->celdas), 1, fp) != 1) {
        perror("[SERVIDOR] Error escribiendo celdas del mapa");
        fclose(fp);
        return;
    }

    /* 2. Escribir tarifas */
    if (fwrite(&mapa->tarifas, sizeof(TarifasComerciales), 1, fp) != 1) {
        perror("[SERVIDOR] Error escribiendo tarifas");
        fclose(fp);
        return;
    }

    /* 3. Contar y escribir asteroides activos */
    int cant_asteroides_activos = 0;
    for (int i = 0; i < MAX_ASTEROIDES; i++) {
        if (mapa->asteroides[i].activo) {
            cant_asteroides_activos++;
        }
    }
    if (fwrite(&cant_asteroides_activos, sizeof(int), 1, fp) != 1) {
        perror("[SERVIDOR] Error escribiendo cantidad de asteroides");
        fclose(fp);
        return;
    }

    /* Escribir los registros de asteroides activos */
    for (int i = 0; i < MAX_ASTEROIDES; i++) {
        const ASTEROIDE *src = &mapa->asteroides[i];
        if (src->activo) {
            AsteroidePersistente record;
            record.id = src->base.id;
            record.pos_x = src->pos_x;
            record.pos_y = src->pos_y;
            record.es_movil = src->es_movil;
            memcpy(record.minerales, src->minerales, sizeof(src->minerales));

            if (fwrite(&record, sizeof(AsteroidePersistente), 1, fp) != 1) {
                perror("[SERVIDOR] Error escribiendo registro de asteroide");
                fclose(fp);
                return;
            }
        }
    }

    /* 4. Contar y escribir naves activas */
    int cant_naves_activas = 0;
    for (int i = 0; i < MAX_NAVES; i++) {
        if (mapa->naves[i].activo) {
            cant_naves_activas++;
        }
    }
    if (fwrite(&cant_naves_activas, sizeof(int), 1, fp) != 1) {
        perror("[SERVIDOR] Error escribiendo cantidad de naves");
        fclose(fp);
        return;
    }

    /* Escribir los registros de naves activas */
    for (int i = 0; i < MAX_NAVES; i++) {
        const RegistroNave *src = &mapa->naves[i];
        if (src->activo) {
            NavePersistente record;
            record.pid = src->pid;
            record.creditos = src->creditos;
            record.pos_x = src->pos_x;
            record.pos_y = src->pos_y;
            record.combustible = src->combustible;
            record.oxigeno = src->oxigeno;
            record.escudo = src->escudo;
            record.incapacitada = src->incapacitada;
            memcpy(record.inventario, src->inventario, sizeof(src->inventario));

            if (fwrite(&record, sizeof(NavePersistente), 1, fp) != 1) {
                perror("[SERVIDOR] Error escribiendo registro de nave");
                fclose(fp);
                return;
            }
        }
    }

    fclose(fp);
    printf("[SERVIDOR] Estado lógico guardado en estado_servidor.dat.\n");
}

/* ── Renderizado del mapa en la terminal del servidor ───────────────────── */
static void render_servidor(const MapaCompartido *mapa, const Configuracion *cfg,
                             int estaciones_vivas)
{
    printf("\033[H\033[J"); /* Limpiar terminal (ANSI) */
    printf("=== COSMIKERNEL: SERVIDOR DEL CUADRANTE ===\n");
    printf("Estaciones activas: %d | Asteroides conf.: %d\n",
           estaciones_vivas, cfg->asteroides);
    printf("Precios — Deu:%d Mut:%d Sem:%d Ker:%d Fuel:%d O2:%d\n",
           cfg->precio_deuterio, cfg->precio_mutexio,
           cfg->precio_semaforita, cfg->precio_kernelio,
           cfg->precio_combustible, cfg->precio_oxigeno);
    printf("%.80s\n", "--------------------------------------------------------------------------------");
    dibujarMapa(mapa);
    printf("%.80s\n", "--------------------------------------------------------------------------------");
    printf("Ctrl+C para apagar el servidor limpiamente.\n");
}

/* ── Métodos del Ciclo de Vida del Servidor ───────────────── */

int servidor_inicializar(ServidorEstado *srv)
{
    int ret;
    memset(srv, 0, sizeof(ServidorEstado));

    /* 1. Cargar configuración */
    if (cargar_configuracion("config.txt", &srv->config) != 0) {
        fprintf(stderr, "[SERVIDOR] Usando configuración por defecto.\n");
    }

    /* 2. Crear la memoria compartida POSIX e inicializar semáforos de celdas */
    srv->mapa = mapa_crear_servidor();
    if (srv->mapa == NULL) {
        fprintf(stderr, "[SERVIDOR] Error crítico: no se pudo crear la SHM.\n");
        return -1;
    }

    /* 3. Inicializar mutexes de naves (PROCESS_SHARED) para el loot */
    if (inicializar_mutexes_naves(srv->mapa) != 0) {
        fprintf(stderr, "[SERVIDOR] Error crítico: fallo inicializando mutexes de naves.\n");
        return -1;
    }

    /* 4. Publicar tarifas en la SHM */
    publicar_tarifas(srv->mapa, &srv->config);

    /* 5. Poblar el mapa con asteroides */
    asteroide_generar_entorno(srv->mapa, srv->config.asteroides);
    printf("[SERVIDOR] Mapa inicializado. Lanzando hilo de asteroides...\n");

    srv->ast_args.mapa = srv->mapa;
    srv->ast_args.max_asteroides = srv->config.asteroides;
    srv->ast_args.keep_running = &keep_running;

    /* 6. Lanzar hilo de simulación de asteroides */
    ret = pthread_create(&srv->t_asteroides, NULL, asteroide_hilo_movimiento, &srv->ast_args);
    if (ret != 0) {
        fprintf(stderr, "[SERVIDOR] Error crítico al crear el hilo de asteroides: %s\n", strerror(ret));
        return -1;
    }
    srv->asteroide_thread_created = true;

    return 0;
}

void servidor_ejecutar_loop(ServidorEstado *srv)
{
    struct timespec req = {1, 0};
    bool juego_iniciado = false;

    while (keep_running) {
        int estaciones_vivas = srv->mapa->estaciones_activas;

        if (estaciones_vivas > 0) {
            juego_iniciado = true;
        }

        if (juego_iniciado && estaciones_vivas == 0) {
            printf("\033[H\033[J");
            printf("\n===================================================================\n");
            printf("[SERVIDOR] !TODAS LAS ESTACIONES DESTRUIDAS! — GAME OVER GLOBAL\n");
            printf("===================================================================\n");
            srv->mapa->game_over_global = true;
            keep_running = 0;
            continue;
        }

        render_servidor(srv->mapa, &srv->config, estaciones_vivas);
        nanosleep(&req, NULL);
    }
}

void servidor_apagar(ServidorEstado *srv)
{
    printf("\n[SERVIDOR] %s\n",
           (srv->mapa && srv->mapa->game_over_global)
               ? "Apagando por GAME OVER global."
               : "Apagando por señal del operador.");

    /* Guardar estado lógico en disco antes de liberar la SHM */
    if (srv->mapa) {
        guardar_estado(srv->mapa);
    }

    /* Esperar que termine el hilo de asteroides */
    if (srv->asteroide_thread_created) {
        int ret = pthread_join(srv->t_asteroides, NULL);
        if (ret != 0) {
            fprintf(stderr, "[SERVIDOR] Error en pthread_join de asteroides: %s\n", strerror(ret));
        }
    }

    if (srv->mapa) {
        /* Notificar a los clientes poniendo el flag en false. */
        srv->mapa->servidor_activo = false;
        printf("[SERVIDOR] Flag servidor_activo desactivado. Esperando clientes...\n");
        sleep(2);

        /* Destruir la SHM y los semáforos internos */
        mapa_destruir_servidor(srv->mapa);
        printf("[SERVIDOR] SHM destruida. Salida limpia.\n");
    }
}

/* ── main ─────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    int exit_status = EXIT_SUCCESS;

    /* 1. Registrar manejadores de señal para salida limpia */
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT,  &sa, NULL) == -1) { perror("sigaction SIGINT");  exit(EXIT_FAILURE); }
    if (sigaction(SIGTERM, &sa, NULL) == -1) { perror("sigaction SIGTERM"); exit(EXIT_FAILURE); }

    printf("[SERVIDOR] Iniciando...\n");

    ServidorEstado srv;
    memset(&srv, 0, sizeof(ServidorEstado));

    /* Inicializar el servidor */
    if (servidor_inicializar(&srv) != 0) {
        fprintf(stderr, "[SERVIDOR] Falló la inicialización del servidor.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    /* Ejecutar el loop del juego */
    servidor_ejecutar_loop(&srv);

cleanup:
    /* Apagar de manera centralizada */
    servidor_apagar(&srv);

    exit(exit_status);
}
