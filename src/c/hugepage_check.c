#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t huge_free_bytes(void) {
    FILE *f = fopen("/proc/buddyinfo", "r");
    if (!f) return 0;

    uint64_t total = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *tok = line;
        char *saveptr;
        tok = strtok_r(line, " \t\n", &saveptr);
        int field = 0;
        int order = 0;
        while (tok) {
            if (field >= 4) {
                if (order >= 9) {
                    uint64_t count = 0;
                    for (const char *p = tok; *p; p++) {
                        if (*p < '0' || *p > '9') { count = 0; break; }
                        count = count * 10 + (uint64_t)(*p - '0');
                    }
                    total += count << (12 + order);
                }
                order++;
            }
            field++;
            tok = strtok_r(NULL, " \t\n", &saveptr);
        }
    }
    fclose(f);
    return total;
}

#define ASNIX_HUGE_FREE_MIN (1ULL << 30)

static int g_hugepage_state = -1;

int asnix_huge_pages_available(void) {
    if (g_hugepage_state < 0) {
        g_hugepage_state = (huge_free_bytes() >= ASNIX_HUGE_FREE_MIN) ? 1 : 0;
    }
    return g_hugepage_state;
}

