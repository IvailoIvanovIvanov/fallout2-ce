#ifndef FALLOUT_VIRTUAL_INPUT_H_
#define FALLOUT_VIRTUAL_INPUT_H_

#include <SDL.h>

#include "geometry.h"

namespace fallout {

struct VirtualMouseMappingSample {
    int windowX;
    int windowY;
    int physicalX;
    int physicalY;
    double virtualExactX;
    double virtualExactY;
    bool insideViewport;
    Rect viewport;
};

void virtualInputReset();
void virtualInputCaptureEvent(const SDL_Event* event);
bool virtualInputPeekMouseWindowPoint(int* windowX, int* windowY);
void virtualInputConsumeWheelDeltas(int* wheelX, int* wheelY);
void virtualInputPublishMouseMapping(const VirtualMouseMappingSample& sample);
void virtualInputRenderOverlay(SDL_Renderer* renderer);

} // namespace fallout

#endif /* FALLOUT_VIRTUAL_INPUT_H_ */
