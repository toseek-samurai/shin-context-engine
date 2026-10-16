// Compile only into the observation probe, never into the Server library or executable.
#define evaluate_pack_cases evaluate_pack_cases_with_observation_barrier
#include "../src/preparation_inspection.cpp"
#undef evaluate_pack_cases
