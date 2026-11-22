#pragma once
/* webMAN MOD integration (HTTP commands on 127.0.0.1:80) */
enum { WM_OK = 0, WM_NOT_RUNNING = -1, WM_HTTP_ERR = -2, WM_NO_CACHE = -3 };

int webman_is_game(const char *name, int is_dir, const char *full_path);   /* ISO / JB folder webMAN can mount */
/* resolves exFAT/NTFS paths to webMAN's prepISO cache entry; returns WM_* */
int webman_resolve(const char *path, char *out, int n);
int webman_mount(const char *path);                                         /* /mount_ps3/<path> */
