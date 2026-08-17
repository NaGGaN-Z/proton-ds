#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
int main() {
    GUID hidGuid; HidD_GetHidGuid(&hidGuid);
    HDEVINFO ds = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA d = {.cbSize = sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(ds, NULL, &hidGuid, i, &d); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(ds, &d, NULL, 0, &need, NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W dd = malloc(need);
        dd->cbSize = 8;
        if (!SetupDiGetDeviceInterfaceDetailW(ds, &d, dd, need, &need, NULL)) { free(dd); continue; }
        HANDLE h = CreateFileW(dd->DevicePath, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        printf("[%lu] open=%d path=", i, h != INVALID_HANDLE_VALUE);
        for (WCHAR *p = dd->DevicePath; *p && p < dd->DevicePath + 400; p++)
            printf((*p > 126 || *p < 32) ? "<%04x>" : "%lc", *p);
        printf("\n");
        if (h != INVALID_HANDLE_VALUE) {
            HIDD_ATTRIBUTES a = {.Size = sizeof(a)};
            if (HidD_GetAttributes(h, &a)) {
                printf("    VID=%04X PID=%04X VER=%04X\n", a.VendorID, a.ProductID, a.VersionNumber);
                PHIDP_PREPARSED_DATA pd; HIDP_CAPS caps;
                if (HidD_GetPreparsedData(h, &pd)) {
                    if (HidP_GetCaps(pd, &caps) == 0x110000)
                        printf("    InLen=%d OutLen=%d FeatLen=%d Usage=%04X:%04X\n",
                               caps.InputReportByteLength, caps.OutputReportByteLength, caps.FeatureReportByteLength, caps.UsagePage, caps.Usage);
                    HidD_FreePreparsedData(pd);
                }
            }
            CloseHandle(h);
        }
        free(dd);
    }
    SetupDiDestroyDeviceInfoList(ds);
    return 0;
}
#include <setupapi.h>
#include <dinput.h>
