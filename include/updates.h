#pragma once
#include <stdint.h>
/* Official PS3 game updates from Sony's update server (a0.ww.np.dl.playstation.net). */
enum { UPD_IDLE = 0, UPD_BUSY, UPD_DONE, UPD_NONE, UPD_ERROR };
typedef struct { char ver[16]; uint64_t size; char url[512]; char sysver[16]; } upd_pkg_t;
void upd_check(const char *title_id);
int  upd_state(void);
int  upd_count(void);
const upd_pkg_t *upd_get(int i);
const char *upd_title(void);
const char *upd_error(void);
