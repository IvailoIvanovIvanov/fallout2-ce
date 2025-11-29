#include "phantom_display.h"

#include "window_manager.h"

namespace fallout {

unsigned char* PhantomDisplay::getScreenBuffer() const
{
    return windowGetVirtualScreenBuffer();
}

int PhantomDisplay::getScreenPitch() const
{
    return windowGetVirtualScreenPitch();
}

void PhantomDisplay::invalidateRect(const Rect& rect) const
{
    windowVirtualScreenInvalidateRect(rect);
}

void PhantomDisplay::invalidateAll() const
{
    windowVirtualScreenInvalidateAll();
}

} // namespace fallout
