#include "ripple_state.h"

#include "hal_config.h"

namespace {

constexpr const char *kConfigSlug = "ripple";
constexpr const char *kDepthKey = "depth";
constexpr int kInactiveDepth = -1;

bool save_depth(int depth)
{
    hal_config_set_int(kConfigSlug, kDepthKey, depth);
    return hal_config_get_int(kConfigSlug, kDepthKey, kInactiveDepth) == depth;
}

} // namespace

RippleState ripple_state_load()
{
    RippleState state;
    const int depth = hal_config_get_int(kConfigSlug, kDepthKey, kInactiveDepth);
    if (depth < 0 || depth > kRippleMaxDepth) return state;
    state.active = true;
    state.depth = static_cast<uint8_t>(depth);
    return state;
}

bool ripple_state_seed()
{
    return save_depth(0);
}

bool ripple_state_adopt(uint8_t peer_depth)
{
    if (ripple_state_load().active) return false;
    const unsigned next = peer_depth < kRippleMaxDepth
        ? static_cast<unsigned>(peer_depth) + 1
        : kRippleMaxDepth;
    return save_depth(static_cast<int>(next));
}

bool ripple_state_set_depth(unsigned depth)
{
    return depth <= kRippleMaxDepth && save_depth(static_cast<int>(depth));
}

bool ripple_state_clear()
{
    // Keep an explicit inactive value in the snapshot after a test clear.
    return save_depth(kInactiveDepth);
}
