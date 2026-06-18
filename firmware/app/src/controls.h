#ifndef CONTROLS_H
#define CONTROLS_H
int controls_init(void);
int controls_read_raw(int idx);   /* idx 0..6 per zephyr_user; -1 on error, else 0..4095 */
#endif
