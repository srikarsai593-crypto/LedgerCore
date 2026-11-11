#include "audit.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdatomic.h>

#define AUDIT_LINE_MAX  512
#define HISTORY_SCAN_MAX 100000

static FILE            *g_log_fp  = NULL;
static pthread_mutex_t  g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_seq     = 0;

static void iso8601_now(char *buf, size_t len) {
    time_t t = time(NULL);
    struct tm tm_info;
    gmtime_r(&t, &tm_info);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_info);
}

int audit_init(const char *log_path) {
    pthread_mutex_lock(&g_log_mtx);
    g_log_fp = fopen(log_path, "a");
    if (!g_log_fp) {
        perror("audit_init: fopen");
        pthread_mutex_unlock(&g_log_mtx);
        return -1;
    }
    setvbuf(g_log_fp, NULL, _IOLBF, 0);  /* line-buffered: flush on each '\n' */
    pthread_mutex_unlock(&g_log_mtx);
    return 0;
}

void audit_close(void) {
    pthread_mutex_lock(&g_log_mtx);
    if (g_log_fp) {
        fflush(g_log_fp);
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    pthread_mutex_unlock(&g_log_mtx);
}

void audit_log(const char *event, const char *username,
               const char *detail, int64_t amount_paise,
               const char *result) {
    char ts[32];
    iso8601_now(ts, sizeof(ts));

    /* Format amount as "rupees.paise" for human readability */
    uint64_t ap = amount_paise < 0
                    ? (uint64_t)(-(amount_paise + 1)) + 1
                    : (uint64_t)amount_paise;
    const char *sign = (amount_paise < 0) ? "-" : "";

    pthread_mutex_lock(&g_log_mtx);
    uint64_t seq = atomic_fetch_add(&g_seq, 1) + 1;
    if (g_log_fp) {
        fprintf(g_log_fp,
                "%" PRIu64 "|%s|%s|%s|%s|%s%" PRIu64 ".%02" PRIu64 "|%s\n",
                seq, ts,
                event    ? event    : "",
                username ? username : "",
                detail   ? detail   : "",
                sign, ap / 100, ap % 100,
                result   ? result   : "");
    }
    pthread_mutex_unlock(&g_log_mtx);
}

int audit_history(const char *username, int n, LCConn *conn) {
    if (!g_log_fp || n <= 0) return 0;

    pthread_mutex_lock(&g_log_mtx);
    fflush(g_log_fp);
    pthread_mutex_unlock(&g_log_mtx);

    FILE *rfp = fopen(AUDIT_FILE, "r");
    if (!rfp) return 0;

    char **ring = calloc((size_t)n, sizeof(char *));
    if (!ring) { fclose(rfp); return 0; }

    int head = 0, count = 0, scanned = 0;
    char linebuf[AUDIT_LINE_MAX];

    while (scanned++ < HISTORY_SCAN_MAX && fgets(linebuf, sizeof(linebuf), rfp)) {
        /* Field 3 (0-based) is username: seq|ts|EVENT|username|... */
        char copy[AUDIT_LINE_MAX];
        memcpy(copy, linebuf, strlen(linebuf) + 1);

        char *fields[8];
        int fi = 0;
        char *tok = strtok(copy, "|");
        while (tok && fi < 8) { fields[fi++] = tok; tok = strtok(NULL, "|"); }
        if (fi < 5) continue;
        if (strncmp(fields[3], username, MAX_USERNAME) != 0) continue;

        free(ring[head]);
        ring[head] = strdup(linebuf);
        head = (head + 1) % n;
        if (count < n) count++;
    }
    fclose(rfp);

    int written = 0;
    int start = (count < n) ? 0 : head;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % n;
        if (ring[idx]) {
            size_t len = strlen(ring[idx]);
            if (len > 0 && ring[idx][len-1] == '\n')
                ring[idx][len-1] = '\0';
            if (lc_sendline(conn, ring[idx]) == 0) written++;
        }
    }

    for (int i = 0; i < n; i++) free(ring[i]);
    free(ring);
    return written;
}
