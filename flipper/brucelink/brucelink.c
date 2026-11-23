/*
 * BruceLink Flipper app — native UI + session driver.
 *
 * A ViewDispatcher app: a main menu, a scrollable text view for
 * device info / capabilities / results, and a settings list. Protocol
 * traffic runs over a BlTransport (loopback by default; BLE is a
 * hardware-session stub). A periodic timer pumps the transport, feeds
 * received frames into the central session, and updates the UI. The
 * app stays responsive and reports a clean status when the peer is
 * absent or disconnects.
 * SPDX-License-Identifier: MIT
 */
#include "brucelink_i.h"

#include "brucelink_core/src/bl_capability.h"

#include <furi_hal_random.h>
#include <stdio.h>
#include <string.h>

/* The demo pairing key, matching the loopback mock peripheral. On real
 * hardware this is provisioned per device, not compiled in. */
static const uint8_t DEMO_PSK[BL_KEY_LEN] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
    0xcc, 0xdd, 0xee, 0xff, 0x00, 0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a,
    0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0};

extern const BlTransportApi BL_BLE_TRANSPORT_API;

typedef enum {
    MenuConnect,
    MenuDeviceInfo,
    MenuWifiScan,
    MenuCapabilities,
    MenuSettings,
    MenuAbout,
} MenuIndex;

static void bl_rng(void* ctx, uint8_t* buf, size_t len) {
    UNUSED(ctx);
    furi_hal_random_fill_buf(buf, len);
}

void bl_app_set_text(BruceLinkApp* app, const char* title, const char* body) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 2, 2, AlignLeft, AlignTop, FontPrimary, title);
    widget_add_text_scroll_element(app->widget, 2, 16, 124, 46, body);
}

void bl_app_show_view(BruceLinkApp* app, BlViewId id) {
    view_dispatcher_switch_to_view(app->view_dispatcher, id);
}

/* ------------------------------------------------- session / transport */

static void bl_set_status(BruceLinkApp* app, const char* s) {
    strncpy(app->status, s, sizeof(app->status) - 1);
    app->status[sizeof(app->status) - 1] = 0;
}

static void bl_send_frame(BruceLinkApp* app, const uint8_t* f, size_t n) {
    int rc = bl_transport_send(app->transport, f, n);
    if(rc != BL_OK) {
        bl_set_status(app, bl_result_str(rc));
    }
}

/* Start a fresh session and kick off the handshake. */
static void bl_begin_connect(BruceLinkApp* app) {
    if(app->transport) {
        bl_transport_disconnect(app->transport);
        bl_transport_loopback_free(app->transport);
        app->transport = NULL;
    }
    /* only the loopback transport is wired up in software today */
    app->transport = bl_transport_loopback_alloc();
    if(!bl_transport_connect(app->transport)) {
        bl_set_status(app, "connect failed");
        return;
    }
    bl_session_init(&app->session, BL_ROLE_CENTRAL, DEMO_PSK, bl_rng, NULL);
    app->have_session = true;
    uint8_t hello[BL_MAX_MSG];
    int n = bl_session_start(&app->session, hello, sizeof(hello));
    if(n > 0) {
        bl_set_status(app, "handshaking...");
        bl_send_frame(app, hello, (size_t)n);
    }
}

/* Request helpers once established. */
static void bl_request(BruceLinkApp* app, uint8_t type, const uint8_t* pl, uint16_t len) {
    if(!app->have_session || !bl_session_established(&app->session)) {
        bl_set_status(app, "not connected");
        return;
    }
    uint8_t out[BL_MAX_MSG];
    int n = bl_session_seal(&app->session, type, 0, pl, len, out, sizeof(out));
    if(n > 0) bl_send_frame(app, out, (size_t)n);
}

/* Render a CAPS payload as a text list. */
static void bl_render_caps(BruceLinkApp* app, const uint8_t* buf, size_t len) {
    if(bl_caps_validate(buf, len) < 0) {
        bl_app_set_text(app, "Capabilities", "malformed CAPS");
        return;
    }
    char* p = app->text;
    size_t room = sizeof(app->text);
    uint8_t count = buf[0];
    size_t off = 1;
    int w = snprintf(p, room, "%u capabilities:\n", count);
    for(uint8_t i = 0; i < count && w >= 0 && (size_t)w < room; i++) {
        uint8_t nlen = buf[off + 5];
        uint8_t opclass = buf[off + 2];
        uint8_t flags = buf[off + 3];
        const char* name = (const char*)(buf + off + 6);
        int add = snprintf(
            p + w, room - (size_t)w, "%.*s%s%s\n", (int)nlen, name,
            opclass == BL_OP_ACTIVE ? " [active]" : "",
            (flags & BL_CAP_F_REQUIRES_EXT_HW) ? " [ext-hw]" : "");
        if(add < 0) break;
        w += add;
        off += 6u + nlen;
    }
    bl_app_set_text(app, "Capabilities", app->text);
}

/* Received-frame handler (called from the poll timer context). */
static void bl_on_frame(const uint8_t* frame, size_t len, void* ctx) {
    BruceLinkApp* app = ctx;
    if(!app->have_session) return;

    if(!bl_session_established(&app->session)) {
        /* drive central handshake steps */
        uint8_t out[BL_MAX_MSG];
        size_t olen = 0;
        int rc = bl_session_handshake_rx(&app->session, frame, len, out, sizeof(out), &olen);
        if(rc != BL_OK) {
            bl_set_status(app, "auth failed");
            return;
        }
        if(olen) bl_send_frame(app, out, olen);
        if(bl_session_established(&app->session)) {
            bl_set_status(app, "connected");
            /* auto-pull device info on connect */
            bl_request(app, BL_MSG_DEVICE_INFO_REQ, NULL, 0);
        }
        return;
    }

    bl_frame_t f;
    int rc = bl_session_open(&app->session, frame, len, &f);
    if(rc != BL_OK) {
        bl_set_status(app, bl_result_str(rc));
        return;
    }
    switch(f.type) {
    case BL_MSG_DEVICE_INFO: {
        char body[BL_TEXT_MAX];
        uint16_t n = f.payload_len < sizeof(body) - 1 ? f.payload_len : sizeof(body) - 1;
        memcpy(body, f.payload, n);
        body[n] = 0;
        bl_app_set_text(app, "Device Info", body);
        break;
    }
    case BL_MSG_CAPS:
        bl_render_caps(app, f.payload, f.payload_len);
        break;
    case BL_MSG_CMD_RESP: {
        char body[BL_TEXT_MAX];
        uint16_t n = f.payload_len < sizeof(body) - 1 ? f.payload_len : sizeof(body) - 1;
        memcpy(body, f.payload, n);
        body[n] = 0;
        bl_app_set_text(app, "Wi-Fi Scan", body);
        break;
    }
    case BL_MSG_PONG: bl_set_status(app, "pong"); break;
    case BL_MSG_ERROR:
        bl_set_status(app, f.payload_len >= 1 ? bl_wire_err_str(f.payload[0]) : "error");
        break;
    default: break;
    }
}

static void bl_poll_timer(void* ctx) {
    BruceLinkApp* app = ctx;
    furi_mutex_acquire(app->lock, FuriWaitForever);
    if(app->transport && bl_transport_is_connected(app->transport)) {
        bl_transport_poll(app->transport, bl_on_frame, app);
    }
    furi_mutex_release(app->lock);
}

/* ----------------------------------------------------------- UI glue */

static void bl_submenu_cb(void* ctx, uint32_t index) {
    BruceLinkApp* app = ctx;
    furi_mutex_acquire(app->lock, FuriWaitForever);
    switch(index) {
    case MenuConnect:
        bl_begin_connect(app);
        bl_app_set_text(app, "Connection", app->status);
        bl_app_show_view(app, BlViewText);
        break;
    case MenuDeviceInfo:
        bl_app_set_text(app, "Device Info", "requesting...");
        bl_request(app, BL_MSG_DEVICE_INFO_REQ, NULL, 0);
        bl_app_show_view(app, BlViewText);
        break;
    case MenuWifiScan:
        bl_app_set_text(app, "Wi-Fi Scan", "scanning...");
        bl_request(app, BL_MSG_CMD_REQ, (const uint8_t*)"\x10\x00", 2);
        bl_app_show_view(app, BlViewText);
        break;
    case MenuCapabilities:
        bl_app_set_text(app, "Capabilities", "requesting...");
        bl_request(app, BL_MSG_CAPS_REQ, NULL, 0);
        bl_app_show_view(app, BlViewText);
        break;
    case MenuSettings: bl_app_show_view(app, BlViewSettings); break;
    case MenuAbout:
        bl_app_set_text(
            app, "BruceLink",
            "Native remote for a StickS3 running Bruce.\n"
            "Authenticated BLE bridge.\n"
            "Transport: loopback demo until BLE is\nhardware-verified.\n"
            "github.com/soupscode/BruceLink");
        bl_app_show_view(app, BlViewText);
        break;
    default: break;
    }
    furi_mutex_release(app->lock);
}

static uint32_t bl_exit_cb(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

static uint32_t bl_back_to_menu_cb(void* ctx) {
    UNUSED(ctx);
    return BlViewMenu;
}

static void bl_active_toggle_cb(VariableItem* item) {
    BruceLinkApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->active_ops_enabled = idx == 1;
    variable_item_set_current_value_text(item, idx == 1 ? "Enabled" : "Disabled");
    /* Note: enabling here only reflects operator intent in the UI. The
     * active-ops gate is enforced on the StickS3 and only honoured over an
     * authenticated session; see bl_safety. */
}

static void bl_build_menu(BruceLinkApp* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "BruceLink");
    submenu_add_item(app->submenu, "Connect", MenuConnect, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "Device Info", MenuDeviceInfo, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "Wi-Fi Scan", MenuWifiScan, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "Capabilities", MenuCapabilities, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "Settings", MenuSettings, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "About", MenuAbout, bl_submenu_cb, app);
}

static void bl_build_settings(BruceLinkApp* app) {
    variable_item_list_reset(app->settings);
    VariableItem* it = variable_item_list_add(
        app->settings, "Active ops", 2, bl_active_toggle_cb, app);
    variable_item_set_current_value_index(it, 0);
    variable_item_set_current_value_text(it, "Disabled");
}

static BruceLinkApp* bl_app_alloc(void) {
    BruceLinkApp* app = malloc(sizeof(BruceLinkApp));
    memset(app, 0, sizeof(*app));
    app->lock = furi_mutex_alloc(FuriMutexTypeNormal);
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    bl_build_menu(app);
    view_set_previous_callback(submenu_get_view(app->submenu), bl_exit_cb);
    view_dispatcher_add_view(app->view_dispatcher, BlViewMenu, submenu_get_view(app->submenu));

    app->widget = widget_alloc();
    view_set_previous_callback(widget_get_view(app->widget), bl_back_to_menu_cb);
    view_dispatcher_add_view(app->view_dispatcher, BlViewText, widget_get_view(app->widget));

    app->settings = variable_item_list_alloc();
    bl_build_settings(app);
    view_set_previous_callback(
        variable_item_list_get_view(app->settings), bl_back_to_menu_cb);
    view_dispatcher_add_view(
        app->view_dispatcher, BlViewSettings, variable_item_list_get_view(app->settings));

    app->poll_timer = furi_timer_alloc(bl_poll_timer, FuriTimerTypePeriodic, app);
    furi_timer_start(app->poll_timer, furi_ms_to_ticks(50));
    return app;
}

static void bl_app_free(BruceLinkApp* app) {
    furi_timer_stop(app->poll_timer);
    furi_timer_free(app->poll_timer);
    if(app->transport) {
        bl_transport_disconnect(app->transport);
        bl_transport_loopback_free(app->transport);
    }
    if(app->have_session) bl_session_close(&app->session);

    view_dispatcher_remove_view(app->view_dispatcher, BlViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, BlViewText);
    view_dispatcher_remove_view(app->view_dispatcher, BlViewSettings);
    submenu_free(app->submenu);
    widget_free(app->widget);
    variable_item_list_free(app->settings);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->lock);
    free(app);
}

int32_t brucelink_app(void* p) {
    UNUSED(p);
    BruceLinkApp* app = bl_app_alloc();
    bl_set_status(app, "idle");
    view_dispatcher_switch_to_view(app->view_dispatcher, BlViewMenu);
    view_dispatcher_run(app->view_dispatcher);
    bl_app_free(app);
    return 0;
}
