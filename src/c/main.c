#include <pebble.h>

// Message protocol shared with src/pkjs/index.js.
typedef enum {
  TYPE_LIST_BEGIN = 1, TYPE_LIST_ITEM, TYPE_LIST_END,
  TYPE_NOTE_BEGIN, TYPE_BODY_CHUNK, TYPE_ITEM, TYPE_NOTE_END,
  TYPE_TOGGLED, TYPE_CREATED, TYPE_ERROR, TYPE_SETTINGS, TYPE_NEW_ITEMS
} MessageType;

typedef enum { CMD_NONE = 0, CMD_LIST, CMD_OPEN, CMD_TOGGLE, CMD_CREATE, CMD_INSERT, CMD_DELETE } Command;

#define FLAG_PINNED    1
#define FLAG_CHECKLIST 2

#define MAX_NOTES      30
#define MAX_ITEMS      60
#define BODY_SIZE      4096
#define STATUS_SIZE    128
#define TOAST_MS       2500

#define H_INSET PBL_IF_ROUND_ELSE(22, 4)
#define HIGHLIGHT_BG PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack)
// Checked items form an inverted block below the open ones.
#define DONE_BG   GColorBlack
#define DONE_TEXT PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite)

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
  uint8_t indent;  // Kept's stored level, 0-3
  uint8_t shown_indent;  // indent capped per block for display, see checklist_update_indents
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

// ---- Vibration pattern --------------------------------------------------------

#define VIBE_SHORT_MS 120
#define VIBE_LONG_MS  400
#define VIBE_GAP_MS   150
#define VIBE_PAUSE_MS 400
#define VIBE_MAX_SEGMENTS 48

// Plays a pattern like ".-": '.' short, '-' long, ' ' an extra pause.
static void vibrate_pattern(const char *pattern) {
  static uint32_t segments[VIBE_MAX_SEGMENTS];
  int count = 0;
  uint32_t pause = 0;
  for (const char *c = pattern; *c; c++) {
    if (*c == ' ') {
      pause += VIBE_PAUSE_MS;
      continue;
    }
    if (*c != '.' && *c != '-') continue;
    if (count > 0) {
      if (count + 2 > VIBE_MAX_SEGMENTS) break;
      segments[count++] = VIBE_GAP_MS + pause;
    }
    segments[count++] = *c == '.' ? VIBE_SHORT_MS : VIBE_LONG_MS;
    pause = 0;
  }
  if (count == 0) return;
  vibes_cancel();
  vibes_enqueue_custom_pattern((VibePattern) { .durations = segments, .num_segments = count });
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

  int16_t y = PBL_IF_ROUND_ELSE(18, 0);
  int16_t title_h = s_open_title[0] ? text_layer_get_content_size(s_title_layer).h : 0;
  layer_set_frame(text_layer_get_layer(s_title_layer), GRect(H_INSET, y, width, title_h));
  y += title_h;

  layer_set_frame(text_layer_get_layer(s_body_layer), GRect(H_INSET, y, width, 2000));
  int16_t body_h = text_layer_get_content_size(s_body_layer).h + 4;
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

// Caps each row at one level deeper than the row above, starting at 0 in each block
// (open, then checked), like Kept's normalizeIndentLevels.
static void checklist_update_indents(void) {
  for (int i = 0; i < s_item_count; i++) {
    bool block_start = i == 0 || s_items[i].done != s_items[i - 1].done;
    uint8_t max = block_start ? 0 : s_items[i - 1].shown_indent + 1;
    s_items[i].shown_indent = s_items[i].indent < max ? s_items[i].indent : max;
  }
}

static bool checklist_has_items(void) {
  return !s_note_loading && !s_note_status[0] && s_item_count > 0;
}

static uint16_t checklist_num_rows(MenuLayer *menu, uint16_t section, void *data) {
  return checklist_has_items() ? s_item_count : 1;
}

#define CHECKBOX_SIZE    14
#define CHECKLIST_ROW_PAD 6

static int16_t checklist_text_x(const ChecklistItem *item) {
  return H_INSET + 2 + item->shown_indent * 12 + CHECKBOX_SIZE + 6;
}

// Height of the item's text box: one line, or two when it wraps (longer text is ellipsized).
static int16_t checklist_text_height(const ChecklistItem *item, int16_t cell_w) {
  GFont font = note_body_font();
  GRect box = GRect(0, 0, cell_w - checklist_text_x(item) - H_INSET, 200);
  int16_t one_line = graphics_text_layout_get_content_size("Ag", font, box, GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
  int16_t h = graphics_text_layout_get_content_size(item->text, font, box, GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
  return h > one_line + 2 ? 2 * one_line : one_line;
}

static int16_t checklist_cell_height(MenuLayer *menu, MenuIndex *index, void *data) {
  if (!checklist_has_items()) return 90;
  int16_t cell_w = layer_get_bounds(menu_layer_get_layer(menu)).size.w;
  int16_t visible = checklist_text_height(&s_items[index->row], cell_w) - checklist_text_nudge();
  int16_t h = visible + CHECKLIST_ROW_PAD;
  return h < CHECKBOX_SIZE + CHECKLIST_ROW_PAD ? CHECKBOX_SIZE + CHECKLIST_ROW_PAD : h;
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
  GColor bg = highlighted ? HIGHLIGHT_BG : GColorWhite;
  GColor text_color = fg;
  if (item->done) {
    if (!highlighted) {
      bg = DONE_BG;
      fg = GColorWhite;
      text_color = DONE_TEXT;
    }
#if !defined(PBL_COLOR)
    else {
      // The normal black highlight would vanish in the black block, so invert it there.
      bg = GColorWhite;
      fg = text_color = GColorBlack;
    }
#endif
    graphics_context_set_fill_color(ctx, bg);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  }

  int16_t x = H_INSET + 2 + item->shown_indent * 12;
  GRect box = GRect(x, (bounds.size.h - CHECKBOX_SIZE) / 2, CHECKBOX_SIZE, CHECKBOX_SIZE);
  graphics_context_set_stroke_color(ctx, fg);
  graphics_context_set_fill_color(ctx, fg);
  if (item->done) {
    graphics_fill_rect(ctx, box, 2, GCornersAll);
    graphics_context_set_stroke_color(ctx, bg);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(x + 3, box.origin.y + 7), GPoint(x + 6, box.origin.y + 10));
    graphics_draw_line(ctx, GPoint(x + 6, box.origin.y + 10), GPoint(x + 11, box.origin.y + 3));
    graphics_context_set_stroke_width(ctx, 1);
  } else {
    graphics_draw_round_rect(ctx, box, 2);
  }

  int16_t text_x = checklist_text_x(item);
  int16_t text_h = checklist_text_height(item, bounds.size.w);
  int16_t visible = text_h - checklist_text_nudge();
  GRect text_box = GRect(text_x, (bounds.size.h - visible) / 2 - checklist_text_nudge(),
                         bounds.size.w - text_x - H_INSET, text_h);
  graphics_context_set_text_color(ctx, text_color);
  graphics_draw_text(ctx, item->text, note_body_font(), text_box, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
#if defined(PBL_COLOR)
  if (!highlighted) {
    graphics_context_set_stroke_color(ctx, item->done ? GColorDarkGray : GColorLightGray);
    graphics_draw_line(ctx, GPoint(x, bounds.size.h - 1), GPoint(bounds.size.w - H_INSET, bounds.size.h - 1));
  }
#endif
}

// Reverses s_items[from..to).
static void checklist_reverse(int from, int to) {
  for (to--; from < to; from++, to--) {
    ChecklistItem tmp = s_items[from];
    s_items[from] = s_items[to];
    s_items[to] = tmp;
  }
}


// Mirrors toggledRows in src/pkjs/kept.js: sets the row and the rows nested below it to done
// and moves them to the top of the checked block, which is also the end of the open block.
static void checklist_toggle(int row, bool done) {
  bool was_done = s_items[row].done;
  uint8_t indent = s_items[row].indent;
  int end = row + 1;
  while (end < s_item_count && s_items[end].done == was_done && s_items[end].indent > indent) end++;
  for (int i = row; i < end; i++) s_items[i].done = done;

  // The first checked row outside the group: the group goes right before it.
  int at = 0;
  while (at < s_item_count && ((at >= row && at < end) || !s_items[at].done)) at++;
  // Rotate the group into place with three reversals (no buffer needed).
  if (at > end) {
    checklist_reverse(row, end);
    checklist_reverse(end, at);
    checklist_reverse(row, at);
  } else if (at < row) {
    checklist_reverse(at, row);
    checklist_reverse(row, end);
    checklist_reverse(at, end);
  }
  checklist_update_indents();
}

static void checklist_select(MenuLayer *menu, MenuIndex *index, void *data) {
  if (!checklist_has_items()) return;
  bool done = !s_items[index->row].done;
  if (!send_command(CMD_TOGGLE, s_open_id, index->row, done, NULL)) return;
  // The highlight stays on this row, which now shows the next item.
  checklist_toggle(index->row, done);
  vibes_short_pulse();
  menu_layer_reload_data(menu);
}

// ---- Checklist context menu (long-press Select) ---------------------------------

typedef enum { ITEM_ACTION_NONE, ITEM_ACTION_DICTATE, ITEM_ACTION_DELETE } ItemAction;
static ItemAction s_item_action;
static int s_action_row;

static void item_action_performed(ActionMenu *menu, const ActionMenuItem *action, void *context) {
  s_item_action = (ItemAction)(uintptr_t)action_menu_item_get_action_data(action);
}

// Runs the chosen action once the menu has fully closed, so dictation can open its own window.
static void item_action_menu_closed(ActionMenu *menu, const ActionMenuItem *performed, void *context) {
  action_menu_hierarchy_destroy(action_menu_get_root_level(menu), NULL, NULL);
  ItemAction action = s_item_action;
  s_item_action = ITEM_ACTION_NONE;
  if (!s_open_id) return;

  switch (action) {
#if defined(PBL_MICROPHONE)
    case ITEM_ACTION_DICTATE:
      s_insert_after = s_item_count > 0 ? s_action_row : -1;
      start_dictation(DICTATE_ITEM);
      break;
#endif
    case ITEM_ACTION_DELETE:
      if (s_action_row < s_item_count && send_command(CMD_DELETE, s_open_id, s_action_row, 0, NULL)) {
        s_select_after_load = s_action_row;
      }
      break;
    default:
      break;
  }
}

static void checklist_long_select(MenuLayer *menu, MenuIndex *index, void *data) {
  if (s_note_loading || s_note_status[0]) return;
  bool has_item = s_item_count > 0;
#if !defined(PBL_MICROPHONE)
  if (!has_item) {
    toast("No microphone on this watch");
    return;
  }
#endif
  s_action_row = index->row;
  s_item_action = ITEM_ACTION_NONE;

  ActionMenuLevel *root = action_menu_level_create(2);
#if defined(PBL_MICROPHONE)
  action_menu_level_add_action(root, has_item ? "Dictate new below" : "Dictate new item",
                               item_action_performed, (void *)ITEM_ACTION_DICTATE);
#endif
  if (has_item) {
    action_menu_level_add_action(root, "Delete line", item_action_performed, (void *)ITEM_ACTION_DELETE);
  }
  action_menu_open(&(ActionMenuConfig) {
    .root_level = root,
    .colors = { .background = HIGHLIGHT_BG, .foreground = GColorWhite },
    .align = ActionMenuAlignCenter,
    .did_close = item_action_menu_closed,
  });
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

// Compact list rows: 18pt bold title, optional 14pt subtitle line.
#define LIST_TITLE_ROW_H    26
#define LIST_SUBTITLE_ROW_H 42
#define LIST_PIN_SPACE      10

static bool list_row_has_subtitle(MenuIndex *index) {
  return index->section == NOTES_SECTION && s_notes[index->row].subtitle[0];
}

static int16_t list_cell_height(MenuLayer *menu, MenuIndex *index, void *data) {
  if (index->section == NOTES_SECTION && s_note_count == 0) return 90;
  return list_row_has_subtitle(index) ? LIST_SUBTITLE_ROW_H : LIST_TITLE_ROW_H;
}

static void list_draw_compact(GContext *ctx, const Layer *cell, const char *title,
                              const char *subtitle, bool pinned) {
  GRect bounds = layer_get_bounds(cell);
  bool highlighted = menu_cell_layer_is_highlighted(cell);
  GTextAlignment align = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);
  int16_t x = H_INSET + 2;
  int16_t w = bounds.size.w - x - H_INSET - LIST_PIN_SPACE;

  // Gothic fonts carry ~4px of space above the glyphs, hence the negative offsets.
  graphics_context_set_text_color(ctx, highlighted ? GColorWhite : GColorBlack);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(x, -2, w, 22), GTextOverflowModeTrailingEllipsis, align, NULL);
  if (subtitle && subtitle[0]) {
    graphics_context_set_text_color(ctx, highlighted ? GColorWhite : PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
    graphics_draw_text(ctx, subtitle, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(x, 20, w, 18), GTextOverflowModeTrailingEllipsis, align, NULL);
  }
  if (pinned) {
    graphics_context_set_fill_color(ctx, highlighted ? GColorWhite : PBL_IF_COLOR_ELSE(GColorOrange, GColorBlack));
    graphics_fill_circle(ctx, GPoint(bounds.size.w - H_INSET - 4, 12), 3);
  }
#if defined(PBL_COLOR)
  if (!highlighted) {
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_draw_line(ctx, GPoint(x, bounds.size.h - 1), GPoint(bounds.size.w - H_INSET, bounds.size.h - 1));
  }
#endif
}

static void list_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *data) {
  if (index->section != NOTES_SECTION) {
    list_draw_compact(ctx, cell, PBL_IF_ROUND_ELSE("+ New note", "+ New note (dictate)"), NULL, false);
    return;
  }
  if (s_note_count == 0) {
    GRect bounds = layer_get_bounds(cell);
    graphics_context_set_text_color(ctx, menu_cell_layer_is_highlighted(cell) ? GColorWhite : GColorBlack);
    graphics_draw_text(ctx, s_list_status, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       GRect(H_INSET, 4, bounds.size.w - 2 * H_INSET, bounds.size.h - 8),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }
  NoteRow *note = &s_notes[index->row];
  list_draw_compact(ctx, cell, note->title, note->subtitle, note->flags & FLAG_PINNED);
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
      // The phone resends the note, which undoes the toggle shown on the watch.
    case CMD_CREATE:
      toast(text);
      return;
    case CMD_INSERT:
    case CMD_DELETE:
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
      if (index >= s_item_count) s_item_count = index + 1;
      break;
    case TYPE_NOTE_END:
      if (note_id != s_open_id) return;
      s_note_loading = false;
      checklist_update_indents();
      note_view_update();
      // The phone knows where an inserted item ended up; it may have moved to the open block.
      if (s_select_after_load >= 0 && index >= 0) s_select_after_load = index;
      if (s_select_after_load >= 0 && s_checklist_menu && s_item_count > 0) {
        // After a delete the row index may now be past the end; keep the highlight on the last item then.
        int row = s_select_after_load < s_item_count ? s_select_after_load : s_item_count - 1;
        menu_layer_set_selected_index(s_checklist_menu, MenuIndex(0, row), MenuRowAlignCenter, true);
        vibes_short_pulse();
      }
      s_select_after_load = -1;
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
    case TYPE_NEW_ITEMS:
      // Someone added items to the open checklist; the phone has already resent it.
      if (note_id != s_open_id || !s_checklist_menu) return;
      vibrate_pattern(tuple_str(iter, MESSAGE_KEY_TEXT));
      light_enable_interaction();
      break;
    case TYPE_TOGGLED:
      break;  // no longer sent; the watch applies toggles itself
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

  // Opt in to the system touch bridge so swipes/taps scroll the notes list, checklists and
  // text notes on touchscreen watches. A no-op macro on platforms without touch.
  (void)app_touch_navigation_enable(true);

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
