#ifndef ASNIX_ATERM_H
#define ASNIX_ATERM_H

#include <stddef.h>

typedef struct {
    const char *key;
    const char *value;
} AtermEnvVar;

typedef struct {
    const char *name;
    const char *path;
    const char *hash_algo;
    const char *hash;
} AtermOutput;

typedef struct {
    const char *drv_path;
    const char **output_names;
    size_t noutput_names;
} AtermInputDrv;

char *asnix_build_derivation_aterm(const AtermOutput *outputs, size_t noutputs,
                                    const AtermInputDrv *input_drvs, size_t ninput_drvs, const char **input_srcs,
                                    size_t ninput_srcs, const char *system, const char *builder, const char **args,
                                    size_t nargs, const AtermEnvVar *env, size_t nenv);

typedef struct AtermEscapeCache AtermEscapeCache;

AtermEscapeCache *asnix_aterm_escape_cache_new(size_t hint);
void asnix_aterm_escape_cache_free(AtermEscapeCache *cache);

char *asnix_build_derivation_aterm_cached(const AtermOutput *outputs, size_t noutputs,
                                           const AtermInputDrv *input_drvs, size_t ninput_drvs,
                                           const char **input_srcs, size_t ninput_srcs, const char *system,
                                           const char *builder, const char **args, size_t nargs,
                                           const AtermEnvVar *env, size_t nenv, AtermEscapeCache *cache);

#endif

