#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    // Agregar código aquí.
    void nave_consumir_oxigeno(Nave* n) {
    if (n->oxigeno > 0) {
        n->oxigeno--;
    }
}
void nave_imprimir_estado(Nave* n) {
    printf("[Nave %d] Pos:(%d,%d) | O2: %d | Combustible: %d\n", 
           n->base.id, n->base.x, n->base.y, n->oxigeno, n->combustible);
}

    // Termina la ejecución del programa.
    exit(EXIT_SUCCESS);
}
