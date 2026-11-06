// XOG: the DualSense for the dashboard: read here, handed to ImGui as its gamepad.
// The dashboard follows your button layout, as the games do. GPL-3.0-or-later.
#include "common.hh"
#include "input-manager.hh"

extern "C" {
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void *param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void *data);
}

InputManager g_input_mgr;

struct PadData {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
    uint8_t pad[2];
    uint8_t rest[256];
};

void InputManager::Update()
{
    ImGuiIO &io = ImGui::GetIO();
    if (m_handle < 0) {
        static bool tried;
        if (tried) {
            return;
        }
        tried = true;
        sceUserServiceInitialize(NULL);
        scePadInit();
        int32_t user = -1;
        sceUserServiceGetInitialUser(&user);
        m_handle = scePadOpen(user, 0, 0, NULL);
        if (m_handle < 0) {
            m_handle = scePadGetHandle(user, 0, 0);
        }
        xog_ui_log("DualSense: user %d, handle %d", user, m_handle);
        if (m_handle < 0) {
            return;
        }
    }
    PadData d;
    memset(&d, 0, sizeof(d));
    if (scePadReadState(m_handle, &d) < 0) {
        return;
    }
    xemu_ps5_pad_seen(d.buttons, d.l2, d.r2);
    auto lvl = [&](int xbox) { return xemu_ps5_xbox_level(xbox, d.buttons, d.l2, d.r2); };
    m_buttons = 0;
    const struct { int xbox; uint32_t bit; ImGuiKey key; } map[] = {
        { XB_A, CONTROLLER_BUTTON_A, ImGuiKey_GamepadFaceDown },
        { XB_B, CONTROLLER_BUTTON_B, ImGuiKey_GamepadFaceRight },
        { XB_X, CONTROLLER_BUTTON_X, ImGuiKey_GamepadFaceLeft },
        { XB_Y, CONTROLLER_BUTTON_Y, ImGuiKey_GamepadFaceUp },
        { XB_UP, CONTROLLER_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp },
        { XB_DOWN, CONTROLLER_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown },
        { XB_LEFT, CONTROLLER_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft },
        { XB_RIGHT, CONTROLLER_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight },
        { XB_BACK, CONTROLLER_BUTTON_BACK, ImGuiKey_GamepadBack },
        { XB_START, CONTROLLER_BUTTON_START, ImGuiKey_GamepadStart },
        { XB_WHITE, CONTROLLER_BUTTON_WHITE, ImGuiKey_GamepadL1 },
        { XB_BLACK, CONTROLLER_BUTTON_BLACK, ImGuiKey_GamepadR1 },
        { XB_LSTICK, CONTROLLER_BUTTON_LSTICK, ImGuiKey_GamepadL3 },
        { XB_RSTICK, CONTROLLER_BUTTON_RSTICK, ImGuiKey_GamepadR3 },
    };
    for (const auto &m : map) {
        const bool down = lvl(m.xbox) > 127;
        if (down) {
            m_buttons |= m.bit;
        }
        io.AddKeyEvent(m.key, down);
    }
    if (d.buttons & 0x00100000u) { // the touchpad
        m_buttons |= CONTROLLER_BUTTON_GUIDE;
    }
    m_lt = lvl(XB_LT) / 255.0f;
    m_rt = lvl(XB_RT) / 255.0f;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, m_lt > 0.1f, m_lt);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, m_rt > 0.1f, m_rt);
    auto axis = [](uint8_t v) {
        float f = (v - 128) / 127.0f;
        return fabsf(f) < 0.25f ? 0.0f : std::clamp(f, -1.0f, 1.0f);
    };
    m_lx = axis(d.lx);
    m_ly = axis(d.ly);
    m_rx = axis(d.rx);
    m_ry = axis(d.ry);
    auto stick = [&](ImGuiKey neg, ImGuiKey pos, float v) {
        io.AddKeyAnalogEvent(neg, v < 0, v < 0 ? -v : 0);
        io.AddKeyAnalogEvent(pos, v > 0, v > 0 ? v : 0);
    };
    stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, m_lx);
    stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, m_ly);
    stick(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, m_rx);
    stick(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, m_ry);
}
