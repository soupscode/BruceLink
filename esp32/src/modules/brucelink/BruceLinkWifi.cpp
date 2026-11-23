/*
 * BruceLink — Wi-Fi scan next to a live BLE link (see BruceLinkWifi.h).
 * Builds only inside the Bruce tree.
 * SPDX-License-Identifier: MIT
 */
#if !defined(LITE_VERSION)
#include "BruceLinkWifi.h"

#include "BruceBackendImpl.h" // logStep

#include <BruceLinkFormat.h>

#include <Arduino.h>
#include <WiFi.h>
#include <core/radio_mem.h> // Bruce: radioLargestDmaBlock
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace brucelink {

/* The scan must end, one way or the other, inside the Flipper's request
 * timeout (BL_CLIENT_REQ_TIMEOUT_MS, 10 s). ~14 channels at up to 120 ms is
 * under 2 s on an unshared radio; the limit covers one shared with BLE. */
static const uint32_t kScanMsPerChannelMin = 60;
static const uint32_t kScanMsPerChannelMax = 120;
static const uint32_t kScanTimeoutMs = 7000;
/* Records fetched from the driver; the 512-byte reply holds fewer lines. */
static const uint16_t kMaxAps = 32;

static void appendHeader(bl_textbuf_t* tb, unsigned found, uint32_t took, bool cut_short) {
    bl_tb_uint(tb, found);
    bl_tb_str(tb, " networks, ");
    bl_tb_uint(tb, took);
    bl_tb_str(tb, cut_short ? " ms (cut short)\n" : " ms\n");
}

static void appendFailure(bl_textbuf_t* tb, const char* step, int code, uint32_t took) {
    bl_tb_str(tb, "scan failed: ");
    bl_tb_str(tb, step);
    bl_tb_str(tb, "\ncode ");
    bl_tb_int(tb, code);
    bl_tb_str(tb, " after ");
    bl_tb_uint(tb, took);
    bl_tb_str(tb, " ms");
    Serial.printf(
        "[BruceLink] wifi scan failed at '%s': %d (%s)\n", step, code,
        esp_err_to_name((esp_err_t)code));
}

/* Wi-Fi is already up under Bruce: the same calls as Bruce's own scan
 * (bjs_interpreter/wifi_js.cpp native_wifiScan), bounded in time. */
static void scanViaArduino(bl_textbuf_t* tb) {
    const uint32_t t0 = millis();
    WiFi.setScanTimeout(kScanTimeoutMs);
    const int found = WiFi.scanNetworks(false, true, false, kScanMsPerChannelMax);
    const uint32_t took = millis() - t0;
    if(found < 0) {
        // WIFI_SCAN_RUNNING (-1) / WIFI_SCAN_FAILED (-2, includes "timed out")
        appendFailure(tb, "scan", found, took);
        WiFi.scanDelete();
        return;
    }
    appendHeader(tb, (unsigned)found, took, false);
    for(int i = 0; i < found; i++) {
        if(!formatWifiApLine(tb, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i))) break;
    }
    WiFi.scanDelete();
}

static SemaphoreHandle_t s_scan_done = nullptr;

static void onScanDone(void*, esp_event_base_t, int32_t, void*) {
    if(s_scan_done != nullptr) xSemaphoreGive(s_scan_done);
}

/* The driver is not initialised: bring it up small, scan, release it. */
static void scanViaTransientDriver(bl_textbuf_t* tb) {
    const uint32_t t0 = millis();

    // The driver posts its events to the default loop. Bruce or Arduino may
    // have created it already.
    esp_err_t e = esp_event_loop_create_default();
    if(e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        appendFailure(tb, "event loop", e, millis() - t0);
        return;
    }
    if(s_scan_done == nullptr) s_scan_done = xSemaphoreCreateBinary();
    if(s_scan_done == nullptr) {
        appendFailure(tb, "no memory", ESP_ERR_NO_MEM, millis() - t0);
        return;
    }
    xSemaphoreTake(s_scan_done, 0); // clear a stale signal

    // Scanning needs no association and no data path, so the buffers sized
    // for throughput are cut to their minimum; that is what makes the
    // driver fit next to NimBLE. Nothing is read from or written to flash.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 2;
    cfg.dynamic_rx_buf_num = 8;
    if(cfg.static_tx_buf_num > 2) cfg.static_tx_buf_num = 2;
    if(cfg.dynamic_tx_buf_num > 8) cfg.dynamic_tx_buf_num = 8;
    cfg.mgmt_sbuf_num = 8; // minimum is 6
    cfg.ampdu_rx_enable = 0;
    cfg.ampdu_tx_enable = 0;
    cfg.amsdu_tx_enable = 0;
    cfg.nvs_enable = 0;
    e = esp_wifi_init(&cfg);
    logStep("wifi scan: driver init");
    if(e != ESP_OK) {
        appendFailure(tb, "driver init", e, millis() - t0);
        return;
    }

    bool started = false;
    bool cut_short = false;
    esp_event_handler_instance_t handler = nullptr;
    wifi_ap_record_t* recs = nullptr;
    uint16_t total = 0;
    uint16_t got = 0;
    const char* step = "storage";
    do {
        if((e = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) break;
        step = "mode";
        if((e = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) break;
        step = "start";
        if((e = esp_wifi_start()) != ESP_OK) break;
        started = true;
        logStep("wifi scan: radio started");

        step = "event handler";
        e = esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_SCAN_DONE, onScanDone, nullptr, &handler);
        if(e != ESP_OK) break;

        wifi_scan_config_t sc = {};
        sc.show_hidden = true;
        sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        sc.scan_time.active.min = kScanMsPerChannelMin;
        sc.scan_time.active.max = kScanMsPerChannelMax;
        step = "scan start";
        if((e = esp_wifi_scan_start(&sc, false)) != ESP_OK) break;
        if(xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(kScanTimeoutMs)) != pdTRUE) {
            esp_wifi_scan_stop(); // report what was found so far
            cut_short = true;
        }
        logStep("wifi scan: scan finished");

        step = "results";
        if((e = esp_wifi_scan_get_ap_num(&total)) != ESP_OK) break;
        got = total < kMaxAps ? total : kMaxAps;
        if(got == 0) {
            esp_wifi_clear_ap_list();
            break;
        }
        // Record storage does not need internal RAM.
        recs = (wifi_ap_record_t*)heap_caps_malloc(
            (size_t)got * sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(recs == nullptr) recs = (wifi_ap_record_t*)malloc((size_t)got * sizeof(wifi_ap_record_t));
        if(recs == nullptr) {
            esp_wifi_clear_ap_list();
            e = ESP_ERR_NO_MEM;
            break;
        }
        // Also frees the driver's own copy of the list.
        e = esp_wifi_scan_get_ap_records(&got, recs);
    } while(false);

    const uint32_t took = millis() - t0;
    if(e != ESP_OK) {
        appendFailure(tb, step, e, took);
    } else {
        appendHeader(tb, total, took, cut_short);
        for(uint16_t i = 0; i < got; i++) {
            recs[i].ssid[sizeof(recs[i].ssid) - 1] = 0;
            if(!formatWifiApLine(tb, (const char*)recs[i].ssid, recs[i].rssi, recs[i].primary)) {
                break;
            }
        }
    }

    // Hand the radio and the memory back, whatever happened above.
    if(recs != nullptr) free(recs);
    if(handler != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, handler);
    }
    if(started) esp_wifi_stop();
    esp_wifi_deinit();
    logStep("wifi scan: driver released");
}

void wifiScanInto(bl_textbuf_t* tb) {
    logStep("wifi scan: begin");
    if(WiFi.getMode() != WIFI_MODE_NULL) {
        scanViaArduino(tb); // Bruce has Wi-Fi up; nothing to start
        logStep("wifi scan: done (Bruce's Wi-Fi)");
        return;
    }
    wifi_mode_t mode;
    if(esp_wifi_get_mode(&mode) != ESP_ERR_WIFI_NOT_INIT) {
        // Initialised, but not through the Arduino layer: one of Bruce's own
        // tools owns the driver. Leave it alone.
        bl_tb_str(tb, "Wi-Fi is in use on the\nStickS3. Close that tool\nand try again.");
        logStep("wifi scan: refused, driver owned by something else");
        return;
    }
    // Starting Wi-Fi next to the BLE link that carries this request. If it
    // will not fit, do not touch the driver at all: a failed init is not
    // recoverable and takes the link down with it (see wifiStartAllowed).
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t dma_block = radioLargestDmaBlock();
    if(!wifiStartAllowed(false, free_internal, dma_block)) {
        formatWifiNoMemory(tb, free_internal, dma_block);
        logStep("wifi scan: refused, Wi-Fi would not fit next to BLE");
        return;
    }
    scanViaTransientDriver(tb);
}

} // namespace brucelink
#endif // !LITE_VERSION
