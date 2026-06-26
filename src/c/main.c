#include <pebble.h>

extern uint32_t MESSAGE_KEY_PrimaryColor;
extern uint32_t MESSAGE_KEY_Canvas;
extern uint32_t MESSAGE_KEY_TempUnit;
extern uint32_t MESSAGE_KEY_HOURLY_TEMP_0;
extern uint32_t MESSAGE_KEY_HOURLY_TEMP_1;
extern uint32_t MESSAGE_KEY_HOURLY_TEMP_2;
extern uint32_t MESSAGE_KEY_HOURLY_PRECIP_0;
extern uint32_t MESSAGE_KEY_HOURLY_PRECIP_1;
extern uint32_t MESSAGE_KEY_HOURLY_PRECIP_2;
extern uint32_t MESSAGE_KEY_HOURLY_UV_0;
extern uint32_t MESSAGE_KEY_HOURLY_UV_1;
extern uint32_t MESSAGE_KEY_HOURLY_UV_2;

enum Canvas {
  CANVAS_PAPER = 0,
  CANVAS_INK = 1
};

static Window *s_main_window;
static TextLayer *s_time_layer;
static Layer *s_status_layer;
static Layer *s_vitals_layer;
static Layer *s_hourly_layer;
static Layer *s_window_layer;

static GBitmap *s_sneaker_bitmap;
static bool s_bt_app_connected;
static bool s_bt_radio_connected;

static GFont s_font_16;
static GFont s_font_20;
static GFont s_font_24;
static GFont s_font_clock;

#define STATUS_HEIGHT 24
#define VITALS_HEIGHT 30
#define HOURLY_HEIGHT 90
#define CLOCK_HEIGHT 60
#define WEATHER_POLL_MINUTES 30
#define SETTINGS_KEY 1
#define WEATHER_KEY 2

// New fields must be appended to the end — never insert or reorder.
// load_settings() reads stored bytes and zero-fills the rest, so
// existing users keep their settings when the struct grows.
typedef struct {
  GColor primary_color;
  uint8_t canvas;
} Settings;

static Settings s_settings;

static GColor fg_color() {
  return s_settings.canvas == CANVAS_INK ? GColorWhite : s_settings.primary_color;
}

static GColor bg_color() {
  return s_settings.canvas == CANVAS_INK ? s_settings.primary_color : GColorWhite;
}

static void default_settings() {
  s_settings.primary_color = GColorBlack;
  s_settings.canvas = CANVAS_PAPER;
}

static void load_settings() {
  default_settings();
  if (persist_exists(SETTINGS_KEY)) {
    int stored = persist_get_size(SETTINGS_KEY);
    if (stored > 0 && (size_t)stored <= sizeof(s_settings)) {
      persist_read_data(SETTINGS_KEY, &s_settings, stored);
    }
  }
}

static void save_settings() {
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

// Hourly weather for the current hour and the next two. Each value is the
// raw integer as a string, or empty when unavailable.
typedef struct {
  char hour_temp[3][6];
  char hour_precip[3][6];
  char hour_uv[3][6];
  bool loaded;
} WeatherCache;

static WeatherCache s_weather;

static void load_weather() {
  memset(&s_weather, 0, sizeof(s_weather));
  if (persist_exists(WEATHER_KEY)) {
    int stored = persist_get_size(WEATHER_KEY);
    if (stored > 0 && (size_t)stored <= sizeof(s_weather)) {
      persist_read_data(WEATHER_KEY, &s_weather, stored);
    }
  }
}

static void save_weather() {
  persist_write_data(WEATHER_KEY, &s_weather, sizeof(s_weather));
}

static int s_battery_level;
static char s_date_buffer[16];
static char s_battery_buffer[8];
static char s_steps_buffer[12];
static char s_hr_buffer[8];

static char s_hour_label[3][8];
static char s_hour_temp_disp[3][8];
static char s_hour_rain_disp[3][8];
static char s_hour_uv_disp[3][8];

static void update_status_buffer(struct tm *t);
static void update_display();

static void update_hourly_buffers() {
  for (int i = 0; i < 3; i++) {
    if (s_weather.loaded && s_weather.hour_temp[i][0]) {
      snprintf(s_hour_temp_disp[i], sizeof(s_hour_temp_disp[i]), "%s°", s_weather.hour_temp[i]);
    } else {
      snprintf(s_hour_temp_disp[i], sizeof(s_hour_temp_disp[i]), "--");
    }
    if (s_weather.loaded && s_weather.hour_precip[i][0]) {
      snprintf(s_hour_rain_disp[i], sizeof(s_hour_rain_disp[i]), "%s%%", s_weather.hour_precip[i]);
    } else {
      snprintf(s_hour_rain_disp[i], sizeof(s_hour_rain_disp[i]), "--");
    }
    if (s_weather.loaded && s_weather.hour_uv[i][0]) {
      snprintf(s_hour_uv_disp[i], sizeof(s_hour_uv_disp[i]), "UV%s", s_weather.hour_uv[i]);
    } else {
      snprintf(s_hour_uv_disp[i], sizeof(s_hour_uv_disp[i]), "UV-");
    }
  }
}

static void read_hourly_int(DictionaryIterator *iter, uint32_t key, char *dest, size_t size) {
  Tuple *t = dict_find(iter, key);
  if (!t) return;
  int v = (int)t->value->int32;
  if (v <= -100) {
    dest[0] = '\0';  // sentinel for unavailable
  } else {
    snprintf(dest, size, "%d", v);
  }
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  bool weather_changed = false;

  uint32_t temp_keys[3] = {MESSAGE_KEY_HOURLY_TEMP_0, MESSAGE_KEY_HOURLY_TEMP_1, MESSAGE_KEY_HOURLY_TEMP_2};
  uint32_t precip_keys[3] = {MESSAGE_KEY_HOURLY_PRECIP_0, MESSAGE_KEY_HOURLY_PRECIP_1, MESSAGE_KEY_HOURLY_PRECIP_2};
  uint32_t uv_keys[3] = {MESSAGE_KEY_HOURLY_UV_0, MESSAGE_KEY_HOURLY_UV_1, MESSAGE_KEY_HOURLY_UV_2};

  for (int i = 0; i < 3; i++) {
    if (dict_find(iterator, temp_keys[i]) || dict_find(iterator, precip_keys[i]) ||
        dict_find(iterator, uv_keys[i])) {
      weather_changed = true;
    }
    read_hourly_int(iterator, temp_keys[i], s_weather.hour_temp[i], sizeof(s_weather.hour_temp[i]));
    read_hourly_int(iterator, precip_keys[i], s_weather.hour_precip[i], sizeof(s_weather.hour_precip[i]));
    read_hourly_int(iterator, uv_keys[i], s_weather.hour_uv[i], sizeof(s_weather.hour_uv[i]));
  }

  if (weather_changed) {
    s_weather.loaded = true;
    save_weather();
    update_hourly_buffers();
    if (s_hourly_layer) {
      layer_mark_dirty(s_hourly_layer);
    }
  }

  // Settings
  Tuple *color_t = dict_find(iterator, MESSAGE_KEY_PrimaryColor);
  Tuple *canvas_t = dict_find(iterator, MESSAGE_KEY_Canvas);

  bool settings_changed = false;
  if (color_t) {
    s_settings.primary_color = GColorFromHEX(color_t->value->int32);
    settings_changed = true;
  }
  if (canvas_t) {
    s_settings.canvas = (uint8_t)canvas_t->value->int32;
    settings_changed = true;
  }
  if (settings_changed) {
    save_settings();
    window_set_background_color(s_main_window, bg_color());
    update_display();
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped!");
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed!");
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Outbox send success!");
}

static struct tm *get_time(struct tm *t) {
  if (t) return t;
  time_t now = time(NULL);
  return localtime(&now);
}

static void update_time(struct tm *tick_time) {
  struct tm *t = get_time(tick_time);

  static char s_time_buffer[8];
  strftime(s_time_buffer, sizeof(s_time_buffer), clock_is_24h_style() ?
                                                    "%H:%M" : "%I:%M", t);
  char *display = s_time_buffer;
  if (display[0] == '0') display++;
  text_layer_set_text(s_time_layer, display);
}

static void update_hour_labels(struct tm *tick_time) {
  struct tm *t = get_time(tick_time);
  bool h24 = clock_is_24h_style();
  for (int i = 0; i < 3; i++) {
    if (i == 0) {
      snprintf(s_hour_label[i], sizeof(s_hour_label[i]), "NOW");
      continue;
    }
    int h = (t->tm_hour + i) % 24;
    if (h24) {
      snprintf(s_hour_label[i], sizeof(s_hour_label[i]), "%d", h);
    } else {
      int h12 = h % 12;
      if (h12 == 0) h12 = 12;
      snprintf(s_hour_label[i], sizeof(s_hour_label[i]), "%d%s", h12, h < 12 ? "AM" : "PM");
    }
  }
}

static void update_heart_rate() {
  HealthValue v = health_service_peek_current_value(HealthMetricHeartRateBPM);
  if (v > 0) {
    snprintf(s_hr_buffer, sizeof(s_hr_buffer), "%d", (int)v);
  } else {
    snprintf(s_hr_buffer, sizeof(s_hr_buffer), "--");
  }
  if (s_vitals_layer) {
    layer_mark_dirty(s_vitals_layer);
  }
}

static void health_handler(HealthEventType event, void *context) {
  if (event == HealthEventHeartRateUpdate || event == HealthEventSignificantUpdate ||
      event == HealthEventMovementUpdate) {
    update_heart_rate();
    update_status_buffer(NULL);
  }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time(tick_time);
  update_hour_labels(tick_time);
  update_status_buffer(tick_time);
  update_heart_rate();

  // Request weather update every 30 minutes (only if phone is connected)
  if (tick_time->tm_min % WEATHER_POLL_MINUTES == 0 &&
      connection_service_peek_pebble_app_connection()) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
      dict_write_uint8(iter, 0, 0);
      app_message_outbox_send();
    }
  }
}

static void update_status_buffer(struct tm *tick_time) {
  struct tm *t = get_time(tick_time);
  static const char *days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char *months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  snprintf(s_date_buffer, sizeof(s_date_buffer), "%s %s %d",
           days[t->tm_wday], months[t->tm_mon], t->tm_mday);
  snprintf(s_battery_buffer, sizeof(s_battery_buffer), "%d%%", s_battery_level);

  int steps = (int)health_service_sum_today(HealthMetricStepCount);
  if (steps >= 1000) {
    snprintf(s_steps_buffer, sizeof(s_steps_buffer), "%d.%dk", steps / 1000, (steps % 1000) / 100);
  } else {
    snprintf(s_steps_buffer, sizeof(s_steps_buffer), "%d", steps);
  }

  if (s_status_layer) {
    layer_mark_dirty(s_status_layer);
  }
  if (s_vitals_layer) {
    layer_mark_dirty(s_vitals_layer);
  }
}

static void battery_callback(BatteryChargeState state) {
  s_battery_level = state.charge_percent;
  update_status_buffer(NULL);
}

static void bt_app_callback(bool connected) {
  s_bt_app_connected = connected;
  if (s_status_layer) layer_mark_dirty(s_status_layer);
}

static void bt_radio_callback(bool connected) {
  s_bt_radio_connected = connected;
  if (s_status_layer) layer_mark_dirty(s_status_layer);
}

static void update_display() {
  if (s_time_layer) {
    text_layer_set_text_color(s_time_layer, fg_color());
  }
  if (s_status_layer) layer_mark_dirty(s_status_layer);
  if (s_vitals_layer) layer_mark_dirty(s_vitals_layer);
  if (s_hourly_layer) layer_mark_dirty(s_hourly_layer);
}

static GSize measure(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, 200, 60),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
}

// Small filled heart: two lobes (circles) and a downward triangle.
static void draw_heart(GContext *ctx, GPoint c, int r, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  graphics_fill_circle(ctx, GPoint(c.x - r + 1, c.y), r);
  graphics_fill_circle(ctx, GPoint(c.x + r - 1, c.y), r);
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 1);
  int span = 2 * r - 1;
  for (int dy = 0; dy <= span; dy++) {
    int w = span - dy;
    graphics_draw_line(ctx, GPoint(c.x - w, c.y + dy), GPoint(c.x + w, c.y + dy));
  }
}

static void status_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GColor fg = fg_color();
  int mid_y = (bounds.size.h - 20) / 2;

  // Weekday + date, left aligned
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, s_date_buffer, s_font_20,
                     GRect(4, mid_y - 2, bounds.size.w - 8, 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Battery, right aligned
  GSize bs = measure(s_battery_buffer, s_font_16);
  int bx = bounds.size.w - 4 - bs.w;
  graphics_draw_text(ctx, s_battery_buffer, s_font_16,
                     GRect(bx, mid_y, bs.w + 2, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Status flag to the left of the battery: BT disconnect (red) > quiet time
  const char *flag = NULL;
  GColor flag_color = fg;
  if (!(s_bt_app_connected && s_bt_radio_connected)) {
    flag = "BT";
    flag_color = GColorRed;
  } else if (quiet_time_is_active()) {
    flag = "QT";
  }
  if (flag) {
    GSize fs = measure(flag, s_font_16);
    graphics_context_set_text_color(ctx, flag_color);
    graphics_draw_text(ctx, flag, s_font_16,
                       GRect(bx - 8 - fs.w, mid_y, fs.w + 2, 20),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
}

static void vitals_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GColor fg = fg_color();
  int half = bounds.size.w / 2;
  int cy = bounds.size.h / 2;
  int gap = 5;

  graphics_context_set_text_color(ctx, fg);

  // Heart rate (left half)
  GSize hs = measure(s_hr_buffer, s_font_24);
  int heart_r = 6;
  int heart_w = 2 * heart_r;
  int grp_w = heart_w + gap + hs.w;
  int gx = (half - grp_w) / 2;
  draw_heart(ctx, GPoint(gx + heart_r, cy - 2), heart_r, fg);
  graphics_draw_text(ctx, s_hr_buffer, s_font_24,
                     GRect(gx + heart_w + gap, cy - 17, hs.w + 4, 30),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Steps (right half)
  GSize ss = measure(s_steps_buffer, s_font_24);
  GSize bmp = gbitmap_get_bounds(s_sneaker_bitmap).size;
  int grp2_w = bmp.w + gap + ss.w;
  int sx = half + (half - grp2_w) / 2;
  #ifdef PBL_COLOR
    static GColor palette[2];
    palette[0] = fg;
    palette[1] = GColorClear;
    gbitmap_set_palette(s_sneaker_bitmap, palette, false);
  #endif
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  graphics_draw_bitmap_in_rect(ctx, s_sneaker_bitmap,
                               GRect(sx, cy - bmp.h / 2, bmp.w, bmp.h));
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, s_steps_buffer, s_font_24,
                     GRect(sx + bmp.w + gap, cy - 17, ss.w + 4, 30),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void hourly_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GColor fg = fg_color();

  // Divider line across the top
  graphics_context_set_stroke_color(ctx, fg);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(8, 1), GPoint(bounds.size.w - 8, 1));

  graphics_context_set_text_color(ctx, fg);
  int col_w = bounds.size.w / 3;
  int y_label = 6;
  int y_temp = y_label + 17;
  int y_rain = y_temp + 27;
  int y_uv = y_rain + 18;

  for (int i = 0; i < 3; i++) {
    int x = i * col_w;
    graphics_draw_text(ctx, s_hour_label[i], s_font_16, GRect(x, y_label, col_w, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, s_hour_temp_disp[i], s_font_24, GRect(x, y_temp, col_w, 28),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, s_hour_rain_disp[i], s_font_16, GRect(x, y_rain, col_w, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, s_hour_uv_disp[i], s_font_16, GRect(x, y_uv, col_w, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

static void update_layout() {
  GRect full = layer_get_bounds(s_window_layer);
  GRect unob = layer_get_unobstructed_bounds(s_window_layer);
  int bottom = unob.size.h;
  bool obstructed = (full.size.h - unob.size.h) > 0;

  layer_set_frame(s_status_layer, GRect(0, 0, full.size.w, STATUS_HEIGHT));

  if (obstructed) {
    // Hide the lower stats and center the clock in the remaining space.
    layer_set_hidden(s_hourly_layer, true);
    layer_set_hidden(s_vitals_layer, true);
    int cy = STATUS_HEIGHT + (bottom - STATUS_HEIGHT - CLOCK_HEIGHT) / 2;
    layer_set_frame(text_layer_get_layer(s_time_layer), GRect(0, cy, full.size.w, CLOCK_HEIGHT));
    return;
  }

  layer_set_hidden(s_hourly_layer, false);
  layer_set_hidden(s_vitals_layer, false);

  int hourly_y = bottom - HOURLY_HEIGHT;
  layer_set_frame(s_hourly_layer, GRect(0, hourly_y, full.size.w, HOURLY_HEIGHT));

  int vitals_y = hourly_y - VITALS_HEIGHT;
  layer_set_frame(s_vitals_layer, GRect(0, vitals_y, full.size.w, VITALS_HEIGHT));

  int clock_y = STATUS_HEIGHT + (vitals_y - STATUS_HEIGHT - CLOCK_HEIGHT) / 2;
  layer_set_frame(text_layer_get_layer(s_time_layer), GRect(0, clock_y, full.size.w, CLOCK_HEIGHT));
}

static void unobstructed_change(AnimationProgress progress, void *context) {
  update_layout();
}

static void unobstructed_did_change(void *context) {
  update_layout();
}

static void main_window_load(Window *window) {
  s_window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(s_window_layer);

  // Load resources
  s_sneaker_bitmap = gbitmap_create_with_resource(RESOURCE_ID_SNEAKER_ICON);
  s_font_16 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_INTER_SEMIBOLD_16));
  s_font_20 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_INTER_SEMIBOLD_20));
  s_font_24 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_INTER_SEMIBOLD_24));
  s_font_clock = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_INTER_SEMIBOLD_52));

  // Top status row (weekday + date, battery, status flags)
  s_status_layer = layer_create(GRect(0, 0, bounds.size.w, STATUS_HEIGHT));
  layer_set_update_proc(s_status_layer, status_update_proc);

  // Hero clock (positioned by update_layout)
  s_time_layer = text_layer_create(GRect(0, 0, bounds.size.w, CLOCK_HEIGHT));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, fg_color());
  text_layer_set_font(s_time_layer, s_font_clock);
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);

  // Vitals row (heart rate + steps)
  s_vitals_layer = layer_create(GRect(0, 0, bounds.size.w, VITALS_HEIGHT));
  layer_set_update_proc(s_vitals_layer, vitals_update_proc);

  // Hourly weather block
  s_hourly_layer = layer_create(GRect(0, 0, bounds.size.w, HOURLY_HEIGHT));
  layer_set_update_proc(s_hourly_layer, hourly_update_proc);

  layer_add_child(s_window_layer, text_layer_get_layer(s_time_layer));
  layer_add_child(s_window_layer, s_vitals_layer);
  layer_add_child(s_window_layer, s_hourly_layer);
  layer_add_child(s_window_layer, s_status_layer);

  UnobstructedAreaHandlers ua_handlers = {
    .change = unobstructed_change,
    .did_change = unobstructed_did_change
  };
  unobstructed_area_service_subscribe(ua_handlers, NULL);
  update_layout();
}

static void main_window_unload(Window *window) {
  unobstructed_area_service_unsubscribe();
  text_layer_destroy(s_time_layer);
  layer_destroy(s_status_layer);
  layer_destroy(s_vitals_layer);
  layer_destroy(s_hourly_layer);
  gbitmap_destroy(s_sneaker_bitmap);
  fonts_unload_custom_font(s_font_16);
  fonts_unload_custom_font(s_font_20);
  fonts_unload_custom_font(s_font_24);
  fonts_unload_custom_font(s_font_clock);
}

static void init() {
  load_settings();
  load_weather();
  update_hourly_buffers();
  snprintf(s_hr_buffer, sizeof(s_hr_buffer), "--");

  s_main_window = window_create();
  window_set_background_color(s_main_window, bg_color());
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });
  window_stack_push(s_main_window, true);

  update_time(NULL);
  update_hour_labels(NULL);
  update_status_buffer(NULL);
  update_heart_rate();
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  battery_state_service_subscribe(battery_callback);
  battery_callback(battery_state_service_peek());

  health_service_events_subscribe(health_handler, NULL);

  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = bt_app_callback
  });
  s_bt_app_connected = connection_service_peek_pebble_app_connection();
  bluetooth_connection_service_subscribe(bt_radio_callback);
  s_bt_radio_connected = bluetooth_connection_service_peek();

  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);
  app_message_open(256, 256);
}

static void deinit() {
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
