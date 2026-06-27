#pragma once

#include <stdint.h>
#include "esp_system.h"
#include "esp_heap_caps.h"

struct rst_info {
    uint32_t reason;
};

class ESPClass {
public:
    void restart() { esp_restart(); }

    uint32_t getFreeHeap() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
    uint32_t getHeapSize() { return heap_caps_get_total_size(MALLOC_CAP_8BIT); }
    uint8_t getHeapFragmentation() { return 0; }
    uint32_t getMaxFreeBlockSize() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

    uint32_t getPsramSize() {
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
        return info.total_free_bytes + info.total_allocated_bytes;
    }
    uint32_t getFreePsram() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

    uint32_t getFlashChipRealSize() { return 0; }
    uint32_t getFlashChipSize() { return 0; }
    uint32_t getFreeSketchSpace() { return 0; }

    rst_info* getResetInfoPtr() { return &reset_info_; }

    uint32_t getVcc() { return 3300; }
    void wdtFeed() {}

private:
    rst_info reset_info_ = {0};
};

extern ESPClass ESP;
