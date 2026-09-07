#include "overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <libgen.h>

static char  g_file_path[512]  = {0};
static char  g_temp_path[512]  = {0};
static int   g_clear_secs      = 30;

/* --- SIGALRM handler --- */

static void sigalrm_handler(int sig)
{
    (void)sig;
    /*
     * Write a single space to the overlay file.
     * Cannot use overlay_clear() here (not async-signal-safe for full
     * implementation), so write directly.
     *
     * A single space ensures P2's inotify fires but displays nothing
     * visible (textoverlay trims leading/trailing spaces).
     */
    if (g_file_path[0] == '\0') return;

    int fd = open(g_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, " ", 1);
        close(fd);
    }
}

/* --- Public API --- */

void overlay_init(const char *file_path, int clear_secs)
{
    strncpy(g_file_path, file_path, sizeof(g_file_path) - 1);
    snprintf(g_temp_path, sizeof(g_temp_path), "%s.tmp", file_path);
    g_clear_secs = (clear_secs > 0) ? clear_secs : 30;

    /* Create directory */
    char path_copy[512];
    strncpy(path_copy, file_path, sizeof(path_copy) - 1);
    char *dir = dirname(path_copy);

    struct stat st;
    if (stat(dir, &st) != 0) {
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "[overlay] mkdir(%s): %s\n", dir, strerror(errno));
        }
    }

    /* Create empty file */
    int fd = open(g_file_path, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);

    /* Install SIGALRM handler for auto-clear */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);

    printf("[overlay] writer ready: %s (clear after %ds)\n",
           g_file_path, g_clear_secs);
}

int overlay_write(const char *text, size_t text_len)
{
    if (text_len == 0) text_len = strlen(text);

    /*
     * Atomic write: write to temp file, rename to target.
     * rename() is atomic on Linux (same filesystem) — P2 will
     * never see a partial file.
     */
    int fd = open(g_temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[overlay] open temp: %s\n", strerror(errno));
        return -1;
    }

    ssize_t written = write(fd, text, text_len);
    close(fd);

    if (written != (ssize_t)text_len) {
        fprintf(stderr, "[overlay] write incomplete\n");
        return -1;
    }

    if (rename(g_temp_path, g_file_path) != 0) {
        fprintf(stderr, "[overlay] rename: %s\n", strerror(errno));
        return -1;
    }

    /* Reset auto-clear alarm */
    alarm((unsigned int)g_clear_secs);

    printf("[overlay] wrote %zu bytes to overlay\n", text_len);
    return 0;
}

void overlay_clear(void)
{
    alarm(0);  /* Cancel pending alarm */
    int fd = open(g_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, " ", 1);
        close(fd);
    }
    printf("[overlay] cleared\n");
}
