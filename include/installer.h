#pragma once
#include <stdint.h>

#define INSTALL_DIR   "/dev_hdd0/vsh/game_pkg"
#define PACKAGES_DIR  "/dev_hdd0/packages"
#define MAX_JOBS      64

enum { MODE_XMB_QUEUE = 0, MODE_PACKAGES_FOLDER = 1 };
enum { INST_IDLE = 0, INST_RUNNING, INST_DONE, INST_ERROR, INST_CANCELLED };

typedef struct {
    char path[1024];
    char name[256];
    char title[128];
    uint64_t size;
    uint8_t *icon;          /* owned copy, may be NULL */
    uint32_t icon_size;
} inst_job_t;

typedef struct {
    volatile int state;
    volatile int current;   /* index of job being processed */
    volatile int count;
    volatile int ok_count;
    volatile uint64_t done_bytes;    /* current file */
    volatile uint64_t total_bytes;   /* current file */
    volatile uint64_t all_done;      /* all files */
    volatile uint64_t all_total;
    volatile uint32_t speed;         /* bytes/sec */
    volatile int error_code;         /* see ERR_* in installer.c (mapped to strings by UI) */
    char current_name[256];
} inst_status_t;

enum { ERR_NONE = 0, ERR_SPACE, ERR_MKDIR, ERR_OPEN_SRC, ERR_OPEN_DST, ERR_READ, ERR_WRITE, ERR_PDB, ERR_MEM };

/* default icon written to ICON_FILE when a package has none */
void inst_set_default_icon(const uint8_t *png, uint32_t size);

/* takes ownership of jobs[i].icon */
int  inst_start(inst_job_t *jobs, int n, int mode, int delete_source);
void inst_cancel(void);
const inst_status_t *inst_status(void);
void inst_reset(void);

uint64_t hdd_free_bytes(void);
