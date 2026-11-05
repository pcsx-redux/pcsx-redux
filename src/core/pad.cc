/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#define _USE_MATH_DEFINES
#include "core/pad.h"

#include <SDL3/SDL.h>
#include <memory.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <magic_enum/magic_enum_all.hpp>
#include <optional>

#include "core/psxemulator.h"
#include "core/shmdisplay.h"
#include "core/system.h"
#include "fmt/format.h"
#include "gui/gui.h"
#include "imgui.h"
#include "support/file.h"
#include "support/imgui-helpers.h"

class PadsImpl : public PCSX::Pads {
  public:
    enum class InputType { Auto, Controller, Keyboard };
    enum class PadType { Digital = 0, Analog, Mouse, Negcon, Gun, Guncon };

    PadsImpl();
    void init() override;
    void shutdown() override;
    bool startPoll(Port port, uint8_t address, uint8_t& reply) override;
    uint8_t poll(uint8_t value, Port port, uint32_t& padState) override;
    bool hasMultitap(Port port) override;

    json getCfg() override;
    void setCfg(const json& j) override;
    void setDefaults() override;
    bool configure(PCSX::GUI* gui) override;

    void scanGamepads();
    void reset() override;
    void getTransferState(Port port, TransferState& state) override;
    void setTransferState(Port port, const TransferState& state) override;
    void map();

    void setLua(PCSX::Lua L) override;

    bool isPadConnected(int pad) override {
        if (pad > m_pads.size()) {
            return false;
        } else {
            return m_pads[pad - 1].isControllerConnected();
        }
    }

  private:
    PCSX::EventBus::Listener m_listener;
    // Open SDL gamepad handles, one per slot we've discovered. nullptr means the
    // slot is unused. The slot index doubles as the user-visible "controller ID"
    // saved in SettingControllerID.
    SDL_Gamepad* m_gamepads[16] = {nullptr};

    // Triggers are reported as axes by SDL but pad bindings treat them as
    // virtual buttons. INVALID is encoded as SDL_GAMEPAD_BUTTON_INVALID (-1) so
    // raw SDL queries naturally short-circuit. The trigger sentinels live above
    // SDL_GAMEPAD_BUTTON_COUNT to avoid collisions with future enum entries.
    static constexpr int PCSX_GAMEPAD_BUTTON_LEFT_TRIGGER = 100;
    static constexpr int PCSX_GAMEPAD_BUTTON_RIGHT_TRIGGER = 101;
    static constexpr int PCSX_GAMEPAD_BUTTON_INVALID = SDL_GAMEPAD_BUTTON_INVALID;

    // settings block
    // Pad keyboard bindings
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadUp"), SDL_SCANCODE_UP> Keyboard_PadUp;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadRight"), SDL_SCANCODE_RIGHT> Keyboard_PadRight;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadDown"), SDL_SCANCODE_DOWN> Keyboard_PadDown;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadLeft"), SDL_SCANCODE_LEFT> Keyboard_PadLeft;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadCross"), SDL_SCANCODE_X> Keyboard_PadCross;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadTriangle"), SDL_SCANCODE_S> Keyboard_PadTriangle;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadSquare"), SDL_SCANCODE_Z> Keyboard_PadSquare;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadCircle"), SDL_SCANCODE_D> Keyboard_PadCircle;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadSelect"), SDL_SCANCODE_BACKSPACE> Keyboard_PadSelect;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadSstart"), SDL_SCANCODE_RETURN> Keyboard_PadStart;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadL1"), SDL_SCANCODE_Q> Keyboard_PadL1;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadL2"), SDL_SCANCODE_A> Keyboard_PadL2;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadL3"), SDL_SCANCODE_W> Keyboard_PadL3;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadR1"), SDL_SCANCODE_R> Keyboard_PadR1;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadR2"), SDL_SCANCODE_F> Keyboard_PadR2;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_PadR3"), SDL_SCANCODE_T> Keyboard_PadR3;
    typedef PCSX::Setting<int, TYPESTRING("Keyboard_AnalogMode"), SDL_SCANCODE_UNKNOWN> Keyboard_AnalogMode;

    // Pad controller bindings. Defaults reference SDL gamepad button enums
    // (PS-style face buttons via SDL_GAMEPAD_BUTTON_SOUTH/EAST/WEST/NORTH).
    // Existing user configs that hand-edited these values to GLFW button enums
    // will not round-trip; the in-tree UI does not expose per-button rebinding,
    // so the impact is limited to hand-rolled JSON.
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadUp"), SDL_GAMEPAD_BUTTON_DPAD_UP> Controller_PadUp;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadRight"), SDL_GAMEPAD_BUTTON_DPAD_RIGHT> Controller_PadRight;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadDown"), SDL_GAMEPAD_BUTTON_DPAD_DOWN> Controller_PadDown;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadLeft"), SDL_GAMEPAD_BUTTON_DPAD_LEFT> Controller_PadLeft;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadCross"), SDL_GAMEPAD_BUTTON_SOUTH> Controller_PadCross;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadTriangle"), SDL_GAMEPAD_BUTTON_NORTH> Controller_PadTriangle;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadSquare"), SDL_GAMEPAD_BUTTON_WEST> Controller_PadSquare;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadCircle"), SDL_GAMEPAD_BUTTON_EAST> Controller_PadCircle;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadSelect"), SDL_GAMEPAD_BUTTON_BACK> Controller_PadSelect;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadSstart"), SDL_GAMEPAD_BUTTON_START> Controller_PadStart;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadL1"), SDL_GAMEPAD_BUTTON_LEFT_SHOULDER> Controller_PadL1;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadL2"), PCSX_GAMEPAD_BUTTON_LEFT_TRIGGER> Controller_PadL2;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadL3"), SDL_GAMEPAD_BUTTON_LEFT_STICK> Controller_PadL3;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadR1"), SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER> Controller_PadR1;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadR2"), PCSX_GAMEPAD_BUTTON_RIGHT_TRIGGER> Controller_PadR2;
    typedef PCSX::Setting<int, TYPESTRING("Controller_PadR3"), SDL_GAMEPAD_BUTTON_RIGHT_STICK> Controller_PadR3;

    typedef PCSX::Setting<InputType, TYPESTRING("PadType"), InputType::Auto> SettingInputType;
    // These typestrings are kind of odd, but it's best not to change so as not to break old config files
    typedef PCSX::Setting<PadType, TYPESTRING("DeviceType"), PadType::Digital> SettingDeviceType;
    typedef PCSX::Setting<int, TYPESTRING("ID")> SettingControllerID;

    typedef PCSX::Setting<bool, TYPESTRING("Connected")> SettingConnected;
    typedef PCSX::Setting<bool, TYPESTRING("Rumble"), true> SettingRumble;
    // A multitap (SCPH-1070) between the port and this pad, which becomes the tap's slot A.
    typedef PCSX::Setting<bool, TYPESTRING("Multitap"), false> SettingMultitap;
    // Default sensitivity = 5/10 = 0.5
    typedef PCSX::SettingFloat<TYPESTRING("MouseSensitivityX"), 5, 10> SettingMouseSensitivityX;
    typedef PCSX::SettingFloat<TYPESTRING("MouseSensitivityY"), 5, 10> SettingMouseSensitivityY;
    // Absent in configs from before keyboard bindings were stored as SDL scancodes.
    typedef PCSX::Setting<int, TYPESTRING("KeyboardBindingsVersion"), 1> SettingKeyboardBindingsVersion;

    typedef PCSX::Settings<
        Keyboard_PadUp, Keyboard_PadRight, Keyboard_PadDown, Keyboard_PadLeft, Keyboard_PadCross, Keyboard_PadTriangle,
        Keyboard_PadSquare, Keyboard_PadCircle, Keyboard_PadSelect, Keyboard_PadStart, Keyboard_PadL1, Keyboard_PadL2,
        Keyboard_PadL3, Keyboard_PadR1, Keyboard_PadR2, Keyboard_PadR3, Keyboard_AnalogMode, Controller_PadUp,
        Controller_PadRight, Controller_PadDown, Controller_PadLeft, Controller_PadCross, Controller_PadTriangle,
        Controller_PadSquare, Controller_PadCircle, Controller_PadSelect, Controller_PadStart, Controller_PadL1,
        Controller_PadL2, Controller_PadL3, Controller_PadR1, Controller_PadR2, Controller_PadR3, SettingInputType,
        SettingDeviceType, SettingControllerID, SettingConnected, SettingRumble, SettingMouseSensitivityX,
        SettingMouseSensitivityY, SettingKeyboardBindingsVersion, SettingMultitap>
        PadSettings;

    struct PadData {
        // status of buttons - every controller fills this field
        uint16_t buttonStatus;

        // overriding from Lua
        uint16_t overrides = 0xffff;

        // input from a host application, see ShmDisplay
        uint16_t hostButtons = 0xffff;

        // Analog stick values in range (0 - 255) where 128 = center
        uint8_t rightJoyX, rightJoyY, leftJoyX, leftJoyY;
    };

    enum class PadCommands : uint8_t {
        Idle = 0x00,
        Read = 0x42,
        SetConfigMode = 0x43,
        SetAnalogMode = 0x44,
        GetAnalogMode = 0x45,
        Unknown46 = 0x46,
        Unknown47 = 0x47,
        Unknown4C = 0x4C,
        UnlockRumble = 0x4D
    };

    struct Pad {
        uint8_t startPoll();
        uint8_t read();
        uint8_t poll(uint8_t value, uint32_t& padState);
        uint8_t doDualshockCommand(uint32_t& padState);
        void getButtons();
        struct GamepadState {
            bool buttons[SDL_GAMEPAD_BUTTON_COUNT];
            float axes[SDL_GAMEPAD_AXIS_COUNT];  // normalized to [-1, 1] like GLFW reported
        };
        bool isControllerButtonPressed(int button, const GamepadState& state);
        bool isControllerConnected() { return m_settings.get<SettingConnected>(); }

        json getCfg();
        void setCfg(const json& j);
        void setDefaults(bool firstController);
        void map();
        void reset();

        bool configure();
        void keyboardEvent(const PCSX::Events::Keyboard&);
        int& getButtonFromGUIIndex(int buttonIndex);

        int m_scancodes[16];
        int m_padMapping[16];
        // m_type is the configured device, m_activeType the one in use, which the host can override.
        PadType m_type = PadType::Digital;
        PadType m_activeType = PadType::Digital;
        // The host picked this pad's device through -shmdisplay, so it is plugged in for this poll
        // whatever the Connected setting says.
        bool m_hostConnected = false;
        PadData m_data;
        // The port whose host mouse fields feed this pad, or -1 for multitap slots B to D.
        int m_port = -1;

        SDL_Gamepad* m_gamepad = nullptr;
        int m_buttonToWait = -1;
        bool m_changed = false;

        bool m_configMode = false;
        bool m_analogMode = false;

        // Rumble. m_motorMapping holds the six bytes last set by config command 4Dh.
        // Each entry says what the matching byte of the 42h read command drives:
        // 00h = right/small motor (bit0, on/off), 01h = left/large motor (bit0-7,
        // analog), FFh = nothing. All FFh is the power-on state, in which the pad
        // still honours the old one-motor method, until config commands are used at
        // all -- after that the old method is gone for good.
        uint8_t m_motorMapping[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        bool m_configCommandsUsed = false;
        uint8_t m_motorSmall = 0, m_motorLarge = 0;
        uint8_t m_oldMethodLatch = 0;
        uint16_t m_rumbleAppliedSmall = 0, m_rumbleAppliedLarge = 0;
        uint64_t m_rumbleRefreshedAt = 0;

        void applyRumble(bool force);
        void lockRumble();

        PadSettings m_settings;

        uint8_t m_buf[256] = {};
        int m_bufferLen = 0, m_currentByte = 0;
        uint8_t m_cmd = magic_enum::enum_integer(PadCommands::Idle);

        uint8_t m_stdpar[8] = {0x41, 0x5a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        uint8_t m_mousepar[6] = {0x12, 0x5a, 0xff, 0xff, 0xff, 0xff};
        uint8_t m_analogpar[8] = {0x73, 0x5a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    };

    std::array<Pad, 2> m_pads;
    unsigned m_selectedPadForConfig = 0;

    // A multitap, SCPH-1070. Slot A is the port's own pad, m_pads[port]; slots B to D are plain digital
    // pads that only the host fills, through hostSlotPads. See the psx-spx multitap page.
    struct Multitap {
        std::array<Pad, 3> slots;
        // Set by the third byte of a read of slot A: the next access to address 1 is the long read.
        bool request = false;
        // Set by a long read whose slot A block the host did not start with 42h: a long read next is garbage.
        bool stale = false;
        // Per access:
        bool present = false;
        bool longRead = false;
        bool garbage = false;
        unsigned slot = 0;
        unsigned byte = 0;
        uint8_t command = 0;
        bool connected[4] = {false, false, false, false};
        bool slotAnswering = false;
    };
    std::array<Multitap, 2> m_taps;

    Pad& tapSlot(int port, unsigned slot) { return slot == 0 ? m_pads[port] : m_taps[port].slots[slot - 1]; }
    uint8_t pollLong(Multitap& tap, int port, uint8_t value, uint32_t& padState);
};

static PadsImpl* s_pads = nullptr;

static ImGuiKey SdlScancodeToImGuiKey(int scancode) {
    switch (scancode) {
        case SDL_SCANCODE_TAB:
            return ImGuiKey_Tab;
        case SDL_SCANCODE_LEFT:
            return ImGuiKey_LeftArrow;
        case SDL_SCANCODE_RIGHT:
            return ImGuiKey_RightArrow;
        case SDL_SCANCODE_UP:
            return ImGuiKey_UpArrow;
        case SDL_SCANCODE_DOWN:
            return ImGuiKey_DownArrow;
        case SDL_SCANCODE_PAGEUP:
            return ImGuiKey_PageUp;
        case SDL_SCANCODE_PAGEDOWN:
            return ImGuiKey_PageDown;
        case SDL_SCANCODE_HOME:
            return ImGuiKey_Home;
        case SDL_SCANCODE_END:
            return ImGuiKey_End;
        case SDL_SCANCODE_INSERT:
            return ImGuiKey_Insert;
        case SDL_SCANCODE_DELETE:
            return ImGuiKey_Delete;
        case SDL_SCANCODE_BACKSPACE:
            return ImGuiKey_Backspace;
        case SDL_SCANCODE_SPACE:
            return ImGuiKey_Space;
        case SDL_SCANCODE_RETURN:
            return ImGuiKey_Enter;
        case SDL_SCANCODE_ESCAPE:
            return ImGuiKey_Escape;
        case SDL_SCANCODE_APOSTROPHE:
            return ImGuiKey_Apostrophe;
        case SDL_SCANCODE_COMMA:
            return ImGuiKey_Comma;
        case SDL_SCANCODE_MINUS:
            return ImGuiKey_Minus;
        case SDL_SCANCODE_PERIOD:
            return ImGuiKey_Period;
        case SDL_SCANCODE_SLASH:
            return ImGuiKey_Slash;
        case SDL_SCANCODE_SEMICOLON:
            return ImGuiKey_Semicolon;
        case SDL_SCANCODE_EQUALS:
            return ImGuiKey_Equal;
        case SDL_SCANCODE_LEFTBRACKET:
            return ImGuiKey_LeftBracket;
        case SDL_SCANCODE_BACKSLASH:
            return ImGuiKey_Backslash;
        case SDL_SCANCODE_RIGHTBRACKET:
            return ImGuiKey_RightBracket;
        case SDL_SCANCODE_GRAVE:
            return ImGuiKey_GraveAccent;
        case SDL_SCANCODE_CAPSLOCK:
            return ImGuiKey_CapsLock;
        case SDL_SCANCODE_SCROLLLOCK:
            return ImGuiKey_ScrollLock;
        case SDL_SCANCODE_NUMLOCKCLEAR:
            return ImGuiKey_NumLock;
        case SDL_SCANCODE_PRINTSCREEN:
            return ImGuiKey_PrintScreen;
        case SDL_SCANCODE_PAUSE:
            return ImGuiKey_Pause;
        case SDL_SCANCODE_KP_0:
            return ImGuiKey_Keypad0;
        case SDL_SCANCODE_KP_1:
            return ImGuiKey_Keypad1;
        case SDL_SCANCODE_KP_2:
            return ImGuiKey_Keypad2;
        case SDL_SCANCODE_KP_3:
            return ImGuiKey_Keypad3;
        case SDL_SCANCODE_KP_4:
            return ImGuiKey_Keypad4;
        case SDL_SCANCODE_KP_5:
            return ImGuiKey_Keypad5;
        case SDL_SCANCODE_KP_6:
            return ImGuiKey_Keypad6;
        case SDL_SCANCODE_KP_7:
            return ImGuiKey_Keypad7;
        case SDL_SCANCODE_KP_8:
            return ImGuiKey_Keypad8;
        case SDL_SCANCODE_KP_9:
            return ImGuiKey_Keypad9;
        case SDL_SCANCODE_KP_PERIOD:
            return ImGuiKey_KeypadDecimal;
        case SDL_SCANCODE_KP_DIVIDE:
            return ImGuiKey_KeypadDivide;
        case SDL_SCANCODE_KP_MULTIPLY:
            return ImGuiKey_KeypadMultiply;
        case SDL_SCANCODE_KP_MINUS:
            return ImGuiKey_KeypadSubtract;
        case SDL_SCANCODE_KP_PLUS:
            return ImGuiKey_KeypadAdd;
        case SDL_SCANCODE_KP_ENTER:
            return ImGuiKey_KeypadEnter;
        case SDL_SCANCODE_KP_EQUALS:
            return ImGuiKey_KeypadEqual;
        case SDL_SCANCODE_LSHIFT:
            return ImGuiKey_LeftShift;
        case SDL_SCANCODE_LCTRL:
            return ImGuiKey_LeftCtrl;
        case SDL_SCANCODE_LALT:
            return ImGuiKey_LeftAlt;
        case SDL_SCANCODE_LGUI:
            return ImGuiKey_LeftSuper;
        case SDL_SCANCODE_RSHIFT:
            return ImGuiKey_RightShift;
        case SDL_SCANCODE_RCTRL:
            return ImGuiKey_RightCtrl;
        case SDL_SCANCODE_RALT:
            return ImGuiKey_RightAlt;
        case SDL_SCANCODE_RGUI:
            return ImGuiKey_RightSuper;
        case SDL_SCANCODE_MENU:
            return ImGuiKey_Menu;
        case SDL_SCANCODE_0:
            return ImGuiKey_0;
        case SDL_SCANCODE_1:
            return ImGuiKey_1;
        case SDL_SCANCODE_2:
            return ImGuiKey_2;
        case SDL_SCANCODE_3:
            return ImGuiKey_3;
        case SDL_SCANCODE_4:
            return ImGuiKey_4;
        case SDL_SCANCODE_5:
            return ImGuiKey_5;
        case SDL_SCANCODE_6:
            return ImGuiKey_6;
        case SDL_SCANCODE_7:
            return ImGuiKey_7;
        case SDL_SCANCODE_8:
            return ImGuiKey_8;
        case SDL_SCANCODE_9:
            return ImGuiKey_9;
        case SDL_SCANCODE_A:
            return ImGuiKey_A;
        case SDL_SCANCODE_B:
            return ImGuiKey_B;
        case SDL_SCANCODE_C:
            return ImGuiKey_C;
        case SDL_SCANCODE_D:
            return ImGuiKey_D;
        case SDL_SCANCODE_E:
            return ImGuiKey_E;
        case SDL_SCANCODE_F:
            return ImGuiKey_F;
        case SDL_SCANCODE_G:
            return ImGuiKey_G;
        case SDL_SCANCODE_H:
            return ImGuiKey_H;
        case SDL_SCANCODE_I:
            return ImGuiKey_I;
        case SDL_SCANCODE_J:
            return ImGuiKey_J;
        case SDL_SCANCODE_K:
            return ImGuiKey_K;
        case SDL_SCANCODE_L:
            return ImGuiKey_L;
        case SDL_SCANCODE_M:
            return ImGuiKey_M;
        case SDL_SCANCODE_N:
            return ImGuiKey_N;
        case SDL_SCANCODE_O:
            return ImGuiKey_O;
        case SDL_SCANCODE_P:
            return ImGuiKey_P;
        case SDL_SCANCODE_Q:
            return ImGuiKey_Q;
        case SDL_SCANCODE_R:
            return ImGuiKey_R;
        case SDL_SCANCODE_S:
            return ImGuiKey_S;
        case SDL_SCANCODE_T:
            return ImGuiKey_T;
        case SDL_SCANCODE_U:
            return ImGuiKey_U;
        case SDL_SCANCODE_V:
            return ImGuiKey_V;
        case SDL_SCANCODE_W:
            return ImGuiKey_W;
        case SDL_SCANCODE_X:
            return ImGuiKey_X;
        case SDL_SCANCODE_Y:
            return ImGuiKey_Y;
        case SDL_SCANCODE_Z:
            return ImGuiKey_Z;
        case SDL_SCANCODE_F1:
            return ImGuiKey_F1;
        case SDL_SCANCODE_F2:
            return ImGuiKey_F2;
        case SDL_SCANCODE_F3:
            return ImGuiKey_F3;
        case SDL_SCANCODE_F4:
            return ImGuiKey_F4;
        case SDL_SCANCODE_F5:
            return ImGuiKey_F5;
        case SDL_SCANCODE_F6:
            return ImGuiKey_F6;
        case SDL_SCANCODE_F7:
            return ImGuiKey_F7;
        case SDL_SCANCODE_F8:
            return ImGuiKey_F8;
        case SDL_SCANCODE_F9:
            return ImGuiKey_F9;
        case SDL_SCANCODE_F10:
            return ImGuiKey_F10;
        case SDL_SCANCODE_F11:
            return ImGuiKey_F11;
        case SDL_SCANCODE_F12:
            return ImGuiKey_F12;
        case SDL_SCANCODE_F13:
            return ImGuiKey_F13;
        case SDL_SCANCODE_F14:
            return ImGuiKey_F14;
        case SDL_SCANCODE_F15:
            return ImGuiKey_F15;
        case SDL_SCANCODE_F16:
            return ImGuiKey_F16;
        case SDL_SCANCODE_F17:
            return ImGuiKey_F17;
        case SDL_SCANCODE_F18:
            return ImGuiKey_F18;
        case SDL_SCANCODE_F19:
            return ImGuiKey_F19;
        case SDL_SCANCODE_F20:
            return ImGuiKey_F20;
        case SDL_SCANCODE_F21:
            return ImGuiKey_F21;
        case SDL_SCANCODE_F22:
            return ImGuiKey_F22;
        case SDL_SCANCODE_F23:
            return ImGuiKey_F23;
        case SDL_SCANCODE_F24:
            return ImGuiKey_F24;
        case SDL_SCANCODE_APPLICATION:
            return ImGuiKey_Menu;
        case SDL_SCANCODE_NONUSBACKSLASH:
            return ImGuiKey_Oem102;
        default:
            return ImGuiKey_None;
    }
}

// GLFW key code -> SDL scancode, for configs written by the GLFW builds. GLFW
// keys are physical keys named after the US layout, same as SDL scancodes.
static std::optional<int> GlfwKeyToScancode(int key) {
    if (key >= 65 && key <= 90) return SDL_SCANCODE_A + (key - 65);        // A-Z
    if (key >= 49 && key <= 57) return SDL_SCANCODE_1 + (key - 49);        // 1-9
    if (key >= 290 && key <= 301) return SDL_SCANCODE_F1 + (key - 290);    // F1-F12
    if (key >= 302 && key <= 313) return SDL_SCANCODE_F13 + (key - 302);   // F13-F24
    if (key >= 321 && key <= 329) return SDL_SCANCODE_KP_1 + (key - 321);  // KP_1-KP_9
    switch (key) {
        case -1:  // UNKNOWN
            return SDL_SCANCODE_UNKNOWN;
        case 32:  // SPACE
            return SDL_SCANCODE_SPACE;
        case 39:  // APOSTROPHE
            return SDL_SCANCODE_APOSTROPHE;
        case 44:  // COMMA
            return SDL_SCANCODE_COMMA;
        case 45:  // MINUS
            return SDL_SCANCODE_MINUS;
        case 46:  // PERIOD
            return SDL_SCANCODE_PERIOD;
        case 47:  // SLASH
            return SDL_SCANCODE_SLASH;
        case 48:  // 0
            return SDL_SCANCODE_0;
        case 59:  // SEMICOLON
            return SDL_SCANCODE_SEMICOLON;
        case 61:  // EQUAL
            return SDL_SCANCODE_EQUALS;
        case 91:  // LEFT_BRACKET
            return SDL_SCANCODE_LEFTBRACKET;
        case 92:  // BACKSLASH
            return SDL_SCANCODE_BACKSLASH;
        case 93:  // RIGHT_BRACKET
            return SDL_SCANCODE_RIGHTBRACKET;
        case 96:  // GRAVE_ACCENT
            return SDL_SCANCODE_GRAVE;
        // GLFW reports the ISO key left of Z as WORLD_2 on Windows and WORLD_1 elsewhere.
        case 161:  // WORLD_1
            return SDL_SCANCODE_NONUSBACKSLASH;
        case 162:  // WORLD_2
            return SDL_SCANCODE_NONUSBACKSLASH;
        case 256:  // ESCAPE
            return SDL_SCANCODE_ESCAPE;
        case 257:  // ENTER
            return SDL_SCANCODE_RETURN;
        case 258:  // TAB
            return SDL_SCANCODE_TAB;
        case 259:  // BACKSPACE
            return SDL_SCANCODE_BACKSPACE;
        case 260:  // INSERT
            return SDL_SCANCODE_INSERT;
        case 261:  // DELETE
            return SDL_SCANCODE_DELETE;
        case 262:  // RIGHT
            return SDL_SCANCODE_RIGHT;
        case 263:  // LEFT
            return SDL_SCANCODE_LEFT;
        case 264:  // DOWN
            return SDL_SCANCODE_DOWN;
        case 265:  // UP
            return SDL_SCANCODE_UP;
        case 266:  // PAGE_UP
            return SDL_SCANCODE_PAGEUP;
        case 267:  // PAGE_DOWN
            return SDL_SCANCODE_PAGEDOWN;
        case 268:  // HOME
            return SDL_SCANCODE_HOME;
        case 269:  // END
            return SDL_SCANCODE_END;
        case 280:  // CAPS_LOCK
            return SDL_SCANCODE_CAPSLOCK;
        case 281:  // SCROLL_LOCK
            return SDL_SCANCODE_SCROLLLOCK;
        case 282:  // NUM_LOCK
            return SDL_SCANCODE_NUMLOCKCLEAR;
        case 283:  // PRINT_SCREEN
            return SDL_SCANCODE_PRINTSCREEN;
        case 284:  // PAUSE
            return SDL_SCANCODE_PAUSE;
        case 314:  // F25, no SDL equivalent
            return SDL_SCANCODE_UNKNOWN;
        case 320:  // KP_0
            return SDL_SCANCODE_KP_0;
        case 330:  // KP_DECIMAL
            return SDL_SCANCODE_KP_PERIOD;
        case 331:  // KP_DIVIDE
            return SDL_SCANCODE_KP_DIVIDE;
        case 332:  // KP_MULTIPLY
            return SDL_SCANCODE_KP_MULTIPLY;
        case 333:  // KP_SUBTRACT
            return SDL_SCANCODE_KP_MINUS;
        case 334:  // KP_ADD
            return SDL_SCANCODE_KP_PLUS;
        case 335:  // KP_ENTER
            return SDL_SCANCODE_KP_ENTER;
        case 336:  // KP_EQUAL
            return SDL_SCANCODE_KP_EQUALS;
        case 340:  // LEFT_SHIFT
            return SDL_SCANCODE_LSHIFT;
        case 341:  // LEFT_CONTROL
            return SDL_SCANCODE_LCTRL;
        case 342:  // LEFT_ALT
            return SDL_SCANCODE_LALT;
        case 343:  // LEFT_SUPER
            return SDL_SCANCODE_LGUI;
        case 344:  // RIGHT_SHIFT
            return SDL_SCANCODE_RSHIFT;
        case 345:  // RIGHT_CONTROL
            return SDL_SCANCODE_RCTRL;
        case 346:  // RIGHT_ALT
            return SDL_SCANCODE_RALT;
        case 347:  // RIGHT_SUPER
            return SDL_SCANCODE_RGUI;
        case 348:  // MENU
            return SDL_SCANCODE_APPLICATION;
        default:
            return std::nullopt;
    }
}

bool PCSX::Pads::migrateKeyboardBindings(json& pad) {
    static constexpr const char* c_keys[] = {
        "Keyboard_PadUp",       "Keyboard_PadRight",   "Keyboard_PadDown",   "Keyboard_PadLeft",   "Keyboard_PadCross",
        "Keyboard_PadTriangle", "Keyboard_PadSquare",  "Keyboard_PadCircle", "Keyboard_PadSelect", "Keyboard_PadSstart",
        "Keyboard_PadL1",       "Keyboard_PadL2",      "Keyboard_PadL3",     "Keyboard_PadR1",     "Keyboard_PadR2",
        "Keyboard_PadR3",       "Keyboard_AnalogMode",
    };
    if (!pad.is_object() || pad.contains("KeyboardBindingsVersion")) return false;
    pad["KeyboardBindingsVersion"] = 1;

    // Unstamped configs come from GLFW builds, or from SDL builds that predate the
    // stamp. GLFW wrote -1 for an unbound key and 256 and up for named keys like
    // the arrows. SDL writes 0 for unbound, and its scancodes from 256 up are
    // mode and media keys nobody binds to a pad. A value that is not a GLFW key
    // at all can only be a scancode, so any of those leaves the config alone.
    bool glfw = false;
    for (auto key : c_keys) {
        if (!pad.contains(key) || !pad[key].is_number_integer()) continue;
        int value = pad[key].get<int>();
        if (!GlfwKeyToScancode(value)) return false;
        if (value < 0 || value >= 256) glfw = true;
    }
    if (!glfw) return false;

    for (auto key : c_keys) {
        if (!pad.contains(key) || !pad[key].is_number_integer()) continue;
        pad[key] = *GlfwKeyToScancode(pad[key].get<int>());
    }
    return true;
}

void PadsImpl::init() {
    s_pads = this;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        PCSX::g_system->log(PCSX::LogClass::UI, "SDL_InitSubSystem(SDL_INIT_GAMEPAD) failed: %s\n", SDL_GetError());
    }
    scanGamepads();
    reset();
    map();
}

void PadsImpl::shutdown() {
    // Motors first: SDL_CloseGamepad on a rumbling pad leaves it buzzing on some
    // backends, and the handles are still live here.
    for (auto& pad : m_pads) pad.lockRumble();
    for (auto& g : m_gamepads) {
        if (g) {
            SDL_CloseGamepad(g);
            g = nullptr;
        }
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    s_pads = nullptr;
}

PadsImpl::PadsImpl() : m_listener(PCSX::g_system->m_eventBus) {
    m_pads[0].m_port = 0;
    m_pads[1].m_port = 1;
    for (auto& tap : m_taps) {
        for (auto& slot : tap.slots) slot.m_settings.get<SettingConnected>() = true;
    }
    m_listener.listen<PCSX::Events::Keyboard>([this](const auto& event) {
        if (m_showCfg) {
            m_pads[m_selectedPadForConfig].keyboardEvent(event);
        }
    });
}

void PadsImpl::scanGamepads() {
    // Close any currently-open handles so re-scans (e.g. after hotplug) don't
    // leak. m_gamepad pointers in each Pad become stale here; the caller is
    // expected to follow up with map() to re-resolve them. Stop the motors before
    // closing, as in shutdown(); the pads keep their motor state, and map() makes
    // the next refresh re-apply it to the re-opened handle.
    for (auto& g : m_gamepads) {
        if (g) {
            SDL_RumbleGamepad(g, 0, 0, 0);
            SDL_CloseGamepad(g);
            g = nullptr;
        }
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;
    const unsigned slots = sizeof(m_gamepads) / sizeof(m_gamepads[0]);
    for (int i = 0; i < count && static_cast<unsigned>(i) < slots; i++) {
        m_gamepads[i] = SDL_OpenGamepad(ids[i]);
    }
    SDL_free(ids);
}

void PadsImpl::reset() {
    m_pads[0].reset();
    m_pads[1].reset();
    for (auto& tap : m_taps) {
        for (auto& slot : tap.slots) slot.reset();
        tap.request = false;
        tap.stale = false;
        tap.longRead = false;
        tap.garbage = false;
    }
}

void PadsImpl::getTransferState(Port port, TransferState& state) {
    const Pad& pad = m_pads[magic_enum::enum_integer(port)];
    state.cmd = pad.m_cmd;
    state.configMode = pad.m_configMode;
    state.analogMode = pad.m_analogMode;
    state.bufferLen = pad.m_bufferLen;
    state.currentByte = pad.m_currentByte;
    std::memcpy(state.buf, pad.m_buf, sizeof(state.buf));
    std::memcpy(state.motorMapping, pad.m_motorMapping, sizeof(state.motorMapping));
    state.configCommandsUsed = pad.m_configCommandsUsed;
    state.oldMethodLatch = pad.m_oldMethodLatch;
}

void PadsImpl::setTransferState(Port port, const TransferState& state) {
    Pad& pad = m_pads[magic_enum::enum_integer(port)];
    pad.m_cmd = state.cmd;
    pad.m_configMode = state.configMode;
    pad.m_analogMode = state.analogMode;
    // Both index m_buf, so a damaged state must not be able to push them out of it.
    pad.m_bufferLen = std::min<uint32_t>(state.bufferLen, sizeof(pad.m_buf));
    pad.m_currentByte = std::min<uint32_t>(state.currentByte, sizeof(pad.m_buf));
    std::memcpy(pad.m_buf, state.buf, sizeof(pad.m_buf));
    std::memcpy(pad.m_motorMapping, state.motorMapping, sizeof(pad.m_motorMapping));
    pad.m_configCommandsUsed = state.configCommandsUsed;
    pad.m_oldMethodLatch = state.oldMethodLatch;
}

void PadsImpl::Pad::reset() {
    // m_analogMode = false;
    m_configMode = false;
    m_cmd = magic_enum::enum_integer(PadCommands::Idle);
    m_bufferLen = 0;
    m_currentByte = 0;
    m_data.buttonStatus = 0xffff;
    m_data.overrides = 0xffff;
    lockRumble();
    m_configCommandsUsed = false;
}

// The controller watchdog resets the pad, and hence stops and locks the motors,
// after about a second without communication. Handing SDL that same deadline gets
// us the same behaviour for free when the emulated machine stops polling, whether
// it was paused, reset, or wedged mid-rumble. Refresh at half of it so an effect
// that is still wanted never gets close to expiring.
static constexpr uint32_t c_rumbleDurationMs = 1000;
static constexpr uint64_t c_rumbleRefreshMs = 500;

static bool gamepadHasRumble(SDL_Gamepad* gamepad) {
    if (!gamepad) return false;
    SDL_PropertiesID props = SDL_GetGamepadProperties(gamepad);
    if (!props) return false;
    return SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
}

void PadsImpl::Pad::lockRumble() {
    std::memset(m_motorMapping, 0xff, sizeof(m_motorMapping));
    m_oldMethodLatch = 0;
    m_motorSmall = 0;
    m_motorLarge = 0;
    applyRumble(true);
}

void PadsImpl::Pad::applyRumble(bool force) {
    if (!m_gamepad) return;
    const bool enabled = m_settings.get<SettingRumble>() && m_settings.get<SettingConnected>();
    // SDL's low frequency channel is the heavy/left motor, the high frequency one is
    // the light/right motor. M1 is 0..255 analog, M2 is a single on/off bit.
    const uint16_t low = enabled ? static_cast<uint16_t>(m_motorLarge * 0x101) : 0;
    const uint16_t high = (enabled && m_motorSmall) ? 0xffff : 0;
    const uint64_t now = SDL_GetTicks();
    const bool unchanged = (low == m_rumbleAppliedLarge) && (high == m_rumbleAppliedSmall);
    const bool idle = low == 0 && high == 0;

    if (!force && unchanged && (idle || (now - m_rumbleRefreshedAt) < c_rumbleRefreshMs)) return;
    // Stopping is worth issuing even to a pad with no actuator; it is a no-op there.
    if (!idle && !gamepadHasRumble(m_gamepad)) return;

    SDL_RumbleGamepad(m_gamepad, low, high, c_rumbleDurationMs);
    m_rumbleAppliedLarge = low;
    m_rumbleAppliedSmall = high;
    m_rumbleRefreshedAt = now;
}

void PadsImpl::map() {
    m_pads[0].map();
    m_pads[1].map();
}

void PadsImpl::Pad::map() {
    int id = m_settings.get<SettingControllerID>();
    const unsigned slots = sizeof(s_pads->m_gamepads) / sizeof(s_pads->m_gamepads[0]);
    m_gamepad = (id >= 0 && static_cast<unsigned>(id) < slots) ? s_pads->m_gamepads[id] : nullptr;
    // The handle may be a freshly opened one that has never been sent anything.
    m_rumbleAppliedSmall = m_rumbleAppliedLarge = 0;
    m_rumbleRefreshedAt = 0;
    m_type = m_settings.get<SettingDeviceType>();
    m_activeType = m_type;

    // L3/R3 are only avalable on analog controllers
    if (m_type == PadType::Analog) {
        m_scancodes[1] = m_settings.get<Keyboard_PadL3>();     // L3
        m_scancodes[2] = m_settings.get<Keyboard_PadR3>();     // R3
        m_padMapping[1] = m_settings.get<Controller_PadL3>();  // L3
        m_padMapping[2] = m_settings.get<Controller_PadR3>();  // R3
    } else {
        m_scancodes[1] = 255;
        m_scancodes[2] = 255;
        m_padMapping[1] = PCSX_GAMEPAD_BUTTON_INVALID;
        m_padMapping[2] = PCSX_GAMEPAD_BUTTON_INVALID;
    }

    // keyboard mappings
    m_scancodes[0] = m_settings.get<Keyboard_PadSelect>();     // SELECT
    m_scancodes[3] = m_settings.get<Keyboard_PadStart>();      // START
    m_scancodes[4] = m_settings.get<Keyboard_PadUp>();         // UP
    m_scancodes[5] = m_settings.get<Keyboard_PadRight>();      // RIGHT
    m_scancodes[6] = m_settings.get<Keyboard_PadDown>();       // DOWN
    m_scancodes[7] = m_settings.get<Keyboard_PadLeft>();       // LEFT
    m_scancodes[8] = m_settings.get<Keyboard_PadL2>();         // L2
    m_scancodes[9] = m_settings.get<Keyboard_PadR2>();         // R2
    m_scancodes[10] = m_settings.get<Keyboard_PadL1>();        // L1
    m_scancodes[11] = m_settings.get<Keyboard_PadR1>();        // R1
    m_scancodes[12] = m_settings.get<Keyboard_PadTriangle>();  // TRIANGLE
    m_scancodes[13] = m_settings.get<Keyboard_PadCircle>();    // CIRCLE
    m_scancodes[14] = m_settings.get<Keyboard_PadCross>();     // CROSS
    m_scancodes[15] = m_settings.get<Keyboard_PadSquare>();    // SQUARE

    // gamepad mappings
    m_padMapping[0] = m_settings.get<Controller_PadSelect>();     // SELECT
    m_padMapping[3] = m_settings.get<Controller_PadStart>();      // START
    m_padMapping[4] = m_settings.get<Controller_PadUp>();         // UP
    m_padMapping[5] = m_settings.get<Controller_PadRight>();      // RIGHT
    m_padMapping[6] = m_settings.get<Controller_PadDown>();       // DOWN
    m_padMapping[7] = m_settings.get<Controller_PadLeft>();       // LEFT
    m_padMapping[8] = m_settings.get<Controller_PadL2>();         // L2
    m_padMapping[9] = m_settings.get<Controller_PadR2>();         // R2
    m_padMapping[10] = m_settings.get<Controller_PadL1>();        // L1
    m_padMapping[11] = m_settings.get<Controller_PadR1>();        // R1
    m_padMapping[12] = m_settings.get<Controller_PadTriangle>();  // TRIANGLE
    m_padMapping[13] = m_settings.get<Controller_PadCircle>();    // CIRCLE
    m_padMapping[14] = m_settings.get<Controller_PadCross>();     // CROSS
    m_padMapping[15] = m_settings.get<Controller_PadSquare>();    // SQUARE
}

static constexpr float THRESHOLD = 0.85f;

// Certain buttons on controllers are actually axis that can be pressed, half-pressed, etc.
bool PadsImpl::Pad::isControllerButtonPressed(int button, const GamepadState& state) {
    int mapped = m_padMapping[button];
    switch (mapped) {
        case PCSX_GAMEPAD_BUTTON_LEFT_TRIGGER:
            return state.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] >= THRESHOLD;
        case PCSX_GAMEPAD_BUTTON_RIGHT_TRIGGER:
            return state.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] >= THRESHOLD;
        case PCSX_GAMEPAD_BUTTON_INVALID:
            return false;
        default:
            if (mapped < 0 || mapped >= SDL_GAMEPAD_BUTTON_COUNT) return false;
            return state.buttons[mapped];
    }
}

static constexpr float π(float fraction = 1.0f) { return fraction * M_PI; }

void PadsImpl::Pad::getButtons() {
    PadData& pad = m_data;
    if (!m_settings.get<SettingConnected>() && !m_hostConnected) {
        pad.buttonStatus = 0xffff;
        pad.leftJoyX = pad.rightJoyX = pad.leftJoyY = pad.rightJoyY = 0x80;
        applyRumble(false);  // an unplugged pad is a silent one
        return;
    }

    GamepadState state{};
    bool hasPad = false;
    const auto& inputType = m_settings.get<SettingInputType>();

    auto getKeyboardButtons = [this]() -> uint16_t {
        if (!ImGui::GetCurrentContext()) return 0xffff;
        // Don't feed host keys into the emulated pad while ImGui text fields
        // own the keyboard (typing in the UI would otherwise hit in-game).
        if (ImGui::GetIO().WantTextInput) return 0xffff;
        uint16_t result = 0;
        for (unsigned i = 0; i < 16; i++) {
            auto key = SdlScancodeToImGuiKey(m_scancodes[i]);
            if (key == ImGuiKey_None) continue;
            result |= (ImGui::IsKeyDown(key)) << i;
        }
        return result ^ 0xffff;  // Controls are inverted, so 0 = pressed
    };

    if (inputType == InputType::Keyboard) {
        pad.buttonStatus = getKeyboardButtons();
        pad.leftJoyX = pad.rightJoyX = pad.leftJoyY = pad.rightJoyY = 0x80;
        return;
    }

    // Drive SDL's internal gamepad state and pluck out hotplug events. Other
    // SDL events stay in the queue (gui.cc still owns the rest of the input
    // surface today). Move this to the central event pump when Phase 3 lands.
    SDL_PumpEvents();
    {
        SDL_Event scratch[16];
        int drained = SDL_PeepEvents(scratch, 16, SDL_GETEVENT, SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_REMOVED);
        if (drained > 0) {
            s_pads->scanGamepads();
            s_pads->map();
        }
    }

    // Push the motor state the last read command asked for. This runs here, once per
    // poll and right after the pump, because SDL needs the pump to service an effect
    // and because poll() sees the motor bytes one at a time -- doing it there would
    // hand SDL half-updated pairs.
    applyRumble(false);

    if (m_gamepad) {
        if (!SDL_GamepadConnected(m_gamepad)) {
            const SDL_JoystickID jid = SDL_GetGamepadID(m_gamepad);
            PCSX::g_system->printf("Gamepad error: joystick id %u disconnected, disabling pad\n",
                                   static_cast<unsigned>(jid));
            m_gamepad = nullptr;
        } else {
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++) {
                state.buttons[i] = SDL_GetGamepadButton(m_gamepad, static_cast<SDL_GamepadButton>(i));
            }
            // SDL reports axes as int16_t [-32768, 32767]. Triggers are signed but
            // only ever positive in practice. Normalize to [-1, 1] to match the
            // shape the rest of this code was built around.
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++) {
                int16_t raw = SDL_GetGamepadAxis(m_gamepad, static_cast<SDL_GamepadAxis>(i));
                state.axes[i] = raw < 0 ? raw / 32768.0f : raw / 32767.0f;
            }
            hasPad = true;
        }
    }

    if (!hasPad) {
        if (inputType == InputType::Auto) {
            pad.buttonStatus = getKeyboardButtons();
        } else {
            pad.buttonStatus = 0xffff;
        }

        pad.leftJoyX = pad.rightJoyX = pad.leftJoyY = pad.rightJoyY = 0x80;
        return;
    }

    bool buttons[16];
    for (unsigned i = 0; i < 16; i++) {
        buttons[i] = isControllerButtonPressed(i, state);
    }

    // For digital gamepads, make the PS1 dpad controllable with our gamepad's left analog stick
    if (m_activeType == PadType::Digital) {
        float x = state.axes[SDL_GAMEPAD_AXIS_LEFTX];
        float y = -state.axes[SDL_GAMEPAD_AXIS_LEFTY];
        float ds = x * x + y * y;
        if (ds >= THRESHOLD * THRESHOLD) {
            float d = std::sqrt(ds);
            x /= d;
            y /= d;
            float a = 0;
            if ((x * x) > (y * y)) {
                a = std::acos(x);
                if (y < 0) {
                    a = π(2.0f) - a;
                }
            } else {
                a = std::asin(y);
                if (x < 0) {
                    a = π() - a;
                } else if (y < 0) {
                    a = π(2.0f) + a;
                }
            }
            if ((a < π(2.5f / 8.0f)) || (a >= π(13.5f / 8.0f))) {
                // right
                buttons[5] = true;
            }
            if ((π(1.5f / 8.0f) <= a) && (a < π(6.5f / 8.0f))) {
                // up
                buttons[4] = true;
            }
            if ((π(5.5f / 8.0f) <= a) && (a < π(10.5f / 8.0f))) {
                // left
                buttons[7] = true;
            }
            if ((π(9.5f / 8.0f) <= a) && (a < π(14.5f / 8.0f))) {
                // down
                buttons[6] = true;
            }
        }
    } else if (m_activeType == PadType::Analog) {
        // Normalize an axis from (-1, 1) to (0, 255) with 128 = center
        const auto axisToUint8 = [](float axis) {
            constexpr float scale = 1.3f;
            const float scaledValue = std::clamp<float>(axis * scale, -1.0f, 1.0f);
            return (uint8_t)(std::clamp<float>(std::round(((scaledValue + 1.0f) / 2.0f) * 255.0f), 0.0f, 255.0f));
        };
        pad.leftJoyX = axisToUint8(state.axes[SDL_GAMEPAD_AXIS_LEFTX]);
        pad.leftJoyY = axisToUint8(state.axes[SDL_GAMEPAD_AXIS_LEFTY]);
        pad.rightJoyX = axisToUint8(state.axes[SDL_GAMEPAD_AXIS_RIGHTX]);
        pad.rightJoyY = axisToUint8(state.axes[SDL_GAMEPAD_AXIS_RIGHTY]);
    }

    uint16_t result = 0;
    for (unsigned i = 0; i < 16; i++) result |= buttons[i] << i;

    pad.buttonStatus = result ^ 0xffff;  // Controls are inverted, so 0 = pressed
}

bool PadsImpl::hasMultitap(Port port) {
    int index = magic_enum::enum_integer(port);
    auto& shmDisplay = PCSX::g_emulator->m_shmDisplay;
    return m_pads[index].m_settings.get<SettingMultitap>() || (shmDisplay && shmDisplay->hostMultitap(index));
}

bool PadsImpl::startPoll(Port port, uint8_t address, uint8_t& reply) {
    int index = magic_enum::enum_integer(port);
    auto& shmDisplay = PCSX::g_emulator->m_shmDisplay;
    Pad& pad = m_pads[index];
    Multitap& tap = m_taps[index];

    const int hostDevice = shmDisplay ? shmDisplay->hostDevice(index) : -1;
    pad.m_activeType = hostDevice >= 0 ? static_cast<PadType>(hostDevice) : pad.m_type;

    // A device the host picks is plugged in, whatever the port's Connected setting says.
    const bool connected = pad.isControllerConnected() || hostDevice >= 0;
    pad.m_hostConnected = hostDevice >= 0;

    tap.present = hasMultitap(port);
    if (!tap.present) {
        if (address != 1 || !connected) return false;
        pad.getButtons();
        pad.m_data.hostButtons = shmDisplay ? shmDisplay->hostPad(index) : 0xffff;
        reply = pad.startPoll();
        return true;
    }

    if (address < 1 || address > 4) return false;
    tap.connected[0] = connected;
    if (tap.connected[0]) {
        pad.getButtons();
        pad.m_data.hostButtons = shmDisplay ? shmDisplay->hostPad(index) : 0xffff;
    }
    for (unsigned slot = 1; slot < 4; slot++) {
        uint16_t buttons = 0xffff;
        tap.connected[slot] = shmDisplay && shmDisplay->hostSlotPad(index, slot, buttons);
        Pad& slotPad = tap.slots[slot - 1];
        slotPad.m_data.buttonStatus = 0xffff;
        slotPad.m_data.hostButtons = buttons;
    }
    tap.byte = 0;
    tap.command = 0;
    tap.slotAnswering = false;

    // "Below LONG response is activated by sending "01h" as third command byte; observe that sending that
    // byte does NOT affect the current response. Instead, it does request that the NEXT command shall
    // return special data". The request is made on slot A, so it is the next slot A access that is long.
    tap.longRead = address == 1 && tap.request;
    // Measured with one tap and a neGcon in slot A: a long read right after a long read in which the host
    // did not send 42h at the start of slot A's block is the "Garbage response", "the multitap returns ONLY
    // four bytes, like so: Hiz,80h,5Ah,LSB". It still takes REQ, and its own slot A byte counts the same
    // way, so it repeats; any other access ends it.
    tap.garbage = tap.longRead && tap.stale;
    tap.stale = false;
    if (tap.longRead) {
        // The tap answers the long read on its own, even when slot A is empty.
        reply = 0xff;
        return true;
    }

    // "receive data from pad number "n" (1..4)", and an empty slot does not answer: "if controller Slot A
    // is empty then the Slot A access aborts after the FIRST byte".
    tap.slot = address - 1;
    if (!tap.connected[tap.slot]) return false;
    reply = tapSlot(index, tap.slot).startPoll();
    return true;
}

uint8_t PadsImpl::poll(uint8_t value, Port port, uint32_t& padState) {
    int index = magic_enum::enum_integer(port);
    Multitap& tap = m_taps[index];
    if (!tap.present) return m_pads[index].poll(value, padState);
    if (tap.longRead) return pollLong(tap, index, value, padState);

    // A single slot: the tap passes the bytes through, and only looks at the third byte of a slot A read
    // for the long read request.
    const unsigned byte = tap.byte++;
    if (byte == 0) tap.command = value;
    if (byte == 1 && tap.slot == 0 && tap.command == magic_enum::enum_integer(PadCommands::Read)) {
        tap.request = value & 1;
    }
    return tapSlot(index, tap.slot).poll(value, padState);
}

// The long read, after the address byte:
//   "Halfword 0      --> Controller ID for MultiTap (5A80h=Multitap)
//    Halfword 1..4   --> Player A (Controller ID, Buttons, Analog Inputs, if any)
//    Halfword 5..8   --> Player B (Controller ID, Buttons, Analog Inputs, if any)
//    Halfword 9..12  --> Player C (Controller ID, Buttons, Analog Inputs, if any)
//    Halfword 13..16 --> Player D (Controller ID, Buttons, Analog Inputs, if any)"
//   "the Multitap is always sending 4 halfwords per slot (padded with FFFFh values for devices like Digital
//    Joypads and Mice; which do use less than 4 halfwords); for empty slots it's padding all 4 halfwords
//    with FFFFh."
// Each slot's pad is read as the host's bytes reach its block, with a 42h read command whatever the host
// sends in that position, and the host's following bytes passed on, so a pad's motor bytes still arrive.
// What the host sends at the start of slot A's block decides whether the next long read is garbage, see
// startPoll; a garbage one ends on slot A's id.
uint8_t PadsImpl::pollLong(Multitap& tap, int port, uint8_t value, uint32_t& padState) {
    const unsigned byte = tap.byte++;
    if (byte == 0) {
        tap.command = value;
        return 0x80;
    }
    if (byte == 1) {
        const bool read = tap.command == magic_enum::enum_integer(PadCommands::Read);
        if (read) tap.request = value & 1;
        // With no pad in any slot the tap only sends its id, and so does it for a command it does not know.
        if (!read || !(tap.connected[0] || tap.connected[1] || tap.connected[2] || tap.connected[3])) {
            padState = PAD_STATE_BAD_COMMAND;
        }
        return 0x5a;
    }
    const unsigned slot = (byte - 2) / 8;
    const unsigned position = (byte - 2) % 8;
    if (slot >= 4) return 0xff;
    Pad& pad = tapSlot(port, slot);
    if (position == 0) {
        if (slot == 0) {
            tap.stale = value != 0x42;
            // No ACK after this byte.
            if (tap.garbage) padState = PAD_STATE_BAD_COMMAND;
        }
        tap.slotAnswering = false;
        if (!tap.connected[slot]) return 0xff;
        pad.startPoll();
        uint32_t slotState = PAD_STATE_READ_DATA;
        const uint8_t id = pad.poll(magic_enum::enum_integer(PadCommands::Read), slotState);
        tap.slotAnswering = slotState != PAD_STATE_BAD_COMMAND;
        return tap.slotAnswering ? id : 0xff;
    }
    if (!tap.slotAnswering) return 0xff;
    uint32_t slotState = PAD_STATE_READ_DATA;
    return pad.poll(value, slotState);
}

uint8_t PadsImpl::Pad::poll(uint8_t value, uint32_t& padState) {
    if (m_currentByte == 0) {
        m_cmd = value;
        m_currentByte = 1;

        if (m_cmd == magic_enum::enum_integer(PadCommands::Read)) {
            return read();
        } else if (m_activeType == PadType::Analog) {
            return doDualshockCommand(padState);
        } else {
            PCSX::g_system->log(PCSX::LogClass::SIO0, _("Unknown command for pad: %02X\n"), value);
            m_cmd = magic_enum::enum_integer(PadCommands::Idle);
            m_bufferLen = 0;
            padState = PAD_STATE_BAD_COMMAND;  // Tell the SIO class we're in an invalid state
            return 0xff;
        }
    } else if (m_currentByte >= m_bufferLen) {
        return 0xff;
    } else if (m_currentByte >= 2 && m_currentByte <= 7 && m_cmd == magic_enum::enum_integer(PadCommands::Read) &&
               m_activeType == PadType::Analog) {
        // Motor bytes of the read command: the 4th..9th byte of the transfer. read()
        // has already filled the reply buffer, so these are pure input, and what they
        // mean is whatever command 4Dh last said they mean.
        const unsigned slot = m_currentByte - 2;
        if (m_configCommandsUsed) {
            switch (m_motorMapping[slot]) {
                case 0x00:
                    m_motorSmall = value & 1;
                    break;
                case 0x01:
                    m_motorLarge = value;
                    break;
            }
        } else if (slot == 0) {
            // Old one-motor method, still live until config commands are used: xx has
            // to be 40h..7Fh and yy has to have bit0 set. Latch xx, decide on yy.
            m_oldMethodLatch = value;
        } else if (slot == 1) {
            m_motorSmall = (((m_oldMethodLatch & 0xc0) == 0x40) && (value & 1)) ? 1 : 0;
        }
    } else if (m_currentByte >= 2 && m_currentByte <= 7 && m_configMode &&
               m_cmd == magic_enum::enum_integer(PadCommands::UnlockRumble)) {
        // 4Dh replies with the OLD mapping and installs the new one.
        // doDualshockCommand() already copied the old mapping into the reply buffer,
        // so overwriting it here cannot disturb what we send back.
        m_motorMapping[m_currentByte - 2] = value;
    } else if (m_currentByte == 2 && m_activeType == PadType::Analog) {
        switch (m_cmd) {
            case magic_enum::enum_integer(PadCommands::SetConfigMode):
                m_configMode = value == 1;
                if (m_configMode) m_configCommandsUsed = true;
                break;
            case magic_enum::enum_integer(PadCommands::SetAnalogMode):
                m_analogMode = value == 1;
                break;
            case magic_enum::enum_integer(PadCommands::Unknown46):
                if (value == 0) {
                    m_buf[4] = 0x01;
                    m_buf[5] = 0x02;
                    m_buf[6] = 0x00;
                    m_buf[7] = 0x0A;
                } else if (value == 1) {
                    m_buf[4] = 0x01;
                    m_buf[5] = 0x01;
                    m_buf[6] = 0x01;
                    m_buf[7] = 0x14;
                }
                break;
            case magic_enum::enum_integer(PadCommands::Unknown47):
                if (value != 0) {
                    m_buf[4] = 0;
                    m_buf[5] = 0;
                    m_buf[6] = 0;
                    m_buf[7] = 0;
                }
                break;
            case magic_enum::enum_integer(PadCommands::Unknown4C):
                if (value == 0) {
                    m_buf[5] = 0x04;
                } else if (value == 1) {
                    m_buf[5] = 0x07;
                }
                break;
        }
    }

    return m_buf[m_currentByte++];
}

uint8_t PadsImpl::Pad::doDualshockCommand(uint32_t& padState) {
    m_bufferLen = 8;

    if (m_cmd == magic_enum::enum_integer(PadCommands::SetConfigMode)) {
        if (m_configMode) {  // The config mode version of this command does not reply with pad data
            static constexpr uint8_t reply[] = {0x00, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            std::memcpy(m_buf, reply, 8);
            return 0xf3;
        } else {
            return read();
        }
    } else if (m_cmd == magic_enum::enum_integer(PadCommands::GetAnalogMode) && m_configMode) {
        static uint8_t reply[] = {0x00, 0x5a, 0x01, 0x02, 0x00, 0x02, 0x01, 0x00};

        reply[4] = m_analogMode ? 1 : 0;
        std::memcpy(m_buf, reply, 8);
        return 0xf3;
    } else if (m_cmd == magic_enum::enum_integer(PadCommands::UnlockRumble) && m_configMode) {
        // 4Dh returns the mapping that was in effect before this command; poll() swaps
        // in the new one byte by byte as the rest of the transfer arrives.
        m_buf[0] = 0x00;
        m_buf[1] = 0x5a;
        std::memcpy(m_buf + 2, m_motorMapping, 6);
        return 0xf3;
    } else if (m_cmd == magic_enum::enum_integer(PadCommands::SetAnalogMode) && m_configMode) {
        static uint8_t reply[] = {0x00, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

        std::memcpy(m_buf, reply, 8);
        return 0xf3;
    } else if (m_cmd == magic_enum::enum_integer(PadCommands::Unknown46) && m_configMode) {
        static uint8_t reply[] = {0x00, 0x5a, 0x00, 0x00, 0x01, 0x02, 0x00, 0x0a};

        std::memcpy(m_buf, reply, 8);
        return 0xf3;
    } else if (m_cmd == magic_enum::enum_integer(PadCommands::Unknown47) && m_configMode) {
        static uint8_t reply[] = {0x00, 0x5a, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00};

        std::memcpy(m_buf, reply, 8);
        return 0xf3;
    } else if (m_cmd == magic_enum::enum_integer(PadCommands::Unknown4C) && m_configMode) {
        static uint8_t reply[] = {0x00, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

        std::memcpy(m_buf, reply, 8);
        return 0xf3;
    } else {
        PCSX::g_system->log(PCSX::LogClass::SIO0, _("Unknown command for pad: %02X\n"), static_cast<uint8_t>(m_cmd));
        m_cmd = magic_enum::enum_integer(PadCommands::Idle);
        m_bufferLen = 0;
        padState = PAD_STATE_BAD_COMMAND;  // Tell the SIO class we're in an invalid state
        return 0xff;
    }
}

uint8_t PadsImpl::Pad::startPoll() {
    m_currentByte = 0;
    return 0xff;
}

uint8_t PadsImpl::Pad::read() {
    const PadData& pad = m_data;
    uint16_t buttonStatus = pad.buttonStatus & pad.overrides & pad.hostButtons;
    if (!m_settings.get<SettingConnected>() && !m_hostConnected) {
        m_bufferLen = 0;
        return 0xff;
    }

    switch (m_activeType) {
        case PadType::Mouse: {
            auto& shmDisplay = PCSX::g_emulator->m_shmDisplay;
            if (m_port >= 0 && shmDisplay && shmDisplay->hostInput()) {
                // The host's own buttons and motion counts, with no sensitivity scaling.
                int8_t deltaX, deltaY;
                uint32_t buttons;
                shmDisplay->hostMouse(m_port, deltaX, deltaY, buttons);
                const int leftClick = (buttons & 1) ? 0 : 1;
                const int rightClick = (buttons & 2) ? 0 : 1;
                m_mousepar[3] = 0xf0 | (leftClick << 3) | (rightClick << 2);
                m_mousepar[4] = deltaX;
                m_mousepar[5] = deltaY;
                memcpy(m_buf, m_mousepar, 6);
                m_bufferLen = 6;
                return 0x12;
            }
            const int leftClick = ImGui::IsMouseDown(ImGuiMouseButton_Left) ? 0 : 1;
            const int rightClick = ImGui::IsMouseDown(ImGuiMouseButton_Right) ? 0 : 1;
            const auto& io = ImGui::GetIO();
            const float scaleX = m_settings.get<SettingMouseSensitivityX>();
            const float scaleY = m_settings.get<SettingMouseSensitivityY>();

            const float deltaX = io.MouseDelta.x * scaleX;
            const float deltaY = io.MouseDelta.y * scaleY;

            // The top 4 bits are always set to 1, the low 2 bits seem to always be set to 0.
            // Left/right click are inverted in the response byte, ie 0 = pressed
            m_mousepar[3] = 0xf0 | (leftClick << 3) | (rightClick << 2);
            m_mousepar[4] = (int8_t)std::clamp<float>(deltaX, -128.f, 127.f);
            m_mousepar[5] = (int8_t)std::clamp<float>(deltaY, -128.f, 127.f);

            memcpy(m_buf, m_mousepar, 6);
            m_bufferLen = 6;
            return 0x12;
            break;
        }

        case PadType::Negcon:  // npc101/npc104(slph00001/slph00069)
            m_analogpar[0] = 0x23;
            m_analogpar[2] = buttonStatus & 0xff;
            m_analogpar[3] = buttonStatus >> 8;
            m_analogpar[4] = pad.rightJoyX;
            m_analogpar[5] = pad.rightJoyY;
            m_analogpar[6] = pad.leftJoyX;
            m_analogpar[7] = pad.leftJoyY;

            memcpy(m_buf, m_analogpar, 8);
            m_bufferLen = 8;
            return 0x23;
            break;
        case PadType::Analog:  // scph1110, scph1150
            if (m_analogMode || m_configMode) {
                m_analogpar[0] = 0x73;
                m_analogpar[2] = buttonStatus & 0xff;
                m_analogpar[3] = buttonStatus >> 8;
                m_analogpar[4] = pad.rightJoyX;
                m_analogpar[5] = pad.rightJoyY;
                m_analogpar[6] = pad.leftJoyX;
                m_analogpar[7] = pad.leftJoyY;

                memcpy(m_buf, m_analogpar, 8);
                m_bufferLen = 8;
                return m_configMode ? 0xf3 : 0x73;
            }
            [[fallthrough]];
        case PadType::Digital:
        default:
            m_stdpar[2] = buttonStatus & 0xff;
            m_stdpar[3] = buttonStatus >> 8;

            memcpy(m_buf, m_stdpar, 4);
            m_bufferLen = 4;
            return 0x41;
            break;
    }
}

bool PadsImpl::configure(PCSX::GUI* gui) {
    // Check for analog mode toggle key
    for (auto& pad : m_pads) {
        if (pad.m_type != PadType::Analog) continue;
        // Configs from before the SDL3 port hold GLFW key codes, which often have no ImGuiKey.
        const ImGuiKey key = SdlScancodeToImGuiKey(pad.m_settings.get<Keyboard_AnalogMode>());
        if ((key != ImGuiKey_None) && ImGui::IsKeyReleased(key)) {
            pad.m_analogMode = !pad.m_analogMode;
        }
    }

    if (!m_showCfg) {
        return false;
    }

    ImGui::SetNextWindowPos(ImVec2(70, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(_("Pad configuration"), &m_showCfg)) {
        ImGui::End();
        return false;
    }

    static std::function<const char*()> const c_padNames[] = {
        l_("Pad 1"),
        l_("Pad 2"),
    };

    if (ImGui::Button(_("Rescan gamepads and re-read game controllers database"))) {
        // Only the host side restarts. The emulated pads keep their protocol state,
        // so a game that set up its motors with 4Dh at boot keeps rumbling.
        for (auto& g : m_gamepads) {
            if (g) {
                SDL_RumbleGamepad(g, 0, 0, 0);
                SDL_CloseGamepad(g);
                g = nullptr;
            }
        }
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            PCSX::g_system->log(PCSX::LogClass::UI, "SDL_InitSubSystem(SDL_INIT_GAMEPAD) failed: %s\n", SDL_GetError());
        }
        scanGamepads();
        map();
    }

    bool changed = false;
    changed |= ImGui::Checkbox(_("Use raw input for mouse"), &gui->isRawMouseMotionEnabled());
    PCSX::ImGuiHelpers::ShowHelpMarker(
        _("When enabled, the cursor will be hidden and captured when the emulator is running. This is useful for games "
          "that require mouse input."));
    changed |= ImGui::Checkbox(_("Allow mouse capture toggle"), &gui->allowMouseCaptureToggle());
    PCSX::ImGuiHelpers::ShowHelpMarker(
        _("When enabled, pressing CTRL and ALT will toggle the setting above, raw input"));

    if (ImGui::BeginCombo(_("Pad"), c_padNames[m_selectedPadForConfig]())) {
        for (unsigned i = 0; i < 2; i++) {
            if (ImGui::Selectable(c_padNames[i](), m_selectedPadForConfig == i)) {
                m_selectedPadForConfig = i;
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::Button(_("Set defaults"))) {
        changed = true;
        m_pads[m_selectedPadForConfig].setDefaults(m_selectedPadForConfig == 0);
    }

    changed |= m_pads[m_selectedPadForConfig].configure();
    ImGui::End();
    return changed;
}

// SDL_GetScancodeName covers ASCII letters/digits but not the common navigation
// keys; we provide friendlier localized labels for those, and fall back to
// SDL_GetScancodeName for the rest.
static std::string sdlScancodeToString(int scancode) {
    switch (scancode) {
        case SDL_SCANCODE_UP:
            return _("Keyboard Up");
        case SDL_SCANCODE_RIGHT:
            return _("Keyboard Right");
        case SDL_SCANCODE_DOWN:
            return _("Keyboard Down");
        case SDL_SCANCODE_LEFT:
            return _("Keyboard Left");
        case SDL_SCANCODE_BACKSPACE:
            return _("Keyboard Backspace");
        case SDL_SCANCODE_RETURN:
            return _("Keyboard Enter");
        case SDL_SCANCODE_SPACE:
            return _("Keyboard Space");
        case SDL_SCANCODE_ESCAPE:
            return _("Keyboard Escape");
        case SDL_SCANCODE_UNKNOWN:
            return _("Unbound");
    };

    const char* keyName = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    if (!keyName || keyName[0] == '\0') {
        return fmt::format(f_("Unknown keyboard key {}"), scancode);
    }

    auto str = std::string(keyName);
    str[0] = toupper(str[0]);
    return fmt::format(f_("Keyboard {}"), str);
}

void PadsImpl::Pad::keyboardEvent(const PCSX::Events::Keyboard& event) {
    if (m_buttonToWait == -1) {
        return;
    }
    // Bindings are stored as SDL_Scancode values to match the runtime lookup
    // path (SdlScancodeToImGuiKey) and the SDL_SCANCODE_* setting defaults.
    // event.key is an SDL_Keycode (layout-aware codepoint); event.scancode is
    // the layout-independent physical-key id we actually want.
    getButtonFromGUIIndex(m_buttonToWait) = event.scancode;
    m_buttonToWait = -1;
    m_changed = true;
    map();
}

bool PadsImpl::Pad::configure() {
    static std::function<const char*()> const c_inputDevices[] = {
        l_("Auto"),
        l_("Controller"),
        l_("Keyboard"),
    };
    static std::function<const char*()> const c_buttonNames[] = {
        []() { return "╳"; },
        []() { return "□"; },
        []() { return "△"; },
        []() { return "◯"; },
        lC_("Controller button", "Select"),
        lC_("Controller button", "Start"),
        []() { return "L1"; },
        []() { return "R1"; },
        []() { return "L2"; },
        []() { return "R2"; },
        []() { return "L3"; },
        []() { return "R3"; },
        l_("Analog Mode"),
    };
    static std::function<const char*()> const c_dpadDirections[] = {
        []() { return "↑"; },
        []() { return "→"; },
        []() { return "↓"; },
        []() { return "←"; },
    };
    static std::function<const char*()> const c_controllerTypes[] = {
        l_("Digital"),
        l_("Analog"),
        l_("Mouse"),
        l_("Negcon (Unimplemented)"),
        l_("Gun (Unimplemented)"),
        l_("Guncon (Unimplemented)"),
    };

    bool changed = false;
    if (ImGui::Checkbox(_("Connected"), &m_settings.get<SettingConnected>().value)) {
        changed = true;
        reset();  // Reset pad state when unplugging/replugging pad
    }

    if (m_type != PadType::Analog) {
        ImGui::BeginDisabled();
    }

    ImGui::Checkbox(_("Analog mode"), &m_analogMode);

    if (m_type != PadType::Analog) {
        ImGui::EndDisabled();
    }

    {
        const bool capable = gamepadHasRumble(m_gamepad);
        if (!capable) ImGui::BeginDisabled();
        if (ImGui::Checkbox(_("Rumble"), &m_settings.get<SettingRumble>().value)) changed = true;
        if (!capable) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextUnformatted(_("(no motors on this gamepad)"));
        }
    }

    {
        const char* currentType = c_controllerTypes[static_cast<int>(m_type)]();
        if (ImGui::BeginCombo(_("Controller Type"), currentType)) {
            for (int i = 0; i < 3; i++) {
                if (ImGui::Selectable(c_controllerTypes[i]())) {
                    changed = true;
                    m_type = static_cast<PadType>(i);
                    m_settings.get<SettingDeviceType>().value = m_type;
                    reset();  // Reset pad state when changing pad type
                    map();
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Checkbox(_("Multitap"), &m_settings.get<SettingMultitap>().value)) changed = true;
        PCSX::ImGuiHelpers::ShowHelpMarker(
            _("Plugs a multitap into this port, with this pad in its slot A. The other three slots only take "
              "input from a host application, through the shared display memory."));
    }

    auto& inputDevice = m_settings.get<SettingInputType>().value;

    if (ImGui::BeginCombo(_("Device type"), c_inputDevices[magic_enum::enum_integer<InputType>(inputDevice)]())) {
        for (auto i : magic_enum::enum_values<InputType>()) {
            if (ImGui::Selectable(c_inputDevices[magic_enum::enum_integer<InputType>(i)](), i == inputDevice)) {
                changed = true;
                inputDevice = i;
            }
        }

        ImGui::EndCombo();
    }
    changed |=
        ImGui::SliderFloat(_("Mouse sensitivity X"), &m_settings.get<SettingMouseSensitivityX>().value, 0.f, 10.f);
    changed |=
        ImGui::SliderFloat(_("Mouse sensitivity Y"), &m_settings.get<SettingMouseSensitivityY>().value, 0.f, 10.f);

    ImGui::TextUnformatted(_("Keyboard mapping"));
    if (ImGui::BeginTable("Mapping", 2, ImGuiTableFlags_SizingFixedSame | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(_("Computer button mapping"));
        ImGui::TableSetupColumn(_("Gamepad button"));
        ImGui::TableHeadersRow();
        for (auto i = 0; i < 13; i++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(c_buttonNames[i]());
            ImGui::TableSetColumnIndex(0);
            bool hasToPop = false;
            if (m_buttonToWait == i) {
                const ImVec4 highlight = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
                ImGui::PushStyleColor(ImGuiCol_Button, highlight);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, highlight);
                hasToPop = true;
            }

            // The name of the mapped key
            const auto keyName = fmt::format("{}##{}", sdlScancodeToString(getButtonFromGUIIndex(i)), i);
            if (ImGui::Button(keyName.c_str(), ImVec2{-1, 0})) {
                m_buttonToWait = i;
            }
            if (hasToPop) {
                ImGui::PopStyleColor(2);
            }
        }
        for (auto i = 0; i < 4; i++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(c_dpadDirections[i]());
            ImGui::TableSetColumnIndex(0);
            bool hasToPop = false;
            const auto absI = i + 13;
            if (m_buttonToWait == absI) {
                const ImVec4 highlight = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
                ImGui::PushStyleColor(ImGuiCol_Button, highlight);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, highlight);
                hasToPop = true;
            }

            // The name of the mapped key
            const auto keyName = fmt::format("{}##{}", sdlScancodeToString(getButtonFromGUIIndex(absI)), absI);
            if (ImGui::Button(keyName.c_str(), ImVec2{-1, 0})) {
                m_buttonToWait = absI;
            }
            if (hasToPop) {
                ImGui::PopStyleColor(2);
            }
        }
        ImGui::EndTable();
    }

    const char* preview = _("No gamepad selected or connected");
    auto& id = m_settings.get<SettingControllerID>().value;
    SDL_Gamepad* selected =
        (id >= 0 && static_cast<unsigned>(id) < sizeof(s_pads->m_gamepads) / sizeof(s_pads->m_gamepads[0]))
            ? s_pads->m_gamepads[id]
            : nullptr;

    // Slot index -> displayable name. Empty slots are skipped during render but
    // we keep the slot index so the user's saved SettingControllerID continues
    // to refer to the same physical position.
    struct Entry {
        int slot;
        const char* name;
    };
    std::vector<Entry> gamepads;
    for (unsigned i = 0; i < sizeof(s_pads->m_gamepads) / sizeof(s_pads->m_gamepads[0]); i++) {
        SDL_Gamepad* g = s_pads->m_gamepads[i];
        if (!g) continue;
        const char* name = SDL_GetGamepadName(g);
        if (!name) name = "<unnamed gamepad>";
        gamepads.push_back({static_cast<int>(i), name});
        if (g == selected) preview = name;
    }

    if (ImGui::BeginCombo(_("Gamepad"), preview)) {
        for (const auto& entry : gamepads) {
            const auto gamepadName = fmt::format("{}##{}", entry.name, entry.slot);
            if (ImGui::Selectable(gamepadName.c_str())) {
                changed = true;
                id = entry.slot;
                map();
            }
        }

        ImGui::EndCombo();
    }

    if (m_changed) {
        changed = true;
        m_changed = false;
    }

    return changed;
}

int& PadsImpl::Pad::getButtonFromGUIIndex(int buttonIndex) {
    switch (buttonIndex) {
        case 0:
            return m_settings.get<Keyboard_PadCross>().value;
        case 1:
            return m_settings.get<Keyboard_PadSquare>().value;
        case 2:
            return m_settings.get<Keyboard_PadTriangle>().value;
        case 3:
            return m_settings.get<Keyboard_PadCircle>().value;
        case 4:
            return m_settings.get<Keyboard_PadSelect>().value;
        case 5:
            return m_settings.get<Keyboard_PadStart>().value;
        case 6:
            return m_settings.get<Keyboard_PadL1>().value;
        case 7:
            return m_settings.get<Keyboard_PadR1>().value;
        case 8:
            return m_settings.get<Keyboard_PadL2>().value;
        case 9:
            return m_settings.get<Keyboard_PadR2>().value;
        case 10:
            return m_settings.get<Keyboard_PadL3>().value;
        case 11:
            return m_settings.get<Keyboard_PadR3>().value;
        case 12:
            return m_settings.get<Keyboard_AnalogMode>().value;
        case 13:
            return m_settings.get<Keyboard_PadUp>().value;
        case 14:
            return m_settings.get<Keyboard_PadRight>().value;
        case 15:
            return m_settings.get<Keyboard_PadDown>().value;
        case 16:
            return m_settings.get<Keyboard_PadLeft>().value;
        default:
            abort();
            break;
    }
}

json PadsImpl::getCfg() {
    json ret;
    ret[0] = m_pads[0].getCfg();
    ret[1] = m_pads[1].getCfg();
    return ret;
}

json PadsImpl::Pad::getCfg() { return m_settings.serialize(); }

void PadsImpl::setCfg(const json& j) {
    if (j.count("pads") && j["pads"].is_array()) {
        auto padsCfg = j["pads"];
        if (padsCfg.size() >= 1) {
            m_pads[0].setCfg(padsCfg[0]);
        } else {
            m_pads[0].setDefaults(true);
        }
        if (padsCfg.size() >= 2) {
            m_pads[1].setCfg(padsCfg[1]);
        } else {
            m_pads[1].setDefaults(false);
        }
    } else {
        setDefaults();
    }
}

void PadsImpl::Pad::setCfg(const json& j) {
    json cfg = j;
    PCSX::Pads::migrateKeyboardBindings(cfg);
    m_settings.deserialize(cfg);
    map();
}

void PadsImpl::setDefaults() {
    m_pads[0].setDefaults(true);
    m_pads[1].setDefaults(false);
}

void PadsImpl::Pad::setDefaults(bool firstController) {
    m_settings.reset();
    if (firstController) {
        m_settings.get<SettingConnected>() = true;
    }
    map();
}

void PadsImpl::setLua(PCSX::Lua L) {
    L.getfieldtable("PCSX", LUA_GLOBALSINDEX);

    // setting constants
    L.getfieldtable("CONSTS");
    L.getfieldtable("PAD");
    L.getfieldtable("BUTTON");

    L.push(lua_Number(0));
    L.setfield("SELECT");
    L.push(lua_Number(3));
    L.setfield("START");
    L.push(lua_Number(4));
    L.setfield("UP");
    L.push(lua_Number(5));
    L.setfield("RIGHT");
    L.push(lua_Number(6));
    L.setfield("DOWN");
    L.push(lua_Number(7));
    L.setfield("LEFT");
    L.push(lua_Number(8));
    L.setfield("L2");
    L.push(lua_Number(9));
    L.setfield("R2");
    L.push(lua_Number(10));
    L.setfield("L1");
    L.push(lua_Number(11));
    L.setfield("R1");
    L.push(lua_Number(12));
    L.setfield("TRIANGLE");
    L.push(lua_Number(13));
    L.setfield("CIRCLE");
    L.push(lua_Number(14));
    L.setfield("CROSS");
    L.push(lua_Number(15));
    L.setfield("SQUARE");

    L.pop();
    L.pop();
    L.pop();

    // pushing settings

    L.getfieldtable("settings");
    L.getfieldtable("pads");
    auto pushSettings = [this, L](unsigned pad) mutable {
        L.push(lua_Number(pad + 1));
        m_pads[pad].m_settings.pushValue(L);
        L.settable();
    };
    pushSettings(0);
    pushSettings(1);
    L.pop();
    L.pop();

    L.getfieldtable("SIO0");
    L.getfieldtable("slots");

    // pads callbacks

    auto setCallbacks = [this, L](unsigned pad) mutable {
        L.getfieldtable(pad + 1);
        L.getfieldtable("pads");
        L.getfieldtable(1);

        L.declareFunc(
            "getButton",
            [this, pad](PCSX::Lua L) -> int {
                int n = L.gettop();
                if (n == 0) {
                    return L.error("Not enough arguments to getButton");
                }
                if (!L.isnumber(1)) {
                    return L.error("Invalid argument to getButton");
                }
                auto buttons = m_pads[pad].m_data.buttonStatus;
                auto overrides = m_pads[pad].m_data.overrides;
                auto hostButtons = m_pads[pad].m_data.hostButtons;
                unsigned button = L.checknumber(1);
                L.push(((overrides & buttons & hostButtons) & (1 << button)) == 0);
                return 1;
            },
            -1);
        L.declareFunc(
            "setOverride",
            [this, pad](PCSX::Lua L) -> int {
                int n = L.gettop();
                if (n == 0) {
                    return L.error("Not enough arguments to setOverride");
                }
                if (!L.isnumber(1)) {
                    return L.error("Invalid argument to setOverride");
                }
                auto& overrides = m_pads[pad].m_data.overrides;
                unsigned button = L.checknumber(1);
                button = 1 << button;
                overrides &= ~button;
                return 0;
            },
            -1);
        L.declareFunc(
            "clearOverride",
            [this, pad](PCSX::Lua L) -> int {
                int n = L.gettop();
                if (n == 0) {
                    return L.error("Not enough arguments to clearOverride");
                }
                if (!L.isnumber(1)) {
                    return L.error("Invalid argument to clearOverride");
                }
                auto& overrides = m_pads[pad].m_data.overrides;
                unsigned button = L.checknumber(1);
                button = 1 << button;
                overrides |= button;
                return 0;
            },
            -1);
        L.declareFunc(
            "setAnalogMode",
            [this, pad](PCSX::Lua L) -> int {
                int n = L.gettop();
                if (n == 0) {
                    m_pads[pad].m_analogMode = false;
                } else {
                    m_pads[pad].m_analogMode = L.toboolean();
                }
                return 0;
            },
            -1);
        L.declareFunc(
            "map",
            [this, pad](PCSX::Lua L) -> int {
                m_pads[pad].map();
                return 0;
            },
            -1);

        L.pop();
        L.pop();
        L.pop();
    };

    setCallbacks(0);
    setCallbacks(1);

    L.pop();
    L.pop();
    L.pop();

    assert(L.gettop() == 0);
}

PCSX::Pads* PCSX::Pads::factory() { return s_pads = new PadsImpl(); }
