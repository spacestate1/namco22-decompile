#include "s21_sched.h"
static uint64_t origin;
#define Q (2048000.0 / 60000.0)
void s21_sched_event(uint64_t t) { if (t > origin) origin = t; }
uint64_t s21_sched_slice_start(uint64_t now)
{
	if (now <= origin) return origin;
	return origin + (uint64_t)((double)(uint64_t)((now - origin) / Q) * Q);
}
