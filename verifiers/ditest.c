#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
static BOOL CALLBACK cb(LPCDIDEVICEINSTANCEW d, void *ctx) {
    printf("DINPUT: name=%ls product=%ls type=%04X instance=%08X\n",
        d->tszInstanceName, d->tszProductName, d->dwDevType & 0xFFFF,
        d->guidInstance.Data1);
    return DIENUM_CONTINUE;
}
int main() {
    IDirectInput8W *di = NULL;
    HRESULT hr = DirectInput8Create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8W, (void**)&di, NULL);
    printf("DirectInput8Create hr=%08X\n", (unsigned)hr);
    if (SUCCEEDED(hr)) {
        hr = IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, cb, NULL, DIEDFL_ATTACHEDONLY);
        printf("EnumDevices hr=%08X\n", (unsigned)hr);
    }
    for (DWORD i = 0; i < 4; i++) {
        XINPUT_CAPABILITIES c;
        ZeroMemory(&c, sizeof(c));
        DWORD r = XInputGetCapabilities(i, 0, &c);
        printf("XInput[%lu] caps=%08X\n", i, r);
        if (r == ERROR_SUCCESS)
            printf("  type=%02X subtype=%02X buttons=%04X\n", c.Type, c.SubType, c.Gamepad.wButtons);
    }
    return 0;
}
