#ifndef FALLOUT_PHANTOM_DISPLAY_H_
#define FALLOUT_PHANTOM_DISPLAY_H_

#include "geometry.h"

namespace fallout {

class PhantomDisplay {
public:
    unsigned char* getScreenBuffer() const;
    int getScreenPitch() const;
    void invalidateRect(const Rect& rect) const;
    void invalidateAll() const;
};

} // namespace fallout

#endif /* FALLOUT_PHANTOM_DISPLAY_H_ */
