#include "real_display.h"

#include "render_display_orchestrator.h"
#include "svga.h"

namespace fallout {

void RealDisplay::processCommands(const RenderCommandBufferView& /*commands*/) const
{
    // The orchestrator already owns command retrieval from internal queues.
    // Trigger the orchestrator processing step.
    renderDisplayOrchestratorProcess();
}

void RealDisplay::present() const
{
    // Use the existing presenter entry point.
    renderPresent();
}

bool RealDisplay::isOrchestratorEnabled() const
{
    return renderDisplayOrchestratorEnabled();
}

bool RealDisplay::orchestratorOwnsPresenter() const
{
    return renderDisplayOrchestratorOwnsPresenter();
}

} // namespace fallout
