#ifndef CAPNP_JANET_H
#define CAPNP_JANET_H

/* Public C headers for embedders (e.g. libgrok_policyd). */

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_kinds.h>
#include <capnp-janet/capnp_message.h>

/* Optional Janet registration (requires janet.h in the TU that links). */
#ifdef CAPNP_JANET_WITH_JANET
#include <janet.h>
CAPNP_JANET_EXPORT void capnp_janet_register(JanetTable *env);
CAPNP_JANET_EXPORT void capnp_janet_env(JanetTable *env);
/* Fill an unmarshal registry so make-image / .jimage packs resolve capnp/*. */
CAPNP_JANET_EXPORT void capnp_janet_lookup_into(JanetTable *lookup);
#endif

#endif /* CAPNP_JANET_H */
