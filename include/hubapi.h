#pragma once
#include "vfs.h"
/* HTTP/JSON API on port 8080 for the PS3 Hub iPhone app */
int  hub_start(void);
void hub_stop(void);
void hub_stop_async(void);
int  hub_running(void);
void hub_set_volumes(const volume_t *v, int n);
int  hub_take_install(char *out, int n);     /* PKG path the phone asked to install */
void hub_set_status(const char *json);
