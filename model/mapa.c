#define _POSIX_C_SOURCE 200809L

#include "mapa.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int mapa_fd = -1;

static void cerrar_fd(void)
{
    if (mapa_fd != -1) {
        close(mapa_fd);
        mapa_fd = -1;
    }
}

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
            if (sem_init(&mapa->semaforos[fila][columna], 1, 1) != 0) {
                size_t indice;

                perror("sem_init");
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

static MapaCompartido *abrir_segmento(int flags)
{
    int fd;
    MapaCompartido *mapa;

    fd = shm_open(MAPA_SHM_NAME, flags, 0660);
    if (fd == -1) {
        perror("shm_open");
        return NULL;
    }

    if ((flags & O_CREAT) != 0) {
        if (ftruncate(fd, (off_t)sizeof(MapaCompartido)) == -1) {
            perror("ftruncate");
            close(fd);
            shm_unlink(MAPA_SHM_NAME);
            return NULL;
        }
    }

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

MapaCompartido *mapa_conectar_cliente(void)
{
    return abrir_segmento(O_RDWR);
}

void mapa_limpiar(MapaCompartido *mapa)
{
    if (mapa == NULL) {
        return;
    }

    memset(mapa->celdas, CHAR_VACIO, sizeof(mapa->celdas));
}

void mapa_desconectar(MapaCompartido *mapa)
{
    if (mapa == NULL) {
        return;
    }

    munmap(mapa, sizeof(MapaCompartido));
    cerrar_fd();
}

void mapa_destruir_servidor(MapaCompartido *mapa)
{
    if (mapa == NULL) {
        return;
    }

    destruir_semaforos(mapa);
    mapa_desconectar(mapa);

    if (shm_unlink(MAPA_SHM_NAME) != 0) {
        perror("shm_unlink");
    }
}

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

