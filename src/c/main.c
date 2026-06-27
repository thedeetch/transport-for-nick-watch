#include <pebble.h>
#include <message_keys.auto.h>

#define MAX_ARRIVALS 12
#define CELL_HEIGHT 44

typedef struct {
  char station[32];
  char route[16];
  char direction[32];
  int eta;
  int type; // 0 for Tube, 1 for Bus
} ArrivalItem;

static Window *s_main_window;
static MenuLayer *s_menu_layer;
static TextLayer *s_status_layer;

static ArrivalItem s_arrivals[MAX_ARRIVALS];
static int s_arrival_count = 0;
static char s_status_text[64];
static AppTimer *s_refresh_timer = NULL;
static bool s_updating = false;

// Forward declarations
static void request_update(void);
static void refresh_timer_callback(void *data);

// Clean out existing data
static void clear_arrivals(void) {
  s_arrival_count = 0;
  memset(s_arrivals, 0, sizeof(s_arrivals));
  menu_layer_reload_data(s_menu_layer);
}

// Format arrival string
static void format_arrival_text(char *buffer, size_t buffer_len, const ArrivalItem *item) {
  // e.g., "In 5 min" or "Due"
  if (item->eta <= 0) {
    snprintf(buffer, buffer_len, "Due");
  } else {
    snprintf(buffer, buffer_len, "In %d min", item->eta);
  }
}

// Format route / direction header string
static void format_route_direction(char *buffer, size_t buffer_len, const ArrivalItem *item) {
  // e.g., "Bus 12 -> Oxford Circus" or "Victoria -> Northbound"
  char *type_str = (item->type == 0) ? "Tube" : "Bus";
  snprintf(buffer, buffer_len, "[%s %s] %s", type_str, item->route, item->direction);
}

// AppMessage Inbox Callback
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *clean_tuple = dict_find(iterator, MESSAGE_KEY_DATA_CLEAN);
  if (clean_tuple) {
    APP_LOG(APP_LOG_LEVEL_INFO, "C side: cleaning arrivals");
    clear_arrivals();
  }

  Tuple *status_tuple = dict_find(iterator, MESSAGE_KEY_STATUS_MSG);
  if (status_tuple) {
    snprintf(s_status_text, sizeof(s_status_text), "%s", status_tuple->value->cstring);
    text_layer_set_text(s_status_layer, s_status_text);
    APP_LOG(APP_LOG_LEVEL_INFO, "C side status: %s", s_status_text);
  }

  Tuple *index_tuple = dict_find(iterator, MESSAGE_KEY_DATA_INDEX);
  Tuple *count_tuple = dict_find(iterator, MESSAGE_KEY_DATA_COUNT);
  if (index_tuple && count_tuple) {
    int index = index_tuple->value->int32;
    int count = count_tuple->value->int32;

    if (index >= 0 && index < MAX_ARRIVALS) {
      Tuple *station_tuple = dict_find(iterator, MESSAGE_KEY_DATA_STATION);
      Tuple *route_tuple = dict_find(iterator, MESSAGE_KEY_DATA_ROUTE);
      Tuple *dir_tuple = dict_find(iterator, MESSAGE_KEY_DATA_DIRECTION);
      Tuple *eta_tuple = dict_find(iterator, MESSAGE_KEY_DATA_ETA);
      Tuple *type_tuple = dict_find(iterator, MESSAGE_KEY_DATA_TYPE);

      if (station_tuple && route_tuple && dir_tuple && eta_tuple && type_tuple) {
        snprintf(s_arrivals[index].station, sizeof(s_arrivals[index].station), "%s", station_tuple->value->cstring);
        snprintf(s_arrivals[index].route, sizeof(s_arrivals[index].route), "%s", route_tuple->value->cstring);
        snprintf(s_arrivals[index].direction, sizeof(s_arrivals[index].direction), "%s", dir_tuple->value->cstring);
        s_arrivals[index].eta = eta_tuple->value->int32;
        s_arrivals[index].type = type_tuple->value->int32;

        if (index + 1 > s_arrival_count) {
          s_arrival_count = index + 1;
        }

        APP_LOG(APP_LOG_LEVEL_INFO, "Added arrival %d: %s %s - %d min", index, s_arrivals[index].station, s_arrivals[index].route, s_arrivals[index].eta);
        menu_layer_reload_data(s_menu_layer);
      }
    }
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped. Reason: %d", reason);
  snprintf(s_status_text, sizeof(s_status_text), "Error %d", reason);
  text_layer_set_text(s_status_layer, s_status_text);
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed! Reason: %d", reason);
  s_updating = false;
  snprintf(s_status_text, sizeof(s_status_text), "Update failed");
  text_layer_set_text(s_status_layer, s_status_text);
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
  text_layer_set_text(s_status_layer, s_status_text);

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

// Click Select button to manually update
static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  request_update();
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
}

// Menu Layer Callbacks
static uint16_t get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return s_arrival_count;
}

static int16_t get_cell_height_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return CELL_HEIGHT;
}

static void draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  int index = cell_index->row;
  if (index < s_arrival_count) {
    ArrivalItem *item = &s_arrivals[index];

    char title_buf[64];
    format_route_direction(title_buf, sizeof(title_buf), item);

    char eta_buf[16];
    format_arrival_text(eta_buf, sizeof(eta_buf), item);

    // Draw station name as header / main text, and the route + ETA as detailed text
    menu_cell_basic_draw(ctx, cell_layer, item->station, title_buf, NULL);

    // Draw the ETA on the right-hand side of the cell
    GRect bounds = layer_get_bounds(cell_layer);
    graphics_context_set_text_color(ctx, GColorBlack);
    
    #ifdef PBL_COLOR
    // On color watches, highlight soonest or different types
    if (item->type == 0) {
      graphics_context_set_text_color(ctx, GColorDarkCandyAppleRed); // Reddish for Tube
    } else {
      graphics_context_set_text_color(ctx, GColorCobaltBlue); // Bluish for Bus
    }
    #endif

    GRect eta_rect = GRect(bounds.size.w - 75, 4, 70, bounds.size.h - 8);
    graphics_draw_text(ctx, eta_buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       eta_rect, GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);
  }
}

// Window Load/Unload
static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // Status Bar / Info Text at bottom
  s_status_layer = text_layer_create(GRect(0, bounds.size.h - 20, bounds.size.w, 20));
  text_layer_set_text(s_status_layer, "Loading...");
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  // Menu Layer filling rest of screen
  s_menu_layer = menu_layer_create(GRect(0, 0, bounds.size.w, bounds.size.h - 20));
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = get_num_rows_callback,
    .get_cell_height = get_cell_height_callback,
    .draw_row = draw_row_callback,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  
  // Register click config for manual select trigger
  window_set_click_config_provider(s_main_window, click_config_provider);

  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
}

static void main_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  text_layer_destroy(s_status_layer);
}

static void init(void) {
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
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
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
