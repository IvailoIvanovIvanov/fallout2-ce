#ifndef FALLOUT_REAL_DISPLAY_H_
#define FALLOUT_REAL_DISPLAY_H_

#include "geometry.h"
#include "render_commands.h"

namespace fallout {

class RealDisplay {
public:
    void processCommands(const RenderCommandBufferView& commands) const;
    void present() const;
    bool isOrchestratorEnabled() const;
    bool orchestratorOwnsPresenter() const;
};

} // namespace fallout

#endif /* FALLOUT_REAL_DISPLAY_H_ */
