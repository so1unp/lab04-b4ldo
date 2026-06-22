#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <semaphore.h>
#include <mqueue.h>
#include <time.h>
#include <stdbool.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>

#include "../include/mapa.h"
#include "../include/estacion.h"
#include "../tools/movement.h"

/* ─── Parámetros de juego de la Estación ────────────────────────── */
#define FUEL_MAX_ESTACION          600
#define FUEL_UMBRAL_ESTACION      200
#define FUEL_DECREMENTO_ESTACION     2
#define FUEL_INTERVALO_MS_ESTACION 1500
#define CREDITOS_INICIALES_ESTACION 10000

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

typedef struct {
    ESTACION          estacion;
    MapaCompartido   *mapa;
    int               x, y;
    char              mq_name[64];
    char              sem_name[64];
    char              pid_file[64];
    mqd_t             mq_fd;
    sem_t            *sem_hangar;
    sem_t            *sem_economia;
    Configuracion     config;
    volatile sig_atomic_t activo;
    pthread_mutex_t   mx_estado;
} EstadoEstacion;

static EstadoEstacion g;

static int buscar_slot_nave_por_pid(pid_t pid)
{
    for (int i = 0; i < MAX_NAVES; i++) {
        if (g.mapa->naves[i].activo && g.mapa->naves[i].pid == (int)pid) {
            return i;
        }
    }
    return -1;
}

static void lock_economia(void)
{
    if (g.sem_economia != SEM_FAILED) {
        sem_wait(g.sem_economia);
    }
}

static void unlock_economia(void)
{
    if (g.sem_economia != SEM_FAILED) {
        sem_post(g.sem_economia);
    }
}

/* ─── Helpers ──────────────────────────────────────────────────── */
static void dormir_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ─── Cargar configuración  ───────────────── */
static int cargar_configuracion(const char *filename, Configuracion *config)
{
    config->precio_deuterio = 10;
    config->precio_mutexio = 20;
    config->precio_semaforita = 30;
    config->precio_kernelio = 40;
    config->precio_combustible = 5;
    config->precio_oxigeno = 5;

    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        char path[128];
        snprintf(path, sizeof(path), "../%s", filename);
        fp = fopen(path, "r");
    }
    if (fp == NULL) {
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
            if (strcmp(clave, "precio_deuterio") == 0) {
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
    return 0;
}

/* ─── Bitácora de Eventos (Atómica mediante O_APPEND y lock) ────── */
static void registrar_transaccion(pid_t pid_nave, const char *operacion, const char *detalle, bool exito)
{
    int fd = open("bitacora.txt", O_WRONLY | O_CREAT | O_APPEND, 0660);
    if (fd == -1) {
        perror("Error al abrir bitacora.txt");
        return;
    }

    /* Lock de escritura exclusivo para garantizar atomicidad */
    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd, F_SETLKW, &fl) == -1) {
        perror("fcntl lock falló");
        close(fd);
        return;
    }

    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char time_str[26];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    char buffer[256];
    int len = snprintf(buffer, sizeof(buffer),
                   "[%s] Estacion (%d,%d) | Nave PID %d | Operacion: %s | Detalle: %s | Resultado: %s\n",
                   time_str, g.x, g.y, pid_nave, operacion, detalle,
                   exito ? "EXITO" : "FALLIDO");

    if (write(fd, buffer, (size_t)len) == -1) {
        perror("write bitacora falló");
    }
    
    /* Imprimir transacción en la terminal de la estación para visualización en tiempo real */
    printf("[ESTACION (%d,%d)] Transaccion - Nave PID %d | %s | %s | %s\n",
           g.x, g.y, pid_nave, operacion, detalle, exito ? "EXITO" : "FALLIDO");
    fflush(stdout);

    /* Unlock y cerrar */
    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    close(fd);
}

/* ─── Enviar alertas de Deuterio a todas las naves del cuadrante ─── */
static void enviar_alerta_deuterio(bool activa)
{
    DIR *dir = opendir("bin");
    if (!dir) {
        dir = opendir(".");
        if (!dir) {
            printf("[ESTACION (%d,%d)] Error: No se pudo abrir ni 'bin' ni '.' para buscar PIDs de naves.\n", g.x, g.y);
            fflush(stdout);
            return;
        }
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int pid_nave;
        if (sscanf(entry->d_name, "nave_%d.pid", &pid_nave) == 1) {
            char mq_nave_name[64];
            snprintf(mq_nave_name, sizeof(mq_nave_name), "/nave_mq_%d", pid_nave);

            mqd_t mq_nave = mq_open(mq_nave_name, O_WRONLY | O_NONBLOCK);
            if (mq_nave != (mqd_t)-1) {
                MensajeNave msg;
                msg.es_alerta = true;
                msg.estacion_x = g.x;
                msg.estacion_y = g.y;
                msg.exito = activa; // true si la alerta está activa, false si está resuelta/cancelada
                msg.cantidad = 0;
                if (activa) {
                    snprintf(msg.mensaje, sizeof(msg.mensaje), "Estacion (%d,%d) necesita deuterio de forma urgente.", g.x, g.y);
                } else {
                    snprintf(msg.mensaje, sizeof(msg.mensaje), "Estacion (%d,%d) alerta resuelta.", g.x, g.y);
                }

                if (mq_send(mq_nave, (const char *)&msg, sizeof(msg), 0) == -1) {
                    printf("[ESTACION (%d,%d)] Fallo al enviar alerta por mq_send a nave PID %d (errno: %d)\n", 
                           g.x, g.y, pid_nave, errno);
                    fflush(stdout);
                } else {
                    printf("[ESTACION (%d,%d)] Alerta de deuterio (%s) enviada con exito a nave PID %d\n", 
                           g.x, g.y, activa ? "ACTIVA" : "RESUELTA", pid_nave);
                    fflush(stdout);
                }
                mq_close(mq_nave);
            } else {
                printf("[ESTACION (%d,%d)] No se pudo abrir la cola de mensajes %s para la nave PID %d (errno: %d)\n",
                       g.x, g.y, mq_nave_name, pid_nave, errno);
                fflush(stdout);
            }
        }
    }
    closedir(dir);
}

/* ─── Enviar alerta de explosión a todas las naves ─── */
static void enviar_explosion_estacion(void)
{
    DIR *dir = opendir("bin");
    if (!dir) {
        dir = opendir(".");
        if (!dir) {
            return;
        }
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int pid_nave;
        if (sscanf(entry->d_name, "nave_%d.pid", &pid_nave) == 1) {
            char mq_nave_name[64];
            snprintf(mq_nave_name, sizeof(mq_nave_name), "/nave_mq_%d", pid_nave);

            mqd_t mq_nave = mq_open(mq_nave_name, O_WRONLY | O_NONBLOCK);
            if (mq_nave != (mqd_t)-1) {
                MensajeNave msg;
                msg.es_alerta = true;
                msg.estacion_x = g.x;
                msg.estacion_y = g.y;
                msg.exito = true; // Activa
                msg.cantidad = -1; // -1 indica explosión
                snprintf(msg.mensaje, sizeof(msg.mensaje), "Estacion (%d,%d) EXPLOTO.", g.x, g.y);

                mq_send(mq_nave, (const char *)&msg, sizeof(msg), 0);
                mq_close(mq_nave);
            }
        }
    }
    closedir(dir);
}

/* ─── Hilo de monitoreo y consumo de combustible ────────────────── */
static void *hilo_combustible(void *arg)
{
    (void)arg;
    time_t ultima_alerta = 0;
    int ultimo_combustible = -1;
    bool alerta_enviada = false;

    while (g.activo) {
        int fuel = barra_get_valor(&g.estacion.barra_combustible);
        
        if (fuel != ultimo_combustible) {
            printf("[ESTACION (%d,%d)] Combustible: %d/%d\n", g.x, g.y, fuel, FUEL_MAX_ESTACION);
            fflush(stdout);
            ultimo_combustible = fuel;

            if (fuel <= 0) {
                printf("[ESTACION (%d,%d)] ¡CRÍTICO! Combustible en 0. Apagando sistemas...\n", g.x, g.y);
                enviar_explosion_estacion();
                pthread_mutex_lock(&g.mx_estado);
                g.activo = 0;
                pthread_mutex_unlock(&g.mx_estado);
                break;
            }

            /* Si está por debajo del umbral, mandar alerta periódicamente */
            if (fuel <= FUEL_UMBRAL_ESTACION) {
                time_t ahora = time(NULL);
                if (!alerta_enviada || ahora - ultima_alerta >= 8) {
                    printf("[ESTACION (%d,%d)] Alerta de combustible bajo (%d/%d). Solicitando Deuterio...\n",
                           g.x, g.y, fuel, FUEL_MAX_ESTACION);
                    enviar_alerta_deuterio(true);
                    alerta_enviada = true;
                    ultima_alerta = ahora;
                }
            }
            /* Si recuperamos por encima del umbral y teníamos una alerta enviada, mandar la cancelación */
            else if (fuel > FUEL_UMBRAL_ESTACION && alerta_enviada) {
                printf("[ESTACION (%d,%d)] Combustible recuperado (%d/%d). Cancelando alerta...\n",
                       g.x, g.y, fuel, FUEL_MAX_ESTACION);
                enviar_alerta_deuterio(false);
                alerta_enviada = false;
            }
        }

        dormir_ms(500); /* Polling eficiente para detectar cambios de combustible */
    }
    return NULL;
}

/* ─── Hilo receptor de transacciones de naves ───────────────────── */
static void *hilo_transacciones(void *arg)
{
    (void)arg;
    struct mq_attr attr;
    mq_getattr(g.mq_fd, &attr);

    char *buf = malloc((size_t)attr.mq_msgsize);
    if (!buf) {
        perror("malloc");
        return NULL;
    }

    while (g.activo) {
        ssize_t bytes_read = mq_receive(g.mq_fd, buf, (size_t)attr.mq_msgsize, NULL);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                if (!g.activo) break;
                continue;
            }
            break;
        }

        if (!g.activo) break;

        MensajeEstacion *msg = (MensajeEstacion *)buf;
        MensajeNave resp;
        memset(&resp, 0, sizeof(resp));
        resp.es_alerta = false;

        char op_str[64] = {0};
        char det_str[128] = {0};

        if (msg->tipo == REQ_VENDER_MINERAL) {
            snprintf(op_str, sizeof(op_str), "VENTA MINERAL");
            // Calcular precio total según el tipo de mineral
            int precio_unitario = 0;
            const char *nombre_mineral = "";
            bool es_deuterio = false;
            switch (msg->recurso) {
                case MINERAL_DEUTERIO:
                    precio_unitario = g.mapa->tarifas.precio_deuterio;
                    nombre_mineral = "Deuterio";
                    es_deuterio = true;
                    break;
                case MINERAL_MUTEXIO:
                    precio_unitario = g.mapa->tarifas.precio_mutexio;
                    nombre_mineral = "Mutexio";
                    break;
                case MINERAL_SEMAFORITA:
                    precio_unitario = g.mapa->tarifas.precio_semaforita;
                    nombre_mineral = "Semaforita";
                    break;
                case MINERAL_KERNELIO:
                    precio_unitario = g.mapa->tarifas.precio_kernelio;
                    nombre_mineral = "Kernelio";
                    break;
                default:
                    precio_unitario = 0;
                    break;
            }
            int total_pago = msg->cantidad * precio_unitario;
            lock_economia();
            int slot_nave = buscar_slot_nave_por_pid(msg->pid_origen);
            if (slot_nave == -1) {
                resp.exito = false;
                resp.cantidad = 0;
                snprintf(resp.mensaje, sizeof(resp.mensaje), "Nave no registrada en servidor.");
                snprintf(det_str, sizeof(det_str), "Nave PID %d no encontrada", msg->pid_origen);
            } else if (precio_unitario <= 0 || msg->cantidad <= 0) {
                resp.exito = false;
                resp.cantidad = 0;
                snprintf(resp.mensaje, sizeof(resp.mensaje), "Solicitud de venta invalida.");
                snprintf(det_str, sizeof(det_str), "Venta invalida (recurso: %d, cantidad: %d)", msg->recurso, msg->cantidad);
            } else if (g.estacion.creditos < total_pago) {
                resp.exito = false;
                resp.cantidad = 0;
                snprintf(resp.mensaje, sizeof(resp.mensaje), "Estacion sin creditos suficientes para comprar.");
                snprintf(det_str, sizeof(det_str), "Sin fondos (pago: %d, saldo: %d)", total_pago, g.estacion.creditos);
            } else {
                if (es_deuterio) {
                    barra_modificar(&g.estacion.barra_combustible, msg->cantidad);
                }
                g.estacion.creditos -= total_pago;
                g.mapa->naves[slot_nave].creditos += total_pago;
                resp.exito = true;
                resp.cantidad = msg->cantidad;
                snprintf(resp.mensaje, sizeof(resp.mensaje), "%s vendido: %d unidades x %d = %d créditos",
                         nombre_mineral, msg->cantidad, precio_unitario, total_pago);
                snprintf(det_str, sizeof(det_str), "%s cantidad: %d, Pago: %d Cr", nombre_mineral, msg->cantidad, total_pago);
            }
            unlock_economia();
        } 
        else if (msg->tipo == REQ_COMPRAR_COMBUSTIBLE) {
            snprintf(op_str, sizeof(op_str), "COMPRA COMBUSTIBLE");
            int fuel_estacion = barra_get_valor(&g.estacion.barra_combustible);
            int costo_combustible = msg->cantidad * g.mapa->tarifas.precio_combustible;
            lock_economia();
            int slot_nave = buscar_slot_nave_por_pid(msg->pid_origen);
            
            if (slot_nave == -1) {
                resp.exito = false;
                resp.cantidad = 0;
                snprintf(resp.mensaje, sizeof(resp.mensaje), "Nave no registrada en servidor.");
                snprintf(det_str, sizeof(det_str), "Nave PID %d no encontrada", msg->pid_origen);
            } else if (g.mapa->naves[slot_nave].creditos < costo_combustible) {
                resp.exito = false;
                resp.cantidad = 0;
                snprintf(resp.mensaje, sizeof(resp.mensaje), "Creditos insuficientes para combustible.");
                snprintf(det_str, sizeof(det_str), "Creditos insuficientes (costo: %d, tiene: %d)",
                         costo_combustible, g.mapa->naves[slot_nave].creditos);
            } else if (fuel_estacion > msg->cantidad + 50) {
                barra_modificar(&g.estacion.barra_combustible, -msg->cantidad);
                g.mapa->naves[slot_nave].creditos -= costo_combustible;
                g.estacion.creditos += costo_combustible;
                resp.exito = true;
                resp.cantidad = msg->cantidad;
                snprintf(resp.mensaje, sizeof(resp.mensaje), "Combustible recargado. Costo: %d Cr.", costo_combustible);
                snprintf(det_str, sizeof(det_str), "Combustible cantidad: %d, Ingresos: %d Cr", msg->cantidad, costo_combustible);
            } else {
                resp.exito = false;
                resp.cantidad = 0;  
                snprintf(resp.mensaje, sizeof(resp.mensaje), "Estacion con combustible insuficiente.");
                snprintf(det_str, sizeof(det_str), "Combustible insuficiente (solicitado: %d)", msg->cantidad);
            }
            unlock_economia();
        } 
        else if (msg->tipo == REQ_COMPRAR_OXIGENO) {
            snprintf(op_str, sizeof(op_str), "COMPRA OXIGENO");
            int costo_oxigeno = msg->cantidad * g.mapa->tarifas.precio_oxigeno;
            lock_economia();
            int slot_nave = buscar_slot_nave_por_pid(msg->pid_origen);
            if (slot_nave == -1) {
                resp.exito = false;
                resp.cantidad = 0;
                snprintf(resp.mensaje, sizeof(resp.mensaje), "Nave no registrada en servidor.");
                snprintf(det_str, sizeof(det_str), "Nave PID %d no encontrada", msg->pid_origen);
            } else if (g.mapa->naves[slot_nave].creditos < costo_oxigeno) {
                resp.exito = false;
                resp.cantidad = 0;
                snprintf(resp.mensaje, sizeof(resp.mensaje), "Creditos insuficientes para oxigeno.");
                snprintf(det_str, sizeof(det_str), "Creditos insuficientes (costo: %d, tiene: %d)",
                         costo_oxigeno, g.mapa->naves[slot_nave].creditos);
            } else {
                resp.exito = true;
                resp.cantidad = msg->cantidad;
                g.mapa->naves[slot_nave].creditos -= costo_oxigeno;
                g.estacion.creditos += costo_oxigeno;
                snprintf(resp.mensaje, sizeof(resp.mensaje), "Oxigeno reabastecido. Costo: %d Cr.", costo_oxigeno);
                snprintf(det_str, sizeof(det_str), "Oxigeno cantidad: %d, Ingresos: %d Cr", msg->cantidad, costo_oxigeno);
            }
            unlock_economia();
        }

        /* Registrar en bitácora */
        registrar_transaccion(msg->pid_origen, op_str, det_str, resp.exito);

        /* Responder a la nave */
        char mq_resp_name[64];
        snprintf(mq_resp_name, sizeof(mq_resp_name), "/nave_mq_%d", msg->pid_origen);
        mqd_t mq_nave = mq_open(mq_resp_name, O_WRONLY);
        if (mq_nave != (mqd_t)-1) {
            mq_send(mq_nave, (const char *)&resp, sizeof(resp), 0);
            mq_close(mq_nave);
        }
    }

    free(buf);
    return NULL;
}

/* ─── Limpieza de recursos IPC de la Estación ───────────────────── */
static void limpiar_recursos_ipc(void)
{
    g.activo = 0;

    if (g.mapa && g.y >= 0 && g.y < MAP_ROWS && g.x >= 0 && g.x < MAP_COLS) {
        if (g.mapa->celdas[g.y][g.x] == CHAR_ESTACION) {
            liberar_posicion(g.mapa, g.x, g.y);
        }
    }

    if (g.mq_fd != (mqd_t)-1) {
        mq_close(g.mq_fd);
        g.mq_fd = (mqd_t)-1;
    }
    mq_unlink(g.mq_name);

    if (g.sem_hangar != SEM_FAILED) {
        sem_close(g.sem_hangar);
        g.sem_hangar = SEM_FAILED;
    }
    sem_unlink(g.sem_name);

    if (strlen(g.pid_file) > 0) {
        unlink(g.pid_file);
    }
}

/* ─── Manejador de señales ──────────────────────────────────────── */
static void handle_signal(int sig)
{
    (void)sig;
    g.activo = 0;
    if (g.mq_fd != (mqd_t)-1) {
        mq_close(g.mq_fd);
        g.mq_fd = (mqd_t)-1;
    }
}

/* ─── Búsqueda y adquisición de posición 'E' en el mapa ─────────── */
static bool adquirir_estacion_libre(void)
{
    // 1. Contar estaciones activas en el mapa compartido
    int estaciones_activas = 0;
    for (int r = 0; r < MAP_ROWS; r++) {
        for (int c = 0; c < MAP_COLS; c++) {
            if (g.mapa->celdas[r][c] == CHAR_ESTACION) {
                estaciones_activas++;
            }
        }
    }

    if (estaciones_activas >= 3) {
        fprintf(stderr, "[ESTACION] Error: Limite maximo de estaciones activas (3) alcanzado en el mapa.\n");
        return false;
    }

    // 2. Intentar buscar una celda vacia aleatoria
    srand((unsigned int)(time(NULL) ^ (time_t)getpid()));
    bool exito = false;
    int intentos = 0;
    int r = 0, c = 0;
    while (!exito && intentos < 10000) {
        c = rand() % MAP_COLS;
        r = rand() % MAP_ROWS;
        if (g.mapa->celdas[r][c] == CHAR_VACIO) {
            exito = adquirir_posicion_inicial(g.mapa, c, r, CHAR_ESTACION, false);
        }
        intentos++;
    }

    if (!exito) {
        fprintf(stderr, "[ESTACION] Error: No se pudo encontrar y adquirir una celda vacia para posicionar la estacion.\n");
        return false;
    }

    // 3. Crear cola de mensajes exclusiva para esta posicion
    char mq_test[64];
    snprintf(mq_test, sizeof(mq_test), "/estacion_mq_%d_%d", c, r);

    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(MensajeEstacion);
    attr.mq_curmsgs = 0;

    mqd_t fd = mq_open(mq_test, O_RDONLY | O_CREAT | O_EXCL, 0660, &attr);
    if (fd == (mqd_t)-1) {
        perror("[ESTACION] mq_open para cola de la estacion fallo");
        liberar_posicion(g.mapa, c, r);
        return false;
    }

    g.mq_fd = fd;
    g.x = c;
    g.y = r;
    strncpy(g.mq_name, mq_test, sizeof(g.mq_name));
    return true;
}

/* ─── main de la Estación ───────────────────────────────────────── */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    memset(&g, 0, sizeof(g));
    g.mq_fd = (mqd_t)-1;
    g.sem_hangar = SEM_FAILED;
    g.sem_economia = SEM_FAILED;

    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 1. Conectar al mapa compartido */
    g.mapa = mapa_conectar_cliente();
    if (!g.mapa) {
        fprintf(stderr, "[ESTACION] No se pudo conectar al servidor de mapa.\n");
        exit(EXIT_FAILURE);
    }

    /* 2. Encontrar una celda 'E' libre y adueñarse de ella */
    if (!adquirir_estacion_libre()) {
        fprintf(stderr, "[ESTACION] No se encontraron posiciones 'E' libres en el mapa compartido.\n");
        mapa_desconectar(g.mapa);
        exit(EXIT_SUCCESS);
    }

    /* 3. Cargar configuración de tarifas */
    cargar_configuracion("config.txt", &g.config);

    g.sem_economia = sem_open("/economia_sem", O_CREAT, 0660, 1);
    if (g.sem_economia == SEM_FAILED) {
        perror("sem_open economia falló");
    }

    /* 4. Crear semáforo contador del Hangar (máximo 3 naves) */
    snprintf(g.sem_name, sizeof(g.sem_name), "/estacion_sem_%d_%d", g.x, g.y);
    sem_unlink(g.sem_name);
    g.sem_hangar = sem_open(g.sem_name, O_CREAT | O_EXCL, 0660, 3);
    if (g.sem_hangar == SEM_FAILED) {
        perror("sem_open hangar falló");
        limpiar_recursos_ipc();
        mapa_desconectar(g.mapa);
        exit(EXIT_FAILURE);
    }

    /* 5. Registrar PID de la estación */
    snprintf(g.pid_file, sizeof(g.pid_file), "bin/estacion_%d_%d.pid", g.x, g.y);
    int pfd = open(g.pid_file, O_WRONLY | O_CREAT | O_TRUNC, 0660);
    if (pfd == -1) {
        snprintf(g.pid_file, sizeof(g.pid_file), "estacion_%d_%d.pid", g.x, g.y);
        pfd = open(g.pid_file, O_WRONLY | O_CREAT | O_TRUNC, 0660);
    }
    if (pfd != -1) {
        char pid_str[16];
        int len = snprintf(pid_str, sizeof(pid_str), "%d", getpid());
        if (write(pfd, pid_str, (size_t)len) == -1) {
            perror("write pid falló");
        }
        close(pfd);
    }

    /* 6. Inicializar barra de combustible */
    barra_init(&g.estacion.barra_combustible,
               FUEL_MAX_ESTACION, FUEL_UMBRAL_ESTACION,
               FUEL_DECREMENTO_ESTACION, FUEL_INTERVALO_MS_ESTACION);

    pthread_mutex_init(&g.mx_estado, NULL);
    g.activo = 1;
    g.estacion.creditos = CREDITOS_INICIALES_ESTACION;

    printf("[ESTACION (%d,%d)] Activada (PID %d). Hangar libre (capacidad: 3).\n", g.x, g.y, getpid());

    /* 7. Lanzar hilos de soporte */
    pthread_t t_combustible, t_transacciones;
    pthread_create(&t_combustible, NULL, hilo_combustible, NULL);
    pthread_create(&t_transacciones, NULL, hilo_transacciones, NULL);

    while (g.activo) {
        if (!g.mapa->servidor_activo) {
            printf("[ESTACION (%d,%d)] Servidor desconectado. Apagando.\n", g.x, g.y);
            g.activo = 0;
            break;
        }
        dormir_ms(200);
    }

    /* 8. Cierre ordenado y liberación de recursos */
    printf("[ESTACION (%d,%d)] Cerrando de forma ordenada...\n", g.x, g.y);
    pthread_cancel(t_combustible);
    pthread_cancel(t_transacciones);
    pthread_join(t_combustible, NULL);
    pthread_join(t_transacciones, NULL);

    barra_destroy(&g.estacion.barra_combustible);
    pthread_mutex_destroy(&g.mx_estado);
    if (g.sem_economia != SEM_FAILED) {
        sem_close(g.sem_economia);
    }
    limpiar_recursos_ipc();
    mapa_desconectar(g.mapa);

    printf("[ESTACION] Sistemas apagados.\n");
    exit(EXIT_SUCCESS);
}
