/* macos/launcher.c -- the .app bundle's executable: a compiled Mach-O that
 * execv()s the game in the same process.
 *
 * macOS Game Mode classifies a game from the program LaunchServices spawns; a
 * #! script bundle executable spawns as the shell and is never classified.
 * This launcher does what the old script did -- find the ROM folder, run the
 * game from the folder that holds it (settings, saves and high scores live
 * there), log to ~/Library/Logs/<name>/last-run.log -- and then becomes the
 * game (same pid). The game opens in its saved window mode.
 *
 * Built by macos/package.sh with the per-game values below:
 *   -DLAUNCH_NAME="Time Crisis"   -DLAUNCH_BIN="Time Crisis"
 *   -DLAUNCH_ROM_ENV="TIMECRIS_ROMS"  -DLAUNCH_ROMS="<default extracted dir>"
 *   -DLAUNCH_ROMS_ARG=1 (TC1 takes the dir as argv[1]) */
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *what, const char *path)
{
    fprintf(stderr, "%s: %s %s: %s\n", LAUNCH_NAME, what, path, strerror(errno));
    exit(1);
}

int main(void)
{
    char self[PATH_MAX], real[PATH_MAX], dir[PATH_MAX], game[PATH_MAX];
    uint32_t n = sizeof self;
    if (_NSGetExecutablePath(self, &n) != 0 || !realpath(self, real)) fail("cannot locate", "itself");
    snprintf(dir, sizeof dir, "%s", dirname(real));
    snprintf(game, sizeof game, "%s/%s", dir, LAUNCH_BIN);

    const char *env = getenv(LAUNCH_ROM_ENV);
    char roms[PATH_MAX];
    snprintf(roms, sizeof roms, "%s", env && *env ? env : LAUNCH_ROMS);
    struct stat st;
    if (stat(roms, &st) != 0 || !S_ISDIR(st.st_mode)) fail("ROMs not found at", roms);

    char home_dir[PATH_MAX];                   /* the folder that holds extracted/ */
    snprintf(home_dir, sizeof home_dir, "%s", roms);
    if (chdir(dirname(home_dir)) != 0) fail("cannot enter", home_dir);

    const char *home = getenv("HOME");
    if (home && *home) {
        char logs[PATH_MAX], log[PATH_MAX];
        snprintf(logs, sizeof logs, "%s/Library", home); mkdir(logs, 0755);
        snprintf(logs, sizeof logs, "%s/Library/Logs", home); mkdir(logs, 0755);
        snprintf(logs, sizeof logs, "%s/Library/Logs/%s", home, LAUNCH_NAME);
        mkdir(logs, 0755);
        snprintf(log, sizeof log, "%s/last-run.log", logs);
        const int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    }

    char *argv[4]; int a = 0;
    argv[a++] = game;
    if (LAUNCH_ROMS_ARG) argv[a++] = roms;
    argv[a++] = "--window";                    /* the saved window mode and size */
    argv[a] = NULL;
    execv(game, argv);
    fail("cannot start", game);
}
