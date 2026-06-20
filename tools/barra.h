#ifndef BARRA_H
#define BARRA_H

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>

// Estructura de Barra reutilizable para vida, oxígeno, combustible, etc.
typedef struct {
    int valor_actual;
    int valor_maximo;
    int umbral_notificacion;
    bool notificado;
    
    // Variables para el decremento automático a lo largo del tiempo
    int decremento_por_intervalo;
    int intervalo_ms; // Cada cuántos milisegundos se aplica el decremento
    volatile bool hilo_activo; // Bandera para controlar la ejecución del hilo
    pthread_t hilo_decremento;
    
    // Sincronización para acceso concurrente
    pthread_mutex_t mutex;
    
    // Semáforo para que otro hilo pueda esperar pasivamente una notificación
    sem_t sem_notificacion;
} Barra;

// Inicializa la barra. 
// valor_maximo: el límite máximo y el valor inicial
// umbral_notificacion: el nivel a partir del cual se manda una notificación (e.g. bajo este nivel)
// decremento_por_intervalo: cantidad de vida/recurso que se pierde por intervalo.
// intervalo_ms: tiempo en milisegundos entre cada decremento. Usar 0 si la barra no pierde vida con el tiempo.
void barra_init(Barra* barra, int valor_maximo, int umbral_notificacion, int decremento_por_intervalo, int intervalo_ms);

// Destruye la barra (detiene el hilo automático si existe, libera mutex y semáforo)
void barra_destroy(Barra* barra);

// Modifica el valor de la barra (cantidad puede ser positiva o negativa).
// Retorna true si al modificar se cruzó el umbral hacia abajo y se generó una notificación.
bool barra_modificar(Barra* barra, int cantidad);

// Establece el valor exacto de la barra, limitando a [0, valor_maximo].
void barra_set_valor(Barra* barra, int valor);

// Retorna el valor actual de manera thread-safe.
int barra_get_valor(Barra* barra);

// Espera a que la barra envíe una notificación (es decir, baje del umbral).
void barra_esperar_notificacion(Barra* barra);

#endif // BARRA_H
