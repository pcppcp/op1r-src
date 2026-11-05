/* Host test for midi/ccmap.c: channels, routing (taken packets get CIN 0), keys / encoders, feedback without echo,
 * COM page turns.   cc -DOP1_HOST -I.. ccmap_test.c ../midi/ccmap.c ../midi/mx.c ../screens/op1r_settings.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../midi/mx.h"
#include "../screens/op1r_settings.h"
int host_op_ch = 0, host_prompt;
static int keys[64][2], nkeys, ticks[4], ntx; static uint32_t tx[64];
void host_key(int c, int d) { keys[nkeys][0] = c; keys[nkeys][1] = d; nkeys++; }
void host_ticks(int k, int t) { ticks[k] += t; }
void host_tx(uint32_t p) { tx[ntx++] = p; }
extern volatile uint8_t cc_keych, cc_parch, cc_fb;
void cc_rx(volatile uint8_t *pk), cc_fb_key(int, int), cc_fb_knob(int, int), cc_tx_poll(void), cc_turn(int, int), cc_text(int, char *);
int cc_keys_channel(void), cc_params_channel(void);
static int rx(int ch, int cc, int v) { uint8_t p[4] = { 0x0B, (uint8_t)(0xB0 | ch), (uint8_t)cc, (uint8_t)v }; cc_rx(p); return (p[0] & 15) == 0; }   /* 1: taken */
int main(void)
{
    /* defaults: keys on the OP-1 channel, params off */
    assert(cc_keys_channel() == 0 && cc_params_channel() == -1);
    assert(rx(0, 39, 127) && nkeys == 1 && keys[0][0] == 0x22 && keys[0][1] == 1);   /* PLAY */
    assert(rx(0, 39, 127) && nkeys == 1);                    /* same state: not again */
    assert(rx(0, 39, 0) && nkeys == 2 && keys[1][1] == 0);
    assert(!rx(0, 1, 64) && !rx(0, 18, 127) && !rx(1, 39, 127));   /* CC 1 (MIDI LFO), unused number, other channel: stock */
    assert(!rx(0, 70, 64));                                  /* params off: a params number goes to the stock */
    assert(rx(0, 102, 3) && ticks[0] == 6 && rx(0, 104, 126) && ticks[2] == -4);   /* encoders, relative */
    host_op_ch = 5; assert(cc_keys_channel() == 5 && rx(5, 40, 127) && !rx(0, 40, 127));   /* "OP" follows the channel */
    host_prompt = 1; int k0 = nkeys; assert(rx(5, 11, 127) && nkeys == k0); host_prompt = 0;   /* T1 never confirms */
    /* params on channel 16 (index 15): taken there, not on the keys channel */
    cc_parch = 16; assert(cc_params_channel() == 15);
    assert(rx(15, 12, 100) && rx(15, 118, 3) && !rx(15, 5, 127) && !rx(15, 1, 10));   /* params CCs only */
    /* feedback: physical key -> CC on the keys channel; a MIDI-injected key is not echoed */
    for (int i = 0; i < nkeys; i++) cc_fb_key(keys[i][0], keys[i][1]);   /* the KeyHandler sees every injected event */
    cc_fb_knob(0, 6 * 512); cc_fb_knob(2, -4 * 512);
    cc_fb = 1; ntx = 0;
    cc_fb_key(0x22, 1); cc_tx_poll();
    assert(ntx == 1 && tx[0] == (0x0Bu | 0xB5u << 8 | 39u << 16 | 127u << 24));
    ntx = 0; assert(rx(5, 40, 0) && rx(5, 40, 127)); cc_fb_key(0x23, 0); cc_fb_key(0x23, 1); cc_tx_poll(); assert(ntx == 0);   /* STOP from MIDI: no echo */
    cc_fb_key(0x23, 1); cc_tx_poll(); assert(ntx == 1);      /* the next physical press is sent */
    ntx = 0; cc_fb_key(0x40, 1); cc_tx_poll(); assert(ntx == 0);   /* keyboard / unmapped codes: not sent */
    /* encoders: 2 ticks per detent, MIDI ticks not echoed */
    ntx = 0; cc_fb_knob(1, 2 * 512 * 3); cc_tx_poll(); assert(ntx == 1 && (tx[0] >> 16 & 127) == 103 && (tx[0] >> 24) == 3);
    ntx = 0; assert(rx(5, 102, 2)); cc_fb_knob(0, 4 * 512); cc_tx_poll(); assert(ntx == 0);
    ntx = 0; cc_fb_knob(3, -2 * 512); cc_tx_poll(); assert(ntx == 1 && (tx[0] >> 24) == 127);   /* one detent down */
    /* COM page: KEYS OFF, OP, 1..16; PARAM OFF, 1..16; FEEDBACK on / off; saved */
    char s[4];
    cc_keych = 0; cc_turn(5, -2048); assert(cc_keych == 17); cc_text(0, s); assert(!strcmp(s, "off"));
    cc_turn(5, 2048); assert(cc_keych == 0); cc_text(0, s); assert(!strcmp(s, "op"));
    cc_turn(5, 4096); assert(cc_keych == 2 && op1r_setting_get("cckeych", -1) == 2); cc_text(0, s); assert(!strcmp(s, "02"));
    cc_parch = 17; cc_turn(6, 2048); assert(cc_parch == 1); cc_turn(6, -2048); assert(cc_parch == 17);
    for (int i = 0; i < 20; i++) cc_turn(6, 2048); assert(cc_parch == 16 && op1r_setting_get("ccparch", -1) == 16);
    cc_text(1, s); assert(!strcmp(s, "16"));
    cc_turn(7, -2048); assert(cc_fb == 0); cc_text(2, s); assert(!strcmp(s, "off")); cc_turn(7, 2048); assert(cc_fb == 1);
    cc_turn(5, 1024); assert(cc_keych == 2);                 /* half a detent: nothing */
    printf("ccmap_test: ok\n");
    return 0;
}
