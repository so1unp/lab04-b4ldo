#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <ncurses.h>
#include <mqueue.h>
#include <semaphore.h>
#include <fcntl.h>
#include <errno.h>

#include "mapa.h"
#include "movement.h"
#include "nave.h"
#include "recursos.h"
#include "estacion.h"
#include "hud.h"

/* ─── Parámetros de juego ──────────────────────────────────────── */
#define FUEL_UMBRAL        20
#define FUEL_DECREMENTO     1
#define FUEL_INTERVALO_MS 3000

#define O2_UMBRAL          20
#define O2_DECREMENTO       1
#define O2_INTERVALO_MS  2000

#define COSTO_FUEL_MOV    1
#define COSTO_FUEL_EXT    3

/* ─── Layout ncurses ───────────────────────────────────────────── */
#define PANEL_W   24
#define BAR_IN    10

EstadoNave      g;
static sem_t   *g_sem_economia = SEM_FAILED;



/* ─── Helpers ──────────────────────────────────────────────────── */
static long long obtener_tiempo_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void dormir_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void lock_economia(void)
{
    if (g_sem_economia != SEM_FAILED) {
        sem_wait(g_sem_economia);
    }
}

static void unlock_economia(void)
{
    if (g_sem_economia != SEM_FAILED) {
        sem_post(g_sem_economia);
    }
}

static ASTEROIDE* buscar_asteroide_adyacente(int *out_x, int *out_y)
{
    int dirs[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    for (int i = 0; i < 4; i++) {
        int tx = g.x + dirs[i][0];
        int ty = g.y + dirs[i][1];
        if (tx >= 0 && tx < MAP_COLS && ty >= 0 && ty < MAP_ROWS) {
            if (g.mapa->celdas[ty][tx] == CHAR_ASTEROIDE) {
                // Buscar en memoria compartida
                for (int j = 0; j < MAX_ASTEROIDES; j++) {
                    if (g.mapa->asteroides[j].activo && 
                        g.mapa->asteroides[j].pos_x == tx && 
                        g.mapa->asteroides[j].pos_y == ty) {
                        if (out_x) *out_x = tx;
                        if (out_y) *out_y = ty;
                        return &g.mapa->asteroides[j];
                    }
                }
            }
        }
    }
    return NULL;
}

static RegistroNave* buscar_nave_incapacitada_adyacente(int *out_x, int *out_y)
{
    int dirs[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    for (int i = 0; i < 4; i++) {
        int tx = g.x + dirs[i][0];
        int ty = g.y + dirs[i][1];
        if (tx >= 0 && tx < MAP_COLS && ty >= 0 && ty < MAP_ROWS) {
            if (g.mapa->celdas[ty][tx] == CHAR_NAVE) {
                // Buscar en memoria compartida
                for (int j = 0; j < MAX_NAVES; j++) {
                    if (g.mapa->naves[j].activo && 
                        g.mapa->naves[j].incapacitada &&
                        g.mapa->naves[j].pos_x == tx && 
                        g.mapa->naves[j].pos_y == ty) {
                        if (out_x) *out_x = tx;
                        if (out_y) *out_y = ty;
                        return &g.mapa->naves[j];
                    }
                }
            }
        }
    }
    return NULL;
}

static void game_over(const char *motivo)
{
    pthread_mutex_lock(&g.mx_estado);
    g.vivo = 0;
    pthread_mutex_unlock(&g.mx_estado);
    (void)motivo;
}



/* ─── Gestión de ingreso y egreso de Hangar ─────────────────────── */
static void salir_de_hangar(void)
{
    if (!g.en_hangar) return;

    if (g.sem_hangar != SEM_FAILED) {
        sem_post(g.sem_hangar);
        sem_close(g.sem_hangar);
        g.sem_hangar = SEM_FAILED;
    }

    if (g.mq_estacion != (mqd_t)-1) {
        mq_close(g.mq_estacion);
        g.mq_estacion = (mqd_t)-1;
    }

    g.en_hangar = false;
}

static void intentar_ingresar_hangar(void)
{
    int dx_dirs[] = {0, 0, -1, 1};
    int dy_dirs[] = {-1, 1, 0, 0};
    int ex = -1, ey = -1;
    bool encontrada = false;

    for (int i = 0; i < 4; i++) {
        int nx = g.x + dx_dirs[i];
        int ny = g.y + dy_dirs[i];
        if (nx >= 0 && nx < MAP_COLS && ny >= 0 && ny < MAP_ROWS) {
            if (g.mapa->celdas[ny][nx] == CHAR_ESTACION) {
                ex = nx;
                ey = ny;
                encontrada = true;
                break;
            }
        }
    }

    if (!encontrada) {
        strncpy(g.hud_error, "Estacion no encontrada", sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
        return;
    }

    char sem_name[64];
    snprintf(sem_name, sizeof(sem_name), "/estacion_sem_%d_%d", ex, ey);

    sem_t *sem = sem_open(sem_name, 0);
    if (sem == SEM_FAILED) {
        strncpy(g.hud_error, "Estacion inactiva", sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
        return;
    }

    if (sem_trywait(sem) != 0) {
        sem_close(sem);
        strncpy(g.hud_error, "Hangar lleno (max 3)", sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
        return;
    }

    char mq_est_name[64];
    snprintf(mq_est_name, sizeof(mq_est_name), "/estacion_mq_%d_%d", ex, ey);

    mqd_t mq_est = mq_open(mq_est_name, O_WRONLY);
    if (mq_est == (mqd_t)-1) {
        sem_post(sem);
        sem_close(sem);
        strncpy(g.hud_error, "Error de conexion IPC", sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
        return;
    }

    g.sem_hangar = sem;
    g.mq_estacion = mq_est;
    g.en_hangar = true;
    g.estacion_x = ex;
    g.estacion_y = ey;
}

/* ─── Transacciones Comerciales ─────────────────────────────────── */
static bool realizar_transaccion_sincrona(MensajeEstacion *req, MensajeNave *resp)
{
    pthread_mutex_lock(&g.mx_estado);
    g.hay_respuesta = false;
    pthread_mutex_unlock(&g.mx_estado);

    if (mq_send(g.mq_estacion, (const char *)req, sizeof(*req), 0) == -1) {
        return false;
    }

    /* Esperar respuesta del hilo receptor */
    pthread_mutex_lock(&g.mx_estado);
    while (!g.hay_respuesta && g.vivo) {
        pthread_cond_wait(&g.cond_respuesta, &g.mx_estado);
    }
    if (!g.vivo) {
        pthread_mutex_unlock(&g.mx_estado);
        return false;
    }
    *resp = g.ultima_respuesta;
    g.hay_respuesta = false;
    pthread_mutex_unlock(&g.mx_estado);

    return true;
}

static void sincronizar_creditos_desde_shm(void)
{
    if (g.nave_slot_shm == -1) {
        return;
    }

    lock_economia();
    g.nave.creditos = g.mapa->naves[g.nave_slot_shm].creditos;
    unlock_economia();
}

static void realizar_venta(void)
{
    int total_creditos_ganados = 0;
    int cant_mut = g.nave.inventario[MINERAL_MUTEXIO];
    int cant_sem = g.nave.inventario[MINERAL_SEMAFORITA];
    int cant_ker = g.nave.inventario[MINERAL_KERNELIO];
    int cant_deu = g.nave.inventario[MINERAL_DEUTERIO];

    bool vendido_algo = false;

    // Vender Mutexio
    if (cant_mut > 0) {
        MensajeEstacion req;
        req.pid_origen = getpid();
        req.tipo = REQ_VENDER_MINERAL;
        req.recurso = MINERAL_MUTEXIO;
        req.cantidad = cant_mut;
        MensajeNave resp;
        if (realizar_transaccion_sincrona(&req, &resp) && resp.exito) {
            total_creditos_ganados += cant_mut * g.mapa->tarifas.precio_mutexio;
            g.nave.inventario[MINERAL_MUTEXIO] = 0;
            vendido_algo = true;
        } else {
            cant_mut = 0; // No se vendió
        }
    }

    // Vender Semaforita
    if (cant_sem > 0) {
        MensajeEstacion req;
        req.pid_origen = getpid();
        req.tipo = REQ_VENDER_MINERAL;
        req.recurso = MINERAL_SEMAFORITA;
        req.cantidad = cant_sem;
        MensajeNave resp;
        if (realizar_transaccion_sincrona(&req, &resp) && resp.exito) {
            total_creditos_ganados += cant_sem * g.mapa->tarifas.precio_semaforita;
            g.nave.inventario[MINERAL_SEMAFORITA] = 0;
            vendido_algo = true;
        } else {
            cant_sem = 0;
        }
    }

    // Vender Kernelio
    if (cant_ker > 0) {
        MensajeEstacion req;
        req.pid_origen = getpid();
        req.tipo = REQ_VENDER_MINERAL;
        req.recurso = MINERAL_KERNELIO;
        req.cantidad = cant_ker;
        MensajeNave resp;
        if (realizar_transaccion_sincrona(&req, &resp) && resp.exito) {
            total_creditos_ganados += cant_ker * g.mapa->tarifas.precio_kernelio;
            g.nave.inventario[MINERAL_KERNELIO] = 0;
            vendido_algo = true;
        } else {
            cant_ker = 0;
        }
    }

    // Vender Deuterio
    if (cant_deu > 0) {
        MensajeEstacion req;
        req.pid_origen = getpid();
        req.tipo = REQ_VENDER_MINERAL;
        req.recurso = MINERAL_DEUTERIO;
        req.cantidad = cant_deu;
        MensajeNave resp;
        if (realizar_transaccion_sincrona(&req, &resp) && resp.exito) {
            total_creditos_ganados += cant_deu * g.mapa->tarifas.precio_deuterio;
            g.nave.inventario[MINERAL_DEUTERIO] = 0;
            vendido_algo = true;
        } else {
            cant_deu = 0;
        }
    }

    if (vendido_algo) {
        sincronizar_creditos_desde_shm();

        // Construir el mensaje de notificación
        char buffer[80] = "Vendidos:";
        char temp[32];
        bool first = true;

        if (cant_deu > 0) {
            snprintf(temp, sizeof(temp), " %d Deu", cant_deu);
            strncat(buffer, temp, sizeof(buffer) - strlen(buffer) - 1);
            first = false;
        }
        if (cant_mut > 0) {
            snprintf(temp, sizeof(temp), "%s %d Mut", first ? "" : ",", cant_mut);
            strncat(buffer, temp, sizeof(buffer) - strlen(buffer) - 1);
            first = false;
        }
        if (cant_sem > 0) {
            snprintf(temp, sizeof(temp), "%s %d Sem", first ? "" : ",", cant_sem);
            strncat(buffer, temp, sizeof(buffer) - strlen(buffer) - 1);
            first = false;
        }
        if (cant_ker > 0) {
            snprintf(temp, sizeof(temp), "%s %d Ker", first ? "" : ",", cant_ker);
            strncat(buffer, temp, sizeof(buffer) - strlen(buffer) - 1);
            first = false;
        }

        // Agregar los créditos ganados
        snprintf(temp, sizeof(temp), " por %d Cr", total_creditos_ganados);
        strncat(buffer, temp, sizeof(buffer) - strlen(buffer) - 1);

        strncpy(g.hud_error, buffer, sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
    } else {
        strncpy(g.hud_error, "No hay recursos para vender", sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
    }
}

static void realizar_compra_combustible(void)
{
    int actual = barra_get_valor(&g.nave.barra_combustible);
    int faltante = FUEL_MAX - actual;
    if (faltante <= 0) {
        strncpy(g.hud_error, "Tanque de combustible lleno", sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
        return;
    }

    int cantidad_solicitada = (faltante < 40) ? faltante : 40;
    int costo = cantidad_solicitada * g.mapa->tarifas.precio_combustible;
    
    // Verificar solvencia
    if (g.nave.creditos < costo) {
        snprintf(g.hud_error, sizeof(g.hud_error), 
                 "Créditos insuficientes. Necesitas %d, tienes %d", costo, g.nave.creditos);
        g.hud_error_recibido = time(NULL);
        return;
    }

    MensajeEstacion req;
    req.pid_origen = getpid();
    req.tipo = REQ_COMPRAR_COMBUSTIBLE;
    req.recurso = MINERAL_DEUTERIO;
    req.cantidad = cantidad_solicitada;

    MensajeNave resp;
    memset(&resp, 0, sizeof(resp));
    if (realizar_transaccion_sincrona(&req, &resp) && resp.exito) {
        barra_modificar(&g.nave.barra_combustible, resp.cantidad);
        sincronizar_creditos_desde_shm();
        int costo_real = resp.cantidad * g.mapa->tarifas.precio_combustible;
        snprintf(g.hud_error, sizeof(g.hud_error), "Combustible +%d. Costo: %d Cr", resp.cantidad, costo_real);
        g.hud_error_recibido = time(NULL);
    } else {
        if (resp.mensaje[0] != '\0') {
            strncpy(g.hud_error, resp.mensaje, sizeof(g.hud_error));
        } else {
            strncpy(g.hud_error, "Error de transaccion de combustible", sizeof(g.hud_error));
        }
        g.hud_error_recibido = time(NULL);
    }
}

static void realizar_compra_oxigeno(void)
{
    int actual = barra_get_valor(&g.nave.barra_oxigeno);
    int faltante = O2_MAX - actual;
    if (faltante <= 0) {
        strncpy(g.hud_error, "Tanque de oxigeno lleno", sizeof(g.hud_error));
        g.hud_error_recibido = time(NULL);
        return;
    }

    int cantidad_solicitada = (faltante < 40) ? faltante : 40;
    int costo = cantidad_solicitada * g.mapa->tarifas.precio_oxigeno;
    
    // Verificar solvencia
    if (g.nave.creditos < costo) {
        snprintf(g.hud_error, sizeof(g.hud_error), 
                 "Créditos insuficientes. Necesitas %d, tienes %d", costo, g.nave.creditos);
        g.hud_error_recibido = time(NULL);
        return;
    }

    MensajeEstacion req;
    req.pid_origen = getpid();
    req.tipo = REQ_COMPRAR_OXIGENO;
    req.recurso = 0;
    req.cantidad = cantidad_solicitada;

    MensajeNave resp;
    memset(&resp, 0, sizeof(resp));
    if (realizar_transaccion_sincrona(&req, &resp) && resp.exito) {
        barra_modificar(&g.nave.barra_oxigeno, resp.cantidad);
        sincronizar_creditos_desde_shm();
        int costo_real = resp.cantidad * g.mapa->tarifas.precio_oxigeno;
        snprintf(g.hud_error, sizeof(g.hud_error), "Oxigeno +%d. Costo: %d Cr", resp.cantidad, costo_real);
        g.hud_error_recibido = time(NULL);
    } else {
        if (resp.mensaje[0] != '\0') {
            snprintf(g.hud_error, sizeof(g.hud_error), "Error: %s", resp.mensaje);
        } else {
            strncpy(g.hud_error, "Error de transaccion de oxigeno", sizeof(g.hud_error));
        }
        g.hud_error_recibido = time(NULL);
    }
}

/* ─── Hilos ────────────────────────────────────────────────────── */

/* Hilo receptor de respuestas y de alertas de la cola de la nave */
static void *hilo_receptor_nave(void *arg)
{
    (void)arg;
    struct mq_attr attr;
    mq_getattr(g.mq_respuesta, &attr);
    char *buf = malloc((size_t)attr.mq_msgsize);
    if (!buf) return NULL;

    while (g.vivo) {
        ssize_t bytes = mq_receive(g.mq_respuesta, buf, (size_t)attr.mq_msgsize, NULL);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!g.vivo) break;

        MensajeNave *msg = (MensajeNave *)buf;
        if (msg->es_alerta) {
            pthread_mutex_lock(&g.mx_estado);
            bool activa = msg->exito; // true = activa, false = resuelta
            int found_idx = -1;
            int empty_idx = -1;
            for (int i = 0; i < 3; i++) {
                if (g.alertas[i].activa && g.alertas[i].x == msg->estacion_x && g.alertas[i].y == msg->estacion_y) {
                    found_idx = i;
                    break;
                }
                if (!g.alertas[i].activa && empty_idx == -1) {
                    empty_idx = i;
                }
            }

            if (activa) {
                if (found_idx != -1) {
                    g.alertas[found_idx].timestamp = time(NULL);
                    if (msg->cantidad == -1) {
                        g.alertas[found_idx].explotada = true;
                    }
                } else if (empty_idx != -1) {
                    g.alertas[empty_idx].x = msg->estacion_x;
                    g.alertas[empty_idx].y = msg->estacion_y;
                    g.alertas[empty_idx].activa = true;
                    g.alertas[empty_idx].timestamp = time(NULL);
                    if (msg->cantidad == -1) {
                        g.alertas[empty_idx].explotada = true;
                    }
                }
            } else {
                if (found_idx != -1) {
                    g.alertas[found_idx].activa = false;
                }
            }
            pthread_mutex_unlock(&g.mx_estado);
        } else {
            pthread_mutex_lock(&g.mx_estado);
            g.ultima_respuesta = *msg;
            g.hay_respuesta = true;
            pthread_cond_signal(&g.cond_respuesta);
            pthread_mutex_unlock(&g.mx_estado);
        }
    }
    free(buf);
    return NULL;
}

static void *hilo_soporte_vital(void *arg)
{
    (void)arg;
    while (g.vivo) {
        barra_esperar_notificacion(&g.nave.barra_oxigeno);
        if (!g.vivo) break;
        if (barra_get_valor(&g.nave.barra_oxigeno) <= 0)
            game_over("oxigeno");
    }
    return NULL;
}

static void *hilo_propulsion(void *arg)
{
    (void)arg;

    while (g.vivo) {
        /*
         * Leer teclado desde stdscr con nodelay=TRUE.
         * NO tomamos g_mx_ncurses aquí: getch() sobre stdscr es seguro
         * en un único hilo lector, y evitamos bloquear hilo_radar.
         */
        int ch = getch();
        if (ch == ERR) { dormir_ms(10); continue; }

        int dx = 0, dy = 0;
        switch (ch) {
            case 'w': case 'W': case KEY_UP:    dy = -1; break;
            case 's': case 'S': case KEY_DOWN:  dy =  1; break;
            case 'a': case 'A': case KEY_LEFT:  dx = -1; break;
            case 'd': case 'D': case KEY_RIGHT: dx =  1; break;
            case 'e': case 'E': {
                pthread_mutex_lock(&g.mx_estado);
                int ast_x = -1, ast_y = -1;
                ASTEROIDE *ast = buscar_asteroide_adyacente(&ast_x, &ast_y);
                if (ast == NULL) {
                    RegistroNave *nave_muerta = buscar_nave_incapacitada_adyacente(&ast_x, &ast_y);
                    if (nave_muerta != NULL) {
                        // Saqueo instantáneo
                        pthread_mutex_lock(&nave_muerta->mutex);
                        if (nave_muerta->activo && nave_muerta->incapacitada) {
                            int looted_fuel = nave_muerta->combustible;
                            int looted_o2 = nave_muerta->oxigeno;
                            for (int m = 0; m < CANTIDAD_RECURSOS; m++) {
                                g.nave.inventario[m] += nave_muerta->inventario[m];
                            }
                            barra_modificar(&g.nave.barra_combustible, looted_fuel);
                            barra_modificar(&g.nave.barra_oxigeno, looted_o2);
                            
                            nave_muerta->activo = false;
                            nave_muerta->incapacitada = false;
                            liberar_posicion(g.mapa, ast_x, ast_y);
                            
                            snprintf(g.hud_error, sizeof(g.hud_error), "Loot! F:+%d O2:+%d", looted_fuel, looted_o2);
                            g.hud_error_recibido = time(NULL);
                        } else {
                            strncpy(g.hud_error, "La nave ya fue saqueada", sizeof(g.hud_error));
                            g.hud_error_recibido = time(NULL);
                        }
                        pthread_mutex_unlock(&nave_muerta->mutex);
                        g.prog_ext = -1;
                    } else {
                        strncpy(g.hud_error, "Nada cerca para extraer/saquear", sizeof(g.hud_error));
                        g.hud_error_recibido = time(NULL);
                        g.prog_ext = -1;
                    }
                } else {
                    if (g.prog_ext < 0) {
                        g.prog_ext = 0;
                    }
                    g.prog_ext += 10;
                    g.ultimo_e_press = obtener_tiempo_ms();

                    if (g.prog_ext >= 100) {
                        g.prog_ext = -1;
                        pthread_mutex_unlock(&g.mx_estado);

                        int extraido[CANTIDAD_RECURSOS] = {0};
                        int res_minado = asteroide_minar(ast, extraido);

                        pthread_mutex_lock(&g.mx_estado);
                        if (res_minado >= 0) {
                            for (int m = 0; m < CANTIDAD_RECURSOS; m++) {
                                g.nave.inventario[m] += extraido[m];
                            }
                            barra_modificar(&g.nave.barra_combustible, -COSTO_FUEL_EXT);
                            char buffer[128];
                            int offset = snprintf(buffer, sizeof(buffer), "Minado:\n");
                            for (int m = 0; m < CANTIDAD_RECURSOS; m++) {
                                if (extraido[m] > 0) {
                                    const char* nombre = "";
                                    switch (m) {
                                        case MINERAL_DEUTERIO:   nombre = "  Deu"; break;
                                        case MINERAL_MUTEXIO:    nombre = "  Mut"; break;
                                        case MINERAL_SEMAFORITA: nombre = "  Sem"; break;
                                        case MINERAL_KERNELIO:   nombre = "  Ker"; break;
                                    }
                                    offset += snprintf(buffer + offset, sizeof(buffer) - (size_t)offset, "%s: %d\n", nombre, extraido[m]);
                                }
                            }
                            if (offset > 0 && buffer[offset - 1] == '\n') {
                                buffer[offset - 1] = '\0';
                            }
                            strncpy(g.hud_error, buffer, sizeof(g.hud_error));
                            g.hud_error_recibido = time(NULL);

                            if (res_minado == 0) {
                                liberar_posicion(g.mapa, ast_x, ast_y);
                                strncpy(g.hud_error, "Asteroide AGOTADO!", sizeof(g.hud_error));
                                g.hud_error_recibido = time(NULL);
                            }
                        } else {
                            strncpy(g.hud_error, "Asteroide ya vacio", sizeof(g.hud_error));
                            g.hud_error_recibido = time(NULL);
                        }
                    }
                }
                pthread_mutex_unlock(&g.mx_estado);
                continue;
            }
            case 'h': case 'H':
                if (g.en_hangar) {
                    salir_de_hangar();
                } else {
                    intentar_ingresar_hangar();
                }
                continue;
            case 'v': case 'V':
                if (g.en_hangar) realizar_venta();
                continue;
            case 'c': case 'C':
                if (g.en_hangar) realizar_compra_combustible();
                continue;
            case 'o': case 'O':
                if (g.en_hangar) realizar_compra_oxigeno();
                continue;
            case 'q': case 'Q':
                pthread_mutex_lock(&g.mx_estado);
                g.vivo = 0;
                pthread_mutex_unlock(&g.mx_estado);
                continue;
            default: continue;
        }

        if (!g.vivo) break;
        if (barra_get_valor(&g.nave.barra_combustible) <= 0) continue;

        /* Salir de hangar automáticamente si decide moverse */
        if (g.en_hangar && (dx != 0 || dy != 0)) {
            salir_de_hangar();
        }

        pthread_mutex_lock(&g.mx_estado);
        int xn = g.x + dx, yn = g.y + dy;
        bool ok = intentar_mover_objeto(g.mapa, &g.x, &g.y, xn, yn, CHAR_NAVE, false);
        pthread_mutex_unlock(&g.mx_estado);

        if (ok) {
            barra_modificar(&g.nave.barra_combustible, -COSTO_FUEL_MOV);
            if (barra_get_valor(&g.nave.barra_combustible) <= 0)
                game_over("combustible");
        }
    }
    return NULL;
}

static void *hilo_extraccion(void *arg)
{
    (void)arg;
    int nave_x = -1, nave_y = -1;
    while (g.vivo) {
        dormir_ms(100);

        pthread_mutex_lock(&g.mx_estado);
        if (g.prog_ext < 0) {
            // Guardar posición inicial de la nave
            nave_x = g.x;
            nave_y = g.y;
            pthread_mutex_unlock(&g.mx_estado);
            continue;
        }

        // Si la nave se mueve, se aborta la extracción
        if (g.x != nave_x || g.y != nave_y) {
            g.prog_ext = -1;
            strncpy(g.hud_error, "Minado cancelado:\n movimiento", sizeof(g.hud_error));
            g.hud_error_recibido = time(NULL);
            pthread_mutex_unlock(&g.mx_estado);
            continue;
        }

        // Si la nave se queda sin combustible
        if (barra_get_valor(&g.nave.barra_combustible) <= 0) {
            g.prog_ext = -1;
            pthread_mutex_unlock(&g.mx_estado);
            game_over("combustible");
            continue;
        }

        // Decaimiento por inactividad
        long long delta = obtener_tiempo_ms() - g.ultimo_e_press;
        if (delta > 600) { // Si pasan más de 600ms sin recibir 'e', decae el progreso
            g.prog_ext -= 10;
            if (g.prog_ext <= 0) {
                g.prog_ext = -1;
                strncpy(g.hud_error, "Minado cancelado:\n inactividad", sizeof(g.hud_error));
                g.hud_error_recibido = time(NULL);
            }
        }
        pthread_mutex_unlock(&g.mx_estado);
    }
    return NULL;
}

static void *hilo_radar(void *arg)
{
    (void)arg;
    while (g.vivo) {
        hud_render();
        dormir_ms(120);
    }
    /* Último frame mostrando GAME OVER */
    hud_render();
    return NULL;
}

/* ─── Spawn ────────────────────────────────────────────────────── */
static bool spawnear_nave(void)
{
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (int i = 0; i < 1000; i++) {
        int x = rand() % MAP_COLS;
        int y = rand() % MAP_ROWS;
        if (adquirir_posicion_inicial(g.mapa, x, y, CHAR_NAVE, false)) {
            g.x = x; g.y = y;
            return true;
        }
    }
    return false;
}

/* ─── main ─────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    memset(&g, 0, sizeof(g));
    g.mq_respuesta = (mqd_t)-1;
    g.mq_estacion = (mqd_t)-1;
    g.sem_hangar = SEM_FAILED;
    g_sem_economia = SEM_FAILED;

    /* 1. Conectar al mapa del servidor */
    g.mapa = mapa_conectar_cliente();
    if (!g.mapa) {
        fprintf(stderr, "[NAVE] No se pudo conectar al servidor.\n");
        exit(EXIT_FAILURE);
    }

    g_sem_economia = sem_open("/economia_sem", O_CREAT, 0660, 1);
    if (g_sem_economia == SEM_FAILED) {
        perror("sem_open economia nave falló");
    }

    g.nave_slot_shm = -1;
    lock_economia();
    for (int i = 0; i < MAX_NAVES; i++) {
        if (!g.mapa->naves[i].activo) {
            g.mapa->naves[i].pid = getpid();
            g.mapa->naves[i].creditos = 0;
            g.mapa->naves[i].activo = true;
            g.nave_slot_shm = i;
            break;
        }
    }
    unlock_economia();

    /* 2. Crear archivo indicador PID y cola de respuesta de la Nave */
    snprintf(g.pid_file, sizeof(g.pid_file), "bin/nave_%d.pid", getpid());
    int pfd = open(g.pid_file, O_WRONLY | O_CREAT | O_TRUNC, 0660);
    if (pfd == -1) {
        snprintf(g.pid_file, sizeof(g.pid_file), "nave_%d.pid", getpid());
        pfd = open(g.pid_file, O_WRONLY | O_CREAT | O_TRUNC, 0660);
    }
    if (pfd != -1) {
        char pid_str[16];
        int len = snprintf(pid_str, sizeof(pid_str), "%d", getpid());
        if (write(pfd, pid_str, (size_t)len) == -1) {
            perror("write pid nave falló");
        }
        close(pfd);
    }

    snprintf(g.mq_respuesta_name, sizeof(g.mq_respuesta_name), "/nave_mq_%d", getpid());
    mq_unlink(g.mq_respuesta_name);

    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(MensajeNave);
    attr.mq_curmsgs = 0;

    g.mq_respuesta = mq_open(g.mq_respuesta_name, O_RDONLY | O_CREAT | O_EXCL, 0660, &attr);
    if (g.mq_respuesta == (mqd_t)-1) {
        perror("mq_open respuesta nave falló");
        unlink(g.pid_file);
        mapa_desconectar(g.mapa);
        exit(EXIT_FAILURE);
    }

    /* 3. Inicializar barras */
    barra_init(&g.nave.barra_combustible,
               FUEL_MAX, FUEL_UMBRAL, FUEL_DECREMENTO, FUEL_INTERVALO_MS);
    barra_init(&g.nave.barra_oxigeno,
               O2_MAX, O2_UMBRAL, O2_DECREMENTO, O2_INTERVALO_MS);

    pthread_mutex_init(&g.mx_estado, NULL);
    pthread_cond_init(&g.cond_respuesta, NULL);

    g.vivo = 1;
    g.extrayendo = 0;
    g.prog_ext = -1;
    g.en_hangar = false;
    memset(g.nave.inventario, 0, sizeof(g.nave.inventario));
    g.nave.creditos       = 0;
    g.nave.base.id        = getpid();
    g.nave.base.tipo      = TIPO_NAVE;
    g.nave.base.velocidad = 1.0f;

    /* 4. Spawn en el mapa */
    if (!spawnear_nave()) {
        fprintf(stderr, "[NAVE] No hay lugar libre en el mapa.\n");
        mq_close(g.mq_respuesta);
        mq_unlink(g.mq_respuesta_name);
        unlink(g.pid_file);
        mapa_desconectar(g.mapa);
        exit(EXIT_FAILURE);
    }
    g.nave.base.x = (float)g.x;
    g.nave.base.y = (float)g.y;

    /* 5. Inicializar ncurses */
    init_ncurses();

    /* 6. Lanzar hilos */
    pthread_t t_vital, t_prop, t_ext, t_radar, t_receptor;
    pthread_create(&t_receptor, NULL, hilo_receptor_nave, NULL);
    pthread_create(&t_vital, NULL, hilo_soporte_vital, NULL);
    pthread_create(&t_prop,  NULL, hilo_propulsion,    NULL);
    pthread_create(&t_ext,   NULL, hilo_extraccion,    NULL);
    pthread_create(&t_radar, NULL, hilo_radar,         NULL);

    /* Loop principal: espera a que el juego termine */
    while (g.vivo) {
        if (!g.mapa->servidor_activo) {
            if (g.mapa->game_over_global) {
                strncpy(g.hud_error, "GAME OVER: Estaciones DESTRUIDAS", sizeof(g.hud_error));
            } else {
                strncpy(g.hud_error, "SERVIDOR DESCONECTADO", sizeof(g.hud_error));
            }
            g.hud_error_recibido = time(NULL);
            g.vivo = 0;
            break;
        }
        dormir_ms(100);
    }
    sleep(2);   /* pausa para que el jugador lea el GAME OVER       */

    /* 7. Limpieza */
    pthread_cancel(t_receptor);
    pthread_cancel(t_vital);
    pthread_join(t_receptor, NULL);
    pthread_join(t_vital, NULL);
    pthread_join(t_prop,  NULL);
    pthread_join(t_ext,   NULL);
    pthread_join(t_radar, NULL);

    cleanup_ncurses();
    salir_de_hangar();

    if (g.mapa->servidor_activo) {
        // La nave murió, dejar los recursos como "presa"
        if (g.nave_slot_shm != -1) {
            lock_economia();
            g.mapa->naves[g.nave_slot_shm].incapacitada = true;
            g.mapa->naves[g.nave_slot_shm].pos_x = g.x;
            g.mapa->naves[g.nave_slot_shm].pos_y = g.y;
            for (int i = 0; i < CANTIDAD_RECURSOS; i++) {
                g.mapa->naves[g.nave_slot_shm].inventario[i] = g.nave.inventario[i];
            }
            g.mapa->naves[g.nave_slot_shm].combustible = barra_get_valor(&g.nave.barra_combustible);
            if (g.mapa->naves[g.nave_slot_shm].combustible < 0) g.mapa->naves[g.nave_slot_shm].combustible = 0;
            g.mapa->naves[g.nave_slot_shm].oxigeno = barra_get_valor(&g.nave.barra_oxigeno);
            if (g.mapa->naves[g.nave_slot_shm].oxigeno < 0) g.mapa->naves[g.nave_slot_shm].oxigeno = 0;
            unlock_economia();
        }
    } else {
        // El servidor se cerró, limpieza completa
        liberar_posicion(g.mapa, g.x, g.y);
        if (g.nave_slot_shm != -1) {
            lock_economia();
            g.mapa->naves[g.nave_slot_shm].activo = false;
            unlock_economia();
        }
    }

    barra_destroy(&g.nave.barra_combustible);
    barra_destroy(&g.nave.barra_oxigeno);
    
    pthread_cond_destroy(&g.cond_respuesta);
    pthread_mutex_destroy(&g.mx_estado);
    if (g_sem_economia != SEM_FAILED) {
        sem_close(g_sem_economia);
    }
    mapa_desconectar(g.mapa);

    printf("[NAVE] Desconectada.\n");
    exit(EXIT_SUCCESS);
}
