/* eng_cfg.c -- see eng_cfg.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "eng_cfg.h"

#define MAXKV 160
static struct { char k[48]; char v[112]; } kv[MAXKV];
static int nkv;
static char cfg_path[512];

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int find(const char *key)
{
    for (int i = 0; i < nkv; i++) if (!strcmp(kv[i].k, key)) return i;
    return -1;
}

static void remember(const char *key, const char *val)
{
    int i = find(key);
    if (i < 0) { if (nkv >= MAXKV) return; i = nkv++; snprintf(kv[i].k, sizeof kv[i].k, "%s", key); }
    snprintf(kv[i].v, sizeof kv[i].v, "%s", val);
}

void eng_cfg_load(const char *path)
{
    nkv = 0;
    snprintf(cfg_path, sizeof cfg_path, "%s", path ? path : "");
    FILE *f = cfg_path[0] ? fopen(cfg_path, "r") : NULL;
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(p), *v = trim(eq + 1);
        if (*k) remember(k, v);
    }
    fclose(f);
}

const char *eng_cfg_get(const char *key)
{
    const int i = find(key);
    return i < 0 ? NULL : kv[i].v;
}

int eng_cfg_int(const char *key, int def)
{
    const char *v = eng_cfg_get(key);
    return v && *v ? atoi(v) : def;
}

/* rewrite ONE line of the file, keeping the others (comments, the user's own ordering) as they were */
bool eng_cfg_set(const char *key, const char *val)
{
    remember(key, val);
    if (!cfg_path[0]) return false;
    static char buf[16384];
    size_t n = 0;
    bool done = false;
    FILE *f = fopen(cfg_path, "r");
    if (f) {
        char line[256];
        const size_t kl = strlen(key);
        while (fgets(line, sizeof line, f) && n + 300 < sizeof buf) {
            const char *p = line;
            while (isspace((unsigned char)*p)) p++;
            if (!strncmp(p, key, kl) && (isspace((unsigned char)p[kl]) || p[kl] == '=')) {
                if (!done) n += (size_t)snprintf(buf + n, sizeof buf - n, "%s = %s\n", key, val);
                done = true;
            } else n += (size_t)snprintf(buf + n, sizeof buf - n, "%s", line);
        }
        fclose(f);
    }
    if (!done) n += (size_t)snprintf(buf + n, sizeof buf - n, "%s = %s\n", key, val);
    if (!(f = fopen(cfg_path, "w"))) return false;
    fwrite(buf, 1, n, f);
    fclose(f);
    return true;
}

bool eng_cfg_set_int(const char *key, int val)
{
    char b[24];
    snprintf(b, sizeof b, "%d", val);
    return eng_cfg_set(key, b);
}
