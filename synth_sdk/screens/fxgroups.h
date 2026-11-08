/* type list groups (screens/fxgroups.c, docs/73) */
#ifndef FXGROUPS_H
#define FXGROUPS_H
#include <stdint.h>
enum { FG_FX, FG_SYNTH, FG_KINDS };
#define FG_ROW      0x30                       /* preset browser row */
#define FG_MAX_ROWS 64
#define FG_LABEL_X  8                          /* under the "1" of the left column (screen units) */
#define FG_LABEL_Y  74
extern const char *const *const fg_names[FG_KINDS];
extern const uint8_t fg_ngroups[FG_KINDS];
extern int fg_cur, fg_kind;
int  fg_group_of(int kind, const char *name, int n);
int  fg_count(int kind, const void *src, int g);
void *fg_list(int kind, const void *src, int g);
int  fg_next(int kind, const void *src, int g, int dir);
#endif
