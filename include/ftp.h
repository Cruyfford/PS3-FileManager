#pragma once
#include "vfs.h"

int  ftp_start(void);                 /* starts the server thread on port 21 */
void ftp_stop(void);
int  ftp_running(void);
const char *ftp_ip(void);             /* console IP or "" */
int  ftp_clients(void);               /* 1 while a client is connected */
void ftp_set_volumes(const volume_t *v, int n);
