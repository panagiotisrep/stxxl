#ifndef STXXL_CONTAINERS_BOA_PARAMS_HEADER
#define STXXL_CONTAINERS_BOA_PARAMS_HEADER

#include <stxxl/bits/namespace.h>

STXXL_BEGIN_NAMESPACE

// Scales the routing filter size per tier (boa) / offsets its tier exponent (boa_opt).
// Set before constructing a boa. Defaults to the ROUTING_FILTER_MULT macro if defined.
#ifdef ROUTING_FILTER_MULT
double routing_filter_mult = ROUTING_FILTER_MULT;
#else
double routing_filter_mult = 1;
#endif

// How the online-suspended (lazy_insert / consolidate_structure) mode builds runs from the staging log.
//   merge:  each buffer is sorted when flushed to the log; consolidation k-way merges those chunks.
//   sort:   buffers are appended unsorted; the whole log is sorted once at consolidation.
//   hybrid: as sort, but displaced old runs are merged instead of re-sorted.
// Set before calling lazy_insert. Defaults to the SORT_BASED_MATERIALIZATION / HYBRID_MATERIALIZATION
// macros if defined.
enum class materialization_strategy { merge, sort, hybrid };

#if defined(SORT_BASED_MATERIALIZATION)
materialization_strategy materialization = materialization_strategy::sort;
#elif defined(HYBRID_MATERIALIZATION)
materialization_strategy materialization = materialization_strategy::hybrid;
#else
materialization_strategy materialization = materialization_strategy::merge;
#endif

STXXL_END_NAMESPACE

#endif // !STXXL_CONTAINERS_BOA_PARAMS_HEADER
