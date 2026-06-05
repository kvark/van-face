#include <pebble.h>

#define VEHICLE_COUNT 14
#define FRAMES_PER_VEHICLE 6
#define SETTINGS_KEY 1
#define SESSION_INDEX_KEY 2

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

#define STEP_GOAL           10000  // daily step target

// Top-corner gauges in the style of Vangers' driving HUD: dark outer ring,
// cream inner disc, coloured wedge that sweeps clockwise from 12 o'clock
// proportional to the fill ratio. Steps gauge (top-left) sweeps black;
// charge gauge (top-right) sweeps red.
#define GAUGE_SIZE          32     // px; outer diameter
#define GAUGE_MARGIN        3      // px from the screen edge
#define TOP_STRIP_HEIGHT    (GAUGE_SIZE + GAUGE_MARGIN * 2)
#define SPIRAL_SEGMENTS     6      // (kept symbol — charge gauge uses it for granularity)

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
static Layer *s_steps_gauge_layer;
static Layer *s_spiral_layer;
static char s_time_text[8];
static int s_battery_pct = 100;
static bool s_battery_charging = false;

static int s_vehicle = 0;
static int s_frame_index = 0;
static uint32_t s_session_index = 0;  // bumps on each deactivation
static int s_last_color_group = -1;   // detect color-group rollover

static bool s_active = false;
static AppTimer *s_active_timeout = NULL;

// PebbleOS's libc populates `tm_hour` AND `strftime("%H")` according to the
// user's clock-format preference, so both return 1..12 in 12h mode. To get
// a true 24h hour for our time-of-day routing, reconstruct from %I (1..12)
// + %p (AM/PM) when the preference is 12h. See pebble-libc-12h-trap memory
// note.
static int compute_hour_24(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (clock_is_24h_style()) {
    char buf_H[4];
    strftime(buf_H, sizeof(buf_H), "%H", t);
    return atoi(buf_H);
  }
  char buf_I[4], buf_p[8];
  strftime(buf_I, sizeof(buf_I), "%I", t);
  strftime(buf_p, sizeof(buf_p), "%p", t);
  bool pm = (buf_p[0] == 'P' || buf_p[0] == 'p');
  return (atoi(buf_I) % 12) + (pm ? 12 : 0);
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
  if (s_settings.Vehicle <= 0) {
    return vehicle_for_session(s_session_index);
  }
  if (s_settings.Vehicle == 1) {
    return vehicle_in_group(current_color_group(), (int)s_session_index);
  }
  int idx = s_settings.Vehicle - 2;
  if (idx < 0 || idx >= VEHICLE_COUNT) idx = 0;
  return idx;
}

static void load_settings(void) {
  s_settings.Vehicle = 0;
  s_settings.FrameAdvanceSeconds = 5;
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  // Sanitize corrupted persist (e.g. Vehicle was read as a string pointer in
  // pre-v1.0.12 builds and stored as a huge garbage int). Both fields are
  // bounded enums; clamp anything out of range.
  if (s_settings.Vehicle < 0 || s_settings.Vehicle > 15) {
    s_settings.Vehicle = 0;
  }
  if (s_settings.FrameAdvanceSeconds < 0 || s_settings.FrameAdvanceSeconds > 3600) {
    s_settings.FrameAdvanceSeconds = 5;
  }
  if (persist_exists(SESSION_INDEX_KEY)) {
    s_session_index = (uint32_t)persist_read_int(SESSION_INDEX_KEY);
  }
}

static void save_settings(void) {
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

static void save_session_index(void) {
  persist_write_int(SESSION_INDEX_KEY, (int32_t)s_session_index);
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

// Vangers-style circular gauge: a thick dark outer ring, a cream inner disc,
// and a coloured wedge growing clockwise from 12 o'clock to indicate the fill
// ratio. Used for both top-corner gauges; only the wedge colour differs.
static void draw_circular_gauge(GContext *ctx, GRect bounds,
                                int fill_pct, GColor wedge_c) {
  if (fill_pct < 0) fill_pct = 0;
  if (fill_pct > 100) fill_pct = 100;
  GPoint c = GPoint(bounds.size.w / 2, bounds.size.h / 2);
  int outer_r = bounds.size.w / 2;
  int inner_r = outer_r - 3;  // 3 px outer ring

  // Outer dark ring
  graphics_context_set_fill_color(ctx, COLOR_FALLBACK(GColorOxfordBlue, GColorBlack));
  graphics_fill_circle(ctx, c, outer_r);

  // Cream inner disc
  graphics_context_set_fill_color(ctx, COLOR_FALLBACK(GColorPastelYellow, GColorWhite));
  graphics_fill_circle(ctx, c, inner_r);

  // Pie-wedge fill, sweeping clockwise from 12 o'clock
  if (fill_pct > 0) {
    int32_t end_angle = (int32_t)((int64_t)TRIG_MAX_ANGLE * fill_pct / 100);
    graphics_context_set_fill_color(ctx, wedge_c);
    GRect inner_box = GRect(bounds.origin.x + (outer_r - inner_r),
                            bounds.origin.y + (outer_r - inner_r),
                            inner_r * 2, inner_r * 2);
    graphics_fill_radial(ctx, inner_box, GOvalScaleModeFitCircle,
                         inner_r, 0, end_angle);
  }
}

static void steps_gauge_update_proc(Layer *layer, GContext *ctx) {
  int steps = 0;
  HealthServiceAccessibilityMask access = health_service_metric_accessible(
      HealthMetricStepCount, time_start_of_today(), time(NULL));
  if (access & HealthServiceAccessibilityMaskAvailable) {
    steps = (int)health_service_sum_today(HealthMetricStepCount);
  }
  int pct = (steps >= STEP_GOAL) ? 100 : (steps * 100 / STEP_GOAL);
  draw_circular_gauge(ctx, layer_get_bounds(layer), pct,
                      COLOR_FALLBACK(GColorBlack, GColorBlack));
}

static void spiral_update_proc(Layer *layer, GContext *ctx) {
  // Red wedge for charge — matches the Vangers HUD palette. While charging
  // the wedge brightens slightly so the indicator reads as filling.
  GColor wedge = COLOR_FALLBACK(
      s_battery_charging ? GColorRed : GColorDarkCandyAppleRed,
      GColorWhite);
  draw_circular_gauge(ctx, layer_get_bounds(layer), s_battery_pct, wedge);
}

static void battery_handler(BatteryChargeState state) {
  s_battery_pct = state.charge_percent;
  s_battery_charging = state.is_charging;
  if (s_spiral_layer) layer_mark_dirty(s_spiral_layer);
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
    save_session_index();
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
    if (s_steps_gauge_layer) layer_mark_dirty(s_steps_gauge_layer);
  }
  // Re-pick the vehicle when the color group rolls over (noon, 6 pm, 2 am)
  // — but only in random/sequential modes; pinned vehicles ignore the clock.
  // Also bump session_index so the random hash actually varies — without
  // this, a user who never taps would see the same vehicle within a group
  // forever.
  if (s_settings.Vehicle <= 1) {
    int cg = current_color_group();
    if (cg != s_last_color_group) {
      s_last_color_group = cg;
      s_session_index++;
      save_session_index();
      s_vehicle = resolve_vehicle();
      s_frame_index = 0;
      show_frame(s_frame_index);
      if (s_steps_gauge_layer) layer_mark_dirty(s_steps_gauge_layer);
    }
  }
  if ((units_changed & DAY_UNIT) && s_settings.Vehicle == 0) {
    s_session_index++;
    s_vehicle = resolve_vehicle();
    s_frame_index = 0;
    show_frame(s_frame_index);
  }
}

// Clay's <select> with numeric option values *sometimes* arrives as TUPLE_INT
// and sometimes as TUPLE_CSTRING depending on JS/webview path. Read whichever
// type the dict actually carries — reading int32 off a cstring tuple gives us
// the string pointer cast as an int and silently corrupts persist storage.
static int32_t tuple_to_int(Tuple *t, int32_t fallback) {
  if (!t) return fallback;
  switch (t->type) {
    case TUPLE_INT:
      return t->value->int32;
    case TUPLE_UINT:
      return (int32_t)t->value->uint32;
    case TUPLE_CSTRING:
      return (int32_t)atoi(t->value->cstring);
    default:
      return fallback;
  }
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  bool changed = false;
  Tuple *v = dict_find(iter, MESSAGE_KEY_Vehicle);
  if (v) {
    s_settings.Vehicle = tuple_to_int(v, s_settings.Vehicle);
    changed = true;
  }
  Tuple *r = dict_find(iter, MESSAGE_KEY_FrameAdvanceSeconds);
  if (r) {
    s_settings.FrameAdvanceSeconds = tuple_to_int(r, s_settings.FrameAdvanceSeconds);
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

  // Mech sits below the top-strip so the corner gauges have clear backdrop.
  if (s_current_bitmap) {
    GSize fs = gbitmap_get_bounds(s_current_bitmap).size;
    GRect mech_rect = (GRect){
      .origin = { (bounds.size.w - fs.w) / 2, TOP_STRIP_HEIGHT },
      .size   = fs,
    };
    layer_set_frame(bitmap_layer_get_layer(s_mech_layer), mech_rect);
  }

  GSize fs = s_current_bitmap ? gbitmap_get_bounds(s_current_bitmap).size
                              : (GSize){ bounds.size.w, bounds.size.h / 2 };
  int16_t time_top = TOP_STRIP_HEIGHT + fs.h;
  int16_t time_h = bounds.size.h - time_top;
  // BITHAM_42 needs ≥42 px; smaller screens fall back to GOTHIC_28.
  const char *font_key = (time_h >= 42) ? FONT_KEY_BITHAM_42_BOLD : FONT_KEY_GOTHIC_28_BOLD;
  s_time_layer = text_layer_create(GRect(0, time_top, bounds.size.w, time_h));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_font(s_time_layer, fonts_get_system_font(font_key));
  layer_add_child(root, text_layer_get_layer(s_time_layer));
  apply_active_visual();
  update_time();

  // Top-left: daily steps gauge (black wedge fills as you walk).
  s_steps_gauge_layer = layer_create(GRect(GAUGE_MARGIN, GAUGE_MARGIN,
                                           GAUGE_SIZE, GAUGE_SIZE));
  layer_set_update_proc(s_steps_gauge_layer, steps_gauge_update_proc);
  layer_add_child(root, s_steps_gauge_layer);

  // Top-right: battery (red wedge — Vangers HUD style).
  s_spiral_layer = layer_create(GRect(
      bounds.size.w - GAUGE_SIZE - GAUGE_MARGIN, GAUGE_MARGIN,
      GAUGE_SIZE, GAUGE_SIZE));
  layer_set_update_proc(s_spiral_layer, spiral_update_proc);
  layer_add_child(root, s_spiral_layer);
}

static void window_unload(Window *window) {
  if (s_spiral_layer) layer_destroy(s_spiral_layer);
  if (s_steps_gauge_layer) layer_destroy(s_steps_gauge_layer);
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

  // Seed the initial battery reading and subscribe for changes.
  BatteryChargeState bs = battery_state_service_peek();
  s_battery_pct = bs.charge_percent;
  s_battery_charging = bs.is_charging;
  battery_state_service_subscribe(battery_handler);

  app_message_register_inbox_received(inbox_received_callback);
  app_message_open(128, 128);
}

static void deinit(void) {
  battery_state_service_unsubscribe();
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
