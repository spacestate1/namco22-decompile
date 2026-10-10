/* ss22_host.h -- TC2's stand-in: where the mouse / absolute-mouse gun points in the picture (src/tc2_window.c) */
#ifndef TC2_SS22_HOST_SHIM_H
#define TC2_SS22_HOST_SHIM_H
#include <stdbool.h>
bool ss22_host_pointer(float *nx, float *ny, bool *inside);
#endif
