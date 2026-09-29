#include <windows.h>

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "trace.h"

static char *trim_whitespace(char *str)
{
    char *end;

    while (isspace((unsigned char)*str)) {
        str++;
    }

    if (*str == '\0') {
        return str;
    }

    end = str + strlen(str) - 1;

    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return str;
}

void config_load(struct config *cfg)
{
    char path[MAX_PATH];
    char line[512];
    char *sep;
    char *sep_slash;
    FILE *fp;
    HMODULE hmod;
    DWORD len;

    trace_enter();
    assert(cfg != NULL);

    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate = 44100;
    cfg->bit_depth = 24;
    cfg->buffer_size = 192;

    hmod = GetModuleHandleA("dsound.dll");

    if (hmod == NULL) {
        hmod = GetModuleHandleA("dsound");
    }

    if (hmod == NULL) {
        GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)config_load,
                &hmod);
    }

    if (hmod == NULL) {
        trace("Could not get module handle for dsound.dll, using defaults");

        goto validate;
    }

    len = GetModuleFileNameA(hmod, path, sizeof(path));

    if (len == 0 || len >= sizeof(path)) {
        trace("GetModuleFileNameA failed, using defaults");

        goto validate;
    }

    sep = strrchr(path, '\\');
    sep_slash = strrchr(path, '/');

    if (sep_slash != NULL && (sep == NULL || sep_slash > sep)) {
        sep = sep_slash;
    }

    if (sep != NULL) {
        size_t dirlen = (size_t)(sep - path + 1);
        int r = snprintf(path + dirlen, sizeof(path) - dirlen, "ultrasonik.ini");

        if (r < 0 || (size_t)r >= sizeof(path) - dirlen) {
            trace("Config file path too long, using defaults");

            goto validate;
        }
    } else {
        snprintf(path, sizeof(path), "ultrasonik.ini");
    }

    trace("Config file path: %s", path);

    fp = fopen(path, "r");

    if (fp == NULL) {
        trace("Config file '%s' not found, using defaults", path);

        goto validate;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p;
        char *eq;
        char *key;
        char *val;

        p = trim_whitespace(line);

        if (*p == '\0' || *p == ';' || *p == '#') {
            continue;
        }

        eq = strchr(p, '=');

        if (eq == NULL) {
            continue;
        }

        *eq = '\0';
        key = trim_whitespace(p);
        val = trim_whitespace(eq + 1);

        if (_stricmp(key, "device") == 0) {
            strncpy(cfg->device, val, sizeof(cfg->device) - 1);
            cfg->device[sizeof(cfg->device) - 1] = '\0';
        } else if (_stricmp(key, "sample_rate") == 0) {
            char *endptr;
            long v = strtol(val, &endptr, 10);

            if (endptr != val) {
                cfg->sample_rate = (uint32_t)v;
            }
        } else if (_stricmp(key, "bit_depth") == 0) {
            char *endptr;
            long v = strtol(val, &endptr, 10);

            if (endptr != val) {
                cfg->bit_depth = (uint32_t)v;
            }
        } else if (_stricmp(key, "buffer_size") == 0) {
            char *endptr;
            long v = strtol(val, &endptr, 10);

            if (endptr != val && v >= 0) {
                cfg->buffer_size = (uint32_t)v;
            }
        }
    }

    fclose(fp);

validate:
    if (cfg->sample_rate < 8000) {
        trace("sample_rate %u below 8000, clamping to 8000", cfg->sample_rate);
        cfg->sample_rate = 8000;
    } else if (cfg->sample_rate > 384000) {
        trace("sample_rate %u above 384000, clamping to 384000", cfg->sample_rate);
        cfg->sample_rate = 384000;
    }

    if (cfg->bit_depth != 16 && cfg->bit_depth != 24 && cfg->bit_depth != 32) {
        trace("bit_depth %u invalid (must be 16, 24, or 32), defaulting to 24", cfg->bit_depth);
        cfg->bit_depth = 24;
    }

    trace("Config loaded: device='%s', sample_rate=%u, bit_depth=%u, buffer_size=%u",
            cfg->device,
            cfg->sample_rate,
            cfg->bit_depth,
            cfg->buffer_size);

    trace_exit();
}
