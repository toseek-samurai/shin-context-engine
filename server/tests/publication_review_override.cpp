// Test-only forwarding translation unit; production has no observation hook.
#define load_verified_snapshot load_verified_snapshot_with_review_barrier
#include "../src/publication_review.cpp"
#undef load_verified_snapshot
