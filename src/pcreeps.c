#include <pebble.h>
#include <pebble-gbitmap-lib/gbitmap_tools.h>

#define MAX_FRAMES 20

static Window *s_window;
static TextLayer *s_text_layer;
static BitmapLayer *s_bg_layer;
static char s_room_name[32];
static GBitmap *s_terrain_bitmap;
static GBitmap *s_scaled_terrain;
struct frame {
    uint16_t len;
    uint8_t *data;
};
static struct frame frames[MAX_FRAMES];
static int frame_total = 0;
static int cur_frame = 0;
static bool animating = true;
static bool loading = true;
static int room_index = 0;
static int room_count = 0;

/* Replays an animation of the room history, one frame per second.
 * An initial image (keyframe) is sent as the background terrain. Animation
 * farmes are arrays of direct patches of pixel N at position X/Y applied
 * consecutively to the same preimage.
 * Saves a lot of space.
 * Can still overflow/break if more than 30% of an image changes within a
 * single frame.
 */
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    if (!s_terrain_bitmap) return;

    struct frame *f = &frames[cur_frame];
    uint8_t *gdata = gbitmap_get_data(s_terrain_bitmap);
    for (int x = 0; x < f->len; x += 3) {
        uint8_t pixel = f->data[x];
        uint8_t xpos = f->data[x+1];
        uint8_t ypos = f->data[x+2];
        //APP_LOG(APP_LOG_LEVEL_INFO, "pixel update: p:%u, x:%u, y:%u", pixel, xpos, ypos);
        gdata[xpos + (ypos * 50)] = pixel;
    }
    if (s_scaled_terrain) {
        gbitmap_destroy(s_scaled_terrain);
    }
    s_scaled_terrain = scaleBitmap(s_terrain_bitmap, 250, 250);
    bitmap_layer_set_bitmap(s_bg_layer, s_scaled_terrain);
    cur_frame++;
    if (cur_frame == MAX_FRAMES) {
        //APP_LOG(APP_LOG_LEVEL_DEBUG, "finished animation");
        tick_timer_service_unsubscribe();
    }
    //APP_LOG(APP_LOG_LEVEL_DEBUG, "updated frame");
}

/* Initializes the original-sized 50x50 image we'll be animating from.
 * The JS side creates the image from API data and sends us whole-bytes to
 * drop directly into a gbitmap's data section.
 */
static void create_terrain_bitmap(uint8_t *data) {
    // Kill any animation that might already be playing.
    tick_timer_service_unsubscribe();
    animating = false;
    // FIXME: Move dealloc to function.
    // Clear existing data before we load new ones.
    if (s_terrain_bitmap) {
        gbitmap_destroy(s_terrain_bitmap);
        s_terrain_bitmap = NULL;
    }
    if (s_scaled_terrain) {
        gbitmap_destroy(s_scaled_terrain);
        s_scaled_terrain = NULL;
    }
    for (int x = 0; x < 20; x++) {
        if (frames[x].data) {
            free(frames[x].data);
            frames[x].data = NULL;
            frames[x].len = 0;
        }
    }

    //APP_LOG(APP_LOG_LEVEL_INFO, "create_terrain_bitmap called: %u", data[0]);
    s_terrain_bitmap = gbitmap_create_blank(GSize(50, 50), GBitmapFormat8Bit);
    uint8_t *gdata = gbitmap_get_data(s_terrain_bitmap);
    memcpy(gdata, data, 2500);
    s_scaled_terrain = scaleBitmap(s_terrain_bitmap, 250, 250);
    bitmap_layer_set_bitmap(s_bg_layer, s_scaled_terrain);
    return;
}

/* MESSAGE HANDLERS */
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
    Tuple *tupper = dict_read_first(iterator);
    //APP_LOG(APP_LOG_LEVEL_INFO, "arrived at inbox callback");
    while (tupper) {
        //APP_LOG(APP_LOG_LEVEL_INFO, "tupper key: %lu", (unsigned long)tupper->key);
        /* Can't do a switch statement since these are variables */
        if (tupper->key == MESSAGE_KEY_ROOMNAME) {
            if (tupper->length <= 32) {
                memcpy(s_room_name, tupper->value->cstring, tupper->length);
                text_layer_set_text(s_text_layer, s_room_name);
            }
        } else if (tupper->key == MESSAGE_KEY_ROOMCOUNT) {
            room_count = tupper->value->int32;
            //APP_LOG(APP_LOG_LEVEL_DEBUG, "received room count: %d", room_count);
        } else if (tupper->key == MESSAGE_KEY_TERRAIN) {
            if (tupper->length != 2500) {
                APP_LOG(APP_LOG_LEVEL_ERROR, "received lopsided terrain size: %d", tupper->length);
            } else {
                create_terrain_bitmap(tupper->value->data);
            }
        } else if (tupper->key == MESSAGE_KEY_FRAME) {
            uint8_t index = tupper->value->data[0];
            uint8_t *fdata = malloc(sizeof(uint8_t) * tupper->length-1);
            memcpy(fdata, tupper->value->data+1, tupper->length-1);
            frames[index].data = fdata;
            frames[index].len = tupper->length-1;
            if (frame_total < index) {
                frame_total = index;
            }
            // FIXME: send an end-cap message to confirm finished loading.
            if (index >= 18) {
                loading = false;
                // TODO: Need to keep a copy of original terrain data if we
                // want to be able to replay the animation. Would instead set
                // this to false in the tick_handler above.
                animating = false;
            }
            //APP_LOG(APP_LOG_LEVEL_INFO, "received frame: %d", frame_total);
        }
        tupper = dict_read_next(iterator);
    }
}

// TODO: Schedule retries/reliability.
static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped!");
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed!");
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Outbox send success!");
}

/* CLICK HANDLERS */

/* Play the room history animation if select is pushed */
static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (!animating) {
        cur_frame = 0;
        tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
        animating = true;
    }
}

/* TODO: Would be much faster for many-roomed instances to bring up a
 * scrollback to select from. This is stupid for simplicity right now
 */
static void send_room_switch(const uint8_t roomid) {
    DictionaryIterator *out;

    AppMessageResult res = app_message_outbox_begin(&out);

    if (res == APP_MSG_OK) {
        dict_write_uint8(out, MESSAGE_KEY_SWITCH, roomid);
        res = app_message_outbox_send();
        if (res != APP_MSG_OK) {
            APP_LOG(APP_LOG_LEVEL_ERROR, "error sending outbox");
        }
    } else {
        APP_LOG(APP_LOG_LEVEL_ERROR, "error preparing outbox");
    }
}

static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (!loading) {
        if (room_index == 0) {
            room_index = room_count-1;
        } else {
            room_index--;
        }
        send_room_switch(room_index);
        text_layer_set_text(s_text_layer, "loading...");
        loading = true;
    } else {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "up pressed, but still loading");
    }
}

static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (!loading) {
        if (room_index == room_count-1) {
            room_index = 0;
        } else {
            room_index++;
        }
        send_room_switch(room_index);
        text_layer_set_text(s_text_layer, "loading...");
        loading = true;
    } else {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "down pressed, but still loading");
    }
}

static void prv_click_config_provider(void *context) {
    window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
    window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
    window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
}

static void prv_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_text_layer = text_layer_create(GRect(0, PBL_IF_ROUND_ELSE(16, 6), bounds.size.w, 20));
    text_layer_set_text(s_text_layer, "loading...");
    text_layer_set_text_alignment(s_text_layer, GTextAlignmentCenter);
    text_layer_set_background_color(s_text_layer, GColorBlack);
    text_layer_set_text_color(s_text_layer, GColorWhite);

    s_bg_layer = bitmap_layer_create(GRect(0, 13, bounds.size.w, bounds.size.h));

    // Add them as child layers to the Window's root layer
    layer_add_child(window_layer, text_layer_get_layer(s_text_layer));
    layer_add_child(window_layer, bitmap_layer_get_layer(s_bg_layer));
}

static void prv_window_unload(Window *window) {
    text_layer_destroy(s_text_layer);
    bitmap_layer_destroy(s_bg_layer);
}

static void prv_init(void) {
    s_window = window_create();
    window_set_click_config_provider(s_window, prv_click_config_provider);
    window_set_window_handlers(s_window, (WindowHandlers) {
        .load = prv_window_load,
        .unload = prv_window_unload,
    });
    const bool animated = true;

    window_set_background_color(s_window, GColorLightGray);

    // Show the window with animation.
    window_stack_push(s_window, animated);

    // Register callbacks
    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    app_message_register_outbox_sent(outbox_sent_callback);
    // Open AppMessage
    const int inbox_size = 2532;
    const int outbox_size = 128;
    app_message_open(inbox_size, outbox_size);

}

static void prv_deinit(void) {
    window_destroy(s_window);
    if (s_terrain_bitmap) {
        gbitmap_destroy(s_terrain_bitmap);
    }
    if (s_scaled_terrain) {
        gbitmap_destroy(s_scaled_terrain);
    }
    for (int x = 0; x < 20; x++) {
        if (frames[x].data) {
            free(frames[x].data);
        }
    }
}

int main(void) {
    prv_init();

    APP_LOG(APP_LOG_LEVEL_DEBUG, "pcreeps started");
    // Been using this to quickly get pebble-side info on a color since the
    // web picker only has HTML values.
    //APP_LOG(APP_LOG_LEVEL_DEBUG, "colors: %u %u", GColorRed.argb, GColorCeleste.argb);

    app_event_loop();
    prv_deinit();
}
