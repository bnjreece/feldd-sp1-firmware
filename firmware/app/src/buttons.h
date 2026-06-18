#ifndef BUTTONS_H
#define BUTTONS_H
#include <stdint.h>
#define BTN_COUNT 9   /* 0=Play,1..4=Track1..4,5=VolUp,6=VolDn,7=FWD,8=RWD */
struct button_event { uint8_t idx; uint8_t pressed; };   /* one edge */
int  buttons_init(void);
/* Scan both ladders once; emit edges (press/release) for the 9 buttons into
   evt[] (cap entries); returns number of edges. Call ~every 8ms. */
int  buttons_scan(struct button_event *evt, int cap);
/* True while the Track1+4 DFU band has been held long enough; the caller acts. */
int  buttons_dfu_held(void);
#endif
