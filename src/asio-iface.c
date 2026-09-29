#include <windows.h>

#include <objbase.h>

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "asio-iface.h"
#include "defs.h"
#include "hr.h"
#include "trace.h"

/*  ASIO drivers register themselves under:
    HKEY_LOCAL_MACHINE\SOFTWARE\ASIO\<DriverName>
    Each subkey contains:
      CLSID       (REG_SZ) - The COM CLSID string e.g. "{XXXXXXXX-...}"
      Description (REG_SZ) - Human readable description

    On 64-bit Windows with 32-bit drivers, WOW64 automatically redirects
    accesses to HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\ASIO. */

#define ASIO_REG_PATH "SOFTWARE\\ASIO"

static HRESULT asio_clsid_from_string(CLSID *clsid, const char *str)
{
    wchar_t wstr[64];
    int len;

    len = MultiByteToWideChar(CP_ACP, 0, str, -1, wstr, lengthof(wstr));

    if (len == 0) {
        return hr_from_win32();
    }

    return CLSIDFromString(wstr, clsid);
}

/*  Direct DLL loader fallback: Loads the in-process ASIO driver DLL directly
    from its registered InprocServer32 path and instantiates it via
    DllGetClassObject and IClassFactory.
    This completely bypasses COM apartment boundary and proxy/stub requirements
    that cause CoCreateInstance to fail with E_NOINTERFACE (0x80004002) when
    called from a Multi-Threaded Apartment (MTA) thread. */

static HRESULT asio_open_driver_from_dll(IASIO **out, const CLSID *clsid)
{
    wchar_t clsid_wstr[64];
    char clsid_astr[64];
    char subkey[128];
    char dll_path[MAX_PATH];
    DWORD path_size;
    DWORD type;
    HKEY key;
    LSTATUS ls;
    HMODULE hmod;
    HRESULT hr;

    assert(out != NULL);
    assert(clsid != NULL);

    StringFromGUID2(clsid, clsid_wstr, lengthof(clsid_wstr));
    WideCharToMultiByte(
            CP_ACP,
            0,
            clsid_wstr,
            -1,
            clsid_astr,
            sizeof(clsid_astr),
            NULL,
            NULL);

    snprintf(subkey, sizeof(subkey), "CLSID\\%s\\InprocServer32", clsid_astr);
    ls = RegOpenKeyExA(HKEY_CLASSES_ROOT, subkey, 0, KEY_READ, &key);

    if (ls != ERROR_SUCCESS) {
        trace("ASIO: Cannot open HKCR\\%s: %ld", subkey, ls);

        return E_FAIL;
    }

    path_size = sizeof(dll_path);
    ls = RegQueryValueExA(
            key,
            NULL,
            NULL,
            &type,
            (BYTE *) dll_path,
            &path_size);

    RegCloseKey(key);

    if (ls != ERROR_SUCCESS || type != REG_SZ) {
        trace("ASIO: Cannot read InprocServer32 path: %ld", ls);

        return E_FAIL;
    }

    trace("ASIO: Loading driver DLL directly: %s", dll_path);

    hmod = LoadLibraryA(dll_path);

    if (hmod == NULL) {
        hr = hr_from_win32();
        hr_trace("LoadLibraryA", hr);

        return hr;
    }

    typedef HRESULT (STDAPICALLTYPE *pfnDllGetClassObject)(
            REFCLSID,
            REFIID,
            LPVOID *);

    pfnDllGetClassObject fn_get_class_object =
            (pfnDllGetClassObject) GetProcAddress(hmod, "DllGetClassObject");

    if (fn_get_class_object == NULL) {
        trace("ASIO: Driver DLL does not export DllGetClassObject");

        return E_FAIL;
    }

    static const IID iid_iclassfactory = {
        0x00000001, 0x0000, 0x0000,
        {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
    };
    static const IID iid_iunknown = {
        0x00000000, 0x0000, 0x0000,
        {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
    };

    IClassFactory *cf = NULL;

    hr = fn_get_class_object(clsid, &iid_iclassfactory, (void **) &cf);

    if (FAILED(hr)) {
        trace("ASIO: DllGetClassObject failed: hr=%08x", hr);

        return hr;
    }

    /* Try instantiating using clsid as IID, then fallback to IID_IUnknown */
    hr = cf->lpVtbl->CreateInstance(cf, NULL, clsid, (void **) out);

    if (FAILED(hr)) {
        trace("ASIO: CreateInstance(clsid) failed: hr=%08x, trying IID_IUnknown",
                hr);
        hr = cf->lpVtbl->CreateInstance(cf, NULL, &iid_iunknown, (void **) out);
    }

    cf->lpVtbl->Release(cf);

    if (SUCCEEDED(hr)) {
        trace("ASIO: Successfully instantiated driver directly from DLL");
    } else {
        trace("ASIO: Direct CreateInstance failed: hr=%08x", hr);
    }

    return hr;
}

static HRESULT asio_open_driver(IASIO **out, const CLSID *clsid)
{
    HRESULT hr;

    assert(out != NULL);
    assert(clsid != NULL);

    *out = NULL;

    /* First attempt: Standard COM CoCreateInstance with CLSID as IID */
    hr = CoCreateInstance(
            clsid,
            NULL,
            CLSCTX_INPROC_SERVER,
            clsid,
            (void **) out);

    if (SUCCEEDED(hr)) {
        return S_OK;
    }

    trace("ASIO: CoCreateInstance failed: hr=%08x, falling back to direct DLL loader",
            hr);

    /* Second attempt: Direct DLL loader bypassing COM apartment boundary issues */
    return asio_open_driver_from_dll(out, clsid);
}

static bool string_matches(const char *haystack, const char *needle)
{
    char h_lower[256];
    char n_lower[256];
    size_t i;

    if (haystack == NULL || needle == NULL) {
        return false;
    }

    if (_stricmp(haystack, needle) == 0) {
        return true;
    }

    for (i = 0; haystack[i] && i < sizeof(h_lower) - 1; i++) {
        h_lower[i] = (char) tolower((unsigned char) haystack[i]);
    }
    h_lower[i] = '\0';

    for (i = 0; needle[i] && i < sizeof(n_lower) - 1; i++) {
        n_lower[i] = (char) tolower((unsigned char) needle[i]);
    }
    n_lower[i] = '\0';

    return (strstr(h_lower, n_lower) != NULL);
}

HRESULT asio_driver_open_by_name(IASIO **out, const char *driver_name)
{
    HKEY asio_key;
    HKEY driver_key;
    char subkey_name[256];
    char desc_str[256];
    char clsid_str[128];
    DWORD subkey_name_size;
    DWORD desc_size;
    DWORD clsid_str_size;
    DWORD type;
    DWORD index;
    CLSID clsid;
    LSTATUS ls;
    HRESULT hr;

    assert(out != NULL);
    assert(driver_name != NULL);

    *out = NULL;

    trace("ASIO: Looking up driver '%s' in registry", driver_name);

    ls = RegOpenKeyExA(
            HKEY_LOCAL_MACHINE,
            ASIO_REG_PATH,
            0,
            KEY_READ,
            &asio_key);

    if (ls != ERROR_SUCCESS) {
        trace("ASIO: Cannot open registry key %s: %ld",
                ASIO_REG_PATH, ls);

        return E_FAIL;
    }

    /* Enumerate all subkeys and check subkey name and Description */

    for (index = 0; ; index++) {
        subkey_name_size = sizeof(subkey_name);

        ls = RegEnumKeyExA(
                asio_key,
                index,
                subkey_name,
                &subkey_name_size,
                NULL,
                NULL,
                NULL,
                NULL);

        if (ls != ERROR_SUCCESS) {
            break;
        }

        ls = RegOpenKeyExA(asio_key, subkey_name, 0, KEY_READ, &driver_key);

        if (ls != ERROR_SUCCESS) {
            continue;
        }

        desc_str[0] = '\0';
        desc_size = sizeof(desc_str);
        RegQueryValueExA(
                driver_key,
                "Description",
                NULL,
                &type,
                (BYTE *) desc_str,
                &desc_size);

        if (string_matches(subkey_name, driver_name) ||
            string_matches(desc_str, driver_name)) {
            trace("ASIO: Matched driver '%s' (key: '%s', desc: '%s')",
                    driver_name,
                    subkey_name,
                    desc_str);

            clsid_str_size = sizeof(clsid_str);
            ls = RegQueryValueExA(
                    driver_key,
                    "CLSID",
                    NULL,
                    &type,
                    (BYTE *) clsid_str,
                    &clsid_str_size);

            RegCloseKey(driver_key);

            if (ls != ERROR_SUCCESS || type != REG_SZ) {
                trace("ASIO: Cannot read CLSID for driver '%s': %ld",
                        subkey_name, ls);
                continue;
            }

            trace("ASIO: Driver '%s' CLSID: %s", subkey_name, clsid_str);

            hr = asio_clsid_from_string(&clsid, clsid_str);

            if (FAILED(hr)) {
                trace("ASIO: Invalid CLSID string '%s': hr=%08x",
                        clsid_str, hr);
                continue;
            }

            hr = asio_open_driver(out, &clsid);

            if (SUCCEEDED(hr)) {
                RegCloseKey(asio_key);

                return S_OK;
            }
        } else {
            RegCloseKey(driver_key);
        }
    }

    RegCloseKey(asio_key);

    trace("ASIO: Driver '%s' not found or failed to open", driver_name);

    return E_FAIL;
}

HRESULT asio_driver_open_first(
        IASIO **out,
        char *name_out,
        size_t name_out_size)
{
    HKEY asio_key;
    HKEY driver_key;
    char subkey_name[256];
    char desc_str[256];
    char clsid_str[128];
    DWORD subkey_name_size;
    DWORD desc_size;
    DWORD clsid_str_size;
    DWORD type;
    DWORD index;
    CLSID clsid;
    LSTATUS ls;
    HRESULT hr;

    assert(out != NULL);

    *out = NULL;

    if (name_out != NULL && name_out_size > 0) {
        name_out[0] = '\0';
    }

    ls = RegOpenKeyExA(
            HKEY_LOCAL_MACHINE,
            ASIO_REG_PATH,
            0,
            KEY_READ,
            &asio_key);

    if (ls != ERROR_SUCCESS) {
        trace("ASIO: Cannot open registry key %s: %ld",
                ASIO_REG_PATH, ls);

        return E_FAIL;
    }

    /* Enumerate all ASIO drivers and try them in order */

    for (index = 0; ; index++) {
        subkey_name_size = sizeof(subkey_name);

        ls = RegEnumKeyExA(
                asio_key,
                index,
                subkey_name,
                &subkey_name_size,
                NULL,
                NULL,
                NULL,
                NULL);

        if (ls != ERROR_SUCCESS) {
            break;
        }

        ls = RegOpenKeyExA(asio_key, subkey_name, 0, KEY_READ, &driver_key);

        if (ls != ERROR_SUCCESS) {
            continue;
        }

        desc_str[0] = '\0';
        desc_size = sizeof(desc_str);
        RegQueryValueExA(
                driver_key,
                "Description",
                NULL,
                &type,
                (BYTE *) desc_str,
                &desc_size);

        clsid_str_size = sizeof(clsid_str);
        ls = RegQueryValueExA(
                driver_key,
                "CLSID",
                NULL,
                &type,
                (BYTE *) clsid_str,
                &clsid_str_size);

        RegCloseKey(driver_key);

        if (ls != ERROR_SUCCESS || type != REG_SZ) {
            continue;
        }

        trace("ASIO: Found driver: key='%s', desc='%s'",
                subkey_name,
                desc_str[0] ? desc_str : subkey_name);

        hr = asio_clsid_from_string(&clsid, clsid_str);

        if (FAILED(hr)) {
            continue;
        }

        hr = asio_open_driver(out, &clsid);

        if (SUCCEEDED(hr)) {
            if (name_out != NULL && name_out_size > 0) {
                const char *chosen_name = desc_str[0] ? desc_str : subkey_name;
                strncpy(name_out, chosen_name, name_out_size - 1);
                name_out[name_out_size - 1] = '\0';
            }

            RegCloseKey(asio_key);

            return S_OK;
        }

        trace("ASIO: Driver '%s' failed to open, trying next", subkey_name);
    }

    RegCloseKey(asio_key);

    trace("ASIO: No usable ASIO driver found");

    return E_FAIL;
}
