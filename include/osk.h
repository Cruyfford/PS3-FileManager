#pragma once
enum { OSK_RESULT_NONE = 0, OSK_RESULT_OK = 1, OSK_RESULT_CANCEL = 2 };
int osk_open(const char *title, const char *text);
int osk_active(void);
/* call every frame while active; returns OSK_RESULT_* */
int osk_update(char *out, int size);
