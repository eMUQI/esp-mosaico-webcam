#pragma once
#include "host.h"

typedef enum {
    MOSAICO_MODULE_PRESENCE_UNKNOWN,
    MOSAICO_MODULE_PRESENCE_ABSENT,
    MOSAICO_MODULE_PRESENCE_PRESENT,
} mosaico_module_presence_t;
typedef enum { MOSAICO_MODULE_MGR_SLOT_LEFT } mosaico_module_mgr_slot_t;
typedef struct { mosaico_module_presence_t presence; } mosaico_module_mgr_info_t;

static mosaico_module_presence_t host_module_presence = MOSAICO_MODULE_PRESENCE_PRESENT;
static esp_err_t host_module_query_error = ESP_OK;

static esp_err_t mosaico_module_mgr_get_info(mosaico_module_mgr_slot_t slot,
                                            mosaico_module_mgr_info_t *info)
{
    assert(slot == MOSAICO_MODULE_MGR_SLOT_LEFT);
    info->presence = host_module_presence;
    return host_module_query_error;
}
