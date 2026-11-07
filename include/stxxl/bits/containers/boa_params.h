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

STXXL_END_NAMESPACE

#endif // !STXXL_CONTAINERS_BOA_PARAMS_HEADER
