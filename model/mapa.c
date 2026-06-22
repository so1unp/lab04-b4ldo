/* =============================================================================
 * model/mapa.c — Implementación de la memoria compartida
 *
 * Responsabilidades:
 *   1. Gestionar la creación, apertura y cierre del segmento de Memoria
 *      Compartida POSIX (shm_open / mmap).
 *   2. Proporcionar funciones para el servidor (crear, inicializar, destruir)
 *      y para los clientes (conectar, desconectar).
 *   3. Inicializar y destruir los semáforos binarios POSIX que protegen cada
 *      celda del mapa, garantizando la exclusión mutua posicional.
 * ============================================================================= */

#define _POSIX_C_SOURCE 200809L

#include "mapa.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int mapa_fd = -1;

/* ── Utilidades internas ────────────────────────────────────────────────── */

static void cerrar_fd(void)
{
    if (mapa_fd != -1) {
        close(mapa_fd);
        mapa_fd = -1;
    }
}

/*
 * Destruye todos los semáforos de la grilla del mapa.
 */
static void destruir_semaforos(MapaCompartido *mapa)
{
    size_t fila;
    size_t columna;

    if (mapa == NULL) {
        return;
    }

    for (fila = 0; fila < MAP_ROWS; ++fila) {
        for (columna = 0; columna < MAP_COLS; ++columna) {
            if (sem_destroy(&mapa->semaforos[fila][columna]) != 0) {
                perror("sem_destroy");
            }
        }
    }
}

/*
 * Inicializa la matriz de semáforos. Cada celda arranca en 1 (libre).
 * Si ocurre un error a la mitad, hace rollback destruyendo los ya creados.
 * Como el struct MapaCompartido fue mapeado con MAP_SHARED, estos
 * semáforos anónimos son automáticamente compartidos entre procesos
 * (no es necesario el flag pshared=1 en algunos OS, pero POSIX manda pshared=1;
 * en Linux, si la memoria es compartida, basta con 1).
 */
static int inicializar_semaforos(MapaCompartido *mapa)
{
    size_t inicializados = 0;
    size_t fila;
    size_t columna;

    if (mapa == NULL) {
        return -1;
    }

    for (fila = 0; fila < MAP_ROWS; ++fila) {
        for (columna = 0; columna < MAP_COLS; ++columna) {
            // Inicializar con pshared = 1 (compartido entre procesos) y valor = 1 (libre)
            if (sem_init(&mapa->semaforos[fila][columna], 1, 1) != 0) {
                size_t indice;
                perror("sem_init");
                
                // Rollback en caso de error
                for (indice = inicializados; indice > 0; --indice) {
                    size_t pos = indice - 1;
                    size_t fila_prev = pos / MAP_COLS;
                    size_t columna_prev = pos % MAP_COLS;
                    sem_destroy(&mapa->semaforos[fila_prev][columna_prev]);
                }
                return -1;
            }
            ++inicializados;
        }
    }

    return 0;
}

/*
 * Función base para abrir el segmento de SHM.
 * El servidor la llama con O_CREAT. Los clientes sin él.
 */
static MapaCompartido *abrir_segmento(int flags)
{
    int fd;
    MapaCompartido *mapa;

    if ((flags & O_CREAT) != 0) {
        /* Intento de creación exclusiva. Si falla porque el archivo SHM 
         * quedó sucio de un crasheo previo, se limpia y se reintenta. */
        fd = shm_open(MAPA_SHM_NAME, flags | O_EXCL, 0660);
        if (fd == -1 && errno == EEXIST) {
            shm_unlink(MAPA_SHM_NAME);
            fd = shm_open(MAPA_SHM_NAME, flags | O_EXCL, 0660);
        }
    } else {
        fd = shm_open(MAPA_SHM_NAME, flags, 0660);
    }

    if (fd == -1) {
        perror("shm_open");
        return NULL;
    }

    /* Asignar tamaño al segmento recién creado */
    if ((flags & O_CREAT) != 0) {
        if (ftruncate(fd, (off_t)sizeof(MapaCompartido)) == -1) {
            perror("ftruncate");
            close(fd);
            shm_unlink(MAPA_SHM_NAME);
            return NULL;
        }
    }

    /* Mapear la memoria a nuestro espacio de direcciones */
    mapa = mmap(NULL,
                sizeof(MapaCompartido),
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd,
                0);
                
    if (mapa == MAP_FAILED) {
        perror("mmap");
        close(fd);
        if ((flags & O_CREAT) != 0) {
            shm_unlink(MAPA_SHM_NAME);
        }
        return NULL;
    }

    mapa_fd = fd;
    return mapa;
}

/* ── Funciones de API Pública ───────────────────────────────────────────── */

/*
 * Llamada por el servidor al iniciar. Crea el segmento y los semáforos.
 */
MapaCompartido *mapa_crear_servidor(void)
{
    MapaCompartido *mapa;

    mapa = abrir_segmento(O_CREAT | O_RDWR);
    if (mapa == NULL) {
        return NULL;
    }

    mapa_limpiar(mapa);
    if (inicializar_semaforos(mapa) != 0) {
        munmap(mapa, sizeof(MapaCompartido));
        cerrar_fd();
        shm_unlink(MAPA_SHM_NAME);
        return NULL;
    }

    return mapa;
}

/*
 * Llamada por procesos clientes (Naves, Estaciones) para conectarse.
 */
MapaCompartido *mapa_conectar_cliente(void)
{
    return abrir_segmento(O_RDWR);
}

/*
 * Limpia la estructura (usada por el servidor al crearla).
 */
void mapa_limpiar(MapaCompartido *mapa)
{
    if (mapa == NULL) {
        return;
    }

    memset(mapa->celdas, CHAR_VACIO, sizeof(mapa->celdas));
    for (int i = 0; i < MAX_ASTEROIDES; i++) {
        mapa->asteroides[i].activo = false;
    }
    for (int i = 0; i < MAX_NAVES; i++) {
        mapa->naves[i].activo = false;
        mapa->naves[i].pid = 0;
        mapa->naves[i].creditos = 0;
    }
    mapa->servidor_activo = true;
    mapa->game_over_global = false;
}

/*
 * Llamada por clientes para desvincularse limpiamente sin destruir
 * el segmento de la memoria del sistema.
 */
void mapa_desconectar(MapaCompartido *mapa)
{
    if (mapa == NULL) {
        return;
    }

    munmap(mapa, sizeof(MapaCompartido));
    cerrar_fd();
}

/*
 * Llamada por el servidor al apagarse. Destruye semáforos, mutexes y
 * luego borra el segmento SHM del sistema operativo (shm_unlink).
 */
void mapa_destruir_servidor(MapaCompartido *mapa)
{
    if (mapa == NULL) {
        return;
    }

    // Destruir mutexes de asteroides
    for (int i = 0; i < MAX_ASTEROIDES; i++) {
        if (mapa->asteroides[i].activo) {
            pthread_mutex_destroy(&mapa->asteroides[i].mutex);
        }
    }
    
    // Destruir mutexes de naves
    for (int i = 0; i < MAX_NAVES; i++) {
        pthread_mutex_destroy(&mapa->naves[i].mutex);
    }

    destruir_semaforos(mapa);
    mapa_desconectar(mapa);

    // Liberar la estructura del kernel
    if (shm_unlink(MAPA_SHM_NAME) != 0) {
        perror("shm_unlink");
    }
}

/*
 * Imprime el mapa en formato ASCII.
 */
void dibujarMapa(const MapaCompartido *mapa)
{
    size_t fila;
    size_t columna;

    if (mapa == NULL) {
        return;
    }

    for (fila = 0; fila < MAP_ROWS; ++fila) {
        for (columna = 0; columna < MAP_COLS; ++columna) {
            putchar(mapa->celdas[fila][columna]);
        }
        putchar('\n');
    }
}
