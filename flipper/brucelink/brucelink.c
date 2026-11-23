/* BruceLink native UI stub. No BLE connection implemented. */
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <gui/canvas.h>

typedef struct {
    FuriMessageQueue* queue;
} BruceLinkApp;

static void draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 14, "BruceLink");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 30, "Transport disconnected");
    canvas_draw_str(canvas, 2, 44, "BLE bridge: TODO");
    canvas_draw_str(canvas, 2, 60, "Back to exit");
}

static void input_callback(InputEvent* event, void* context) {
    BruceLinkApp* app = context;
    furi_message_queue_put(app->queue, event, 0);
}

int32_t brucelink_app(void* p) {
    UNUSED(p);
    BruceLinkApp app = {.queue = furi_message_queue_alloc(8, sizeof(InputEvent))};
    ViewPort* viewport = view_port_alloc();
    view_port_draw_callback_set(viewport, draw_callback, &app);
    view_port_input_callback_set(viewport, input_callback, &app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, viewport, GuiLayerFullscreen);
    InputEvent event;
    while(true) {
        if(furi_message_queue_get(app.queue, &event, FuriWaitForever) == FuriStatusOk &&
           event.type == InputTypeShort && event.key == InputKeyBack) break;
    }
    gui_remove_view_port(gui, viewport);
    view_port_free(viewport);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app.queue);
    return 0;
}
