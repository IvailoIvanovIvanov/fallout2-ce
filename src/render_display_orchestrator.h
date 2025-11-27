#ifndef FALLOUT_RENDER_DISPLAY_ORCHESTRATOR_H_
#define FALLOUT_RENDER_DISPLAY_ORCHESTRATOR_H_

namespace fallout {

bool renderDisplayOrchestratorEnabled();
bool renderDisplayOrchestratorConsumesTileOverlays();
bool renderDisplayOrchestratorOwnsPresenter();
void renderDisplayOrchestratorProcess();

} // namespace fallout

#endif /* FALLOUT_RENDER_DISPLAY_ORCHESTRATOR_H_ */
