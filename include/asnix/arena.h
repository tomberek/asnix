#ifndef ASNIX_ARENA_H
#define ASNIX_ARENA_H

#define ASNIX_ARENA_OFF_CUR   0
#define ASNIX_ARENA_OFF_END   8
#define ASNIX_ARENA_OFF_TOTAL 16
#define ASNIX_ARENA_OFF_MAX   24

#define ASNIX_ARENA_CHUNK_SIZE 0x200000

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct {
    uint8_t *cur;
    uint8_t *end;
    uint64_t total_allocated;
    uint64_t max_memory;
} AsnixArenaState;

extern AsnixArenaState asnix_arena;

void asnix_arena_init(uint64_t max_memory_bytes);

void *asnix_arena_alloc(uint64_t n);

int asnix_huge_pages_available(void);

#endif

#endif

