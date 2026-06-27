#include <pebble.h>
#include <message_keys.auto.h>

#define MAX_STOPS 6
#define MAX_ARRIVALS 15
#define CELL_HEIGHT 44

typedef struct {
  char name[32];
  char detail[32];
  int type; // 0 for Tube, 1 for Bus
} StopItem;

typedef struct {
  int stop_id; // Links to stop index (0 to s_stop_count-1)
  char route[16];
  char direction[32];
  int eta;
  int type; // 0 for Tube, 1 for Bus
} ArrivalItem;

static Window *s_main_window;
static Window *s_arrivals_window;

static MenuLayer *s_stops_menu_layer;
static MenuLayer *s_arrivals_menu_layer;
static TextLayer *s_status_layer;

static StopItem s_stops[MAX_STOPS];
static int s_stop_count = 0;

static ArrivalItem s_arrivals[MAX_ARRIVALS];
static int s_arrival_count = 0;

static int s_selected_stop_index = 0;
static char s_status_text[64];
static AppTimer *s_refresh_timer = NULL;
static bool s_updating = false;

// Forward declarations
static void request_update(void);
static void refresh_timer_callback(void *data);

// Clear out existing data
static void clear_data(void) {
  s_stop_count = 0;
  s_arrival_count = 0;
  memset(s_stops, 0, sizeof(s_stops));
  memset(s_arrivals, 0, sizeof(s_arrivals));
  
  if (s_stops_menu_layer) {
    menu_layer_reload_data(s_stops_menu_layer);
  }
  if (s_arrivals_menu_layer) {
    menu_layer_reload_data(s_arrivals_menu_layer);
  }
}

// Get number of arrivals for currently selected stop
static int get_arrivals_count_for_selected_stop(void) {
  int count = 0;
  for (int i = 0; i < s_arrival_count; i++) {
    if (s_arrivals[i].stop_id == s_selected_stop_index) {
      count++;
    }
  }
  return count;
}

// Get the N-th arrival belonging to the selected stop
static ArrivalItem* get_arrival_at_row(int row) {
  int current_row = 0;
  for (int i = 0; i < s_arrival_count; i++) {
    if (s_arrivals[i].stop_id == s_selected_stop_index) {
      if (current_row == row) {
        return &s_arrivals[i];
      }
      current_row++;
    }
  }
  return NULL;
}

// AppMessage Inbox Callback
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *clean_tuple = dict_find(iterator, MESSAGE_KEY_DATA_CLEAN);
  if (clean_tuple) {
    APP_LOG(APP_LOG_LEVEL_INFO, "C side: cleaning lists");
    clear_data();
  }

  Tuple *status_tuple = dict_find(iterator, MESSAGE_KEY_STATUS_MSG);
  if (status_tuple) {
    snprintf(s_status_text, sizeof(s_status_text), "%s", status_tuple->value->cstring);
    if (s_status_layer) {
      text_layer_set_text(s_status_layer, s_status_text);
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "C side status: %s", s_status_text);
  }

  // Handle incoming Stops
  Tuple *stop_index_tuple = dict_find(iterator, MESSAGE_KEY_STOP_INDEX);
  Tuple *stop_count_tuple = dict_find(iterator, MESSAGE_KEY_STOP_COUNT);
  if (stop_index_tuple && stop_count_tuple) {
    int index = stop_index_tuple->value->int32;
    int count = stop_count_tuple->value->int32;

    if (index >= 0 && index < MAX_STOPS) {
      Tuple *name_tuple = dict_find(iterator, MESSAGE_KEY_STOP_NAME);
      Tuple *detail_tuple = dict_find(iterator, MESSAGE_KEY_STOP_DETAIL);
      Tuple *type_tuple = dict_find(iterator, MESSAGE_KEY_STOP_TYPE);

      if (name_tuple && detail_tuple && type_tuple) {
        snprintf(s_stops[index].name, sizeof(s_stops[index].name), "%s", name_tuple->value->cstring);
        snprintf(s_stops[index].detail, sizeof(s_stops[index].detail), "%s", detail_tuple->value->cstring);
        s_stops[index].type = type_tuple->value->int32;

        if (index + 1 > s_stop_count) {
          s_stop_count = index + 1;
        }

        APP_LOG(APP_LOG_LEVEL_INFO, "Added Stop %d/%d: %s (%s)", index + 1, count, s_stops[index].name, s_stops[index].detail);
        if (s_stops_menu_layer) {
          menu_layer_reload_data(s_stops_menu_layer);
        }
      }
    }
  }

  // Handle incoming Arrivals
  Tuple *index_tuple = dict_find(iterator, MESSAGE_KEY_DATA_INDEX);
  Tuple *count_tuple = dict_find(iterator, MESSAGE_KEY_DATA_COUNT);
  if (index_tuple && count_tuple) {
    int index = index_tuple->value->int32;
    int count = count_tuple->value->int32;

    if (index >= 0 && index < MAX_ARRIVALS) {
      Tuple *stop_id_tuple = dict_find(iterator, MESSAGE_KEY_DATA_STOP_ID);
      Tuple *route_tuple = dict_find(iterator, MESSAGE_KEY_DATA_ROUTE);
      Tuple *dir_tuple = dict_find(iterator, MESSAGE_KEY_DATA_DIRECTION);
      Tuple *eta_tuple = dict_find(iterator, MESSAGE_KEY_DATA_ETA);
      Tuple *type_tuple = dict_find(iterator, MESSAGE_KEY_DATA_TYPE);

      if (stop_id_tuple && route_tuple && dir_tuple && eta_tuple && type_tuple) {
        s_arrivals[index].stop_id = stop_id_tuple->value->int32;
        snprintf(s_arrivals[index].route, sizeof(s_arrivals[index].route), "%s", route_tuple->value->cstring);
        snprintf(s_arrivals[index].direction, sizeof(s_arrivals[index].direction), "%s", dir_tuple->value->cstring);
        s_arrivals[index].eta = eta_tuple->value->int32;
        s_arrivals[index].type = type_tuple->value->int32;

        if (index + 1 > s_arrival_count) {
          s_arrival_count = index + 1;
        }

        APP_LOG(APP_LOG_LEVEL_INFO, "Added Arrival %d/%d: StopId %d, Route %s - %d min", index + 1, count, s_arrivals[index].stop_id, s_arrivals[index].route, s_arrivals[index].eta);
        
        if (s_stops_menu_layer) {
          menu_layer_reload_data(s_stops_menu_layer);
        }
        if (s_arrivals_menu_layer) {
          menu_layer_reload_data(s_arrivals_menu_layer);
        }
      }
    }
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped. Reason: %d", reason);
  snprintf(s_status_text, sizeof(s_status_text), "Error %d", reason);
  if (s_status_layer) {
    text_layer_set_text(s_status_layer, s_status_text);
  }
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed! Reason: %d", reason);
  s_updating = false;
  snprintf(s_status_text, sizeof(s_status_text), "Update failed");
  if (s_status_layer) {
    text_layer_set_text(s_status_layer, s_status_text);
  }
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Outbox send success!");
  s_updating = false;
}

// Request update from JS
static void request_update(void) {
  if (s_updating) {
    return;
  }
  s_updating = true;
  snprintf(s_status_text, sizeof(s_status_text), "Updating...");
  if (s_status_layer) {
    text_layer_set_text(s_status_layer, s_status_text);
  }

  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (iter == NULL) {
    s_updating = false;
    return;
  }

  dict_write_uint8(iter, MESSAGE_KEY_REQ_UPDATE, 1);
  app_message_outbox_send();
}

// Timer for 30s auto-refresh
static void refresh_timer_callback(void *data) {
  request_update();
  s_refresh_timer = app_timer_register(30000, refresh_timer_callback, NULL);
}

// --- Menu Layer Callbacks for Main Stops Menu ---

static uint16_t get_stop_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return s_stop_count;
}

static int16_t get_stop_cell_height_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return CELL_HEIGHT;
}

static void draw_stop_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  int row = cell_index->row;
  if (row < s_stop_count) {
    StopItem *item = &s_stops[row];
    char subtitle_buf[64];
    char *type_str = (item->type == 0) ? "Tube" : "Bus";
    snprintf(subtitle_buf, sizeof(subtitle_buf), "[%s] %s", type_str, item->detail);
    menu_cell_basic_draw(ctx, cell_layer, item->name, subtitle_buf, NULL);
  }
}

static void select_stop_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  s_selected_stop_index = cell_index->row;
  if (s_selected_stop_index < s_stop_count) {
    window_stack_push(s_arrivals_window, true);
  }
}

// --- Menu Layer Callbacks for Arrivals Window ---

static uint16_t get_arrivals_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  int count = get_arrivals_count_for_selected_stop();
  return (count == 0) ? 1 : count;
}

static int16_t get_arrivals_cell_height_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return CELL_HEIGHT;
}

static int16_t get_arrivals_header_height_callback(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return 24;
}

static void draw_arrivals_header_callback(GContext *ctx, const Layer *cell_layer, uint16_t section_index, void *context) {
  if (s_selected_stop_index < s_stop_count) {
    StopItem *stop = &s_stops[s_selected_stop_index];
    char header_buf[64];
    snprintf(header_buf, sizeof(header_buf), "%s", stop->name);
    menu_cell_basic_header_draw(ctx, cell_layer, header_buf);
  }
}

static void draw_arrival_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  int row = cell_index->row;
  int count = get_arrivals_count_for_selected_stop();

  if (count == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "No departures", "Check again later", NULL);
    return;
  }

  ArrivalItem *item = get_arrival_at_row(row);
  if (item) {
    char subtitle_buf[64];
    if (item->type == 0) {
      snprintf(subtitle_buf, sizeof(subtitle_buf), "Tube - Line %s", item->route);
    } else {
      snprintf(subtitle_buf, sizeof(subtitle_buf), "Bus %s", item->route);
    }

    menu_cell_basic_draw(ctx, cell_layer, item->direction, subtitle_buf, NULL);

    // Draw the ETA on the right-hand side of the cell
    GRect bounds = layer_get_bounds(cell_layer);
    graphics_context_set_text_color(ctx, GColorBlack);
    
    #ifdef PBL_COLOR
    if (item->type == 0) {
      graphics_context_set_text_color(ctx, GColorDarkCandyAppleRed); // Red for Tube
    } else {
      graphics_context_set_text_color(ctx, GColorCobaltBlue); // Blue for Bus
    }
    #endif

    char eta_buf[16];
    if (item->eta <= 0) {
      snprintf(eta_buf, sizeof(eta_buf), "Due");
    } else {
      snprintf(eta_buf, sizeof(eta_buf), "%d min", item->eta);
    }

    GRect eta_rect = GRect(bounds.size.w - 75, 12, 70, 20);
    graphics_draw_text(ctx, eta_buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       eta_rect, GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);
  }
}

// --- Window Loading and Unloading ---

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // Status Bar at bottom
  s_status_layer = text_layer_create(GRect(0, bounds.size.h - 20, bounds.size.w, 20));
  text_layer_set_text(s_status_layer, "Loading...");
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  // Menu Layer filling rest of screen
  s_stops_menu_layer = menu_layer_create(GRect(0, 0, bounds.size.w, bounds.size.h - 20));
  menu_layer_set_callbacks(s_stops_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = get_stop_num_rows_callback,
    .get_cell_height = get_stop_cell_height_callback,
    .draw_row = draw_stop_row_callback,
    .select_click = select_stop_callback,
  });
  menu_layer_set_click_config_onto_window(s_stops_menu_layer, window);

  layer_add_child(window_layer, menu_layer_get_layer(s_stops_menu_layer));
}

static void main_window_unload(Window *window) {
  menu_layer_destroy(s_stops_menu_layer);
  text_layer_destroy(s_status_layer);
}

static void arrivals_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_arrivals_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_arrivals_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = get_arrivals_num_rows_callback,
    .get_cell_height = get_arrivals_cell_height_callback,
    .draw_row = draw_arrival_row_callback,
    .get_header_height = get_arrivals_header_height_callback,
    .draw_header = draw_arrivals_header_callback,
  });
  menu_layer_set_click_config_onto_window(s_arrivals_menu_layer, window);
  layer_add_child(window_layer, menu_layer_get_layer(s_arrivals_menu_layer));
}

static void arrivals_window_unload(Window *window) {
  menu_layer_destroy(s_arrivals_menu_layer);
}

static void init(void) {
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });

  s_arrivals_window = window_create();
  window_set_window_handlers(s_arrivals_window, (WindowHandlers) {
    .load = arrivals_window_load,
    .unload = arrivals_window_unload,
  });

  window_stack_push(s_main_window, true);

  // Register AppMessage callbacks
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);

  // Open AppMessage with generous buffers for full lists
  app_message_open(1024, 128);

  // Initial update
  request_update();

  // Setup 30-second refresh timer
  s_refresh_timer = app_timer_register(30000, refresh_timer_callback, NULL);
}

static void deinit(void) {
  if (s_refresh_timer != NULL) {
    app_timer_cancel(s_refresh_timer);
    s_refresh_timer = NULL;
  }
  window_destroy(s_main_window);
  window_destroy(s_arrivals_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
