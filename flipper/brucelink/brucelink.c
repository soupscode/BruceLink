/*
 * BruceLink Flipper app — native UI.
 *
 * A ViewDispatcher app: a main menu, a scrollable text view for
 * connection status / device info / capabilities / results, and a
 * settings list. All protocol logic lives in the host-tested shared core:
 * bl_client drives the handshake, the single outstanding request and
 * their timeouts; the BLE transport carries the frames to and from the
 * StickS3. This file only connects those to the screen, from the
 * ViewDispatcher tick on the app thread.
 * SPDX-License-Identifier: MIT
 */
#include "brucelink_i.h"

#include "../../shared/brucelink_core/src/bl_capability.h"
#include "../../shared/brucelink_core/src/bl_crypto.h"

#include <furi_hal_random.h>
#include <furi_hal_version.h>
#include <stdio.h>
#include <string.h>

#define BL_TICK_MS 50

/* Every request the app sends fits this payload: a capture-file read with
 * the longest name, or a job start with its largest arguments. Review
 * 2026-11-23: requests were sealed into 16 bytes of payload, so no request
 * carrying a file name could be sent at all. */
#define BL_APP_REQ_PAYLOAD_MAX \
    (BL_FILES_REQ_PAYLOAD_MAX > (4u + BL_JOB_ARGS_MAX) ? BL_FILES_REQ_PAYLOAD_MAX : (4u + BL_JOB_ARGS_MAX))
#define BL_APP_REQ_FRAME_MAX (BL_HDR_LEN + BL_APP_REQ_PAYLOAD_MAX + BL_MAC_TAG_LEN)
_Static_assert(BL_APP_REQ_FRAME_MAX >= BL_FILES_REQ_FRAME_MAX, "file requests must fit");

typedef enum {
    MenuConnect,
    MenuDeviceInfo,
    MenuWifiScan,
    MenuCapture,
    MenuCaptures,
    MenuCapabilities,
    MenuPair,
    MenuForget,
    MenuSettings,
    MenuAbout,
} MenuIndex;

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
        return app->client.last_error == BL_E_AUTH ? "wrong key: pair again" : "handshake failed";
    default:
        /* the StickS3 drops a link it could not authenticate; keep saying why */
        return app->client.last_error == BL_E_AUTH ? "wrong key: pair again" : "waiting for StickS3";
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
        app->text, sizeof(app->text), "%s\n%s\n\nKey %02X%02X, not encrypted%s%s", app->shown_status,
        bl_client_state_text(app), app->key_id[0], app->key_id[1], diag[0] ? "\n\n" : "", diag);
    bl_show_text(app, "Connection", app->text);
}

static void bl_send(BruceLinkApp* app, const uint8_t* frame, size_t len) {
    if(app->transport) (void)app->transport->api->send(app->transport, frame, len);
}

static void bl_job_link_down(BruceLinkApp* app);
static void bl_files_link_down(BruceLinkApp* app, bool transport_gone);

static void bl_stop_transport(BruceLinkApp* app) {
    if(app->transport == NULL) return;
    app->transport->api->free(app->transport); /* stops it first */
    app->transport = NULL;
    app->link_id = 0;
    bl_client_link_down(&app->client);
    /* No link means no use for the client's copy of the key until the next
     * Connect, which sets it again. */
    bl_secure_zero(&app->client, sizeof(app->client));
    bl_client_init(&app->client, NULL, bl_rng, NULL);
    bl_job_link_down(app);
    /* Connect restarts the transport: the download continues on the next
     * link (review 2026-11-23: it used to be dropped here). Forget and app
     * exit end it themselves. */
    bl_files_link_down(app, false);
}

/* (Re)start the BLE transport. The handshake begins by itself, from the
 * tick, as soon as a link appears. */
static void bl_connect(BruceLinkApp* app) {
    bl_stop_transport(app);
    if(!app->paired) {
        bl_show_text(
            app, "Connection",
            app->key_state == BlKeyfileForeign ?
                "The stored pairing is from\nanother Flipper or damaged.\nUse Pair." :
                "Not paired with a StickS3.\nUse Pair first.");
        return;
    }
    /* The StickS3 only accepts the key from pairing (ADR 0005). */
    bl_client_init(&app->client, app->psk, bl_rng, NULL);
    app->transport = bl_transport_ble_alloc();
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
    uint8_t out[BL_APP_REQ_FRAME_MAX];
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

/* ------------------------------------------------- jobs (ADR 0006) */

static void bl_job_draw(BruceLinkApp* app) {
    if(app->screen != BlScreenJob) return;
    const bl_jobc_t* j = &app->job;
    const uint32_t s = bl_jobc_elapsed_ms(j, furi_get_tick()) / 1000u;
    const uint32_t limit_s = (uint32_t)j->max_s + BL_JOB_RESTART_ALLOWANCE_MS / 1000u;
    switch(j->state) {
    case BL_JOBC_STARTING: snprintf(app->text, sizeof(app->text), "Asking the StickS3..."); break;
    case BL_JOBC_RUNNING:
        snprintf(
            app->text, sizeof(app->text),
            "Running on the StickS3.\nIt restarts to free its\nradio: the link drops and\ncomes back by itself.\n\n%lu s (gives up at %lu s)",
            (unsigned long)s, (unsigned long)limit_s);
        break;
    case BL_JOBC_FETCHING:
        snprintf(app->text, sizeof(app->text), "StickS3 is back.\nFetching the result...\n\n%lu s", (unsigned long)s);
        break;
    case BL_JOBC_DONE:
    case BL_JOBC_FAILED: bl_show_text(app, app->job_title, app->job_text); return;
    default: app->text[0] = 0; break;
    }
    app->job_drawn_s = s;
    bl_show_text(app, app->job_title, app->text);
}

/* The job ended: keep its screen text, whatever screen is up now. */
static void bl_job_finish_text(BruceLinkApp* app, const char* result, size_t len) {
    const bl_jobc_t* j = &app->job;
    app->job_unseen = app->screen != BlScreenJob;
    char* t = app->job_text;
    const size_t cap = sizeof(app->job_text);
    if(j->state == BL_JOBC_DONE) {
        if(len > BL_JOB_RESULT_MAX) len = BL_JOB_RESULT_MAX;
        /* Total time on the Flipper's clock: from pressing the item to the
         * result, both restarts and the reconnect included. */
        const unsigned long took_ds = bl_jobc_elapsed_ms(j, furi_get_tick()) / 100u;
        int w = snprintf(t, cap, "Took %lu.%lu s\n", took_ds / 10u, took_ds % 10u);
        if(w < 0) w = 0;
        if(j->outcome != BL_JOB_DONE) {
            const int o = snprintf(t + w, cap - (size_t)w, "%s\n\n", bl_job_outcome_str(j->outcome));
            if(o > 0) w += o;
        }
        if((size_t)w + len >= cap) len = cap - (size_t)w - 1;
        memcpy(t + w, result, len);
        t[(size_t)w + len] = 0;
        return;
    }
    switch(j->fail) {
    case BL_JOBC_F_REFUSED: snprintf(t, cap, "StickS3 refused:\n%s", bl_wire_err_str(j->err)); break;
    case BL_JOBC_F_NO_ANSWER: snprintf(t, cap, "No answer from the StickS3."); break;
    case BL_JOBC_F_TIMEOUT:
        snprintf(
            t, cap, "The StickS3 did not come\nback within %lu s.\nIs it on, with BruceLink on?",
            (unsigned long)((uint32_t)j->max_s + BL_JOB_RESTART_ALLOWANCE_MS / 1000u));
        break;
    case BL_JOBC_F_LOST: snprintf(t, cap, "The StickS3 no longer has\nthis job (lost power?)."); break;
    default: snprintf(t, cap, "Unexpected reply from\nthe StickS3."); break;
    }
}

/* The job may have just ended because of `before` -> now: report it once,
 * like every other ending (screen text, redraw, vibration). */
static void bl_job_after(BruceLinkApp* app, bl_jobc_state_t before) {
    if(before == app->job.state || app->job.state != BL_JOBC_FAILED) return;
    bl_job_finish_text(app, NULL, 0);
    bl_job_draw(app);
    notification_message(app->notifications, &sequence_error);
}

/* The link the job started on is gone (dropped, or the transport stopped). */
static void bl_job_link_down(BruceLinkApp* app) {
    const bl_jobc_state_t before = app->job.state;
    bl_jobc_link_down(&app->job);
    bl_job_after(app, before);
}

/* Seal and send a job request; false if the request slot is taken. */
/* Seal and send a request: the frame length, or a negative bl_result_t
 * (BL_E_BUSY: the one request slot is taken). */
static int bl_send_request(BruceLinkApp* app, uint8_t type, const uint8_t* pl, size_t len) {
    if(len > BL_APP_REQ_PAYLOAD_MAX) return BL_E_TOO_LARGE;
    uint8_t out[BL_APP_REQ_FRAME_MAX];
    const int n = bl_client_request(&app->client, type, pl, (uint16_t)len, furi_get_tick(), out, sizeof(out));
    if(n > 0) bl_send(app, out, (size_t)n);
    return n;
}

static bool bl_job_send(BruceLinkApp* app, uint8_t type, const uint8_t* pl, size_t len) {
    return bl_send_request(app, type, pl, len) > 0;
}

/* "busy" for a taken slot, else what went wrong. */
static void bl_show_send_error(BruceLinkApp* app, const char* title, int rc) {
    if(rc == BL_E_BUSY) {
        bl_show_text(app, title, "busy: still waiting for the\nprevious reply");
    } else {
        snprintf(app->text, sizeof(app->text), "could not send:\n%s", bl_result_str(rc));
        bl_show_text(app, title, app->text);
    }
}

static void bl_job_begin(
    BruceLinkApp* app,
    const char* title,
    uint16_t cap_id,
    uint16_t max_s,
    const uint8_t* args,
    size_t args_len) {
    app->screen = BlScreenJob;
    const bl_jobc_state_t st = app->job.state;
    const bool active = st == BL_JOBC_STARTING || st == BL_JOBC_RUNNING || st == BL_JOBC_FETCHING;
    if(app->job.cap_id == cap_id && (active || app->job_unseen)) {
        app->job_unseen = false;
        bl_job_draw(app); /* still running, or finished while away: show it */
        return;
    }
    app->job_title = title;
    if(!bl_client_ready(&app->client)) {
        bl_show_text(app, title, "not connected\nUse Connect first.");
        return;
    }
    uint8_t pl[8];
    const int n = bl_jobc_start(&app->job, cap_id, max_s, args, args_len, furi_get_tick(), pl, sizeof(pl));
    if(n < 0 || !bl_job_send(app, BL_MSG_JOB_START, pl, (size_t)n)) {
        bl_jobc_init(&app->job);
        bl_show_text(app, title, "busy: still waiting for the\nprevious reply");
        return;
    }
    bl_job_draw(app);
}

/* Every tick: follow the job through the restarts. */
static void bl_job_tick(BruceLinkApp* app, uint32_t now) {
    bl_jobc_t* j = &app->job;
    if(bl_jobc_want_fetch(j, bl_client_ready(&app->client))) {
        uint8_t pl[8];
        const bl_jobc_t before = *j;
        const int n = bl_jobc_fetch(j, pl, sizeof(pl));
        if(n > 0 && !bl_job_send(app, BL_MSG_JOB_RESULT_REQ, pl, (size_t)n)) {
            *j = before; /* slot taken: try again next tick */
        }
        bl_job_draw(app);
    }
    const bl_jobc_state_t before = j->state;
    bl_jobc_tick(j, now);
    bl_job_after(app, before);
    if(j->state == BL_JOBC_STARTING || j->state == BL_JOBC_RUNNING || j->state == BL_JOBC_FETCHING) {
        if(bl_jobc_elapsed_ms(j, now) / 1000u != app->job_drawn_s) bl_job_draw(app);
    }
}

/* ------------------------------------- capture files (bl_files.h) */

static void bl_size_text(uint32_t size, char* out, size_t cap) {
    if(size < 1024u) {
        snprintf(out, cap, "%lu B", (unsigned long)size);
    } else {
        const unsigned long tenths = (unsigned long)size * 10u / 1024u;
        snprintf(out, cap, "%lu.%lu kB", tenths / 10u, tenths % 10u);
    }
}

/* What to call a file on a 128 px screen: the SSID if Bruce put one in
 * the name (HS_<MAC>_<SSID>.pcap), else the MAC. */
static void bl_file_label(const bl_file_entry_t* e, char* out, size_t cap) {
    const char* n = e->name;
    size_t l = e->name_len > 5 ? e->name_len - 5u : e->name_len; /* drop ".pcap" */
    if(l > 3 && memcmp(n, "HS_", 3) == 0) {
        n += 3;
        l -= 3;
    }
    if(l > 13 && n[12] == '_') {
        n += 13;
        l -= 13;
    }
    char size[16];
    bl_size_text(e->size, size, sizeof(size));
    snprintf(out, cap, "%.*s  %s", (int)l, n, size);
}

static void bl_files_menu_cb(void* ctx, uint32_t index);

static void bl_files_show_menu(BruceLinkApp* app) {
    app->screen = BlScreenNone; /* the list is complete: nothing more to land here */
    submenu_reset(app->files_menu);
    submenu_set_header(
        app->files_menu,
        app->files_count >= BL_APP_FILES_MAX ? "Captures (first 32)" : "Captures on StickS3");
    for(size_t i = 0; i < app->files_count; i++) {
        char label[48];
        bl_file_label(&app->files[i], label, sizeof(label));
        submenu_add_item(app->files_menu, label, (uint32_t)i, bl_files_menu_cb, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, BlViewFiles);
}

static void bl_files_request_page(BruceLinkApp* app, uint8_t start) {
    uint8_t pl[1];
    bl_files_encode_list_req(start, pl, sizeof(pl));
    const int rc = bl_send_request(app, BL_MSG_CAP_LIST, pl, sizeof(pl));
    if(rc < 0) {
        /* only the listing gives up; a delete still waiting keeps its reply */
        if(app->pending_op == BlOpList) app->pending_op = BlOpNone;
        bl_show_send_error(app, "Captures", rc);
        return;
    }
    app->pending_op = BlOpList;
}

static void bl_files_open(BruceLinkApp* app) {
    app->screen = BlScreenFiles;
    app->files_count = 0;
    if(!bl_client_ready(&app->client)) {
        bl_show_text(app, "Captures", "not connected\nUse Connect first.");
        return;
    }
    bl_show_text(app, "Captures", "listing...");
    bl_files_request_page(app, 0);
}

/* A CAP_LIST page: keep its entries, ask for the next page or show them. */
static void bl_files_on_list(BruceLinkApp* app, const bl_frame_t* f) {
    app->pending_op = BlOpNone;
    bl_file_entry_t page[BL_FILE_LIST_PAGE];
    uint8_t total = 0, start = 0;
    size_t count = 0;
    if(bl_files_parse_list(f->payload, f->payload_len, &total, &start, page, BL_FILE_LIST_PAGE, &count) !=
       BL_OK) {
        if(app->screen == BlScreenFiles) bl_show_text(app, "Captures", "malformed reply");
        return;
    }
    for(size_t i = 0; i < count && app->files_count < BL_APP_FILES_MAX; i++) {
        app->files[app->files_count++] = page[i];
    }
    if(app->screen != BlScreenFiles) return; /* user moved on */
    const unsigned next = (unsigned)start + BL_FILE_LIST_PAGE;
    if(next < total && app->files_count < BL_APP_FILES_MAX) {
        bl_files_request_page(app, (uint8_t)next);
        return;
    }
    if(app->files_count == 0) {
        bl_show_text(app, "Captures", "No captures on the StickS3.\n\nRun Handshake Capture\nfirst.");
        return;
    }
    bl_files_show_menu(app);
}

static void bl_file_button_cb(GuiButtonType button, InputType type, void* ctx) {
    BruceLinkApp* app = ctx;
    if(type != InputTypeShort) return;
    const bool right = button == GuiButtonTypeRight;
    const uint32_t evt = app->file_confirm ? (right ? BlEvtFileDeleteYes : BlEvtFileDeleteNo) :
                                             (right ? BlEvtFileDownload : BlEvtFileDelete);
    view_dispatcher_send_custom_event(app->view_dispatcher, evt);
}

static void bl_file_draw(BruceLinkApp* app) {
    Widget* w = app->file_widget;
    widget_reset(w);
    if(app->file_sel >= app->files_count) return;
    const bl_file_entry_t* e = &app->files[app->file_sel];
    char size[16];
    bl_size_text(e->size, size, sizeof(size));
    if(app->file_confirm) {
        snprintf(app->text, sizeof(app->text), "Delete on the StickS3?\n%s", e->name);
        widget_add_text_box_element(w, 2, 2, 124, 48, AlignLeft, AlignTop, app->text, false);
        widget_add_button_element(w, GuiButtonTypeLeft, "No", bl_file_button_cb, app);
        widget_add_button_element(w, GuiButtonTypeRight, "Yes", bl_file_button_cb, app);
    } else {
        snprintf(app->text, sizeof(app->text), "%s\n%s\nSent unencrypted over BLE.", e->name, size);
        widget_add_text_box_element(w, 2, 2, 124, 48, AlignLeft, AlignTop, app->text, false);
        widget_add_button_element(w, GuiButtonTypeLeft, "Delete", bl_file_button_cb, app);
        widget_add_button_element(w, GuiButtonTypeRight, "Download", bl_file_button_cb, app);
    }
}

static void bl_files_menu_cb(void* ctx, uint32_t index) {
    BruceLinkApp* app = ctx;
    if(index >= app->files_count) return;
    app->file_sel = index;
    app->file_confirm = false;
    bl_file_draw(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, BlViewFile);
}

static void bl_file_delete(BruceLinkApp* app) {
    app->screen = BlScreenDelete;
    app->req_title = "Delete";
    const bl_file_entry_t* e = &app->files[app->file_sel];
    uint8_t pl[1 + BL_FILE_NAME_MAX];
    const int n = bl_files_encode_name(e->name, e->name_len, pl, sizeof(pl));
    const int rc = n < 0 ? n : bl_send_request(app, BL_MSG_CAP_DELETE, pl, (size_t)n);
    if(!bl_client_ready(&app->client)) {
        bl_show_text(app, "Delete", "not connected\nUse Connect first.");
    } else if(rc < 0) {
        bl_show_send_error(app, "Delete", rc);
    } else {
        app->pending_op = BlOpDelete;
        bl_show_text(app, "Delete", "deleting...");
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, BlViewText);
}

static void bl_file_on_deleted(BruceLinkApp* app, const bl_frame_t* f) {
    app->pending_op = BlOpNone;
    if(f->type == BL_MSG_CAP_DELETE && app->file_sel < app->files_count) {
        snprintf(app->text, sizeof(app->text), "Deleted on the StickS3:\n%s", app->files[app->file_sel].name);
        memmove(
            &app->files[app->file_sel], &app->files[app->file_sel + 1],
            (app->files_count - app->file_sel - 1) * sizeof(app->files[0]));
        app->files_count--;
    } else {
        const uint16_t code = f->payload_len >= 2 ? (uint16_t)(f->payload[0] | (f->payload[1] << 8)) :
                                                    (uint16_t)BL_ERR_INTERNAL;
        snprintf(app->text, sizeof(app->text), "StickS3 refused:\n%s", bl_wire_err_str(code));
    }
    if(app->screen == BlScreenDelete) bl_show_text(app, "Delete", app->text);
}

/* ---- the download ---- */

static void bl_xfer_finish_file(BruceLinkApp* app, bool keep) {
    char part[sizeof(app->xfer_path) + 8];
    snprintf(part, sizeof(part), "%s.part", app->xfer_path);
    if(app->xfer_file) {
        storage_file_close(app->xfer_file);
        storage_file_free(app->xfer_file);
        app->xfer_file = NULL;
    }
    if(keep) {
        if(storage_common_rename(app->storage, part, app->xfer_path) != FSE_OK) app->xfer_write_failed = true;
    }
    if(!keep || app->xfer_write_failed) storage_simply_remove(app->storage, part);
}

static void bl_xfer_draw(BruceLinkApp* app) {
    if(app->screen != BlScreenXfer) return;
    const bl_xferc_t* x = &app->xfer;
    switch(x->state) {
    case BL_XFER_INFO:
        snprintf(app->text, sizeof(app->text), "%s\n\nAsking the StickS3...", x->name);
        break;
    case BL_XFER_DATA:
        snprintf(
            app->text, sizeof(app->text), "%s\n\n%lu / %lu bytes\n(not encrypted over BLE)", x->name,
            (unsigned long)x->offset, (unsigned long)x->size);
        break;
    case BL_XFER_DONE:
        snprintf(
            app->text, sizeof(app->text), "Saved to the SD card:\n%s\n%lu bytes, CRC-32 checked.",
            app->xfer_path, (unsigned long)x->size);
        break;
    case BL_XFER_FAILED:
        if(app->xfer_closed) {
            snprintf(app->text, sizeof(app->text), "The link was closed before\nthe end. Nothing kept.");
        } else if(app->xfer_write_failed) {
            snprintf(app->text, sizeof(app->text), "Writing to the SD card\nfailed. Nothing kept.");
        } else if(x->fail == BL_XFER_F_REFUSED) {
            snprintf(app->text, sizeof(app->text), "StickS3 refused:\n%s", bl_wire_err_str(x->err));
        } else if(x->fail == BL_XFER_F_CHANGED) {
            snprintf(app->text, sizeof(app->text), "The file changed on the\nStickS3. Download it again.");
        } else if(app->xfer_send_failed) {
            snprintf(app->text, sizeof(app->text), "Could not send the\nrequest. Nothing kept.");
        } else if(x->fail == BL_XFER_F_GAVE_UP) {
            snprintf(app->text, sizeof(app->text), "Gave up: the link kept\ndropping. Nothing kept.");
        } else if(x->fail == BL_XFER_F_CRC) {
            snprintf(app->text, sizeof(app->text), "CRC-32 mismatch: the copy\nis damaged. Nothing kept.");
        } else {
            snprintf(app->text, sizeof(app->text), "Unexpected reply from\nthe StickS3. Nothing kept.");
        }
        break;
    default: app->text[0] = 0; break;
    }
    bl_show_text(app, "Download", app->text);
}

/* The download ended (done or failed): keep or drop the file, say so once. */
static void bl_xfer_ended(BruceLinkApp* app) {
    bool ok = app->xfer.state == BL_XFER_DONE && !app->xfer_write_failed;
    bl_xfer_finish_file(app, ok);
    /* review 2026-11-23: a failed rename used to be reported as saved */
    ok = ok && !app->xfer_write_failed;
    if(!ok && app->xfer.state == BL_XFER_DONE) app->xfer.state = BL_XFER_FAILED; /* the SD write failed */
    app->xfer_unseen = app->screen != BlScreenXfer;
    bl_xfer_draw(app);
    notification_message(app->notifications, ok ? &sequence_success : &sequence_error);
}

static void bl_xfer_begin(BruceLinkApp* app) {
    app->screen = BlScreenXfer;
    const bl_file_entry_t* e = &app->files[app->file_sel];
    const bool same = strcmp(app->xfer.name, e->name) == 0;
    /* One at a time: a running download is shown, whichever file was
     * picked; a finished one not seen yet is shown if it is this file. */
    if(bl_xferc_active(&app->xfer) || (app->xfer_unseen && same)) {
        app->xfer_unseen = false;
        bl_xfer_draw(app);
        return;
    }
    app->xfer_unseen = false;
    if(!bl_client_ready(&app->client)) {
        bl_show_text(app, "Download", "not connected\nUse Connect first.");
        return;
    }
    storage_simply_mkdir(app->storage, APP_DATA_PATH("captures"));
    snprintf(app->xfer_path, sizeof(app->xfer_path), "%s/%s", APP_DATA_PATH("captures"), e->name);
    char part[sizeof(app->xfer_path) + 8];
    snprintf(part, sizeof(part), "%s.part", app->xfer_path);
    app->xfer_write_failed = false;
    app->xfer_closed = false;
    app->xfer_send_failed = false;
    app->xfer_file = storage_file_alloc(app->storage);
    if(!storage_file_open(app->xfer_file, part, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(app->xfer_file);
        app->xfer_file = NULL;
        bl_show_text(app, "Download", "Could not create the file\non the SD card.");
        return;
    }
    if(bl_xferc_start(&app->xfer, e->name, e->name_len) != BL_OK) {
        bl_xfer_finish_file(app, false);
        bl_show_text(app, "Download", "Not a capture file name.");
        return;
    }
    bl_xfer_draw(app);
}

/* Every tick: the next request, when the link is up and the slot free. */
static void bl_xfer_tick(BruceLinkApp* app) {
    if(!bl_xferc_active(&app->xfer) || !bl_client_ready(&app->client)) return;
    uint8_t type = 0;
    uint8_t pl[BL_FILES_REQ_PAYLOAD_MAX];
    const int n = bl_xferc_next(&app->xfer, &type, pl, sizeof(pl));
    if(n <= 0) return;
    const int rc = bl_send_request(app, type, pl, (size_t)n);
    if(rc == BL_E_BUSY) {
        bl_xferc_unsend(&app->xfer); /* another request is out: next tick */
    } else if(rc < 0) {
        /* review 2026-11-23: any other failure was retried every tick,
         * forever; it will not get better */
        app->xfer_send_failed = true;
        app->xfer.state = BL_XFER_FAILED;
        bl_xfer_ended(app);
    }
}

/* A reply the download asked for (true), written to the file. */
static bool bl_xfer_on_message(BruceLinkApp* app, const bl_frame_t* f) {
    if(!(f->flags & BL_FLAG_RESPONSE)) return false; /* an event is never the download's */
    const uint8_t* d = NULL;
    size_t dl = 0;
    uint32_t at = 0;
    if(!bl_xferc_on_response(&app->xfer, f->type, f->payload, f->payload_len, &d, &dl, &at)) return false;
    if(dl > 0 && app->xfer_file != NULL) {
        /* chunks come in order: `at` is always the end of the file */
        if(storage_file_write(app->xfer_file, d, dl) != dl) {
            app->xfer_write_failed = true;
            app->xfer.state = BL_XFER_FAILED;
        }
    }
    if(app->xfer.state == BL_XFER_DONE || app->xfer.state == BL_XFER_FAILED) {
        bl_xfer_ended(app);
    } else {
        bl_xfer_draw(app);
    }
    return true;
}

/* The link or the transport is gone. */
static void bl_files_link_down(BruceLinkApp* app, bool transport_gone) {
    if(app->pending_op != BlOpNone) {
        app->pending_op = BlOpNone;
        if(app->screen == BlScreenFiles) bl_show_text(app, "Captures", "link lost");
        if(app->screen == BlScreenDelete) bl_show_text(app, "Delete", "link lost\nIt may or may not\nhave been deleted.");
    }
    if(!bl_xferc_active(&app->xfer)) return;
    if(transport_gone) {
        /* no link will come back: give up, keep nothing */
        app->xfer.state = BL_XFER_FAILED;
        app->xfer_closed = true;
        bl_xfer_ended(app);
        return;
    }
    bl_xferc_retry(&app->xfer); /* continues on the next session */
    if(app->xfer.state == BL_XFER_FAILED) bl_xfer_ended(app); /* gave up */
    else bl_xfer_draw(app);
}

/* ------------------------------------------------ handshake capture */

static const uint16_t kCapDurations[] = {30, 60, 120, 300};
static const char* const kCapDurationText[] = {"30 s", "1 min", "2 min", "5 min"};
#define BL_CAP_CHANNELS 14u /* hop, then 1..13 */

static void bl_cap_duration_cb(VariableItem* item) {
    BruceLinkApp* app = variable_item_get_context(item);
    app->cap_duration_idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kCapDurationText[app->cap_duration_idx]);
}

static void bl_cap_channel_text(VariableItem* item, uint8_t idx) {
    char t[8];
    if(idx == 0) snprintf(t, sizeof(t), "Hop");
    else snprintf(t, sizeof(t), "%u", (unsigned)idx);
    variable_item_set_current_value_text(item, t);
}

static void bl_cap_channel_cb(VariableItem* item) {
    BruceLinkApp* app = variable_item_get_context(item);
    app->cap_channel_idx = variable_item_get_current_value_index(item);
    bl_cap_channel_text(item, app->cap_channel_idx);
}

static void bl_capture_enter_cb(void* ctx, uint32_t index) {
    BruceLinkApp* app = ctx;
    if(index != 2) return; /* "Start capture" */
    /* Runs as a job: the StickS3 restarts, captures with BLE off, and
     * restarts again (ADR 0006). Passive: nothing is transmitted. */
    const uint8_t ch = app->cap_channel_idx;
    bl_job_begin(app, "Handshake Capture", BL_CAP_SNIFF_HANDSHAKE, kCapDurations[app->cap_duration_idx], &ch, 1);
    view_dispatcher_switch_to_view(app->view_dispatcher, BlViewText);
}

static void bl_build_capture_setup(BruceLinkApp* app) {
    VariableItemList* l = app->capture_setup;
    variable_item_list_reset(l);
    app->cap_duration_idx = 1; /* 1 min */
    app->cap_channel_idx = 0; /* hop */
    VariableItem* it =
        variable_item_list_add(l, "Duration", COUNT_OF(kCapDurations), bl_cap_duration_cb, app);
    variable_item_set_current_value_index(it, app->cap_duration_idx);
    variable_item_set_current_value_text(it, kCapDurationText[app->cap_duration_idx]);
    it = variable_item_list_add(l, "Channel", BL_CAP_CHANNELS, bl_cap_channel_cb, app);
    variable_item_set_current_value_index(it, app->cap_channel_idx);
    bl_cap_channel_text(it, app->cap_channel_idx);
    variable_item_list_add(l, "Start capture", 0, NULL, app);
    variable_item_list_set_enter_callback(l, bl_capture_enter_cb, app);
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
    const char* result = NULL;
    size_t result_len = 0;
    if(bl_jobc_on_response(
           &app->job, f->type, f->payload, f->payload_len, furi_get_tick(), &result, &result_len)) {
        if(app->job.state == BL_JOBC_DONE || app->job.state == BL_JOBC_FAILED) {
            bl_job_finish_text(app, result, result_len);
            notification_message(
                app->notifications,
                app->job.state == BL_JOBC_DONE ? &sequence_success : &sequence_error);
        }
        bl_job_draw(app);
        return;
    }
    if(bl_xfer_on_message(app, f)) return;
    const bool response = (f->flags & BL_FLAG_RESPONSE) != 0;
    if(response && app->pending_op == BlOpList &&
       (f->type == BL_MSG_CAP_LIST || f->type == BL_MSG_ERROR)) {
        if(f->type == BL_MSG_CAP_LIST) {
            bl_files_on_list(app, f);
        } else {
            app->pending_op = BlOpNone;
            const uint16_t code = f->payload_len >= 2 ? (uint16_t)(f->payload[0] | (f->payload[1] << 8)) :
                                                        (uint16_t)BL_ERR_INTERNAL;
            snprintf(app->text, sizeof(app->text), "StickS3 refused:\n%s", bl_wire_err_str(code));
            if(app->screen == BlScreenFiles) bl_show_text(app, "Captures", app->text);
        }
        return;
    }
    if(response && app->pending_op == BlOpDelete &&
       (f->type == BL_MSG_CAP_DELETE || f->type == BL_MSG_ERROR)) {
        bl_file_on_deleted(app, f);
        return;
    }
    if(app->screen != BlScreenRequest) return; /* user moved on */
    switch(f->type) {
    case BL_MSG_DEVICE_INFO: bl_show_payload(app, "Device Info", f); break;
    case BL_MSG_CAPS: bl_render_caps(app, f->payload, f->payload_len); break;
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

static void bl_pairing_tick(BruceLinkApp* app);

static void bl_pair_start_mode(BruceLinkApp* app);

static void bl_tick(void* ctx) {
    BruceLinkApp* app = ctx;
    if(app->pair_pending) {
        /* Like Connect: switching BLE profiles blocks for a few seconds,
         * so it runs here, after the screen has changed. */
        app->pair_pending = false;
        bl_pair_start_mode(app);
    }
    if(app->pair_mode) {
        bl_pairing_tick(app);
        bl_job_tick(app, furi_get_tick()); /* a job still gives up in time */
        return;
    }
    if(app->connect_pending) {
        /* Switching BLE profiles blocks for a few seconds, so it runs
         * here, after the "starting..." screen is up, not in the menu
         * callback. */
        app->connect_pending = false;
        bl_connect(app);
    }
    BlTransport* t = app->transport;
    const uint32_t now = furi_get_tick();
    if(t == NULL) {
        bl_job_tick(app, now);
        return;
    }

    t->api->poll(t, bl_on_frame, app);

    /* Link came up, went down, or was replaced by a new one. */
    const uint32_t link = t->api->link_id(t);
    if(link != app->link_id) {
        const bool lost = app->link_id != 0; /* a link went away (not just one came up) */
        app->link_id = link;
        bl_client_link_down(&app->client);
        bl_job_link_down(app); /* normally: the StickS3 is restarting for it */
        if(lost) bl_files_link_down(app, false); /* one retry per lost link */
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
    case BL_CL_EVT_REQUEST_TIMEOUT: {
        const bl_jobc_state_t before = app->job.state;
        bl_jobc_request_timed_out(&app->job);
        bl_job_after(app, before);
        if(bl_xferc_active(&app->xfer) && app->xfer.waiting) {
            /* its own request went unanswered: ask again (gives up after
             * BL_XFER_RETRY_MAX in a row without progress) */
            bl_xferc_retry(&app->xfer);
            if(app->xfer.state == BL_XFER_FAILED) bl_xfer_ended(app);
            else bl_xfer_draw(app);
        }
        if(app->pending_op != BlOpNone) {
            const BlPendingOp op = app->pending_op;
            app->pending_op = BlOpNone;
            if(op == BlOpList && app->screen == BlScreenFiles) {
                bl_show_text(app, "Captures", "no reply (timed out)");
            } else if(op == BlOpDelete && app->screen == BlScreenDelete) {
                bl_show_text(app, "Delete", "no reply (timed out)\nIt may or may not\nhave been deleted.");
            }
        }
        if(app->screen == BlScreenRequest) {
            /* Say what the link saw, so a lost reply can be told apart
             * from a StickS3 that never answered. */
            char diag[96] = "";
            if(t->api->diag) t->api->diag(t, diag, sizeof(diag));
            snprintf(app->text, sizeof(app->text), "no reply (timed out)\n%s", diag);
            bl_show_text(app, app->req_title ? app->req_title : "BruceLink", app->text);
        }
        break;
    }
    default: break;
    }

    bl_job_tick(app, now);
    bl_xfer_tick(app);

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

/* -------------------------------------------- pairing (ADR 0005) */

static void bl_pair_send(BruceLinkApp* app, const uint8_t* frame, size_t len) {
    if(len) bl_send(app, frame, len);
}

static const char* bl_pair_fail_text(const bl_pair_t* p) {
    switch(p->fail) {
    case BL_PF_REJECTED: return "You said the codes differ.";
    case BL_PF_PEER_REJECTED:
        switch(p->peer_reason) {
        case BL_PAIR_ABORT_REJECTED: return "The StickS3 said the\ncodes differ.";
        case BL_PAIR_ABORT_TIMEOUT: return "The StickS3 timed out.";
        case BL_PAIR_ABORT_MISMATCH: return "Key check failed\n(interference?).";
        default: return "The StickS3 cancelled.";
        }
    case BL_PF_MISMATCH: return "Key check failed\n(interference?).";
    case BL_PF_CRYPTO: return "Bad key from the StickS3.";
    case BL_PF_TIMEOUT: return "Timed out.";
    case BL_PF_CANCELLED: return "Link lost.";
    default: return "Failed.";
    }
}

static void bl_pair_button_cb(GuiButtonType button, InputType type, void* ctx) {
    BruceLinkApp* app = ctx;
    if(type != InputTypeShort) return;
    const bool yes = button == GuiButtonTypeRight;
    const uint32_t evt = app->screen == BlScreenForget ? (yes ? BlEvtForgetYes : BlEvtForgetNo) :
                                                         (yes ? BlEvtPairYes : BlEvtPairNo);
    view_dispatcher_send_custom_event(app->view_dispatcher, evt);
}

/* Draw the pairing screen for the current state. */
static void bl_pair_draw(BruceLinkApp* app) {
    Widget* w = app->pair_widget;
    widget_reset(w);
    const bl_pair_state_t st = app->pair_done    ? BL_PS_DONE :
                               app->pair_running ? bl_pair_state(&app->pair) :
                                                   BL_PS_IDLE;
    uint32_t code = 0;
    if(st == BL_PS_COMPARE && !app->pair_answered && bl_pair_code(&app->pair, &code)) {
        /* code < 10^6, so each half is exactly three digits */
        char a[8], b[8], who[40];
        snprintf(a, sizeof(a), "%03lu", (unsigned long)((code / 1000u) % 1000u));
        snprintf(b, sizeof(b), "%03lu", (unsigned long)(code % 1000u));
        const char* peer = bl_pair_peer_name(&app->pair);
        snprintf(who, sizeof(who), "%s", peer[0] ? peer : "StickS3");
        widget_add_string_element(w, 64, 0, AlignCenter, AlignTop, FontSecondary, who);
        widget_add_string_element(w, 33, 12, AlignCenter, AlignTop, FontBigNumbers, a);
        widget_add_string_element(w, 95, 12, AlignCenter, AlignTop, FontBigNumbers, b);
        widget_add_string_element(
            w, 64, 40, AlignCenter, AlignTop, FontSecondary, "Same code on the StickS3?");
        widget_add_button_element(w, GuiButtonTypeLeft, "No", bl_pair_button_cb, app);
        widget_add_button_element(w, GuiButtonTypeRight, "Yes", bl_pair_button_cb, app);
        return;
    }
    if(st == BL_PS_DONE) {
        snprintf(
            app->text, sizeof(app->text),
            "Paired with %s\nKey %02X%02X\n\nUse Connect.\nThe link is not encrypted\nyet; frames are\nauthenticated.",
            app->peer[0] ? app->peer : "StickS3", app->key_id[0], app->key_id[1]);
    } else if(st == BL_PS_COMPARE || st == BL_PS_WAIT_PEER) {
        snprintf(app->text, sizeof(app->text), "Waiting for the StickS3\nto confirm...");
    } else {
        snprintf(
            app->text, sizeof(app->text),
            "%s%s%sOn the StickS3:\nBruceLink (main menu) >\nPair with Flipper\n\nBack: stop",
            app->pair_last_failure ? "Last try: " : "",
            app->pair_last_failure ? app->pair_last_failure : "",
            app->pair_last_failure ? "\n\n" : "");
    }
    widget_add_string_element(w, 2, 2, AlignLeft, AlignTop, FontPrimary, "Pairing");
    widget_add_text_scroll_element(w, 2, 16, 124, 46, app->text);
}

/* Finish an attempt that is still open, without anyone left to tell. */
static void bl_pair_drop_attempt(BruceLinkApp* app) {
    if(!app->pair_running) return;
    uint8_t out[BL_PAIR_FRAME_MAX];
    size_t ol = 0;
    (void)bl_pair_cancel(&app->pair, out, sizeof(out), &ol);
}

static void bl_pair_start_mode(BruceLinkApp* app) {
    bl_stop_transport(app);
    app->screen = BlScreenPairing;
    app->pair_mode = true;
    app->pair_running = false;
    app->pair_done = false;
    app->pair_last_failure = NULL;
    bl_pair_wipe(&app->pair);
    app->transport = bl_transport_ble_alloc();
    if(!app->transport->api->start(app->transport)) {
        bl_stop_transport(app);
        app->pair_mode = false;
        widget_reset(app->pair_widget);
        widget_add_text_scroll_element(
            app->pair_widget, 2, 2, 124, 60, "Could not start Bluetooth.\n\nBack: menu");
        return;
    }
    bl_pair_draw(app);
}

static void bl_pair_end_mode(BruceLinkApp* app) {
    if(!app->pair_mode) return;
    if(app->pair_running && app->transport && app->link_id != 0) {
        uint8_t out[BL_PAIR_FRAME_MAX];
        size_t ol = 0;
        if(bl_pair_cancel(&app->pair, out, sizeof(out), &ol) == BL_OK) bl_pair_send(app, out, ol);
    }
    bl_pair_wipe(&app->pair);
    app->pair_running = false;
    app->pair_mode = false;
    bl_stop_transport(app); /* gives the radio back */
}

static void bl_on_pair_frame(const uint8_t* frame, size_t len, void* ctx) {
    BruceLinkApp* app = ctx;
    if(!app->pair_running) return;
    uint8_t out[BL_PAIR_FRAME_MAX];
    size_t ol = 0;
    (void)bl_pair_rx(&app->pair, frame, len, out, sizeof(out), &ol);
    bl_pair_send(app, out, ol); /* the tick compares states and redraws */
}

/* The attempt just finished well: keep the key. */
static void bl_pair_keep(BruceLinkApp* app) {
    uint8_t psk[BL_KEY_LEN];
    uint8_t kid[BL_PAIR_KEY_ID_LEN];
    if(bl_pair_result(&app->pair, psk, kid) != BL_OK) return;
    const char* peer = bl_pair_peer_name(&app->pair);
    if(!bl_keyfile_save(psk, peer)) {
        bl_secure_zero(psk, sizeof(psk));
        app->pair_last_failure = "Paired, but saving the\nkey failed (SD card?).";
        app->pair_running = false;
        return;
    }
    memcpy(app->psk, psk, BL_KEY_LEN);
    memcpy(app->key_id, kid, sizeof(kid));
    strncpy(app->peer, peer, BL_PAIR_NAME_MAX);
    app->peer[BL_PAIR_NAME_MAX] = '\0';
    app->paired = true;
    app->pair_done = true;
    app->key_state = BlKeyfileOk;
    bl_secure_zero(psk, sizeof(psk));
    /* Nothing more to send: our confirm already went out. The screen uses
     * app->peer / app->key_id from here on. */
    bl_pair_wipe(&app->pair);
    app->pair_running = false;
}

static void bl_build_menu(BruceLinkApp* app);

static void bl_pairing_tick(BruceLinkApp* app) {
    BlTransport* t = app->transport;
    if(t == NULL) return;
    const uint32_t now = furi_get_tick();
    const bl_pair_state_t before = app->pair_running ? bl_pair_state(&app->pair) : BL_PS_IDLE;

    t->api->poll(t, bl_on_pair_frame, app);

    const uint32_t link = t->api->link_id(t);
    if(link != app->link_id && app->pair_done) {
        /* Paired. The StickS3 drops this link and comes back in normal
         * mode; stop advertising in pairing mode so it does not find us
         * here. Connect starts a normal BLE session. */
        app->link_id = link;
        if(link == 0) bl_stop_transport(app);
        return;
    }
    if(link != app->link_id) {
        app->link_id = link;
        bl_pair_drop_attempt(app); /* whatever the old link was doing is over */
        if(app->pair_running && bl_pair_state(&app->pair) == BL_PS_FAILED &&
           app->pair.fail != BL_PF_CANCELLED) {
            app->pair_last_failure = bl_pair_fail_text(&app->pair);
        }
        app->pair_running = false;
        if(link != 0) {
            const char* name = furi_hal_version_get_name_ptr();
            if(bl_pair_init(&app->pair, BL_PAIR_INITIATOR, name ? name : "Flipper", bl_rng, NULL, now) ==
               BL_OK) {
                uint8_t out[BL_PAIR_FRAME_MAX];
                const int n = bl_pair_start(&app->pair, out, sizeof(out));
                if(n > 0) {
                    bl_pair_send(app, out, (size_t)n);
                    app->pair_running = true;
                    app->pair_answered = false;
                }
            }
        }
        bl_pair_draw(app);
        return;
    }
    if(!app->pair_running) return;

    uint8_t out[BL_PAIR_FRAME_MAX];
    size_t ol = 0;
    if(bl_pair_tick(&app->pair, now, out, sizeof(out), &ol)) {
        bl_pair_send(app, out, ol);
        app->pair_last_failure = bl_pair_fail_text(&app->pair);
    }

    const bl_pair_state_t st = bl_pair_state(&app->pair);
    /* One attempt per link. A failed one has sent its abort; a second later
     * drop the link so advertising resumes and the next link gets a fresh
     * attempt (a central that stays connected would otherwise hold the
     * only link). pair_failed_ms is set below on the tick it failed. */
    if(st == BL_PS_FAILED && st == before && now - app->pair_failed_ms > 1000u) {
        t->api->drop_link(t);
        return;
    }
    if(st == before) return;
    if(st == BL_PS_DONE) {
        bl_pair_keep(app);
        bl_build_menu(app); /* "Forget pairing" appears */
        notification_message(
            app->notifications, app->pair_done ? &sequence_success : &sequence_error);
    } else if(st == BL_PS_FAILED) {
        app->pair_last_failure = bl_pair_fail_text(&app->pair);
        app->pair_failed_ms = now;
        notification_message(app->notifications, &sequence_error);
    } else if(st == BL_PS_COMPARE) {
        notification_message(app->notifications, &sequence_single_vibro);
    }
    bl_pair_draw(app);
}

static void bl_forget_draw(BruceLinkApp* app) {
    Widget* w = app->pair_widget;
    widget_reset(w);
    snprintf(
        app->text, sizeof(app->text),
        "Forget the pairing with\n%s (key %02X%02X)?\nThe StickS3 keeps its copy\nuntil you forget it there.",
        app->peer[0] ? app->peer : "StickS3", app->key_id[0], app->key_id[1]);
    widget_add_string_element(w, 2, 2, AlignLeft, AlignTop, FontPrimary, "Forget pairing");
    widget_add_text_box_element(w, 2, 14, 124, 36, AlignLeft, AlignTop, app->text, false);
    widget_add_button_element(w, GuiButtonTypeLeft, "No", bl_pair_button_cb, app);
    widget_add_button_element(w, GuiButtonTypeRight, "Yes", bl_pair_button_cb, app);
}

static bool bl_custom_event_cb(void* ctx, uint32_t event) {
    BruceLinkApp* app = ctx;
    switch(event) {
    case BlEvtPairYes:
    case BlEvtPairNo: {
        if(!app->pair_mode || !app->pair_running || bl_pair_state(&app->pair) != BL_PS_COMPARE) {
            return true; /* stale button press */
        }
        uint8_t out[BL_PAIR_FRAME_MAX];
        size_t ol = 0;
        if(bl_pair_accept(&app->pair, event == BlEvtPairYes, out, sizeof(out), &ol) == BL_OK) {
            bl_pair_send(app, out, ol);
        }
        app->pair_answered = true;
        const bl_pair_state_t st = bl_pair_state(&app->pair);
        if(st == BL_PS_DONE) { /* the StickS3 had already confirmed */
            bl_pair_keep(app);
            bl_build_menu(app);
            notification_message(
                app->notifications, app->pair_done ? &sequence_success : &sequence_error);
        } else if(st == BL_PS_FAILED) {
            app->pair_last_failure = bl_pair_fail_text(&app->pair);
            app->pair_failed_ms = furi_get_tick();
        }
        bl_pair_draw(app);
        return true;
    }
    case BlEvtForgetYes:
        if(!bl_keyfile_forget()) {
            widget_reset(app->pair_widget);
            widget_add_text_scroll_element(
                app->pair_widget, 2, 2, 124, 60,
                "Could not delete the key\nfile (SD card?).\nStill paired.\n\nBack: menu");
            notification_message(app->notifications, &sequence_error);
            return true;
        }
        bl_jobc_init(&app->job); /* no key, no way to fetch a result */
        bl_files_link_down(app, true); /* nor to finish a download */
        bl_secure_zero(app->psk, sizeof(app->psk));
        memset(app->key_id, 0, sizeof(app->key_id));
        app->peer[0] = '\0';
        app->paired = false;
        app->key_state = BlKeyfileNone;
        bl_stop_transport(app);
        bl_secure_zero(&app->client, sizeof(app->client)); /* its copy of the key */
        bl_client_init(&app->client, NULL, bl_rng, NULL);
        bl_build_menu(app);
        app->screen = BlScreenNone;
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewMenu);
        return true;
    case BlEvtForgetNo:
        app->screen = BlScreenNone;
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewMenu);
        return true;
    case BlEvtFileDownload:
        if(app->file_sel >= app->files_count) return true;
        bl_xfer_begin(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewText);
        return true;
    case BlEvtFileDelete:
        app->file_confirm = true;
        bl_file_draw(app);
        return true;
    case BlEvtFileDeleteNo:
        app->file_confirm = false;
        bl_file_draw(app);
        return true;
    case BlEvtFileDeleteYes:
        app->file_confirm = false;
        if(app->file_sel < app->files_count) bl_file_delete(app);
        return true;
    default: return false;
    }
}

/* Back on the text, pairing and forget screens (they have no "previous"
 * view, so the dispatcher asks us): stop pairing if it runs, release its
 * radio, and go to the menu with no screen current. The text view used to
 * have its own "previous" callback, which left app->screen pointing at the
 * hidden view (review 2026-11-23: a job result then counted as seen). */
static bool bl_navigation_cb(void* ctx) {
    BruceLinkApp* app = ctx;
    app->pair_pending = false;
    bl_pair_end_mode(app);
    app->screen = BlScreenNone;
    view_dispatcher_switch_to_view(app->view_dispatcher, BlViewMenu);
    return true;
}

/* ----------------------------------------------------------- menus */

static void bl_submenu_cb(void* ctx, uint32_t index) {
    BruceLinkApp* app = ctx;
    switch(index) {
    case MenuConnect:
        app->screen = BlScreenConnection;
        if(app->transport != NULL && app->link_id != 0) {
            /* Already linked: show the status (and counters) instead of
             * tearing a working link down. To restart, leave the app. */
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
        /* Runs as a job: the StickS3 restarts to give the scan its radio
         * (ADR 0006). */
        bl_job_begin(app, "Wi-Fi Scan", BL_CAP_WIFI_SCAN, 0, NULL, 0);
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewText);
        break;
    }
    case MenuCapture:
        app->screen = BlScreenNone;
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewCapture);
        break;
    case MenuCaptures:
        bl_files_open(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewText);
        break;
    case MenuCapabilities:
        bl_request(app, "Capabilities", BL_MSG_CAPS_REQ, NULL, 0);
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewText);
        break;
    case MenuPair:
        app->screen = BlScreenPairing;
        widget_reset(app->pair_widget);
        widget_add_string_element(app->pair_widget, 2, 2, AlignLeft, AlignTop, FontPrimary, "Pairing");
        widget_add_text_scroll_element(app->pair_widget, 2, 16, 124, 46, "starting...");
        app->pair_pending = true; /* done by the next tick */
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewPair);
        break;
    case MenuForget:
        app->screen = BlScreenForget;
        bl_forget_draw(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, BlViewPair);
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
            "Pair once (Pair, and on the\nStickS3: BruceLink > Pair\nwith Flipper). The key is\nstored encrypted with this\nFlipper's own key.\n\n"
            "Not yet: link encryption.\nFrames are authenticated\nbut readable over the air.\n\n"
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

static uint32_t bl_back_to_files_cb(void* ctx) {
    UNUSED(ctx);
    return BlViewFiles;
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
    /* Nothing works before pairing, so that is what comes first. */
    if(!app->paired) submenu_add_item(app->submenu, "Pair", MenuPair, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "Connect", MenuConnect, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "Device Info", MenuDeviceInfo, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "Wi-Fi Scan", MenuWifiScan, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "Handshake Capture", MenuCapture, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "Captures", MenuCaptures, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "Capabilities", MenuCapabilities, bl_submenu_cb, app);
    if(app->paired) {
        submenu_add_item(app->submenu, "Pair again", MenuPair, bl_submenu_cb, app);
        submenu_add_item(app->submenu, "Forget pairing", MenuForget, bl_submenu_cb, app);
    }
    submenu_add_item(app->submenu, "Settings", MenuSettings, bl_submenu_cb, app);
    submenu_add_item(app->submenu, "About", MenuAbout, bl_submenu_cb, app);
}

static void bl_build_settings(BruceLinkApp* app) {
    variable_item_list_reset(app->settings);
    VariableItem* it = variable_item_list_add(app->settings, "Active ops", 2, bl_active_toggle_cb, app);
    variable_item_set_current_value_index(it, 0);
    variable_item_set_current_value_text(it, "Disabled");
}

static BruceLinkApp* bl_app_alloc(void) {
    BruceLinkApp* app = malloc(sizeof(BruceLinkApp));
    memset(app, 0, sizeof(*app));
    app->key_state = bl_keyfile_load(app->psk, app->key_id, app->peer);
    app->paired = app->key_state == BlKeyfileOk;
    bl_client_init(&app->client, NULL, bl_rng, NULL); /* keyed by bl_connect */

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
    view_dispatcher_add_view(app->view_dispatcher, BlViewText, widget_get_view(app->widget));

    app->settings = variable_item_list_alloc();
    bl_build_settings(app);
    view_set_previous_callback(
        variable_item_list_get_view(app->settings), bl_back_to_menu_cb);
    view_dispatcher_add_view(
        app->view_dispatcher, BlViewSettings, variable_item_list_get_view(app->settings));

    app->pair_widget = widget_alloc();
    /* no previous callback: Back reaches bl_navigation_cb */
    view_dispatcher_add_view(app->view_dispatcher, BlViewPair, widget_get_view(app->pair_widget));

    app->capture_setup = variable_item_list_alloc();
    bl_build_capture_setup(app);
    view_set_previous_callback(variable_item_list_get_view(app->capture_setup), bl_back_to_menu_cb);
    view_dispatcher_add_view(
        app->view_dispatcher, BlViewCapture, variable_item_list_get_view(app->capture_setup));

    app->files_menu = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->files_menu), bl_back_to_menu_cb);
    view_dispatcher_add_view(app->view_dispatcher, BlViewFiles, submenu_get_view(app->files_menu));

    app->file_widget = widget_alloc();
    view_set_previous_callback(widget_get_view(app->file_widget), bl_back_to_files_cb);
    view_dispatcher_add_view(app->view_dispatcher, BlViewFile, widget_get_view(app->file_widget));

    app->storage = furi_record_open(RECORD_STORAGE);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, bl_custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, bl_navigation_cb);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, bl_tick, furi_ms_to_ticks(BL_TICK_MS));
    return app;
}

static void bl_app_free(BruceLinkApp* app) {
    /* Hands the radio back to the firmware's default BLE profile. */
    bl_pair_end_mode(app);
    bl_stop_transport(app);
    bl_pair_wipe(&app->pair);
    bl_secure_zero(app->psk, sizeof(app->psk));
    bl_client_link_down(&app->client);
    bl_secure_zero(&app->client, sizeof(app->client)); /* its copy of the key */
    if(app->xfer_file != NULL) bl_xfer_finish_file(app, false); /* an unfinished download */

    view_dispatcher_remove_view(app->view_dispatcher, BlViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, BlViewCapture);
    view_dispatcher_remove_view(app->view_dispatcher, BlViewFiles);
    view_dispatcher_remove_view(app->view_dispatcher, BlViewFile);
    view_dispatcher_remove_view(app->view_dispatcher, BlViewText);
    view_dispatcher_remove_view(app->view_dispatcher, BlViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, BlViewPair);
    submenu_free(app->submenu);
    widget_free(app->widget);
    widget_free(app->pair_widget);
    variable_item_list_free(app->settings);
    variable_item_list_free(app->capture_setup);
    submenu_free(app->files_menu);
    widget_free(app->file_widget);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_STORAGE);
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
