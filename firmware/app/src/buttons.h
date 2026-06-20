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
/* True if THIS scan's raw ladder reads show any button pulling the rail
   (instantaneous, pre-debounce). The control loop gates the fader read on this:
   a button sags the shared rail and drops the fader ADC reads, and the raw read
   catches the press edge on the first sagged tick (the committed state lags it
   by ~24 ms). Valid after buttons_scan() has run this tick. */
int  buttons_rail_loaded(void);
/* The tracks-ladder committed (debounced) button index: -1 idle, 0=Play,
   1..4 = Track1..4. Drives the track-LED button-press feedback (light track LED
   N while its Track button is held). */
int  buttons_track_committed(void);
#endif
