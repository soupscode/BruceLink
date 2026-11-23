/*
 * BruceLink Flipper app — internal app state.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "brucelink_core/src/bl_session.h"
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
} BlTransportKind;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    NotificationApp* notifications;
    Submenu* submenu;
    Widget* widget;
    VariableItemList* settings;

    BlTransport* transport;
    BlTransportKind transport_kind;
    bl_session_t session; /* central */
    bool have_session;
    bool active_ops_enabled; /* operator opt-in for active ops (default off) */

    FuriTimer* poll_timer;
    FuriMutex* lock;

    char text[BL_TEXT_MAX]; /* scrollable text view contents */
    char status[64];
} BruceLinkApp;

/* defined in brucelink.c */
void bl_app_set_text(BruceLinkApp* app, const char* title, const char* body);
void bl_app_show_view(BruceLinkApp* app, BlViewId id);
