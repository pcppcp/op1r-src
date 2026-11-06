/* integer settings in op1.db (screens/op1r_settings.c) */
#ifndef OP1R_SETTINGS_H
#define OP1R_SETTINGS_H
int  op1r_setting_get(const char *key, int dflt);           /* dflt when missing or not a number */
void op1r_setting_put(const char *key, int val);
#ifdef OP1_HOST
void op1r_setting_host_clear(void);
#endif
#endif
