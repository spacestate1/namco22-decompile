/*
 * eng_args.h -- settings from the command line, for every game (GitHub #85). Header only.
 *
 *   <game> --set widescreen=1 --set fullscreen=1 --set volume=50 ...
 *
 * Any line of the game's settings file (the keys the menu writes there: widescreen, fullscreen, window_scale, resolution, volume,
 * frame_rate, net_name, ...) can be given as --set KEY=VALUE (or --set=KEY=VALUE), as many times as needed. It holds for THIS run
 * only, over the file's value: the file is not changed unless the menu changes that setting.
 *
 * main() calls eng_args_strip(&argc, argv) first: it takes the --set arguments out (so each game's own argument parsing never sees
 * them) and keeps them in the environment variable ENG_SET (one KEY=VALUE per line), which also lets a launcher pass settings by
 * environment alone. A settings loader then calls eng_args_each() after reading its file.
 */
#ifndef ENG_ARGS_H
#define ENG_ARGS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void eng_args_setenv(const char *k, const char *v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

/* take every --set KEY=VALUE out of argv (argc shrinks), adding them to $ENG_SET; returns how many */
static inline int eng_args_strip(int *argc, char **argv)
{
    static char acc[8192];
    const char *old = getenv("ENG_SET");
    size_t n = (size_t)snprintf(acc, sizeof acc, "%s", old ? old : "");
    int found = 0, o = 1;
    for (int i = 1; i < *argc; i++) {
        const char *kv = NULL;
        if (!strcmp(argv[i], "--set") && i + 1 < *argc) kv = argv[++i];
        else if (!strncmp(argv[i], "--set=", 6)) kv = argv[i] + 6;
        if (!kv) { argv[o++] = argv[i]; continue; }
        if (!strchr(kv, '=')) { fprintf(stderr, "[SET] ignored \"%s\": use --set KEY=VALUE\n", kv); continue; }
        if (n + strlen(kv) + 2 < sizeof acc) n += (size_t)snprintf(acc + n, sizeof acc - n, "%s%s", n ? "\n" : "", kv);
        found++;
    }
    argv[o] = NULL;
    *argc = o;
    if (found) eng_args_setenv("ENG_SET", acc);
    return found;
}

/* add one KEY=VALUE as if it had been given as --set (a game's own shorthand, e.g. --fullscreen) */
static inline void eng_args_add(const char *k, const char *v)
{
    static char acc[8192];
    const char *old = getenv("ENG_SET");
    snprintf(acc, sizeof acc, "%s%s%s=%s", old ? old : "", old && *old ? "\n" : "", k, v);
    eng_args_setenv("ENG_SET", acc);
}

/* every KEY=VALUE from $ENG_SET, in order (a later one wins), keys and values trimmed */
static inline void eng_args_each(void (*fn)(const char *key, const char *val, void *ctx), void *ctx)
{
    const char *e = getenv("ENG_SET");
    if (!e || !*e) return;
    while (*e) {
        const char *nl = strchr(e, '\n'), *end = nl ? nl : e + strlen(e);
        char line[256];
        size_t len = (size_t)(end - e) < sizeof line - 1 ? (size_t)(end - e) : sizeof line - 1;
        memcpy(line, e, len); line[len] = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char *k = line, *v = eq + 1, *t;
            while (*k == ' ' || *k == '\t') k++;
            for (t = k + strlen(k); t > k && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r'); ) *--t = 0;
            while (*v == ' ' || *v == '\t') v++;
            for (t = v + strlen(v); t > v && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r'); ) *--t = 0;
            if (*k) { fprintf(stderr, "[SET] %s = %s (command line, this run only)\n", k, v); fn(k, v, ctx); }
        }
        e = nl ? nl + 1 : end;
    }
}
#endif
