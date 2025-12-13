#pragma once

namespace krkrspeed {

class WwiseHook {
public:
    static WwiseHook& instance();
    void initialize();

private:
    WwiseHook() = default;
};

} // namespace krkrspeed
