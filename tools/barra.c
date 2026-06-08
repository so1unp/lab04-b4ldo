#include "barra.h"
#include <stdio.h>
#include <unistd.h>

void* _hilo_decremento_automatico(void* arg) {
    Barra* barra = (Barra*)arg;
    int tiempo_esperado = 0;
    
    while (barra->hilo_activo) {
        // Dormir en intervalos pequeños (100ms) para poder salir rápido si se destruye la barra
        usleep(100000); 
        if (!barra->hilo_activo) break;
        
        tiempo_esperado += 100;
        if (tiempo_esperado >= barra->intervalo_ms) {
            // Se cumplió el intervalo, aplicar el decremento
            barra_modificar(barra, -barra->decremento_por_intervalo);
            tiempo_esperado = 0;
        }
    }
    return NULL;
}

void barra_init(Barra* barra, int valor_maximo, int umbral_notificacion, int decremento_por_intervalo, int intervalo_ms) {
    barra->valor_maximo = valor_maximo;
    barra->valor_actual = valor_maximo; // Inicialmente llena
    barra->umbral_notificacion = umbral_notificacion;
    barra->notificado = false;
    
    barra->decremento_por_intervalo = decremento_por_intervalo;
    barra->intervalo_ms = intervalo_ms;
    barra->hilo_activo = false;
    
    // Inicializamos el mutex para proteger el acceso a los datos
    pthread_mutex_init(&barra->mutex, NULL);
    
    // Inicializamos el semáforo en 0, para que quien espere la notificación se bloquee
    // Se puede usar sem_init(&sem, 0, 0) para semáforo local al proceso
    sem_init(&barra->sem_notificacion, 0, 0);
    
    // Si se configuró un decremento automático, levantamos el hilo
    if (intervalo_ms > 0 && decremento_por_intervalo > 0) {
        barra->hilo_activo = true;
        pthread_create(&barra->hilo_decremento, NULL, _hilo_decremento_automatico, (void*)barra);
    }
}

void barra_destroy(Barra* barra) {
    // Si hay un hilo corriendo, lo detenemos y esperamos a que termine
    if (barra->hilo_activo) {
        barra->hilo_activo = false;
        pthread_join(barra->hilo_decremento, NULL);
    }
    
    pthread_mutex_destroy(&barra->mutex);
    sem_destroy(&barra->sem_notificacion);
}

bool barra_modificar(Barra* barra, int cantidad) {
    bool alcanzo_umbral = false;
    
    pthread_mutex_lock(&barra->mutex);
    
    barra->valor_actual += cantidad;
    
    // Limitar al máximo
    if (barra->valor_actual > barra->valor_maximo) {
        barra->valor_actual = barra->valor_maximo;
    }
    // Limitar al mínimo (0)
    if (barra->valor_actual < 0) {
        barra->valor_actual = 0;
    }
    
    // Comprobar notificación (asumimos notificación cuando baja del umbral)
    if (barra->valor_actual <= barra->umbral_notificacion && !barra->notificado) {
        barra->notificado = true; // Para no notificar continuamente
        sem_post(&barra->sem_notificacion); // Despierta a quien esté esperando
        alcanzo_umbral = true;
    }
    // Si recuperamos por encima del umbral, reseteamos el flag para poder notificar de nuevo
    else if (barra->valor_actual > barra->umbral_notificacion && barra->notificado) {
        barra->notificado = false;
    }
    
    pthread_mutex_unlock(&barra->mutex);
    
    return alcanzo_umbral;
}

void barra_set_valor(Barra* barra, int valor) {
    pthread_mutex_lock(&barra->mutex);
    
    barra->valor_actual = valor;
    
    if (barra->valor_actual > barra->valor_maximo) {
        barra->valor_actual = barra->valor_maximo;
    }
    if (barra->valor_actual < 0) {
        barra->valor_actual = 0;
    }
    
    if (barra->valor_actual <= barra->umbral_notificacion && !barra->notificado) {
        barra->notificado = true;
        sem_post(&barra->sem_notificacion);
    } else if (barra->valor_actual > barra->umbral_notificacion && barra->notificado) {
        barra->notificado = false;
    }
    
    pthread_mutex_unlock(&barra->mutex);
}

int barra_get_valor(Barra* barra) {
    int valor;
    pthread_mutex_lock(&barra->mutex);
    valor = barra->valor_actual;
    pthread_mutex_unlock(&barra->mutex);
    return valor;
}

void barra_esperar_notificacion(Barra* barra) {
    // Se queda bloqueado hasta que otro hilo haga sem_post cuando se alcance el umbral
    sem_wait(&barra->sem_notificacion);
}
