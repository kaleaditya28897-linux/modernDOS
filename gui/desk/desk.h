/* MDDESK - shared between the desktop and its applications */
#ifndef DESK_H
#define DESK_H

#include "mdgui.h"

#define PANEL_H 30

void tlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int test_mode;
void panel_damage(void);
void desk_run(const char *cmd);         /* run a DOS program, come back */
void desk_widget(const char *name, const struct widget *w);   /* test log */

/* applications */
void files_open(const char *path);      /* file manager */
void editor_open(const char *path);     /* text editor; 0: a new text */

#endif
