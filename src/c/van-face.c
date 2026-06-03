#include <pebble.h>

#define VEHICLE_COUNT 14
#define FRAMES_PER_VEHICLE 6
#define SETTINGS_KEY 1

// Time-of-day color groups (random + sequential modes draw from these). Slots
// tile 24h with no gaps. `tm_hour` is always 0..23 regardless of clock format.
//   02:00-12:00  green   m1..m5   (Zeex variant videos)
//   12:00-18:00  yellow  m6..m10  (Khox variant videos, render as orange)
//   18:00-02:00  blue    m11..m14 (Fee base variant; covers 18..23 and 0..1)
#define COLOR_GROUP_GREEN  0
#define COLOR_GROUP_YELLOW 1
#define COLOR_GROUP_BLUE   2
#define COLOR_GROUP_COUNT  3

// Vehicle indices (0-based, so m1 == index 0) per color group, terminated
// with -1 since the blue group only has 4 entries.
static const int COLOR_GROUP_VEHICLES[COLOR_GROUP_COUNT][5] = {
  { 0, 1, 2, 3,  4 },  // green:  m1..m5
  { 5, 6, 7, 8,  9 },  // yellow: m6..m10
  { 10, 11, 12, 13, -1 },  // blue: m11..m14
};
static const int COLOR_GROUP_SIZE[COLOR_GROUP_COUNT] = { 5, 5, 4 };

// Tap / wrist-raise puts the face into "active" mode: the time text turns
// amber so you can confirm the gesture registered. After a short idle period
// it reverts to neutral, and on that transition we pick the next vehicle
// (random / sequential modes). Rotation is always compass-driven now.
#define ACTIVE_TIMEOUT_MS  5000   // active state expires this long after last tap

#define STEP_GOAL           10000  // daily step target — for the bottom bar
#define STEP_BAR_HEIGHT     10     // px; bottom of the face. Segmented + framed.
#define STEP_SEGMENTS       10     // one notch per 1000 steps

#define V(n) { RESOURCE_ID_M##n##_01, RESOURCE_ID_M##n##_02, RESOURCE_ID_M##n##_03, \
               RESOURCE_ID_M##n##_04, RESOURCE_ID_M##n##_05, RESOURCE_ID_M##n##_06 }

static const uint32_t VEHICLE_FRAMES[VEHICLE_COUNT][FRAMES_PER_VEHICLE] = {
  V(1),  V(2),  V(3),  V(4),  V(5),  V(6),  V(7),
  V(8),  V(9),  V(10), V(11), V(12), V(13), V(14),
};

#undef V

// Vehicle mode (from Clay config):
//   0  = random (day-hash, re-rolled on each deactivation)
//   1  = sequential (cycles m1..m14, advanced on each deactivation)
//   2..15 = fixed m(Vehicle-1) — value 2 means m1, 15 means m14
typedef struct ClaySettings {
  int32_t Vehicle;
  int32_t FrameAdvanceSeconds;  // inactive rotation period; 0 = off
} ClaySettings;

static ClaySettings s_settings;

static Window *s_window;
static BitmapLayer *s_mech_layer;
static GBitmap *s_current_bitmap;
static TextLayer *s_time_layer;
static Layer *s_step_bar_layer;
static char s_time_text[8];

static int s_vehicle = 0;
static int s_frame_index = 0;
static uint32_t s_session_index = 0;  // bumps on each deactivation
static int s_last_color_group = -1;   // detect color-group rollover

static bool s_active = false;
static AppTimer *s_active_timeout = NULL;

// Pebble's libc populates tm_hour AND honors %H according to the user's
// clock-format preference, so both return 1..12 in 12h mode. To get a true
// 24h hour for our time-of-day routing, reconstruct from %I (1..12) + %p
// (AM/PM) when the preference is 12h.
static int compute_hour_24(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  int h_tm = t->tm_hour;
  int h_final;
  char buf_H[4], buf_I[4], buf_p[8];
  strftime(buf_H, sizeof(buf_H), "%H", t);
  strftime(buf_I, sizeof(buf_I), "%I", t);
  strftime(buf_p, sizeof(buf_p), "%p", t);
  int h_H = atoi(buf_H);
  int h_I = atoi(buf_I);
  bool pm = (buf_p[0] == 'P' || buf_p[0] == 'p');

  if (clock_is_24h_style()) {
    h_final = h_H;  // user wants 24h — %H is what we want
  } else {
    h_final = (h_I % 12) + (pm ? 12 : 0);  // 12h preference: reconstruct
  }
  APP_LOG(APP_LOG_LEVEL_INFO,
          "clock: tm_hour=%d %%H=%d %%I=%d %%p='%s' is24h=%d -> h=%d",
          h_tm, h_H, h_I, buf_p, clock_is_24h_style(), h_final);
  return h_final;
}

static int current_color_group(void) {
  int h = compute_hour_24();
  if (h >= 2  && h < 12) return COLOR_GROUP_GREEN;
  if (h >= 12 && h < 18) return COLOR_GROUP_YELLOW;
  return COLOR_GROUP_BLUE;  // 18..23 and 0..1
}

static int vehicle_in_group(int group, int idx_in_group) {
  int n = COLOR_GROUP_SIZE[group];
  return COLOR_GROUP_VEHICLES[group][((idx_in_group % n) + n) % n];
}

static int vehicle_for_session(uint32_t session) {
  // Random pick within the current color group; mix in yday so consecutive
  // days don't always start with the same vehicle.
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  unsigned seed = (unsigned)t->tm_yday * 2654435761u
                + (unsigned)t->tm_year
                + session * 2246822519u;
  int group = current_color_group();
  return vehicle_in_group(group, (int)(seed % COLOR_GROUP_SIZE[group]));
}

static int resolve_vehicle(void) {
  if (s_settings.Vehicle <= 0) {  // 0 = random within current color group
    return vehicle_for_session(s_session_index);
  }
  if (s_settings.Vehicle == 1) {  // sequential within current color group
    return vehicle_in_group(current_color_group(), (int)s_session_index);
  }
  // 2..15 = pin a specific vehicle (color-group-agnostic)
  int idx = s_settings.Vehicle - 2;
  if (idx < 0 || idx >= VEHICLE_COUNT) idx = 0;
  return idx;
}

static void load_settings(void) {
  s_settings.Vehicle = 0;
  s_settings.FrameAdvanceSeconds = 5;
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

static void save_settings(void) {
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

static void show_frame(int frame) {
  if (s_current_bitmap) {
    gbitmap_destroy(s_current_bitmap);
    s_current_bitmap = NULL;
  }
  s_current_bitmap = gbitmap_create_with_resource(VEHICLE_FRAMES[s_vehicle][frame]);
  if (s_current_bitmap && s_mech_layer) {
    bitmap_layer_set_bitmap(s_mech_layer, s_current_bitmap);
    layer_mark_dirty(bitmap_layer_get_layer(s_mech_layer));
  }
}

static GColor color_for_group(int group) {
  switch (group) {
    case COLOR_GROUP_GREEN:  return COLOR_FALLBACK(GColorGreen,        GColorWhite);
    case COLOR_GROUP_YELLOW: return COLOR_FALLBACK(GColorChromeYellow, GColorWhite);
    case COLOR_GROUP_BLUE:   return COLOR_FALLBACK(GColorBlueMoon,     GColorWhite);
  }
  return GColorWhite;
}

// Compass-driven frame pick: the mech rotates so its "forward" always points
// (magnetic) north as you turn your wrist. Each frame covers 360°/N of yaw.
//
// magnetic_heading is 0..TRIG_MAX_ANGLE (= 0..0xFFFF), 0 = north, increasing
// clockwise viewed from above. As you turn the watch CW, the world (and N)
// rotates CCW relative to the screen. So the mech frame index must DECREASE
// to keep its world orientation fixed — we invert the heading. Half-bucket
// bias centers frame 0 on heading 0 (range -30°..+30° rather than 0..60°).
static void compass_handler(CompassHeadingData data) {
  if (data.compass_status == CompassStatusDataInvalid) return;
  uint32_t h = (uint32_t)data.magnetic_heading & (TRIG_MAX_ANGLE - 1);
  uint32_t inv = (TRIG_MAX_ANGLE - h) & (TRIG_MAX_ANGLE - 1);
  uint32_t biased = (inv + TRIG_MAX_ANGLE / (FRAMES_PER_VEHICLE * 2))
                    & (TRIG_MAX_ANGLE - 1);
  int new_frame = (int)((biased * FRAMES_PER_VEHICLE) / TRIG_MAX_ANGLE)
                  % FRAMES_PER_VEHICLE;
  APP_LOG(APP_LOG_LEVEL_DEBUG, "compass: heading=%lu status=%d -> frame=%d",
          (unsigned long)h, (int)data.compass_status, new_frame);
  if (new_frame != s_frame_index) {
    s_frame_index = new_frame;
    show_frame(s_frame_index);
  }
}

// Step bar in Vangers-gauge style: a thin light frame around a dark inset,
// the inset divided into 10 vertical segments separated by 1 px gutters.
// Filled segments are the current color group; empty are deep-red track.
// Roughly resembles the in-game ammo / gun-charge readouts.
static void step_bar_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Outer frame
  GColor frame_c = COLOR_FALLBACK(GColorLightGray, GColorWhite);
  graphics_context_set_stroke_color(ctx, frame_c);
  graphics_draw_rect(ctx, bounds);

  // 1 px inset all around → segmented area
  GRect inner = GRect(bounds.origin.x + 1, bounds.origin.y + 1,
                      bounds.size.w - 2, bounds.size.h - 2);
  // Dark track
  graphics_context_set_fill_color(ctx, COLOR_FALLBACK(GColorBulgarianRose, GColorBlack));
  graphics_fill_rect(ctx, inner, 0, GCornerNone);

  // Steps
  int steps = 0;
  HealthServiceAccessibilityMask access = health_service_metric_accessible(
      HealthMetricStepCount, time_start_of_today(), time(NULL));
  if (access & HealthServiceAccessibilityMaskAvailable) {
    steps = (int)health_service_sum_today(HealthMetricStepCount);
  }
  if (steps > STEP_GOAL) steps = STEP_GOAL;
  int filled_segs = (steps * STEP_SEGMENTS + STEP_GOAL / 2) / STEP_GOAL;

  // One segment per 1000 steps. Gutters between segments make it read as a
  // chunky readout, not a slick continuous bar.
  int total_gutter = STEP_SEGMENTS - 1;        // 1 px between adjacent segs
  int seg_w_total = inner.size.w - total_gutter;
  GColor fill_c = color_for_group(current_color_group());
  graphics_context_set_fill_color(ctx, fill_c);
  for (int i = 0; i < filled_segs; i++) {
    int seg_x = inner.origin.x + (i * seg_w_total) / STEP_SEGMENTS + i;
    int seg_end = inner.origin.x + ((i + 1) * seg_w_total) / STEP_SEGMENTS + i;
    graphics_fill_rect(ctx, GRect(seg_x, inner.origin.y,
                                   seg_end - seg_x, inner.size.h),
                       0, GCornerNone);
  }
}

static void advance_one_frame(void) {
  s_frame_index = (s_frame_index + 1) % FRAMES_PER_VEHICLE;
  show_frame(s_frame_index);
}

static void update_time(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  strftime(s_time_text, sizeof(s_time_text), clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  text_layer_set_text(s_time_layer, s_time_text);
}

// Visible state cue. White when inactive (default), amber-ish when active.
// On the PT2 panel the difference reads clearly even without the backlight.
static void apply_active_visual(void) {
  if (!s_time_layer) return;
  GColor c = s_active
    ? COLOR_FALLBACK(GColorChromeYellow, GColorWhite)
    : COLOR_FALLBACK(GColorLightGray,     GColorWhite);
  text_layer_set_text_color(s_time_layer, c);
}

static void deactivate_cb(void *ctx) {
  s_active_timeout = NULL;
  s_active = false;
  apply_active_visual();
  // Switch to next vehicle if the mode allows it. Frame stays at whatever
  // the compass last picked, so the new vehicle shows up at the same heading.
  if (s_settings.Vehicle <= 1) {  // 0 = random, 1 = sequential
    s_session_index++;
    int next = resolve_vehicle();
    if (next != s_vehicle) {
      s_vehicle = next;
      show_frame(s_frame_index);
    }
  }
}

static void activate(void) {
  bool was_inactive = !s_active;
  s_active = true;
  if (was_inactive) apply_active_visual();
  if (s_active_timeout) {
    app_timer_reschedule(s_active_timeout, ACTIVE_TIMEOUT_MS);
  } else {
    s_active_timeout = app_timer_register(ACTIVE_TIMEOUT_MS, deactivate_cb, NULL);
  }
}

static void tap_handler(AccelAxisType axis, int32_t direction) {
  activate();
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  if (units_changed & MINUTE_UNIT) {
    update_time();
    if (s_step_bar_layer) layer_mark_dirty(s_step_bar_layer);
  }
  // Re-pick the vehicle when the color group rolls over (noon, 6 pm, 2 am)
  // — but only in random/sequential modes; pinned vehicles ignore the clock.
  if (s_settings.Vehicle <= 1) {
    int cg = current_color_group();
    if (cg != s_last_color_group) {
      s_last_color_group = cg;
      s_vehicle = resolve_vehicle();
      s_frame_index = 0;
      show_frame(s_frame_index);
      if (s_step_bar_layer) layer_mark_dirty(s_step_bar_layer);  // re-tints
    }
  }
  if ((units_changed & DAY_UNIT) && s_settings.Vehicle == 0) {
    s_session_index++;
    s_vehicle = resolve_vehicle();
    s_frame_index = 0;
    show_frame(s_frame_index);
  }
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  bool changed = false;
  Tuple *v = dict_find(iter, MESSAGE_KEY_Vehicle);
  if (v) {
    s_settings.Vehicle = v->value->int32;
    changed = true;
  }
  Tuple *r = dict_find(iter, MESSAGE_KEY_FrameAdvanceSeconds);
  if (r) {
    s_settings.FrameAdvanceSeconds = r->value->int32;
    changed = true;
  }
  if (changed) {
    save_settings();
    int next = resolve_vehicle();
    if (next != s_vehicle) {
      s_vehicle = next;
      s_frame_index = 0;
      show_frame(s_frame_index);
    }
  }
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  window_set_background_color(window, GColorBlack);

  s_last_color_group = current_color_group();
  s_vehicle = resolve_vehicle();
  s_frame_index = 0;

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

  GSize fs = s_current_bitmap ? gbitmap_get_bounds(s_current_bitmap).size
                              : (GSize){ bounds.size.w, bounds.size.h / 2 };
  int16_t time_top = fs.h;
  // Leave room at the very bottom for the step progress bar.
  int16_t time_h = bounds.size.h - time_top - STEP_BAR_HEIGHT;
  s_time_layer = text_layer_create(GRect(0, time_top, bounds.size.w, time_h));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  layer_add_child(root, text_layer_get_layer(s_time_layer));
  apply_active_visual();
  update_time();

  s_step_bar_layer = layer_create(GRect(0, bounds.size.h - STEP_BAR_HEIGHT,
                                         bounds.size.w, STEP_BAR_HEIGHT));
  layer_set_update_proc(s_step_bar_layer, step_bar_update_proc);
  layer_add_child(root, s_step_bar_layer);
}

static void window_unload(Window *window) {
  if (s_step_bar_layer) layer_destroy(s_step_bar_layer);
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
  accel_tap_service_subscribe(tap_handler);

  // Compass: filter at ~70° (slightly more than one frame width of 60°) so
  // small noise around a bucket boundary doesn't trigger frame thrash.
  compass_service_set_heading_filter((TRIG_MAX_ANGLE * 70) / 360);
  compass_service_subscribe(compass_handler);

  app_message_register_inbox_received(inbox_received_callback);
  app_message_open(128, 128);
}

static void deinit(void) {
  compass_service_unsubscribe();
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  if (s_active_timeout) app_timer_cancel(s_active_timeout);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
