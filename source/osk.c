/* System on-screen keyboard (adapted from PKGi PS3 by bucanero, GPL-2.0). Uses sysutil slot 1. */
#include <sysutil/osk.h>
#include <sysutil/sysutil.h>
#include <sys/memory.h>
#include <string.h>
#include <stdint.h>
#include "osk.h"
#include "arabic.h"

#define OSK_LEN 255

#define OSKDIALOG_FINISHED       0x503
#define OSKDIALOG_UNLOADED       0x504
#define OSKDIALOG_INPUT_ENTERED  0x505
#define OSKDIALOG_INPUT_CANCELED 0x506

static sys_mem_container_t container_mem;
static oskCallbackReturnParam ret_param;
static uint16_t ime_title[128];
static uint16_t ime_text[OSK_LEN + 1];
static uint16_t ime_input[OSK_LEN + 1];
static volatile int osk_event, osk_unloaded;
static int osk_level, osk_action, active;

static void to_utf16(const char *utf8, uint16_t *out, int max)
{
    uint32_t cp[512];
    int n = utf8_decode(utf8, cp, 512), o = 0;
    for (int i = 0; i < n && o < max - 2; i++) {
        uint32_t c = cp[i];
        if (c < 0x10000) out[o++] = (uint16_t)c;
        else { c -= 0x10000; out[o++] = 0xD800 | (c >> 10); out[o++] = 0xDC00 | (c & 0x3FF); }
    }
    out[o] = 0;
}

static void from_utf16(const uint16_t *in, char *out, int size)
{
    uint32_t cp[512];
    int n = 0;
    while (*in && n < 511) {
        uint32_t c = *in++;
        if (c >= 0xD800 && c < 0xDC00 && *in) { c = 0x10000 + ((c & 0x3FF) << 10) + (*in++ & 0x3FF); }
        cp[n++] = c;
    }
    utf8_encode(cp, n, out, size);
}

static void osk_cb(u64 status, u64 param, void *ud)
{
    (void)param; (void)ud;
    switch ((u32)status) {
        case OSKDIALOG_INPUT_CANCELED: osk_event = OSKDIALOG_INPUT_CANCELED; break;
        case OSKDIALOG_UNLOADED:       osk_unloaded = 1; break;
        case OSKDIALOG_INPUT_ENTERED:  osk_event = OSKDIALOG_INPUT_ENTERED; break;
        case OSKDIALOG_FINISHED:       osk_event = OSKDIALOG_FINISHED; break;
    }
}

static void osk_exit(void)
{
    if (osk_level == 2) { oskAbort(); oskUnloadAsync(&ret_param); osk_event = 0; osk_action = -1; }
    if (osk_level >= 1) { sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT1); sysMemContainerDestroy(container_mem); }
    osk_level = 0;
}

int osk_open(const char *title, const char *text)
{
    oskParam prm;
    oskInputFieldInfo fld;
    osk_level = 0;
    if (sysMemContainerCreate(&container_mem, 8 * 1024 * 1024) < 0) return 0;
    osk_level = 1;

    to_utf16(title, ime_title, 128);
    to_utf16(text, ime_text, OSK_LEN);
    fld.message = ime_title;
    fld.startText = ime_text;
    fld.maxLength = OSK_LEN;
    ret_param.res = OSK_NO_TEXT;
    ret_param.len = OSK_LEN;
    ret_param.str = ime_input;
    memset(ime_input, 0, sizeof(ime_input));

    if (oskSetKeyLayoutOption(OSK_10KEY_PANEL | OSK_FULLKEY_PANEL) < 0) goto fail;
    prm.firstViewPanel = OSK_PANEL_TYPE_ALPHABET_FULL_WIDTH;
    prm.allowedPanels = (OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_NUMERAL | OSK_PANEL_TYPE_ENGLISH);
    if (oskAddSupportLanguage(prm.allowedPanels) < 0) goto fail;
    if (oskSetLayoutMode(OSK_LAYOUTMODE_HORIZONTAL_ALIGN_CENTER | OSK_LAYOUTMODE_VERTICAL_ALIGN_CENTER) < 0) goto fail;
    oskPoint pos = { 0.0, 0.0 };
    prm.controlPoint = pos;
    prm.prohibitFlags = OSK_PROHIBIT_RETURN;
    if (oskSetInitialInputDevice(OSK_DEVICE_PAD) < 0) goto fail;

    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT1, osk_cb, NULL);
    osk_action = 0; osk_unloaded = 0; osk_event = 0;
    if (oskLoadAsync(container_mem, (const void *)&prm, (const void *)&fld) < 0) goto fail;
    osk_level = 2;
    active = 1;
    return 1;
fail:
    osk_exit();
    return 0;
}

int osk_active(void) { return active; }

int osk_update(char *out, int size)
{
    if (!active) return OSK_RESULT_NONE;
    if (!osk_unloaded) {
        switch (osk_event) {
            case OSKDIALOG_INPUT_ENTERED: oskGetInputText(&ret_param); osk_event = 0; break;
            case OSKDIALOG_INPUT_CANCELED: oskAbort(); oskUnloadAsync(&ret_param); osk_event = 0; osk_action = -1; break;
            case OSKDIALOG_FINISHED: if (osk_action != -1) osk_action = 1; oskUnloadAsync(&ret_param); osk_event = 0; break;
        }
        return OSK_RESULT_NONE;
    }
    active = 0;
    int ok = (ret_param.res == OSK_OK && osk_action == 1);
    osk_level = 1;   /* already unloaded: only release callback + container */
    osk_exit();
    if (ok) { from_utf16(ime_input, out, size); return OSK_RESULT_OK; }
    return OSK_RESULT_CANCEL;
}
