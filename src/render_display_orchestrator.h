#ifndef FALLOUT_RENDER_DISPLAY_ORCHESTRATOR_H_
#define FALLOUT_RENDER_DISPLAY_ORCHESTRATOR_H_

namespace fallout {

bool renderDisplayOrchestratorEnabled();
bool renderDisplayOrchestratorConsumesTileOverlays();
bool renderDisplayOrchestratorOwnsPresenter();
void renderDisplayOrchestratorProcess();
// Call when leaving the game world (map change, exit to menu) to clear persistent HD content state.
void renderDisplayOrchestratorResetContent();

} // namespace fallout

#endif /* FALLOUT_RENDER_DISPLAY_ORCHESTRATOR_H_ */
