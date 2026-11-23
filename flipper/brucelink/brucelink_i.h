/*
 * BruceLink Flipper app — internal app state.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "../../shared/brucelink_core/src/bl_client.h"
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
} BlViewId;

typedef enum {
    BlTransportLoopback,
    BlTransportBle,
    BlTransportCount,
} BlTransportKind;

/* What the text view is currently showing, so asynchronous updates
 * (link changes, responses, timeouts) land on the right screen. */
typedef enum {
    BlScreenNone,
    BlScreenConnection,
    BlScreenRequest, /* waiting for / showing a response */
    BlScreenStatic, /* About */
} BlScreen;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    NotificationApp* notifications;
    Submenu* submenu;
    Widget* widget;
    VariableItemList* settings;

    BlTransport* transport; /* NULL until Connect */
    BlTransportKind transport_kind; /* chosen in Settings */
    bl_client_t client; /* session initiator state machine */
    uint32_t link_id; /* last transport link id seen (0 = none) */
    bool connect_pending; /* Connect pressed: start the transport on the next tick */
    bool active_ops_enabled; /* operator opt-in for active ops (default off) */

    /* All protocol work runs on the app thread via the ViewDispatcher
     * tick event (never in a FuriTimer callback: the timer service task
     * has a 1 KB stack). No locking needed. */

    BlScreen screen;
    const char* req_title; /* title of the request being shown */
    char text[BL_TEXT_MAX]; /* scrollable text view contents */
} BruceLinkApp;
