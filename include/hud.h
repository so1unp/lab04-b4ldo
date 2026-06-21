#ifndef HUD_H
#define HUD_H

#include "nave.h"

/* Inicialización del sistema de renderizado */
void init_ncurses(void);

/* Finalización del sistema de renderizado */
void cleanup_ncurses(void);

/* Renderizado seguro de un frame */
void hud_render(void);

#endif /* HUD_H */
