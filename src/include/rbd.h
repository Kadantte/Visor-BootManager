#ifndef RBD_H
#define RBD_H

#include <efi.h>
#include "gui.h"

void rbd_arm_on_reboot(int enabled);

int rbd_check_and_play(gui_state_t *gui);

int  menu_sound_prepare(int enabled, CHAR16 *path);
void menu_sound_start(void);
void menu_sound_poll(void);
void menu_sound_stop(void);
void menu_sound_finish(void);

#endif