#pragma once
/* Subtitles: external SRT / ASS / SSA / VTT files and embedded MKV text tracks */
typedef struct { double start, end; char *text; } subcue_t;
typedef struct { subcue_t *c; int n, cap; int sorted; } sublist_t;

int  subs_load_file(const char *path, sublist_t *out);       /* 1 = loaded */
/* embedded packet: ass = 1 for ASS/SSA block text (ReadOrder,Layer,Style,...,Text) */
void subs_add_packet(sublist_t *l, double start, double dur, const char *data, int len, int ass);
/* active text at time t ("" if none); lines separated by '\n' */
const char *subs_at(sublist_t *l, double t);
void subs_free(sublist_t *l);
/* finds external subtitle files next to the video; returns count, paths in out[] */
int  subs_find_external(const char *video_path, char out[][1024], int max);
