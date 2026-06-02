#include <pebble.h>

#define VEHICLE_COUNT 14
#define FRAMES_PER_VEHICLE 6
#define SETTINGS_KEY 1

#define V(n) { RESOURCE_ID_M##n##_01, RESOURCE_ID_M##n##_02, RESOURCE_ID_M##n##_03, \
               RESOURCE_ID_M##n##_04, RESOURCE_ID_M##n##_05, RESOURCE_ID_M##n##_06 }

// Resource IDs grouped per vehicle. The day-of-year picks the row (when
// Vehicle == 0), the frame index walks across.
static const uint32_t VEHICLE_FRAMES[VEHICLE_COUNT][FRAMES_PER_VEHICLE] = {
  V(1),  V(2),  V(3),  V(4),  V(5),  V(6),  V(7),
  V(8),  V(9),  V(10), V(11), V(12), V(13), V(14),
};

#undef V

// User-controlled config, mirrored from the Clay config page. Stored as a
// single blob in persist_storage so adding fields is a one-byte cost.
typedef struct ClaySettings {
  int32_t Vehicle;              // 0 = random/daily, 1..14 = pin specific mN
  int32_t FrameAdvanceSeconds;  // 0 = no rotation
} ClaySettings;

static ClaySettings s_settings;

static Window *s_window;
static BitmapLayer *s_mech_layer;
static GBitmap *s_current_bitmap;
static TextLayer *s_time_layer;
static char s_time_text[8];

static int s_vehicle = 0;
static int s_frame_index = 0;
static int s_seconds_since_advance = 0;

static int vehicle_for_today(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  // Mix yday with year so consecutive days don't pick adjacent vehicles.
  unsigned seed = (unsigned)t->tm_yday * 2654435761u + (unsigned)t->tm_year;
  return (int)(seed % VEHICLE_COUNT);
}

static int resolve_vehicle(void) {
  if (s_settings.Vehicle <= 0 || s_settings.Vehicle > VEHICLE_COUNT) {
    return vehicle_for_today();
  }
  return s_settings.Vehicle - 1;  // config exposes 1-indexed mN
}

static void load_settings(void) {
  s_settings.Vehicle = 0;
  s_settings.FrameAdvanceSeconds = 5;
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

static void save_settings(void) {
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

// Load one frame's bitmap, freeing the previous one. Only one bitmap is held
// at a time so we don't blow the 128 KB heap on emery (each 200×150 paletted
// bitmap is a few KB, but holding all 84 would still be too much).
static void show_frame(int frame) {
  if (s_current_bitmap) {
    gbitmap_destroy(s_current_bitmap);
    s_current_bitmap = NULL;
  }
  s_current_bitmap = gbitmap_create_with_resource(VEHICLE_FRAMES[s_vehicle][frame]);
  if (s_current_bitmap) {
    bitmap_layer_set_bitmap(s_mech_layer, s_current_bitmap);
    layer_mark_dirty(bitmap_layer_get_layer(s_mech_layer));
  }
}

static void update_time(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  strftime(s_time_text, sizeof(s_time_text), clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  text_layer_set_text(s_time_layer, s_time_text);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  if (units_changed & MINUTE_UNIT) update_time();
  if ((units_changed & DAY_UNIT) && s_settings.Vehicle == 0) {
    s_vehicle = resolve_vehicle();
    s_frame_index = 0;
    show_frame(s_frame_index);
  }
  if (s_settings.FrameAdvanceSeconds > 0 &&
      ++s_seconds_since_advance >= s_settings.FrameAdvanceSeconds) {
    s_seconds_since_advance = 0;
    s_frame_index = (s_frame_index + 1) % FRAMES_PER_VEHICLE;
    show_frame(s_frame_index);
  }
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "inbox_received fired");
  bool changed = false;
  Tuple *v = dict_find(iter, MESSAGE_KEY_Vehicle);
  if (v) {
    s_settings.Vehicle = v->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "  Vehicle = %ld", (long)s_settings.Vehicle);
    changed = true;
  }
  Tuple *r = dict_find(iter, MESSAGE_KEY_FrameAdvanceSeconds);
  if (r) {
    s_settings.FrameAdvanceSeconds = r->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "  FrameAdvanceSeconds = %ld", (long)s_settings.FrameAdvanceSeconds);
    changed = true;
  }
  if (changed) {
    save_settings();
    s_vehicle = resolve_vehicle();
    APP_LOG(APP_LOG_LEVEL_INFO, "  applied vehicle=%d", s_vehicle);
    s_frame_index = 0;
    s_seconds_since_advance = 0;
    show_frame(s_frame_index);
  }
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  window_set_background_color(window, GColorBlack);

  s_vehicle = resolve_vehicle();
  s_frame_index = 0;

  // The mech frames have the source video's ≈4:3 aspect (200×150 here). Place
  // them top-aligned so the watch's spare vertical pixels at the bottom hold
  // the time text without overlap.
  s_mech_layer = bitmap_layer_create(bounds);
  bitmap_layer_set_compositing_mode(s_mech_layer, GCompOpSet);
  layer_add_child(root, bitmap_layer_get_layer(s_mech_layer));
  show_frame(s_frame_index);

  if (s_current_bitmap) {
    GSize fs = gbitmap_get_bounds(s_current_bitmap).size;
    GRect mech_rect = (GRect){
      .origin = { (bounds.size.w - fs.w) / 2, 0 },
      .size   = fs,
    };
    layer_set_frame(bitmap_layer_get_layer(s_mech_layer), mech_rect);
  }

  // Time fills the strip below the mech.
  GSize fs = s_current_bitmap ? gbitmap_get_bounds(s_current_bitmap).size
                              : (GSize){ bounds.size.w, bounds.size.h / 2 };
  int16_t time_top = fs.h;
  int16_t time_h = bounds.size.h - time_top;
  s_time_layer = text_layer_create(GRect(0, time_top, bounds.size.w, time_h));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorWhite);
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  update_time();
}

static void window_unload(Window *window) {
  text_layer_destroy(s_time_layer);
  bitmap_layer_destroy(s_mech_layer);
  if (s_current_bitmap) gbitmap_destroy(s_current_bitmap);
}

static void init(void) {
  load_settings();

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);

  app_message_register_inbox_received(inbox_received_callback);
  app_message_open(128, 128);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
