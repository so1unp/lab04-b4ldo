#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>

#include "../include/nave.h"
// #include "mapa.h"

typedef struct
{
    Nave *mi_nave;
    // MapaCompartido* mapa; // Descomentar en el futuro
    bool juego_activo;
    pthread_mutex_t mutex_nave; // Protege los datos de esta nave
} ContextoNave;

// --- 2. IMPLEMENTACIÓN DE COMPORTAMIENTOS ---
void nave_inicializar(Nave *n, int id, int x_inicial, int y_inicial)
{
    n->base.id = id;
    n->base.tipo = TIPO_NAVE;
    n->base.x = x_inicial;
    n->base.y = y_inicial;
    n->combustible = 100;
    n->oxigeno = 20; // Ponemos poco para probar que se asfixie rápido
    for (int i = 0; i < CANTIDAD_RECURSOS; i++)
    {
        n->inventario[i] = 0;
    }
}

void nave_consumir_oxigeno(Nave *n)
{
    if (n->oxigeno > 0)
    {
        n->oxigeno--;
    }
}

bool nave_esta_operativa(Nave *n)
{
    return (n->oxigeno > 0 && n->combustible > 0);
}

void nave_imprimir_estado(Nave *n)
{
    printf("[Nave %d] Pos:(%d,%d) | O2: %d | Combustible: %d\n",
           n->base.id, n->base.x, n->base.y, n->oxigeno, n->combustible);
}

// --- 3. HILOS DE LA NAVE ---
void *hilo_soporte_vital(void *arg)
{
    ContextoNave *ctx = (ContextoNave *)arg;

    while (ctx->juego_activo)
    {
        sleep(1); // Cada 1 segundo consume oxígeno

        pthread_mutex_lock(&ctx->mutex_nave);

        nave_consumir_oxigeno(ctx->mi_nave);
        nave_imprimir_estado(ctx->mi_nave);

        if (!nave_esta_operativa(ctx->mi_nave))
        {
            printf("\n¡ALERTA CRÍTICA! Tripulación incapacitada. Game Over.\n");
            ctx->juego_activo = false;
        }

        pthread_mutex_unlock(&ctx->mutex_nave);
    }
    return NULL;
}

void *hilo_propulsion(void *arg)
{
    ContextoNave *ctx = (ContextoNave *)arg;
    char tecla;

    while (ctx->juego_activo)
    {
        // En un entorno real usaremos getch() de ncurses de forma no bloqueante.
        // Por ahora, leemos de la entrada estándar para probar la lógica.
        tecla = getchar();

        // Limpiamos el salto de línea del buffer
        if (tecla == '\n')
            continue;

        pthread_mutex_lock(&ctx->mutex_nave);

        // Solo nos movemos si hay combustible
        if (ctx->mi_nave->combustible > 0)
        {
            int se_movio = 0;

            switch (tecla)
            {
            case 'w':
                ctx->mi_nave->base.y -= 1;
                se_movio = 1;
                break;
            case 's':
                ctx->mi_nave->base.y += 1;
                se_movio = 1;
                break;
            case 'a':
                ctx->mi_nave->base.x -= 1;
                se_movio = 1;
                break;
            case 'd':
                ctx->mi_nave->base.x += 1;
                se_movio = 1;
                break;
            case 'q':
                ctx->juego_activo = false;
                break; // Botón de pánico para salir
            }

            if (se_movio)
            {
                ctx->mi_nave->combustible -= 5; // Moverse gasta 5 unidades
                printf("\n[PROPULSIÓN] Movimiento detectado. Combustible restante: %d\n", ctx->mi_nave->combustible);
            }

            // Verificamos si nos quedamos varados
            if (!nave_esta_operativa(ctx->mi_nave))
            {
                printf("\n¡ALERTA CRÍTICA! Sin combustible. Nave varada.\n");
                ctx->juego_activo = false;
            }
        }

        pthread_mutex_unlock(&ctx->mutex_nave);
    }
    return NULL;
}

// --- 4. BUCLE PRINCIPAL (El Simulador Local) ---
int main(int argc, char *argv[])
{
    printf("--- INICIANDO SIMULADOR LOCAL DE NAVE ---\n");

    // 1. Instanciar nuestra nave y contexto de forma local (Mocking)
    Nave mi_nave_local;
    nave_inicializar(&mi_nave_local, 1, 10, 10);

    ContextoNave ctx;
    ctx.mi_nave = &mi_nave_local;
    ctx.juego_activo = true;
    pthread_mutex_init(&ctx.mutex_nave, NULL);

    // TODO: Cuando el servidor exista, aquí harías:
    // ctx.mapa = mapa_conectar_cliente();

    // 2. Lanzar los hilos
    pthread_t vital_id, prop_id;
    pthread_create(&vital_id, NULL, hilo_soporte_vital, &ctx);
    pthread_create(&prop_id, NULL, hilo_propulsion, &ctx);

    // 3. Esperar a que la nave muera o termine el juego
    pthread_join(vital_id, NULL);
    pthread_join(prop_id, NULL);

    // 4. Limpiar
    pthread_mutex_destroy(&ctx.mutex_nave);
    printf("--- SIMULACIÓN FINALIZADA ---\n");

    exit(EXIT_SUCCESS);
}