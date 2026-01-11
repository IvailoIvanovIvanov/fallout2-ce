#ifndef DINPUT_H
#define DINPUT_H

#include <SDL.h>

namespace fallout {

typedef struct MouseData {
    int x;
    int y;
    int absoluteX;
    int absoluteY;
    unsigned char buttons[2];
    int wheelX;
    int wheelY;
} MouseData;

typedef struct KeyboardData {
    int key;
    char down;
} KeyboardData;

bool directInputInit();
void directInputFree();
bool mouseDeviceAcquire();
bool mouseDeviceUnacquire();
bool mouseDeviceGetData(MouseData* mouseData);
bool keyboardDeviceAcquire();
bool keyboardDeviceUnacquire();
bool keyboardDeviceReset();
bool keyboardDeviceGetData(KeyboardData* keyboardData);
bool mouseDeviceInit();
void mouseDeviceAccumulateWheelDelta(int x, int y);
void mouseDeviceHandleEvent(const SDL_Event* event);
void mouseDeviceFree();
bool keyboardDeviceInit();
void keyboardDeviceFree();

} // namespace fallout

#endif /* DINPUT_H */
