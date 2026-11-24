/* platform services for the SSH programs: plat_dos.c (DJGPP + Watt-32 +
   the MDVMM monitor) and plat_host.c (Linux, for protocol tests) */
#ifndef PLAT_H
#define PLAT_H
#include <stddef.h>
#include <stdint.h>

extern const char *plat_prog;           /* name for the log lines */
int plat_init(void);                    /* network up; 0 on failure */
void plat_log(const char *fmt, ...);    /* screen + monitor log */
uint32_t plat_ms(void);                 /* milliseconds, wraps */
void plat_idle(void);                   /* nothing to do: let others run */
int plat_key(void);                     /* a key pressed on this console, or -1 */
int plat_bioskey(void);                 /* BIOS key (scan << 8 | ASCII), or -1 */
/* show an 80x25 screen of cells (attribute << 8 | char) and the cursor */
void plat_screen_show(const uint16_t *cells, int row, int col, int cursor_on);

int plat_listen(int port);              /* non-blocking listening socket */
int plat_accept(int lfd, char *peer, size_t n);   /* -1: none waiting */
int plat_connect(const char *host, int port);     /* blocking; -1 on failure */
void plat_nonblock(int fd);
void plat_close(int fd);

/* job slots (MDVMM sessions running COMMAND.COM, see SSHSLOT.COM) */
enum { JOB_NONE, JOB_ASSIGNED, JOB_RUNNING, JOB_DONE };
int plat_job_start(const char *cmd);    /* "" = interactive shell; slot or -1 */
int plat_job_state(int slot, int *code);  /* JOB_*, -1 if not a slot */
/* text screen: cells (char, attribute) 80x25, cursor; 0 on failure */
int plat_screen(int slot, uint8_t cells[4000], int *row, int *col, int *hidden);
int plat_keys(int slot, const uint16_t *keys, int n);   /* keys taken */

/* file names: "/C/DIR/FILE" style SFTP paths <-> the native ones */
int plat_native_path(const char *sftp, char *out, size_t n);   /* 0: bad */
int plat_drive_ok(char letter);         /* drive to list under "/" */
const char *plat_home(void);            /* initial SFTP directory */
const char *plat_tempdir(void);         /* for exec output, ends in a separator */

#endif
