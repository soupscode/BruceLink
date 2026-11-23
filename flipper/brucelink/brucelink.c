/*
 * BruceLink Flipper app — native UI.
 *
 * A ViewDispatcher app: a main menu, a scrollable text view for
 * connection status / device info / capabilities / results, and a
 * settings list. All protocol logic lives in the host-tested shared core:
 * bl_client drives the handshake, the single outstanding request and
 * their timeouts; a BlTransport (BLE to a StickS3, or the built-in
 * loopback mock) carries the frames. This file only connects those to the
 * screen, from the ViewDispatcher tick on the app thread.
 * SPDX-License-Identifier: MIT
 */
#include "brucelink_i.h"

#include "../../shared/brucelink_core/src/bl_capability.h"
#include "../../shared/brucelink_core/src/bl_demo_key.h"

#include <furi_hal_random.h>
#include <stdio.h>
#include <string.h>

#define BL_TICK_MS 50

typedef enum {
    MenuConnect,
    MenuDeviceInfo,
    MenuWifiScan,
    MenuCapabilities,
    MenuSettings,
    MenuAbout,
} MenuIndex;

static const char* const TRANSPORT_NAMES[BlTransportCount] = {"Loopback", "BLE"};

static void bl_rng(void* ctx, uint8_t* buf, size_t len) {
    UNUSED(ctx);
    furi_hal_random_fill_buf(buf, len);
}

static void bl_show_text(BruceLinkApp* app, const char* title, const char* body) {
    widget_reset(app->widget);
    widget_add_string_element(app->widget, 2, 2, AlignLeft, AlignTop, FontPrimary, title);
    widget_add_text_scroll_element(app->widget, 2, 16, 124, 46, body);
}

/* ----------------------------------------------------- connection */

static const char* bl_client_state_text(const BruceLinkApp* app) {
    switch(app->client.state) {
    case BL_CL_HANDSHAKING: return "authenticating...";
    case BL_CL_READY: return "connected";
    case BL_CL_FAILED:
        return app->client.last_error == BL_E_AUTH ? "wrong pairing key" : "handshake failed";
    default:
        /* the StickS3 drops a link it could not authenticate; keep saying why */
        return app->client.last_error == BL_E_AUTH ? "wrong pairing key" : "waiting for StickS3";
    }
}

/* Redraw the Connection screen if it is the one on display. */
static void bl_refresh_connection(BruceLinkApp* app) {
    if(app->screen != BlScreenConnection) return;
    if(app->transport == NULL) {
        bl_show_text(app, "Connection", "not started\nPress Connect.");
        return;
    }
    app->shown_status = app->transport->api->status(app->transport);
    char* diag = app->shown_diag;
    diag[0] = 0;
    if(app->transport->api->diag)
        app->transport->api->diag(app->transport, diag, sizeof(app->shown_diag));
    snprintf(
        app->text, sizeof(app->text), "%s\n%s\n\nDEMO KEY, no encryption:\nnot secure%s%s",
        app->shown_status, bl_client_state_text(app), diag[0] ? "\n\n" : "", diag);
    bl_show_text(app, "Connection", app->text);
}

static void bl_send(BruceLinkApp* app, const uint8_t* frame, size_t len) {
    if(app->transport) (void)app->transport->api->send(app->transport, frame, len);
}

static void bl_stop_transport(BruceLinkApp* app) {
    if(app->transport == NULL) return;
    app->transport->api->free(app->transport); /* stops it first */
    app->transport = NULL;
    app->link_id = 0;
    bl_client_link_down(&app->client);
}

/* (Re)start the transport chosen in Settings. The handshake begins by
 * itself, from the tick, as soon as a link appears. */
static void bl_connect(BruceLinkApp* app) {
    bl_stop_transport(app);
    app->client.last_error = BL_OK;
    app->transport = (app->transport_kind == BlTransportBle) ? bl_transport_ble_alloc() :
                                                             bl_transport_loopback_alloc();
    if(!app->transport->api->start(app->transport)) {
        bl_stop_transport(app);
        if(app->screen == BlScreenConnection)
            bl_show_text(app, "Connection", "could not start transport\n(is Bluetooth available?)");
        return;
    }
    bl_refresh_connection(app);
}

/* ------------------------------------------------------- requests */

static void bl_request(
    BruceLinkApp* app,
    const char* title,
    uint8_t type,
    const uint8_t* pl,
    uint16_t len) {
    app->screen = BlScreenRequest;
    app->req_title = title;
    uint8_t out[BL_HDR_LEN + 16 + BL_MAC_TAG_LEN]; /* requests carry tiny payloads */
    int n = bl_client_request(&app->client, type, pl, len, furi_get_tick(), out, sizeof(out));
    if(n == BL_E_STATE) {
        bl_show_text(app, title, "not connected\nUse Connect first.");
    } else if(n == BL_E_BUSY) {
        bl_show_text(app, title, "busy: still waiting for the\nprevious reply");
    } else if(n < 0) {
        bl_show_text(app, title, bl_result_str(n));
    } else {
        bl_show_text(app, title, "requesting...");
        bl_send(app, out, (size_t)n);
    }
}

/* Render a CAPS payload as a text list. */
static void bl_render_caps(BruceLinkApp* app, const uint8_t* buf, size_t len) {
    if(bl_caps_validate(buf, len) < 0) {
        bl_show_text(app, "Capabilities", "malformed reply");
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
    bl_show_text(app, "Capabilities", app->text);
}

static void bl_show_payload(BruceLinkApp* app, const char* title, const bl_frame_t* f) {
    uint16_t n = f->payload_len < sizeof(app->text) - 1 ? f->payload_len :
                                                           (uint16_t)(sizeof(app->text) - 1);
    if(n) memcpy(app->text, f->payload, n);
    app->text[n] = 0;
    bl_show_text(app, title, app->text);
}

/* An authenticated message from the StickS3. */
static void bl_on_message(BruceLinkApp* app, const bl_frame_t* f) {
    if(app->screen != BlScreenRequest) return; /* user moved on */
    switch(f->type) {
    case BL_MSG_DEVICE_INFO: bl_show_payload(app, "Device Info", f); break;
    case BL_MSG_CAPS: bl_render_caps(app, f->payload, f->payload_len); break;
    case BL_MSG_CMD_RESP: bl_show_payload(app, "Wi-Fi Scan", f); break;
    case BL_MSG_ERROR: {
        uint16_t code = f->payload_len >= 2 ?
                            (uint16_t)(f->payload[0] | (f->payload[1] << 8)) :
                            (uint16_t)BL_ERR_INTERNAL;
        snprintf(app->text, sizeof(app->text), "StickS3 refused:\n%s", bl_wire_err_str(code));
        bl_show_text(app, app->req_title ? app->req_title : "Error", app->text);
        break;
    }
    default: break;
    }
}

/* ----------------------------------------------- tick (app thread) */

/* A complete frame from the transport. */
static void bl_on_frame(const uint8_t* frame, size_t len, void* ctx) {
    BruceLinkApp* app = ctx;
    uint8_t reply[BL_CLIENT_HS_FRAME_MAX];
    size_t reply_len = 0;
    bl_frame_t msg;
    bool have_msg = false;
    const bl_client_state_t before = app->client.state;
    (void)bl_client_rx(
        &app->client, frame, len, reply, sizeof(reply), &reply_len, &msg, &have_msg);
    if(reply_len) bl_send(app, reply, reply_len);
    if(have_msg) bl_on_message(app, &msg);
    if(app->client.state != before) bl_refresh_connection(app);
}

static void bl_tick(void* ctx) {
    BruceLinkApp* app = ctx;
    if(app->connect_pending) {
        /* Switching BLE profiles blocks for a few seconds, so it runs
         * here, after the "starting..." screen is up, not in the menu
         * callback. */
        app->connect_pending = false;
        bl_connect(app);
    }
    BlTransport* t = app->transport;
    if(t == NULL) return;
    const uint32_t now = furi_get_tick();

    t->api->poll(t, bl_on_frame, app);

    /* Link came up, went down, or was replaced by a new one. */
    const uint32_t link = t->api->link_id(t);
    if(link != app->link_id) {
        app->link_id = link;
        bl_client_link_down(&app->client);
        if(link != 0) {
            /* Start authenticating immediately: the StickS3 drops links
             * that are not authenticated within its deadline. */
            uint8_t hello[BL_CLIENT_HS_FRAME_MAX];
            int n = bl_client_link_up(&app->client, now, hello, sizeof(hello));
            if(n > 0) bl_send(app, hello, (size_t)n);
        } else if(app->screen == BlScreenRequest) {
            bl_show_text(app, app->req_title ? app->req_title : "BruceLink", "link lost");
        }
        bl_refresh_connection(app);
    }

    switch(bl_client_tick(&app->client, now)) {
    case BL_CL_EVT_HANDSHAKE_TIMEOUT:
        /* Give up on this link; the transport waits for a fresh one. */
        t->api->drop_link(t);
        bl_refresh_connection(app);
        break;
    case BL_CL_EVT_REQUEST_TIMEOUT:
        if(app->screen == BlScreenRequest) {
            /* Say what the link saw, so a lost reply can be told apart
             * from a StickS3 that never answered. */
            char diag[96] = "";
            if(t->api->diag) t->api->diag(t, diag, sizeof(diag));
            snprintf(app->text, sizeof(app->text), "no reply (timed out)\n%s", diag);
            bl_show_text(app, app->req_title ? app->req_title : "BruceLink", app->text);
        }
        break;
    default: break;
    }

    /* The transport's own state (advertising / connecting / linked) changes
     * without the link id changing, e.g. when a central connects but has
     * not subscribed yet. Statuses are string literals: compare pointers. */
    if(app->screen == BlScreenConnection) {
        bool changed = t->api->status(t) != app->shown_status;
        if(!changed && t->api->diag) {
            char diag[sizeof(app->shown_diag)] = "";
            t->api->diag(t, diag, sizeof(diag));
            changed = strcmp(diag, app->shown_diag) != 0;
        }
        if(changed) bl_refresh_connection(app);
    }
}

/* ----------------------------------------------------------- menus */

static void bl_submenu_cb(void* ctx, uint32_t index) {
    BruceLinkApp* app = ctx;
    switch(index) {
    case MenuConnect:
        app->screen = BlScreenConnection;
        if(app->transport != NULL && app->link_id != 0) {
            /* Already linked: show the status (and counters) instead of
             * tearing a working link down. To restart, change Transport
             * in Settings or leave the app. */
            bl_refresh_connection(app);
        } else {
            bl_show_text(app, "Connection", "starting...");
            app->connect_pending = true; /* done by the next tick */
        }
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewText);
        break;
    case MenuDeviceInfo:
        bl_request(app, "Device Info", BL_MSG_DEVICE_INFO_REQ, NULL, 0);
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewText);
        break;
    case MenuWifiScan: {
        const uint8_t cap[2] = {(uint8_t)(BL_CAP_WIFI_SCAN & 0xFF), (uint8_t)(BL_CAP_WIFI_SCAN >> 8)};
        bl_request(app, "Wi-Fi Scan", BL_MSG_CMD_REQ, cap, sizeof(cap));
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewText);
        break;
    }
    case MenuCapabilities:
        bl_request(app, "Capabilities", BL_MSG_CAPS_REQ, NULL, 0);
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewText);
        break;
    case MenuSettings:
        app->screen = BlScreenNone;
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewSettings);
        break;
    case MenuAbout:
        app->screen = BlScreenStatic;
        bl_show_text(
            app, "BruceLink",
            "Remote for a StickS3 running\nBruce, over an authenticated\nBLE link.\n\n"
            "DEMO KEY build: the pairing\nkey is public and the link\nis not encrypted. Not secure\nuntil key setup is added.\n\n"
            "github.com/soupscode/BruceLink");
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewText);
        break;
    default: break;
    }
}

static uint32_t bl_exit_cb(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

static uint32_t bl_back_to_menu_cb(void* ctx) {
    UNUSED(ctx);
    return BlViewMenu;
}

static void bl_transport_changed_cb(VariableItem* item) {
    BruceLinkApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= BlTransportCount) idx = 0;
    variable_item_set_current_value_text(item, TRANSPORT_NAMES[idx]);
    if((BlTransportKind)idx != app->transport_kind) {
        app->transport_kind = (BlTransportKind)idx;
        bl_stop_transport(app); /* takes effect on the next Connect */
    }
}

static void bl_active_toggle_cb(VariableItem* item) {
    BruceLinkApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->active_ops_enabled = idx == 1;
    variable_item_set_current_value_text(item, idx == 1 ? "Enabled" : "Disabled");
    /* Records operator intent only. The gate is enforced on the StickS3
     * and only over an authenticated session; see bl_safety. */
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
        app->settings, "Transport", BlTransportCount, bl_transport_changed_cb, app);
    variable_item_set_current_value_index(it, (uint8_t)app->transport_kind);
    variable_item_set_current_value_text(it, TRANSPORT_NAMES[app->transport_kind]);

    it = variable_item_list_add(app->settings, "Active ops", 2, bl_active_toggle_cb, app);
    variable_item_set_current_value_index(it, 0);
    variable_item_set_current_value_text(it, "Disabled");
}

static BruceLinkApp* bl_app_alloc(void) {
    BruceLinkApp* app = malloc(sizeof(BruceLinkApp));
    memset(app, 0, sizeof(*app));
    app->transport_kind = BlTransportLoopback;
    bl_client_init(&app->client, BRUCELINK_DEMO_PSK, bl_rng, NULL);

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

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, bl_tick, furi_ms_to_ticks(BL_TICK_MS));
    return app;
}

static void bl_app_free(BruceLinkApp* app) {
    /* Hands the radio back to the firmware's default BLE profile. */
    bl_stop_transport(app);

    view_dispatcher_remove_view(app->view_dispatcher, BlViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, BlViewText);
    view_dispatcher_remove_view(app->view_dispatcher, BlViewSettings);
    submenu_free(app->submenu);
    widget_free(app->widget);
    variable_item_list_free(app->settings);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t brucelink_app(void* p) {
    UNUSED(p);
    BruceLinkApp* app = bl_app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, BlViewMenu);
    view_dispatcher_run(app->view_dispatcher);
    bl_app_free(app);
    return 0;
}
