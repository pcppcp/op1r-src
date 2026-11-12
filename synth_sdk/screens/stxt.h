/* stock-font page text (screens/stxt.c): slot ranges per page; a slot keeps its position / colour from its first use */
#include <stdint.h>
enum { ST_SETTINGS = 0, ST_USBOUT = 6, ST_SCALE = 11, ST_FXROUTE = 17, ST_SLOTS = 22 };
#ifdef OP1_HOST
static inline void st_text(void *r, int slot, int32_t x, int32_t y, const uint8_t *c, const char *s) { (void)r; (void)slot; (void)x; (void)y; (void)c; (void)s; }   /* host tests: logic only */
#else
void st_text(void *r, int slot, int32_t x_q10, int32_t y_px, const uint8_t *rgba, const char *s);
#endif
