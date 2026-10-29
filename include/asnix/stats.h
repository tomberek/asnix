#ifndef ASNIX_STATS_H
#define ASNIX_STATS_H

#ifndef __ASSEMBLER__
#include <stdint.h>

extern uint64_t asnix_stats_env;
extern uint64_t asnix_stats_env_elements;
extern uint64_t asnix_stats_thunk;
extern uint64_t asnix_stats_closure;
extern uint64_t asnix_stats_attrs;
extern uint64_t asnix_stats_attrs_entries;
extern uint64_t asnix_stats_list;
extern uint64_t asnix_stats_list_elements;
extern uint64_t asnix_stats_cell;
extern uint64_t asnix_stats_lazyapp;
extern uint64_t asnix_stats_thunkmap_desc;
extern uint64_t asnix_stats_thunkmap_elem;
extern uint64_t asnix_stats_string;
extern uint64_t asnix_stats_primop;
extern uint64_t asnix_stats_path;

void asnix_stats_print(void);
#endif

#endif

