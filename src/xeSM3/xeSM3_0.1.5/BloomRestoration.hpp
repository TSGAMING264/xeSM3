#pragma once

namespace BloomRestoration {
struct Options {
    bool postProcessFix = true;
    bool retailXbox = false;
    bool debugXbox = false;
    bool restoredBloom = true;
};

constexpr Options PublicOptions(int fix, int retail, int debug, int restored) noexcept {
    return {fix == 100, retail == 100, debug == 100, restored == 100};
}

constexpr bool Selected(Options options) noexcept {
    return options.postProcessFix && options.restoredBloom &&
        !options.retailXbox && !options.debugXbox;
}

void Attach();
bool Finish(long transactionResult);
void OnPresent();
}
