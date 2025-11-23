#ifndef FALLOUT_RENDER_TRACE_H_
#define FALLOUT_RENDER_TRACE_H_

#include "geometry.h"

namespace fallout {

enum class RenderTraceLayer {
    Ui,
    TileFloor,
    TileRoof,
    ObjectPreRoof,
    ObjectPostRoof,
};

struct RenderTraceOp {
    RenderTraceLayer layer;
    int fid;
    int frame;
    int rotation;
    Rect rect;
    int elevation;
    int depthKey;
};

void renderTraceRecord(RenderTraceLayer layer, int fid, int frame, int rotation, const Rect& rect, int elevation, int depthKey);
void renderTraceCommitFrame();
bool renderTraceHandleHotkey(int keyCode);
void renderTraceReset();

} // namespace fallout

#endif /* FALLOUT_RENDER_TRACE_H_ */
