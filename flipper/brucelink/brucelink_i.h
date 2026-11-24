/*
 * BruceLink Flipper app — internal app state.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "../../shared/brucelink_core/src/bl_client.h"
#include "../../shared/brucelink_core/src/bl_job.h"
#include "../../shared/brucelink_core/src/bl_pair.h"
#include "bl_keyfile.h"
#include "transport/bl_transport.h"

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>

#define BL_TEXT_MAX 512

typedef enum {
    BlViewMenu,
    BlViewText,
    BlViewSettings,
    BlViewPair, /* pairing / forget screens; Back is handled by the app */
} BlViewId;

/* Custom events: widget buttons are only allowed to post these; the
 * widget is rebuilt from the event callback, never from inside its own
 * button callback. */
typedef enum {
    BlEvtPairYes = 1,
    BlEvtPairNo,
    BlEvtForgetYes,
    BlEvtForgetNo,
} BlCustomEvent;

/* What the text view is currently showing, so asynchronous updates
 * (link changes, responses, timeouts) land on the right screen. */
typedef enum {
    BlScreenNone,
    BlScreenConnection,
    BlScreenRequest, /* waiting for / showing a response */
    BlScreenStatic, /* About */
    BlScreenPairing,
    BlScreenForget,
    BlScreenJob, /* a job on the StickS3 (ADR 0006) */
} BlScreen;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    NotificationApp* notifications;
    Submenu* submenu;
    Widget* widget;
    VariableItemList* settings;
    Widget* pair_widget;

    BlTransport* transport; /* BLE; NULL until Connect or Pair */
    bl_client_t client; /* session initiator state machine */
    uint32_t link_id; /* last transport link id seen (0 = none) */
    bool connect_pending; /* Connect pressed: start the transport on the next tick */
    const char* shown_status; /* transport status text last drawn (compared by pointer) */
    char shown_diag[96]; /* transport counters last drawn */
    bool active_ops_enabled; /* operator opt-in for active ops (default off) */

    /* Stored pairing (bl_keyfile). Loaded at start; the key stays in RAM
     * while the app runs and is wiped on exit. */
    BlKeyfileResult key_state;
    bool paired;
    uint8_t psk[BL_KEY_LEN];
    uint8_t key_id[BL_PAIR_KEY_ID_LEN];
    char peer[BL_PAIR_NAME_MAX + 1];

    /* Pairing mode: one bl_pair attempt per BLE link. */
    bool pair_mode;
    bl_pair_t pair;
    bool pair_running; /* pair holds an attempt for the current link */
    bool pair_answered; /* the user answered this attempt's code */
    bool pair_done; /* a pairing succeeded and was saved in this pairing session */
    const char* pair_last_failure;

    /* All protocol work runs on the app thread via the ViewDispatcher
     * tick event (never in a FuriTimer callback: the timer service task
     * has a 1 KB stack). No locking needed. */

    /* The current job (ADR 0006). Followed on every tick whatever screen
     * is up, so leaving its screen does not lose it. */
    bl_jobc_t job;
    const char* job_title;
    uint32_t job_drawn_s; /* elapsed seconds last drawn */
    char job_text[BL_JOB_RESULT_MAX + 64]; /* the finished job's screen */
    bool job_unseen; /* finished while its screen was not up */

    BlScreen screen;
    const char* req_title; /* title of the request being shown */
    char text[BL_TEXT_MAX]; /* scrollable text view contents */
} BruceLinkApp;
