/*
 * The Wine project - Xinput Joystick Library
 * Copyright 2008 Andrew Fenn
 * Copyright 2018 Aric Stewart
 * Copyright 2021 Rémi Bernon for CodeWeavers
 * Copyright 2024 BrunoSX 
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winuser.h"
#include "winreg.h"
#include "wingdi.h"
#include "winnls.h"
#include "winternl.h"
#include "winsock2.h"

#include "dbt.h"
#include "setupapi.h"
#include "initguid.h"
#include "devguid.h"
#include "xinput.h"

#include "wine/debug.h"

/* Not defined in the headers, used only by XInputGetStateEx */
#define XINPUT_GAMEPAD_GUIDE 0x0400

#define SERVER_PORT 7949
#define CLIENT_PORT 7947
#define BUFFER_SIZE 64

#define REQUEST_CODE_GET_GAMEPAD 8
#define REQUEST_CODE_GET_GAMEPAD_STATE 9
#define REQUEST_CODE_RELEASE_GAMEPAD 10

#define IDX_BUTTON_A 0
#define IDX_BUTTON_B 1
#define IDX_BUTTON_X 2
#define IDX_BUTTON_Y 3
#define IDX_BUTTON_L1 4
#define IDX_BUTTON_R1 5
#define IDX_BUTTON_L2 10
#define IDX_BUTTON_R2 11
#define IDX_BUTTON_SELECT 6
#define IDX_BUTTON_START 7
#define IDX_BUTTON_L3 8
#define IDX_BUTTON_R3 9

WINE_DEFAULT_DEBUG_CHANNEL(xinput);

struct xinput_controller
{
    XINPUT_CAPABILITIES caps;
    XINPUT_STATE state;
    XINPUT_GAMEPAD last_keystroke;
    BOOL enabled;
    BOOL connected;
    int id;
};

static struct xinput_controller controller = 
{
    .enabled = FALSE,
    .connected = FALSE,
    .id = 0
};

static HMODULE xinput_instance;
static HANDLE start_event;
static HANDLE stop_event;
static HANDLE done_event;
static HANDLE update_event;

static SOCKET server_sock = INVALID_SOCKET;
static BOOL winsock_loaded = FALSE;
static char xinput_min_index = 3;

static void close_server_socket(void) 
{
    if (server_sock != INVALID_SOCKET) 
    {
        closesocket(server_sock);
        server_sock = INVALID_SOCKET;
    }
    
    if (winsock_loaded) 
    {
        WSACleanup();
        winsock_loaded = FALSE;
    }
}

static BOOL create_server_socket(void)
{    
    WSADATA wsa_data;
    struct sockaddr_in server_addr;
    int res;
    
    close_server_socket();
    
    winsock_loaded = WSAStartup(MAKEWORD(2,2), &wsa_data) == NO_ERROR;
    if (!winsock_loaded) return FALSE;
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(SERVER_PORT);
    
    server_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server_sock == INVALID_SOCKET) return FALSE;

    res = bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (res == SOCKET_ERROR) return FALSE;
    
    return TRUE;
}

static int get_gamepad_request(void)
{
    int res, client_addr_len, gamepad_id;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    client_addr.sin_port = htons(CLIENT_PORT);
    client_addr_len = sizeof(client_addr);
    
    buffer[0] = REQUEST_CODE_GET_GAMEPAD;
    buffer[1] = 1;
    res = sendto(server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, client_addr_len);
    if (res == SOCKET_ERROR) return 0;
    
    res = recvfrom(server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, &client_addr_len);
    if (res == SOCKET_ERROR || buffer[0] != REQUEST_CODE_GET_GAMEPAD) return 0;
    
    memcpy(&gamepad_id, buffer + 1, 4);
    return gamepad_id;
}

static void release_gamepad_request(void)
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    int client_addr_len;
    
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    client_addr.sin_port = htons(CLIENT_PORT);
    client_addr_len = sizeof(client_addr);
    
    buffer[0] = REQUEST_CODE_RELEASE_GAMEPAD;
    sendto(server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, client_addr_len);
}

static BOOL controller_check_caps(void)
{
    XINPUT_CAPABILITIES *caps = &controller.caps;
    memset(caps, 0, sizeof(XINPUT_CAPABILITIES));
    
    caps->Gamepad.wButtons = 0xffff;
    caps->Gamepad.bLeftTrigger = (1u << (sizeof(caps->Gamepad.bLeftTrigger) + 1)) - 1;
    caps->Gamepad.bRightTrigger = (1u << (sizeof(caps->Gamepad.bRightTrigger) + 1)) - 1;
    caps->Gamepad.sThumbLX = (1u << (sizeof(caps->Gamepad.sThumbLX) + 1)) - 1;
    caps->Gamepad.sThumbLY = (1u << (sizeof(caps->Gamepad.sThumbLY) + 1)) - 1;
    caps->Gamepad.sThumbRX = (1u << (sizeof(caps->Gamepad.sThumbRX) + 1)) - 1;
    caps->Gamepad.sThumbRY = (1u << (sizeof(caps->Gamepad.sThumbRY) + 1)) - 1;

    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    return TRUE;
}

static void controller_disable(void)
{
    if (!controller.enabled) return;
    controller.enabled = FALSE;
    SetEvent(update_event);
}

static void controller_destroy(void)
{
    release_gamepad_request();
    xinput_min_index = 3;
    
    if (controller.connected)
    {
        controller_disable();
        controller.connected = FALSE;
    }
    
    close_server_socket();
}

static void controller_enable(void)
{
    if (controller.enabled) return;
    controller.enabled = TRUE;
    SetEvent(update_event);
}

static void controller_init(void)
{
    memset(&controller.state, 0, sizeof(controller.state));
    controller.enabled = FALSE;
    controller.connected = TRUE;
    controller_check_caps();
    controller_enable();
}

static void controller_check_connection(void)
{
    controller.id = 0;
    if (server_sock == INVALID_SOCKET) create_server_socket();    
    
    int gamepad_id = get_gamepad_request();
    if (gamepad_id > 0) 
    {
        controller.id = gamepad_id;
        if (!controller.connected) 
        {
            controller.id = gamepad_id;
            controller_init();
        }
    }
}

static void stop_update_thread(void)
{
    ResetEvent(update_event);
    SetEvent(stop_event);
    WaitForSingleObject(done_event, INFINITE);

    CloseHandle(start_event);
    CloseHandle(stop_event);
    CloseHandle(done_event);
    CloseHandle(update_event);
    
    controller_destroy();
}

static void read_controller_state(void)
{
    char buffer[BUFFER_SIZE];
    int i, res, client_addr_len, gamepad_id;
    char dpad;
    short buttons, thumb_lx, thumb_ly, thumb_rx, thumb_ry;
    struct sockaddr_in client_addr;
    XINPUT_STATE state;
    
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    client_addr.sin_port = htons(CLIENT_PORT);
    client_addr_len = sizeof(client_addr);

    buffer[0] = REQUEST_CODE_GET_GAMEPAD_STATE;
    memcpy(buffer + 1, &controller.id, 4);
    
    res = sendto(server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, client_addr_len);
    if (res == SOCKET_ERROR) return;
    
    res = recvfrom(server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, &client_addr_len);
    if (res == SOCKET_ERROR || buffer[0] != REQUEST_CODE_GET_GAMEPAD_STATE || buffer[1] != 1) return;
    
    memcpy(&gamepad_id, buffer + 2, 4);
    if (gamepad_id != controller.id) return;
    
    memcpy(&buttons, buffer + 6, 2);
    
    dpad = buffer[8];
    
    memcpy(&thumb_lx, buffer + 9, 2);
    memcpy(&thumb_ly, buffer + 11, 2);
    memcpy(&thumb_rx, buffer + 13, 2);
    memcpy(&thumb_ry, buffer + 15, 2);    

    state.Gamepad.wButtons = 0;
    for (i = 0; i < 10; i++)
    {    
        if ((buttons & (1<<i))) {
            switch (i)
            {
            case IDX_BUTTON_A: state.Gamepad.wButtons |= XINPUT_GAMEPAD_A; break;
            case IDX_BUTTON_B: state.Gamepad.wButtons |= XINPUT_GAMEPAD_B; break;
            case IDX_BUTTON_X: state.Gamepad.wButtons |= XINPUT_GAMEPAD_X; break;
            case IDX_BUTTON_Y: state.Gamepad.wButtons |= XINPUT_GAMEPAD_Y; break;
            case IDX_BUTTON_L1: state.Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; break;
            case IDX_BUTTON_R1: state.Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
            case IDX_BUTTON_SELECT: state.Gamepad.wButtons |= XINPUT_GAMEPAD_BACK; break;
            case IDX_BUTTON_START: state.Gamepad.wButtons |= XINPUT_GAMEPAD_START; break;
            case IDX_BUTTON_L3: state.Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB; break;
            case IDX_BUTTON_R3: state.Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB; break;
            }
        }
    }
    
    state.Gamepad.bLeftTrigger = (buttons & (1<<10)) ? 255 : 0;
    state.Gamepad.bRightTrigger = (buttons & (1<<11)) ? 255 : 0;

    switch (dpad)
    {
    case 0: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP; break;
    case 1: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 2: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 3: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN; break;
    case 4: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
    case 5: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT; break;
    case 6: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
    case 7: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP; break;
    }

    state.Gamepad.sThumbLX = thumb_lx;
    state.Gamepad.sThumbLY = -thumb_ly;
    state.Gamepad.sThumbRX = thumb_rx;
    state.Gamepad.sThumbRY = -thumb_ry;
    state.dwPacketNumber = controller.state.dwPacketNumber + 1;
    
    controller.state = state;
}

static DWORD WINAPI controller_update_thread_proc(void *param)
{
    DWORD last_time, curr_time;
    const int num_events = 2;
    HANDLE events[num_events];
    DWORD ret = WAIT_TIMEOUT;

    SetThreadDescription(GetCurrentThread(), L"wine_xinput_controller_update");
    controller_check_connection();
    SetEvent(start_event);

    last_time = GetCurrentTime();
    do
    {
        curr_time = GetCurrentTime();
        if (ret == WAIT_TIMEOUT || (curr_time - last_time) >= 2000) {
            controller_check_connection();
            last_time = curr_time;
        }
        if (controller.connected && controller.enabled) 
        {
            read_controller_state();
            Sleep(16);
        }
        
        events[0] = update_event;
        events[1] = stop_event;    
    }
    while ((ret = MsgWaitForMultipleObjectsEx(num_events, events, 2000, QS_ALLINPUT, MWMO_ALERTABLE)) < num_events - 1 ||
            ret == num_events || ret == WAIT_TIMEOUT);

    if (ret != num_events - 1) ERR("update thread exited unexpectedly, ret %lu\n", ret);
    SetEvent(done_event);
    return ret;
}

static BOOL WINAPI start_update_thread_once(INIT_ONCE *once, void *param, void **context)
{
    HANDLE thread;

    start_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!start_event) ERR("failed to create start event, error %lu\n", GetLastError());

    stop_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!stop_event) ERR("failed to create stop event, error %lu\n", GetLastError());

    done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!done_event) ERR("failed to create done event, error %lu\n", GetLastError());

    update_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!update_event) ERR("failed to create update event, error %lu\n", GetLastError());    

    thread = CreateThread(NULL, 0, controller_update_thread_proc, NULL, 0, NULL);
    if (!thread) ERR("failed to create update thread, error %lu\n", GetLastError());
    CloseHandle(thread);

    WaitForSingleObject(start_event, INFINITE);
    return TRUE;
}

static void start_update_thread(void)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&init_once, start_update_thread_once, NULL, NULL);
}

static BOOL controller_is_connected(DWORD index) 
{
    return index == 0 && controller.connected;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    TRACE("inst %p, reason %lu, reserved %p.\n", inst, reason, reserved);

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        xinput_instance = inst;
        DisableThreadLibraryCalls(inst);
        break;
    case DLL_PROCESS_DETACH:
        if (reserved) break;
        stop_update_thread();
        break;
    }
    return TRUE;
}

void WINAPI DECLSPEC_HOTPATCH XInputEnable(BOOL enable)
{
    TRACE("enable %d.\n", enable);

    /* Setting to false will stop messages from XInputSetState being sent
    to the controllers. Setting to true will send the last vibration
    value (sent to XInputSetState) to the controller and allow messages to
    be sent */
    start_update_thread();

    if (!controller.connected) return;
    if (enable) controller_enable();
    else controller_disable();
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputSetState(DWORD index, XINPUT_VIBRATION *vibration)
{
    TRACE("index %lu, vibration %p.\n", index, vibration);

    start_update_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_SUCCESS;
}

/* Some versions of SteamOverlayRenderer hot-patch XInputGetStateEx() and call
 * XInputGetState() in the hook, so we need a wrapper. */
static DWORD xinput_get_state(DWORD index, XINPUT_STATE *state)
{
    if (!state) return ERROR_BAD_ARGUMENTS;

    start_update_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (index < xinput_min_index) xinput_min_index = index;
    if (index == xinput_min_index) index = 0;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;

    *state = controller.state;
    return ERROR_SUCCESS;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetState(DWORD index, XINPUT_STATE *state)
{
    DWORD ret;

    TRACE("index %lu, state %p.\n", index, state);

    ret = xinput_get_state(index, state);
    if (ret != ERROR_SUCCESS) return ret;

    /* The main difference between this and the Ex version is the media guide button */
    state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_GUIDE;

    return ERROR_SUCCESS;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetStateEx(DWORD index, XINPUT_STATE *state)
{
    TRACE("index %lu, state %p.\n", index, state);

    return xinput_get_state(index, state);
}

static const int JS_STATE_OFF = 0;
static const int JS_STATE_LOW = 1;
static const int JS_STATE_HIGH = 2;

static int joystick_state(const SHORT value)
{
    if (value > 20000) return JS_STATE_HIGH;
    if (value < -20000) return JS_STATE_LOW;
    return JS_STATE_OFF;
}

static WORD js_vk_offs(const int x, const int y)
{
    if (y == JS_STATE_OFF)
    {
      /*if (x == JS_STATE_OFF) shouldn't get here */
        if (x == JS_STATE_LOW) return 3; /* LEFT */
      /*if (x == JS_STATE_HIGH)*/ return 2; /* RIGHT */
    }
    if (y == JS_STATE_HIGH)
    {
        if (x == JS_STATE_OFF) return 0; /* UP */
        if (x == JS_STATE_LOW) return 4; /* UPLEFT */
      /*if (x == JS_STATE_HIGH)*/ return 5; /* UPRIGHT */
    }
  /*if (y == JS_STATE_LOW)*/
    {
        if (x == JS_STATE_OFF) return 1; /* DOWN */
        if (x == JS_STATE_LOW) return 7; /* DOWNLEFT */
      /*if (x == JS_STATE_HIGH)*/ return 6; /* DOWNRIGHT */
    }
}

static DWORD check_joystick_keystroke(XINPUT_KEYSTROKE *keystroke, const SHORT *cur_x, const SHORT *cur_y, 
                                      SHORT *last_x, SHORT *last_y, const WORD base_vk)
{
    int cur_vk = 0, cur_x_st, cur_y_st;
    int last_vk = 0, last_x_st, last_y_st;

    cur_x_st = joystick_state(*cur_x);
    cur_y_st = joystick_state(*cur_y);
    if (cur_x_st || cur_y_st)
        cur_vk = base_vk + js_vk_offs(cur_x_st, cur_y_st);

    last_x_st = joystick_state(*last_x);
    last_y_st = joystick_state(*last_y);
    if (last_x_st || last_y_st)
        last_vk = base_vk + js_vk_offs(last_x_st, last_y_st);

    if (cur_vk != last_vk)
    {
        if (last_vk)
        {
            /* joystick was set, and now different. send a KEYUP event, and set
             * last pos to centered, so the appropriate KEYDOWN event will be
             * sent on the next call. */
            keystroke->VirtualKey = last_vk;
            keystroke->Unicode = 0; /* unused */
            keystroke->Flags = XINPUT_KEYSTROKE_KEYUP;
            keystroke->UserIndex = 0;
            keystroke->HidCode = 0;

            *last_x = 0;
            *last_y = 0;

            return ERROR_SUCCESS;
        }

        /* joystick was unset, send KEYDOWN. */
        keystroke->VirtualKey = cur_vk;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = XINPUT_KEYSTROKE_KEYDOWN;
        keystroke->UserIndex = 0;
        keystroke->HidCode = 0;

        *last_x = *cur_x;
        *last_y = *cur_y;

        return ERROR_SUCCESS;
    }

    *last_x = *cur_x;
    *last_y = *cur_y;

    return ERROR_EMPTY;
}

static BOOL trigger_is_on(const BYTE value)
{
    return value > 30;
}

static DWORD check_for_keystroke(XINPUT_KEYSTROKE *keystroke)
{
    const XINPUT_GAMEPAD *cur;
    DWORD ret = ERROR_EMPTY;
    int i;

    static const struct
    {
        int mask;
        WORD vk;
    } buttons[] = {
        { XINPUT_GAMEPAD_DPAD_UP, VK_PAD_DPAD_UP },
        { XINPUT_GAMEPAD_DPAD_DOWN, VK_PAD_DPAD_DOWN },
        { XINPUT_GAMEPAD_DPAD_LEFT, VK_PAD_DPAD_LEFT },
        { XINPUT_GAMEPAD_DPAD_RIGHT, VK_PAD_DPAD_RIGHT },
        { XINPUT_GAMEPAD_START, VK_PAD_START },
        { XINPUT_GAMEPAD_BACK, VK_PAD_BACK },
        { XINPUT_GAMEPAD_LEFT_THUMB, VK_PAD_LTHUMB_PRESS },
        { XINPUT_GAMEPAD_RIGHT_THUMB, VK_PAD_RTHUMB_PRESS },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, VK_PAD_LSHOULDER },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_PAD_RSHOULDER },
        { XINPUT_GAMEPAD_A, VK_PAD_A },
        { XINPUT_GAMEPAD_B, VK_PAD_B },
        { XINPUT_GAMEPAD_X, VK_PAD_X },
        { XINPUT_GAMEPAD_Y, VK_PAD_Y },
        /* note: guide button does not send an event */
    };

    cur = &controller.state.Gamepad;

    /*** buttons ***/
    for (i = 0; i < ARRAY_SIZE(buttons); ++i)
    {
        if ((cur->wButtons & buttons[i].mask) ^ (controller.last_keystroke.wButtons & buttons[i].mask))
        {
            keystroke->VirtualKey = buttons[i].vk;
            keystroke->Unicode = 0; /* unused */
            if (cur->wButtons & buttons[i].mask)
            {
                keystroke->Flags = XINPUT_KEYSTROKE_KEYDOWN;
                controller.last_keystroke.wButtons |= buttons[i].mask;
            }
            else
            {
                keystroke->Flags = XINPUT_KEYSTROKE_KEYUP;
                controller.last_keystroke.wButtons &= ~buttons[i].mask;
            }
            keystroke->UserIndex = 0;
            keystroke->HidCode = 0;
            ret = ERROR_SUCCESS;
            goto done;
        }
    }

    /*** triggers ***/
    if (trigger_is_on(cur->bLeftTrigger) ^ trigger_is_on(controller.last_keystroke.bLeftTrigger))
    {
        keystroke->VirtualKey = VK_PAD_LTRIGGER;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = trigger_is_on(cur->bLeftTrigger) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
        keystroke->UserIndex = 0;
        keystroke->HidCode = 0;
        controller.last_keystroke.bLeftTrigger = cur->bLeftTrigger;
        ret = ERROR_SUCCESS;
        goto done;
    }

    if (trigger_is_on(cur->bRightTrigger) ^ trigger_is_on(controller.last_keystroke.bRightTrigger))
    {
        keystroke->VirtualKey = VK_PAD_RTRIGGER;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = trigger_is_on(cur->bRightTrigger) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
        keystroke->UserIndex = 0;
        keystroke->HidCode = 0;
        controller.last_keystroke.bRightTrigger = cur->bRightTrigger;
        ret = ERROR_SUCCESS;
        goto done;
    }

    /*** joysticks ***/
    ret = check_joystick_keystroke(keystroke, &cur->sThumbLX, &cur->sThumbLY,
            &controller.last_keystroke.sThumbLX,
            &controller.last_keystroke.sThumbLY, VK_PAD_LTHUMB_UP);
    if (ret == ERROR_SUCCESS)
        goto done;

    ret = check_joystick_keystroke(keystroke, &cur->sThumbRX, &cur->sThumbRY,
            &controller.last_keystroke.sThumbRX,
            &controller.last_keystroke.sThumbRY, VK_PAD_RTHUMB_UP);
    if (ret == ERROR_SUCCESS)
        goto done;

done:

    return ret;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetKeystroke(DWORD index, DWORD reserved, PXINPUT_KEYSTROKE keystroke)
{
    TRACE("index %lu, reserved %lu, keystroke %p.\n", index, reserved, keystroke);

    if (index >= XUSER_MAX_COUNT && index != XUSER_INDEX_ANY) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index != XUSER_INDEX_ANY ? index : 0)) return ERROR_DEVICE_NOT_CONNECTED;

    return check_for_keystroke(keystroke);
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *capabilities)
{
    TRACE("index %lu, flags %#lx, capabilities %p.\n", index, flags, capabilities);

    start_update_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;

    if (flags & XINPUT_FLAG_GAMEPAD && controller.caps.SubType != XINPUT_DEVSUBTYPE_GAMEPAD) return ERROR_DEVICE_NOT_CONNECTED;
    memcpy(capabilities, &controller.caps, sizeof(*capabilities));

    return ERROR_SUCCESS;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render_guid, GUID *capture_guid)
{
    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetBatteryInformation(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION* battery)
{
    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_NOT_SUPPORTED;
}
