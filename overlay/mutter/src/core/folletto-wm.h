/* Folletto i3-style tiling engine (private) */
#pragma once

#include "meta/display.h"

typedef struct _FollettoWm FollettoWm;

FollettoWm * folletto_wm_new      (MetaDisplay *display);
void         folletto_wm_free     (FollettoWm  *wm);
void         folletto_wm_set_mode (FollettoWm  *wm,
                                   MetaWmMode   mode);
MetaWmMode   folletto_wm_get_mode (FollettoWm  *wm);
gboolean     folletto_wm_command  (FollettoWm  *wm,
                                   const char  *command);
const char * folletto_wm_get_layout (FollettoWm *wm);
