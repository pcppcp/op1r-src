/* tb3po sequencer (tb3po/tb_fw.c) */
#ifndef TB_FW_H
#define TB_FW_H
#include "tb_core.h"
#define TB_HIST 8
typedef struct {
    tb_pat   pat;
    uint8_t  length, slides, rate, root, playing, step, nhist;
    int8_t   transpose;
    uint16_t hist[TB_HIST];        /* earlier seeds, newest first */
} tb_bank;
extern tb_bank tb_b[2];
void tb_tick(int m);
void tb_restart(int m);
void tb_all_off(int m);
void tb_turn(int m, int knob, int delta, int shift);
void tb_press(int m, int knob);
int  tb_key_down_from(int m, int key, uint32_t caller);
int  tb_serialize(int m, char *out, int n);
int  tb_parse(int m, const char *str, int len);
void tb_draw(void *r, int m, int shift);
#endif
