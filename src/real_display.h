#ifndef FALLOUT_REAL_DISPLAY_H_
#define FALLOUT_REAL_DISPLAY_H_

#include <memory>

#include "geometry.h"
#include "render_asset_registry.h"
#include "render_commands.h"

namespace fallout {

class RealDisplay {
public:
    RealDisplay();
    ~RealDisplay();

    void processCommands(const RenderCommandBufferView& commands) const;
    void present() const;
    bool isOrchestratorEnabled() const;
    bool orchestratorOwnsPresenter() const;

    RenderAssetRegistry& assetRegistry() { return assetRegistry_; }
    const RenderAssetRegistry& assetRegistry() const { return assetRegistry_; }

private:
    RenderAssetRegistry assetRegistry_;
};

} // namespace fallout

#endif /* FALLOUT_REAL_DISPLAY_H_ */
