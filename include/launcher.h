#pragma once

enum { SYS_NONE = 0, SYS_NES, SYS_SNES, SYS_PS1, SYS_GB, SYS_GBA, SYS_MD, SYS_SELF };

int  launch_kind(const char *name);                 /* SYS_* */
const char *launch_system_name(int sys);
/* finds an installed RetroArch core for the system; returns 1 and fills core_path */
int  launch_find_core(int sys, char *core_path, int n, char *core_name, int nn);
int  launch_retroarch_installed(void);
/* copies a file (and, for .cue, its tracks) to the HDD if it isn't on a native drive.
   Fills native_path with something lv2 can open. Returns 1 on success. */
int  launch_prepare(const char *path, char *native_path, int n);
/* never returns on success: exits this app and starts self_path (argv[1] = arg if not NULL) */
void launch_exec(const char *self_path, const char *arg);
