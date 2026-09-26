#ifndef TW_ENV_H
#define TW_ENV_H
/* the trace oracle's replayed environment (src/tw_env.c): real only in tw_trace */
int tw_env_init(const char *path);     /* TW_ENV file -> 1 if loaded */
int tw_env_active(void);
void tw_env_report(void);              /* at exit: where the replayed environment differed from ours */               /* 1: interrupts come from the file, not from the frame clock */
#endif
