#ifndef SFTP_H
#define SFTP_H
#include "buf.h"

struct sftp;
struct sftp *sftp_new(void);
void sftp_free(struct sftp *s);
/* handle the complete requests in `in` (consumed), replies appended to
   `out`; returns the number handled, -1 on a broken stream */
int sftp_input(struct sftp *s, struct buf *in, struct buf *out);

#endif
