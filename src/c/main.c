#include <pebble.h>

// Message protocol shared with src/pkjs/index.js.
typedef enum {
  TYPE_LIST_BEGIN = 1, TYPE_LIST_ITEM, TYPE_LIST_END,
  TYPE_NOTE_BEGIN, TYPE_BODY_CHUNK, TYPE_ITEM, TYPE_NOTE_END,
  TYPE_TOGGLED, TYPE_CREATED, TYPE_ERROR, TYPE_SETTINGS
} MessageType;

typedef enum { CMD_NONE = 0, CMD_LIST, CMD_OPEN, CMD_TOGGLE, CMD_CREATE, CMD_INSERT } Command;

#define FLAG_PINNED    1
#define FLAG_CHECKLIST 2

#define MAX_NOTES      30
#define MAX_ITEMS      60
#define BODY_SIZE      4096
#define STATUS_SIZE    128
#define TOAST_MS       2500

#define H_INSET PBL_IF_ROUND_ELSE(22, 4)
#define HIGHLIGHT_BG PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack)
#define DONE_TEXT PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack)

#define PERSIST_FONT_SIZE 1
#define FONT_SIZE_COUNT   3

typedef struct {
  int32_t id;
  char title[48];
  char subtitle[64];
  uint8_t flags;
} NoteRow;

typedef struct {
  char text[80];
  bool done;
  bool pending;
  uint8_t indent;
} ChecklistItem;

// ---- State ------------------------------------------------------------------

static NoteRow s_notes[MAX_NOTES];
static int s_note_count;
// Rows received so far in a list update; the shown count only changes at LIST_END.
static int s_incoming_count;
// Set when a note is opened so the list reloads from Kept when you come back to it.
static bool s_list_stale;
// Note highlighted when a list update started, so the highlight can follow it.
static int32_t s_keep_selected_id;
static bool s_list_loading = true;
static char s_list_status[STATUS_SIZE] = "Loading notes…";

static int32_t s_open_id;
static uint8_t s_open_flags;
static bool s_note_loading;
static char s_open_title[64];
static char s_note_status[STATUS_SIZE];
static char s_body[BODY_SIZE];
static ChecklistItem s_items[MAX_ITEMS];
static int s_item_count;

static Window *s_list_window, *s_text_window, *s_checklist_window, *s_toast_window;
static MenuLayer *s_list_menu, *s_checklist_menu;
static ScrollLayer *s_scroll;
static TextLayer *s_title_layer, *s_body_layer, *s_toast_layer;
static AppTimer *s_toast_timer;

// Note font size from the phone settings: 0 small, 1 medium, 2 large.
static uint8_t s_font_size = 1;
// Checklist row to highlight once the note is resent after inserting an item, or -1.
static int s_select_after_load = -1;

static GFont note_body_font(void) {
  static const char *keys[FONT_SIZE_COUNT] = { FONT_KEY_GOTHIC_14, FONT_KEY_GOTHIC_18, FONT_KEY_GOTHIC_24 };
  return fonts_get_system_font(keys[s_font_size]);
}

static GFont note_title_font(void) {
  static const char *keys[FONT_SIZE_COUNT] = { FONT_KEY_GOTHIC_18_BOLD, FONT_KEY_GOTHIC_24_BOLD, FONT_KEY_GOTHIC_28_BOLD };
  return fonts_get_system_font(keys[s_font_size]);
}

static int16_t checklist_row_height(void) {
  static const int16_t heights[FONT_SIZE_COUNT] = { 36, 44, 60 };
  return heights[s_font_size];
}

// Gothic glyphs sit a few px below the text box top; offsets line text up with the checkbox.
static int16_t checklist_text_nudge(void) {
  static const int16_t nudges[FONT_SIZE_COUNT] = { 3, 4, 6 };
  return nudges[s_font_size];
}
static char s_toast_text[STATUS_SIZE];

#if defined(PBL_MICROPHONE)
#define NOTES_SECTION 1
static DictationSession *s_dictation;
#else
#define NOTES_SECTION 0
#endif

// ---- Toast ------------------------------------------------------------------

static void toast_hide(void *data) {
  s_toast_timer = NULL;
  window_stack_remove(s_toast_window, true);
}

static void toast_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_toast_layer = text_layer_create(GRect(H_INSET + 4, bounds.size.h / 2 - 50, bounds.size.w - 2 * (H_INSET + 4), 100));
  text_layer_set_background_color(s_toast_layer, GColorClear);
  text_layer_set_text_color(s_toast_layer, GColorWhite);
  text_layer_set_font(s_toast_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_toast_layer, GTextAlignmentCenter);
  text_layer_set_text(s_toast_layer, s_toast_text);
  layer_add_child(root, text_layer_get_layer(s_toast_layer));
}

static void toast_window_unload(Window *window) {
  text_layer_destroy(s_toast_layer);
  s_toast_layer = NULL;
  if (s_toast_timer) {
    app_timer_cancel(s_toast_timer);
    s_toast_timer = NULL;
  }
}

static void toast(const char *text) {
  snprintf(s_toast_text, sizeof(s_toast_text), "%s", text);
  if (window_stack_contains_window(s_toast_window)) {
    if (s_toast_layer) text_layer_set_text(s_toast_layer, s_toast_text);
    app_timer_reschedule(s_toast_timer, TOAST_MS);
    return;
  }
  window_stack_push(s_toast_window, true);
  s_toast_timer = app_timer_register(TOAST_MS, toast_hide, NULL);
}

// ---- Outgoing commands --------------------------------------------------------

static bool send_command(Command cmd, int32_t note_id, int32_t index, int32_t done, const char *text) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    toast("Phone busy, try again");
    return false;
  }
  dict_write_uint8(iter, MESSAGE_KEY_CMD, cmd);
  dict_write_int32(iter, MESSAGE_KEY_NOTE_ID, note_id);
  dict_write_int32(iter, MESSAGE_KEY_INDEX, index);
  dict_write_int32(iter, MESSAGE_KEY_DONE, done);
  if (text) dict_write_cstring(iter, MESSAGE_KEY_TEXT, text);
  return app_message_outbox_send() == APP_MSG_OK;
}

static void refresh_list(void) {
  s_list_loading = true;
  if (s_note_count == 0) snprintf(s_list_status, sizeof(s_list_status), "Loading notes…");
  if (s_list_menu) menu_layer_reload_data(s_list_menu);
  send_command(CMD_LIST, 0, 0, 0, NULL);
}

// ---- Dictation ----------------------------------------------------------------

#if defined(PBL_MICROPHONE)
typedef enum { DICTATE_NOTE, DICTATE_ITEM } DictationMode;
static DictationMode s_dictation_mode;
static int s_insert_after;

static void dictation_callback(DictationSession *session, DictationSessionStatus status,
                               char *transcription, void *context) {
  if (status != DictationSessionStatusSuccess) return;
  if (s_dictation_mode == DICTATE_NOTE) {
    if (send_command(CMD_CREATE, 0, 0, 0, transcription)) toast("Saving note\u2026");
    return;
  }
  if (!s_open_id) return;  // checklist was closed meanwhile
  if (send_command(CMD_INSERT, s_open_id, s_insert_after, 0, transcription)) {
    s_select_after_load = s_insert_after + 1;
  }
}

static void start_dictation(DictationMode mode) {
  if (!s_dictation) {
    s_dictation = dictation_session_create(512, dictation_callback, NULL);
    if (!s_dictation) {
      toast("Dictation unavailable");
      return;
    }
    dictation_session_enable_confirmation(s_dictation, true);
  }
  s_dictation_mode = mode;
  dictation_session_start(s_dictation);
}
#endif

// ---- Text note window ---------------------------------------------------------

static void text_window_update(void) {
  if (!s_body_layer) return;
  GRect bounds = layer_get_bounds(window_get_root_layer(s_text_window));
  int16_t width = bounds.size.w - 2 * H_INSET;

  text_layer_set_font(s_title_layer, note_title_font());
  text_layer_set_font(s_body_layer, note_body_font());
  text_layer_set_text(s_title_layer, s_open_title);
  text_layer_set_text(s_body_layer, s_note_loading || s_note_status[0] ? s_note_status : s_body);

  int16_t y = PBL_IF_ROUND_ELSE(18, 2);
  int16_t title_h = s_open_title[0] ? text_layer_get_content_size(s_title_layer).h + 4 : 0;
  layer_set_frame(text_layer_get_layer(s_title_layer), GRect(H_INSET, y, width, title_h));
  y += title_h;

  layer_set_frame(text_layer_get_layer(s_body_layer), GRect(H_INSET, y, width, 2000));
  int16_t body_h = text_layer_get_content_size(s_body_layer).h + 6;
  layer_set_frame(text_layer_get_layer(s_body_layer), GRect(H_INSET, y, width, body_h));
  y += body_h + PBL_IF_ROUND_ELSE(40, 8);

  scroll_layer_set_content_size(s_scroll, GSize(bounds.size.w, y));
}

static void text_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_scroll = scroll_layer_create(bounds);
  scroll_layer_set_click_config_onto_window(s_scroll, window);
  scroll_layer_set_shadow_hidden(s_scroll, true);

  s_title_layer = text_layer_create(GRect(H_INSET, 0, bounds.size.w - 2 * H_INSET, 2000));
  text_layer_set_overflow_mode(s_title_layer, GTextOverflowModeWordWrap);
  s_body_layer = text_layer_create(GRect(H_INSET, 0, bounds.size.w - 2 * H_INSET, 2000));
  text_layer_set_overflow_mode(s_body_layer, GTextOverflowModeWordWrap);
#if defined(PBL_ROUND)
  text_layer_set_text_alignment(s_title_layer, GTextAlignmentCenter);
  text_layer_set_text_alignment(s_body_layer, GTextAlignmentCenter);
#endif

  scroll_layer_add_child(s_scroll, text_layer_get_layer(s_title_layer));
  scroll_layer_add_child(s_scroll, text_layer_get_layer(s_body_layer));
  layer_add_child(root, scroll_layer_get_layer(s_scroll));
  text_window_update();
}

static void text_window_unload(Window *window) {
  text_layer_destroy(s_title_layer);
  text_layer_destroy(s_body_layer);
  scroll_layer_destroy(s_scroll);
  s_title_layer = s_body_layer = NULL;
  s_scroll = NULL;
  s_open_id = 0;
}

// ---- Checklist window ---------------------------------------------------------

static bool checklist_has_items(void) {
  return !s_note_loading && !s_note_status[0] && s_item_count > 0;
}

static uint16_t checklist_num_rows(MenuLayer *menu, uint16_t section, void *data) {
  return checklist_has_items() ? s_item_count : 1;
}

static int16_t checklist_cell_height(MenuLayer *menu, MenuIndex *index, void *data) {
  return checklist_has_items() ? checklist_row_height() : 90;
}

static int16_t checklist_header_height(MenuLayer *menu, uint16_t section, void *data) {
  return s_open_title[0] ? MENU_CELL_BASIC_HEADER_HEIGHT : 0;
}

static void checklist_draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *data) {
  GRect bounds = layer_get_bounds(cell);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, s_open_title, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(H_INSET, -2, bounds.size.w - 2 * H_INSET, bounds.size.h),
                     GTextOverflowModeTrailingEllipsis,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
}

static void checklist_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *data) {
  GRect bounds = layer_get_bounds(cell);
  bool highlighted = menu_cell_layer_is_highlighted(cell);
  GColor fg = highlighted ? GColorWhite : GColorBlack;

  if (!checklist_has_items()) {
    const char *text = s_note_loading ? "Loading…" : (s_note_status[0] ? s_note_status : "Empty checklist");
    graphics_context_set_text_color(ctx, fg);
    graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       GRect(H_INSET, 4, bounds.size.w - 2 * H_INSET, bounds.size.h - 8),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  ChecklistItem *item = &s_items[index->row];
  int16_t x = H_INSET + 2 + item->indent * 12;
  GRect box = GRect(x, (bounds.size.h - 14) / 2, 14, 14);
  graphics_context_set_stroke_color(ctx, fg);
  graphics_context_set_fill_color(ctx, fg);
  if (item->done) {
    graphics_fill_rect(ctx, box, 2, GCornersAll);
    graphics_context_set_stroke_color(ctx, highlighted ? HIGHLIGHT_BG : GColorWhite);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(x + 3, box.origin.y + 7), GPoint(x + 6, box.origin.y + 10));
    graphics_draw_line(ctx, GPoint(x + 6, box.origin.y + 10), GPoint(x + 11, box.origin.y + 3));
    graphics_context_set_stroke_width(ctx, 1);
  } else {
    graphics_draw_round_rect(ctx, box, 2);
  }

  int16_t text_x = x + 20;
  GFont font = note_body_font();
  GRect text_box = GRect(text_x, 0, bounds.size.w - text_x - H_INSET, bounds.size.h);
  int16_t text_h = graphics_text_layout_get_content_size(item->text, font, text_box,
                                                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft).h;
  text_box.origin.y = (bounds.size.h - text_h) / 2 - checklist_text_nudge();
  graphics_context_set_text_color(ctx, highlighted ? GColorWhite : (item->done ? DONE_TEXT : GColorBlack));
  graphics_draw_text(ctx, item->text, font, text_box, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void checklist_select(MenuLayer *menu, MenuIndex *index, void *data) {
  if (!checklist_has_items()) return;
  ChecklistItem *item = &s_items[index->row];
  if (item->pending) return;
  bool done = !item->done;
  if (!send_command(CMD_TOGGLE, s_open_id, index->row, done, NULL)) return;
  item->done = done;
  item->pending = true;
  vibes_short_pulse();
  menu_layer_reload_data(menu);
}

// Long-press Select: dictate a new item that goes directly below the highlighted one.
static void checklist_long_select(MenuLayer *menu, MenuIndex *index, void *data) {
  if (s_note_loading || s_note_status[0]) return;
#if defined(PBL_MICROPHONE)
  s_insert_after = s_item_count > 0 ? index->row : -1;
  start_dictation(DICTATE_ITEM);
#else
  toast("No microphone on this watch");
#endif
}

static void checklist_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_checklist_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_highlight_colors(s_checklist_menu, HIGHLIGHT_BG, GColorWhite);
  menu_layer_set_callbacks(s_checklist_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = checklist_num_rows,
    .get_cell_height = checklist_cell_height,
    .get_header_height = checklist_header_height,
    .draw_header = checklist_draw_header,
    .draw_row = checklist_draw_row,
    .select_click = checklist_select,
    .select_long_click = checklist_long_select,
  });
  menu_layer_set_click_config_onto_window(s_checklist_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_checklist_menu));
}

static void checklist_window_unload(Window *window) {
  menu_layer_destroy(s_checklist_menu);
  s_checklist_menu = NULL;
  s_open_id = 0;
}

static void note_view_update(void) {
  if (s_checklist_menu) menu_layer_reload_data(s_checklist_menu);
  text_window_update();
}

// ---- Notes list window --------------------------------------------------------

static uint16_t list_num_sections(MenuLayer *menu, void *data) {
  return NOTES_SECTION + 1;
}

static uint16_t list_num_rows(MenuLayer *menu, uint16_t section, void *data) {
  if (section != NOTES_SECTION) return 1;
  return s_note_count > 0 ? s_note_count : 1;
}

static int16_t list_cell_height(MenuLayer *menu, MenuIndex *index, void *data) {
  if (index->section == NOTES_SECTION && s_note_count == 0) return 90;
#if defined(PBL_ROUND)
  return menu_layer_is_index_selected(menu, index) ? 60 : 36;
#else
  bool has_subtitle = index->section != NOTES_SECTION || s_notes[index->row].subtitle[0];
  return has_subtitle ? 50 : 36;
#endif
}

static void list_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *data) {
  if (index->section != NOTES_SECTION) {
    menu_cell_basic_draw(ctx, cell, "+ New note", "Dictate", NULL);
    return;
  }
  GRect bounds = layer_get_bounds(cell);
  bool highlighted = menu_cell_layer_is_highlighted(cell);
  if (s_note_count == 0) {
    graphics_context_set_text_color(ctx, highlighted ? GColorWhite : GColorBlack);
    graphics_draw_text(ctx, s_list_status, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       GRect(H_INSET, 4, bounds.size.w - 2 * H_INSET, bounds.size.h - 8),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }
  NoteRow *note = &s_notes[index->row];
  menu_cell_basic_draw(ctx, cell, note->title, note->subtitle[0] ? note->subtitle : NULL, NULL);
  if (note->flags & FLAG_PINNED) {
    graphics_context_set_fill_color(ctx, highlighted ? GColorWhite : PBL_IF_COLOR_ELSE(GColorOrange, GColorBlack));
    graphics_fill_circle(ctx, GPoint(bounds.size.w - PBL_IF_ROUND_ELSE(30, 8), 10), 3);
  }
}

static void open_note(NoteRow *note) {
  s_open_id = note->id;
  s_open_flags = note->flags;
  s_note_loading = true;
  s_note_status[0] = '\0';
  s_body[0] = '\0';
  s_item_count = 0;
  snprintf(s_open_title, sizeof(s_open_title), "%s", note->title);
  if (!send_command(CMD_OPEN, note->id, 0, 0, NULL)) return;
  s_list_stale = true;
  window_stack_push((note->flags & FLAG_CHECKLIST) ? s_checklist_window : s_text_window, true);
}

static void list_select(MenuLayer *menu, MenuIndex *index, void *data) {
  if (index->section != NOTES_SECTION) {
#if defined(PBL_MICROPHONE)
    start_dictation(DICTATE_NOTE);
#endif
    return;
  }
  if (s_note_count == 0) {
    if (!s_list_loading) refresh_list();
    return;
  }
  open_note(&s_notes[index->row]);
}

static void list_long_select(MenuLayer *menu, MenuIndex *index, void *data) {
  vibes_short_pulse();
  refresh_list();
}

static void list_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_list_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_highlight_colors(s_list_menu, HIGHLIGHT_BG, GColorWhite);
  menu_layer_set_callbacks(s_list_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = list_num_sections,
    .get_num_rows = list_num_rows,
    .get_cell_height = list_cell_height,
    .draw_row = list_draw_row,
    .select_click = list_select,
    .select_long_click = list_long_select,
  });
  menu_layer_set_click_config_onto_window(s_list_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_list_menu));
#if defined(PBL_MICROPHONE)
  // Start on the first note rather than on the "New note" action.
  menu_layer_set_selected_index(s_list_menu, MenuIndex(NOTES_SECTION, 0), MenuRowAlignCenter, false);
#endif
}

static void list_window_appear(Window *window) {
  if (!s_list_stale) return;
  s_list_stale = false;
  refresh_list();
}

static void list_window_unload(Window *window) {
  menu_layer_destroy(s_list_menu);
  s_list_menu = NULL;
}

// ---- Incoming messages --------------------------------------------------------

static int32_t tuple_int(DictionaryIterator *iter, uint32_t key) {
  Tuple *t = dict_find(iter, key);
  if (!t) return 0;
  switch (t->type) {
    case TUPLE_INT:
      return t->length == 1 ? t->value->int8 : t->length == 2 ? t->value->int16 : t->value->int32;
    case TUPLE_UINT:
      return t->length == 1 ? t->value->uint8 : t->length == 2 ? t->value->uint16 : (int32_t)t->value->uint32;
    default:
      return 0;
  }
}

static const char *tuple_str(DictionaryIterator *iter, uint32_t key) {
  Tuple *t = dict_find(iter, key);
  return (t && t->type == TUPLE_CSTRING) ? t->value->cstring : "";
}

static void handle_error(DictionaryIterator *iter) {
  Command cmd = tuple_int(iter, MESSAGE_KEY_CMD);
  int32_t note_id = tuple_int(iter, MESSAGE_KEY_NOTE_ID);
  const char *text = tuple_str(iter, MESSAGE_KEY_TEXT);

  switch (cmd) {
    case CMD_OPEN:
      if (note_id != s_open_id) return;
      s_note_loading = false;
      snprintf(s_note_status, sizeof(s_note_status), "%s", text);
      note_view_update();
      return;
    case CMD_TOGGLE:
      if (note_id == s_open_id) {
        // Roll back optimistic toggles that the phone could not save.
        for (int i = 0; i < s_item_count; i++) {
          if (s_items[i].pending) {
            s_items[i].pending = false;
            s_items[i].done = !s_items[i].done;
          }
        }
        note_view_update();
      }
      toast(text);
      return;
    case CMD_CREATE:
      toast(text);
      return;
    case CMD_INSERT:
      s_select_after_load = -1;
      toast(text);
      return;
    default:
      s_list_loading = false;
      if (s_note_count == 0) {
        snprintf(s_list_status, sizeof(s_list_status), "%s", text);
        if (s_list_menu) menu_layer_reload_data(s_list_menu);
      } else {
        toast(text);
      }
      return;
  }
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  MessageType type = tuple_int(iter, MESSAGE_KEY_TYPE);
  int32_t note_id = tuple_int(iter, MESSAGE_KEY_NOTE_ID);
  int32_t index = tuple_int(iter, MESSAGE_KEY_INDEX);

  switch (type) {
    case TYPE_LIST_BEGIN:
      s_list_loading = true;
      s_incoming_count = 0;
      s_keep_selected_id = 0;
      if (s_list_menu && s_note_count > 0) {
        MenuIndex selected = menu_layer_get_selected_index(s_list_menu);
        if (selected.section == NOTES_SECTION && selected.row < s_note_count) {
          s_keep_selected_id = s_notes[selected.row].id;
        }
      }
      break;
    case TYPE_LIST_ITEM:
      if (index >= 0 && index < MAX_NOTES) {
        NoteRow *note = &s_notes[index];
        note->id = note_id;
        note->flags = tuple_int(iter, MESSAGE_KEY_FLAGS);
        snprintf(note->title, sizeof(note->title), "%s", tuple_str(iter, MESSAGE_KEY_TITLE));
        snprintf(note->subtitle, sizeof(note->subtitle), "%s", tuple_str(iter, MESSAGE_KEY_SUBTITLE));
        if (index >= s_incoming_count) s_incoming_count = index + 1;
      }
      break;
    case TYPE_LIST_END:
      s_list_loading = false;
      s_note_count = s_incoming_count;
      if (s_note_count == 0) snprintf(s_list_status, sizeof(s_list_status), "No notes yet.\nHold Select to refresh.");
      if (s_list_menu) {
        menu_layer_reload_data(s_list_menu);
        for (int i = 0; s_keep_selected_id && i < s_note_count; i++) {
          if (s_notes[i].id == s_keep_selected_id) {
            menu_layer_set_selected_index(s_list_menu, MenuIndex(NOTES_SECTION, i), MenuRowAlignCenter, false);
            break;
          }
        }
      }
      break;

    case TYPE_NOTE_BEGIN:
      if (note_id != s_open_id) return;
      s_open_flags = tuple_int(iter, MESSAGE_KEY_FLAGS);
      if (tuple_str(iter, MESSAGE_KEY_TITLE)[0]) {
        snprintf(s_open_title, sizeof(s_open_title), "%s", tuple_str(iter, MESSAGE_KEY_TITLE));
      }
      s_body[0] = '\0';
      s_item_count = 0;
      break;
    case TYPE_BODY_CHUNK:
      if (note_id != s_open_id) return;
      strncat(s_body, tuple_str(iter, MESSAGE_KEY_TEXT), sizeof(s_body) - strlen(s_body) - 1);
      break;
    case TYPE_ITEM:
      if (note_id != s_open_id || index < 0 || index >= MAX_ITEMS) return;
      snprintf(s_items[index].text, sizeof(s_items[index].text), "%s", tuple_str(iter, MESSAGE_KEY_TEXT));
      s_items[index].done = tuple_int(iter, MESSAGE_KEY_DONE) != 0;
      s_items[index].indent = tuple_int(iter, MESSAGE_KEY_INDENT);
      s_items[index].pending = false;
      if (index >= s_item_count) s_item_count = index + 1;
      break;
    case TYPE_NOTE_END:
      if (note_id != s_open_id) return;
      s_note_loading = false;
      note_view_update();
      if (s_select_after_load >= 0 && s_checklist_menu && s_select_after_load < s_item_count) {
        menu_layer_set_selected_index(s_checklist_menu, MenuIndex(0, s_select_after_load), MenuRowAlignCenter, true);
        vibes_short_pulse();
      }
      s_select_after_load = -1;
      break;

    case TYPE_TOGGLED:
      if (note_id != s_open_id || index < 0 || index >= s_item_count) return;
      s_items[index].done = tuple_int(iter, MESSAGE_KEY_DONE) != 0;
      s_items[index].pending = false;
      note_view_update();
      break;
    case TYPE_SETTINGS: {
      int32_t size = tuple_int(iter, MESSAGE_KEY_FONT_SIZE);
      if (size >= 0 && size < FONT_SIZE_COUNT && size != s_font_size) {
        s_font_size = size;
        persist_write_int(PERSIST_FONT_SIZE, size);
        note_view_update();
      }
      break;
    }
    case TYPE_CREATED:
      vibes_short_pulse();
      toast("Note saved");
      break;
    case TYPE_ERROR:
      handle_error(iter);
      break;
  }
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "Outbox failed: %d", (int)reason);
  toast("Phone not reachable");
}

// ---- App lifecycle ------------------------------------------------------------

static Window *make_window(WindowHandlers handlers) {
  Window *window = window_create();
  window_set_window_handlers(window, handlers);
  return window;
}

static void init(void) {
  if (persist_exists(PERSIST_FONT_SIZE)) {
    int32_t size = persist_read_int(PERSIST_FONT_SIZE);
    if (size >= 0 && size < FONT_SIZE_COUNT) s_font_size = size;
  }
  s_list_window = make_window((WindowHandlers) {
    .load = list_window_load, .appear = list_window_appear, .unload = list_window_unload });
  s_text_window = make_window((WindowHandlers) { .load = text_window_load, .unload = text_window_unload });
  s_checklist_window = make_window((WindowHandlers) { .load = checklist_window_load, .unload = checklist_window_unload });
  s_toast_window = make_window((WindowHandlers) { .load = toast_window_load, .unload = toast_window_unload });
  window_set_background_color(s_toast_window, HIGHLIGHT_BG);

  app_message_register_inbox_received(inbox_received);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(app_message_inbox_size_maximum(), 1024);

  window_stack_push(s_list_window, true);
}

static void deinit(void) {
#if defined(PBL_MICROPHONE)
  if (s_dictation) dictation_session_destroy(s_dictation);
#endif
  window_destroy(s_toast_window);
  window_destroy(s_checklist_window);
  window_destroy(s_text_window);
  window_destroy(s_list_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
