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
        if (h == INVALID_HANDLE_VALUE) { free(dd); continue; }
        HIDD_ATTRIBUTES a = {.Size = sizeof(a)};
        if (HidD_GetAttributes(h, &a) && a.VendorID == 0x054C) {
            printf("== 054C:%04X VER=%04X\n", a.ProductID, a.VersionNumber);
            PHIDP_PREPARSED_DATA pd; HIDP_CAPS caps;
            if (HidD_GetPreparsedData(h, &pd)) {
                NTSTATUS st = HidP_GetCaps(pd, &caps);
                printf("GetCaps st=%08X InLen=%d FeatLen=%d\n", (unsigned)st, caps.InputReportByteLength, caps.FeatureReportByteLength);
                HidD_FreePreparsedData(pd);
                BYTE f12[16] = {0}; f12[0] = 0x12;
                if (HidD_GetFeature(h, f12, sizeof(f12)))
                    printf("GetFeature(0x12) OK: %02X %02X %02X %02X %02X %02X %02X\n", f12[0],f12[1],f12[2],f12[3],f12[4],f12[5],f12[6]);
                else printf("GetFeature(0x12) FAIL err=%lu\n", GetLastError());
                BYTE fa3[49] = {0}; fa3[0] = 0xA3;
                if (HidD_GetFeature(h, fa3, sizeof(fa3)))
                    printf("GetFeature(0xA3) OK: ver@23=%04X\n", *(unsigned short*)(fa3+0x23));
                else printf("GetFeature(0xA3) FAIL err=%lu\n", GetLastError());
            }
        }
        CloseHandle(h); free(dd);
    }
    SetupDiDestroyDeviceInfoList(ds);
    return 0;
}
#include <xinput.h>
