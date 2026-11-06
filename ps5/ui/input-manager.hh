// XOG: the controller as the dashboard reads it. GPL-3.0-or-later.
#pragma once
#include <stdint.h>

enum {
    CONTROLLER_BUTTON_A = 1 << 0, CONTROLLER_BUTTON_B = 1 << 1, CONTROLLER_BUTTON_X = 1 << 2,
    CONTROLLER_BUTTON_Y = 1 << 3, CONTROLLER_BUTTON_DPAD_LEFT = 1 << 4, CONTROLLER_BUTTON_DPAD_UP = 1 << 5,
    CONTROLLER_BUTTON_DPAD_RIGHT = 1 << 6, CONTROLLER_BUTTON_DPAD_DOWN = 1 << 7, CONTROLLER_BUTTON_BACK = 1 << 8,
    CONTROLLER_BUTTON_START = 1 << 9, CONTROLLER_BUTTON_WHITE = 1 << 10, CONTROLLER_BUTTON_BLACK = 1 << 11,
    CONTROLLER_BUTTON_LSTICK = 1 << 12, CONTROLLER_BUTTON_RSTICK = 1 << 13, CONTROLLER_BUTTON_GUIDE = 1 << 14,
};

class InputManager
{
public:
    void Update();
    uint32_t CombinedButtons() const { return m_buttons; }
    float LeftX() const { return m_lx; }
    float LeftY() const { return m_ly; }
    float RightX() const { return m_rx; }
    float RightY() const { return m_ry; }
    float LeftTrigger() const { return m_lt; }
    float RightTrigger() const { return m_rt; }
    bool Connected() const { return m_handle >= 0; }

private:
    int m_handle = -1;
    uint32_t m_buttons = 0;
    float m_lx = 0, m_ly = 0, m_rx = 0, m_ry = 0, m_lt = 0, m_rt = 0;
};

extern InputManager g_input_mgr;
