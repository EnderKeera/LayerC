// GTK 4 prototype of the Linux editor's representative UI (Change 1).
#include "proto_common.hpp"

#include <gtk/gtk.h>

#include <cmath>
#include <string>

namespace {
using layerc::prototype::Model;
struct ValidationDialog;

struct App {
  Model model;
  GtkWidget *window{};
  GtkWidget *list{};
  GtkWidget *cooldowns{};
  GtkWidget *name{};
  GtkWidget *executable{};
  GtkWidget *cooldown_name{};
  GtkWidget *duration{};
  GtkWidget *charges{};
  GtkWidget *binding{};
  GtkWidget *capture{};
  bool capturing{};
  GtkWidget *preview{};
  GtkWidget *status{};
  GtkWidget *menu{};
  int cooldown{};
  bool syncing{};
  ValidationDialog *validation{};
  layerc::prototype::CycleDriver cycles;
};

void set_status(App &app, const std::string &text) {
  gtk_label_set_text(GTK_LABEL(app.status), text.c_str());
}

void refresh_cooldown(App &app) {
  auto &profile = app.model.document().profile();
  if (app.cooldown < 0 || static_cast<std::size_t>(app.cooldown) >= profile.cooldowns.size())
    return;
  const auto &cooldown = profile.cooldowns[static_cast<std::size_t>(app.cooldown)];
  app.syncing = true;
  gtk_editable_set_text(GTK_EDITABLE(app.cooldown_name), cooldown.name.c_str());
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.duration),
                            static_cast<double>(cooldown.duration.count()) / 1000.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.charges), cooldown.max_charges);
  gtk_label_set_text(GTK_LABEL(app.binding),
                     layerc::prototype::binding_text(profile, cooldown.id).c_str());
  app.syncing = false;
}

void refresh_document(App &app) {
  auto &profile = app.model.document().profile();
  app.syncing = true;
  gtk_editable_set_text(GTK_EDITABLE(app.name), profile.name.c_str());
  gtk_editable_set_text(GTK_EDITABLE(app.executable), profile.game.executable_name.c_str());
  gtk_list_box_remove_all(GTK_LIST_BOX(app.cooldowns));
  for (const auto &cooldown : profile.cooldowns)
    gtk_list_box_append(GTK_LIST_BOX(app.cooldowns), gtk_label_new(cooldown.name.c_str()));
  app.syncing = false;
  app.cooldown = 0;
  gtk_list_box_select_row(GTK_LIST_BOX(app.cooldowns),
                          gtk_list_box_get_row_at_index(GTK_LIST_BOX(app.cooldowns), 0));
  refresh_cooldown(app);
  gtk_widget_queue_draw(app.preview);
}

void on_profile_selected(GtkListBox *, GtkListBoxRow *row, gpointer data) {
  auto &app = *static_cast<App *>(data);
  if (row == nullptr || app.syncing) return;
  if (app.model.open(static_cast<std::size_t>(gtk_list_box_row_get_index(row)))) {
    refresh_document(app);
    set_status(app, "Profile loaded.");
  } else {
    set_status(app, "Could not read the selected profile.");
  }
}

void on_cooldown_selected(GtkListBox *, GtkListBoxRow *row, gpointer data) {
  auto &app = *static_cast<App *>(data);
  if (row == nullptr || app.syncing) return;
  app.cooldown = gtk_list_box_row_get_index(row);
  refresh_cooldown(app);
}

void on_field_changed(GtkWidget *, gpointer data) {
  auto &app = *static_cast<App *>(data);
  if (app.syncing) return;
  auto &document = app.model.document();
  auto &profile = document.profile();
  profile.name = gtk_editable_get_text(GTK_EDITABLE(app.name));
  profile.game.executable_name = gtk_editable_get_text(GTK_EDITABLE(app.executable));
  if (app.cooldown >= 0 && static_cast<std::size_t>(app.cooldown) < profile.cooldowns.size()) {
    auto &cooldown = profile.cooldowns[static_cast<std::size_t>(app.cooldown)];
    cooldown.name = gtk_editable_get_text(GTK_EDITABLE(app.cooldown_name));
    cooldown.duration = layerc::core::Milliseconds(std::llround(
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(app.duration)) * 1000.0));
    cooldown.max_charges =
        static_cast<std::uint32_t>(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app.charges)));
  }
  (void)document.history().record(profile, document.visual(), document.automation(), true);
  document.set_dirty(true);
  gtk_widget_queue_draw(app.preview);
}

void schedule_cycle(App &app, double seconds);

// One validation dialog's own state, so a replaced dialog's completion can
// never affect a newer one. A cycle dialog counts only once its own surface
// has painted a frame and the dialog has then been destroyed.
struct ValidationDialog {
  App *app{};
  GtkWidget *window{};
  GdkFrameClock *clock{};
  gulong paint_handler{};
  guint dismiss{};
  guint deadline{};
  bool cycle{};
  bool painted{};
};

void validation_destroyed(GtkWidget *, gpointer data) {
  auto *state = static_cast<ValidationDialog *>(data);
  auto &app = *state->app;
  if (state->paint_handler) g_signal_handler_disconnect(state->clock, state->paint_handler);
  g_clear_object(&state->clock);
  if (state->dismiss) g_source_remove(state->dismiss);
  if (state->deadline) g_source_remove(state->deadline);
  if (app.validation == state) app.validation = nullptr;
  if (state->cycle) {
    if (state->painted) {
      app.cycles.completed();
      schedule_cycle(app, layerc::prototype::CycleDriver::cycle_seconds);
    } else {
      layerc::prototype::announce("cycle-dialog-unpainted");  // Stops the workload.
    }
  }
  delete state;
}

gboolean validation_dismiss(gpointer data) {
  auto *state = static_cast<ValidationDialog *>(data);
  state->dismiss = 0;
  gtk_window_destroy(GTK_WINDOW(state->window));
  return G_SOURCE_REMOVE;
}

void validation_painted(GdkFrameClock *, gpointer data) {
  auto *state = static_cast<ValidationDialog *>(data);
  state->painted = true;
  g_signal_handler_disconnect(state->clock, state->paint_handler);
  state->paint_handler = 0;
  if (state->cycle && !state->dismiss) state->dismiss = g_idle_add(validation_dismiss, state);
}

void validation_mapped(GtkWidget *window, gpointer data) {
  auto *state = static_cast<ValidationDialog *>(data);
  if (state->clock) return;
  state->clock = GDK_FRAME_CLOCK(g_object_ref(gtk_widget_get_frame_clock(window)));
  state->paint_handler =
      g_signal_connect(state->clock, "after-paint", G_CALLBACK(validation_painted), state);
  gtk_widget_queue_draw(window);
}

gboolean validation_deadline(gpointer data) {
  auto *state = static_cast<ValidationDialog *>(data);
  state->deadline = 0;
  if (!state->painted) gtk_window_destroy(GTK_WINDOW(state->window));
  return G_SOURCE_REMOVE;
}

// The one validation dialog path, shared by Save failures and the cycles.
void show_validation(App &app, const std::string &text, const bool cycle = false) {
  if (app.validation) gtk_window_destroy(GTK_WINDOW(app.validation->window));
  auto *state = new ValidationDialog{};
  state->app = &app;
  state->cycle = cycle;
  state->window = gtk_window_new();
  auto *window = GTK_WINDOW(state->window);
  gtk_window_set_title(window, "Validation");
  gtk_window_set_transient_for(window, GTK_WINDOW(app.window));
  gtk_window_set_modal(window, TRUE);
  gtk_window_set_destroy_with_parent(window, TRUE);
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  gtk_box_append(GTK_BOX(box), gtk_label_new(text.c_str()));
  auto *ok = gtk_button_new_with_mnemonic("_OK");
  g_signal_connect_swapped(ok, "clicked", G_CALLBACK(gtk_window_destroy), window);
  gtk_box_append(GTK_BOX(box), ok);
  gtk_window_set_child(window, box);
  gtk_window_set_default_widget(window, ok);
  g_signal_connect(state->window, "map", G_CALLBACK(validation_mapped), state);
  g_signal_connect(state->window, "destroy", G_CALLBACK(validation_destroyed), state);
  if (cycle) state->deadline = g_timeout_add(2000, validation_deadline, state);
  app.validation = state;
  gtk_window_present(window);
}

void on_save(GtkButton *, gpointer data) {
  auto &app = *static_cast<App *>(data);
  const auto outcome = app.model.save();
  set_status(app, outcome);
  if (outcome != "Saved") show_validation(app, outcome);
}

void draw_preview(GtkDrawingArea *, cairo_t *cr, int width, int height, gpointer data) {
  auto &app = *static_cast<App *>(data);
  cairo_set_source_rgb(cr, 0.12, 0.14, 0.16);
  cairo_paint(cr);
  cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 11);
  const auto &profile = app.model.document().profile();
  for (const auto &panel : profile.panels) {
    const double x = panel.anchor_x * width - 90;
    double y = panel.anchor_y * height - 10;
    for (const auto &id : panel.cooldown_ids) {
      cairo_set_source_rgb(cr, 0.31, 0.73, 0.78);
      cairo_rectangle(cr, x, y, 180, 14);
      cairo_fill(cr);
      cairo_set_source_rgb(cr, 0.95, 0.96, 0.97);
      cairo_move_to(cr, x + 4, y + 11);
      cairo_show_text(cr, id.c_str());
      y += 16;
    }
  }
}

void on_list_right_click(GtkGestureClick *, int, double x, double y, gpointer data) {
  auto &app = *static_cast<App *>(data);
  const GdkRectangle where{static_cast<int>(x), static_cast<int>(y), 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(app.menu), &where);
  gtk_popover_popup(GTK_POPOVER(app.menu));
}

void on_menu_action(GSimpleAction *action, GVariant *, gpointer data) {
  auto &app = *static_cast<App *>(data);
  set_status(app, std::string("Menu: ") + g_action_get_name(G_ACTION(action)));
}

// Displays the key symbol (as FLTK's Fl::event_key does), not the hardware code.
gboolean on_capture_key(GtkEventControllerKey *, guint key, guint, GdkModifierType,
                        gpointer data) {
  auto &app = *static_cast<App *>(data);
  if (!app.capturing) return FALSE;
  if (key == GDK_KEY_Tab || key == GDK_KEY_ISO_Left_Tab || key == GDK_KEY_Escape) {
    app.capturing = false;
    set_status(app, "Capture cancelled.");
    return key == GDK_KEY_Escape;
  }
  if (key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R || key == GDK_KEY_Control_L ||
      key == GDK_KEY_Control_R || key == GDK_KEY_Alt_L || key == GDK_KEY_Alt_R) return TRUE;
  app.capturing = false;
  gtk_label_set_text(GTK_LABEL(app.binding), ("Key " + std::to_string(key)).c_str());
  set_status(app, "Captured focused key (prototype display only).");
  return TRUE;
}

gboolean cycle_step(gpointer data);

void schedule_cycle(App &app, double seconds) {
  g_timeout_add(static_cast<guint>(seconds * 1000.0), cycle_step, &app);
}

gboolean cycle_step(gpointer data) {
  auto &app = *static_cast<App *>(data);
  using Step = layerc::prototype::CycleDriver::Step;
  const auto step = app.cycles.next();
  if (step == Step::done) return G_SOURCE_REMOVE;
  if (step == Step::pause) {
    schedule_cycle(app, layerc::prototype::CycleDriver::pause_seconds);
    return G_SOURCE_REMOVE;
  }
  const auto rows = app.model.rows().size();
  if (rows != 0) {
    const auto next = static_cast<int>((app.model.selected() + 1) % rows);
    gtk_list_box_select_row(GTK_LIST_BOX(app.list),
                            gtk_list_box_get_row_at_index(GTK_LIST_BOX(app.list), next));
  }
  // validation_destroyed() schedules the next cycle once the dialog painted.
  show_validation(app, "Validation", true);
  return G_SOURCE_REMOVE;
}

GtkWidget *labeled(GtkWidget *grid, int row, const char *text, GtkWidget *field) {
  auto *label = gtk_label_new_with_mnemonic(text);
  gtk_label_set_mnemonic_widget(GTK_LABEL(label), field);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
  gtk_widget_set_hexpand(field, TRUE);
  gtk_grid_attach(GTK_GRID(grid), field, 1, row, 1, 1);
  return field;
}

void activate(GtkApplication *application, gpointer data) {
  auto &app = *static_cast<App *>(data);
  auto *css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(css, "* { font-family: \"DejaVu Sans\"; font-size: 14px; }");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
  app.window = gtk_application_window_new(application);
  gtk_window_set_title(GTK_WINDOW(app.window), "LayerC Editor (GTK 4 prototype)");
  gtk_window_set_default_size(GTK_WINDOW(app.window), 900, 600);

  auto *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  app.list = gtk_list_box_new();
  gtk_accessible_update_property(GTK_ACCESSIBLE(app.list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Profiles", -1);
  for (const auto &row : app.model.rows())
    gtk_list_box_append(GTK_LIST_BOX(app.list), gtk_label_new(row.label.c_str()));
  g_signal_connect(app.list, "row-selected", G_CALLBACK(on_profile_selected), &app);
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app.list);
  gtk_widget_set_size_request(scroll, 240, -1);
  gtk_paned_set_start_child(GTK_PANED(paned), scroll);

  auto *menu_model = g_menu_new();
  g_menu_append(menu_model, "_New profile", "proto.new");
  g_menu_append(menu_model, "_Duplicate", "proto.duplicate");
  g_menu_append(menu_model, "De_lete", "proto.delete");
  auto *actions = g_simple_action_group_new();
  for (const char *name : {"new", "duplicate", "delete"}) {
    auto *action = g_simple_action_new(name, nullptr);
    g_signal_connect(action, "activate", G_CALLBACK(on_menu_action), &app);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action));
    g_object_unref(action);
  }
  gtk_widget_insert_action_group(app.window, "proto", G_ACTION_GROUP(actions));
  g_object_unref(actions);
  app.menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu_model));
  g_object_unref(menu_model);
  gtk_widget_set_parent(app.menu, app.list);
  // This manually parented popover is not a GtkListBoxRow. Unparent it before
  // GtkListBox's row-only disposal path; normal WM close must be warning-free.
  g_signal_connect(app.window, "close-request", G_CALLBACK(+[](GtkWindow *, gpointer data) -> gboolean {
    auto &closing = *static_cast<App *>(data);
    if (closing.menu) {
      gtk_widget_unparent(closing.menu);
      closing.menu = nullptr;
    }
    return FALSE;
  }), &app);
  auto *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_list_right_click), &app);
  gtk_widget_add_controller(app.list, GTK_EVENT_CONTROLLER(click));

  auto *notebook = gtk_notebook_new();
  auto *profile_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(profile_grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(profile_grid), 8);
  app.name = labeled(profile_grid, 0, "_Name", gtk_entry_new());
  app.executable = labeled(profile_grid, 1, "_Application", gtk_entry_new());
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), profile_grid, gtk_label_new("Profile"));

  auto *cooldown_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  app.cooldowns = gtk_list_box_new();
  gtk_accessible_update_property(GTK_ACCESSIBLE(app.cooldowns), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Cooldowns", -1);
  g_signal_connect(app.cooldowns, "row-selected", G_CALLBACK(on_cooldown_selected), &app);
  auto *cooldown_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(cooldown_scroll), app.cooldowns);
  gtk_widget_set_size_request(cooldown_scroll, 180, -1);
  gtk_box_append(GTK_BOX(cooldown_box), cooldown_scroll);
  auto *fields = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(fields), 6);
  gtk_grid_set_column_spacing(GTK_GRID(fields), 8);
  app.cooldown_name = labeled(fields, 0, "Cooldown _name", gtk_entry_new());
  app.duration = labeled(fields, 1, "_Duration (s)", gtk_spin_button_new_with_range(0.001, 86400, 0.1));
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(app.duration), 3);
  app.charges = labeled(fields, 2, "_Charges", gtk_spin_button_new_with_range(1, 99, 1));
  app.binding = labeled(fields, 3, "Binding", gtk_label_new(""));
  app.capture = gtk_button_new_with_mnemonic("_Capture key");
  g_signal_connect(app.capture, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    auto &capturing = *static_cast<App *>(data);
    capturing.capturing = !capturing.capturing;
    set_status(capturing, capturing.capturing ? "Press a focused key; Escape or Tab cancels." : "Capture cancelled.");
  }), &app);
  auto *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_capture_key), &app);
  gtk_widget_add_controller(app.capture, keys);
  gtk_grid_attach(GTK_GRID(fields), app.capture, 1, 4, 1, 1);
  gtk_widget_set_hexpand(fields, TRUE);
  gtk_box_append(GTK_BOX(cooldown_box), fields);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), cooldown_box, gtk_label_new("Cooldowns"));

  app.preview = gtk_drawing_area_new();
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app.preview), draw_preview, &app, nullptr);
  gtk_accessible_update_property(GTK_ACCESSIBLE(app.preview), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Panel preview", -1);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), app.preview, gtk_label_new("Panels"));

  for (auto *entry : {app.name, app.executable, app.cooldown_name})
    g_signal_connect(entry, "changed", G_CALLBACK(on_field_changed), &app);
  for (auto *spin : {app.duration, app.charges})
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_field_changed), &app);

  auto *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_vexpand(notebook, TRUE);
  gtk_box_append(GTK_BOX(right), notebook);
  auto *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  app.status = gtk_label_new("Ready");
  gtk_widget_set_hexpand(app.status, TRUE);
  gtk_widget_set_halign(app.status, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(bar), app.status);
  auto *save = gtk_button_new_with_mnemonic("_Save");
  g_signal_connect(save, "clicked", G_CALLBACK(on_save), &app);
  gtk_box_append(GTK_BOX(bar), save);
  gtk_box_append(GTK_BOX(right), bar);
  gtk_paned_set_end_child(GTK_PANED(paned), right);
  gtk_window_set_child(GTK_WINDOW(app.window), paned);

  if (!app.model.rows().empty())
    gtk_list_box_select_row(GTK_LIST_BOX(app.list),
                            gtk_list_box_get_row_at_index(GTK_LIST_BOX(app.list), 0));
  gtk_window_present(GTK_WINDOW(app.window));
  app.model.announce_fixture();
  layerc::prototype::announce("ready");
  if (app.cycles.active())
    schedule_cycle(app, layerc::prototype::CycleDriver::cycle_seconds);
}

} // namespace

int main(int argc, char **argv) {
  App app;
  if (!app.model.scan()) return 64;
  if (app.cycles.active() && app.model.rows().empty()) {
    std::fprintf(stderr, "Prototype refused: cycle workload requires a populated fixture\n");
    return 65;
  }
  auto *application =
      gtk_application_new("io.github.EnderKeera.LayerC.GtkPrototype", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(application, "activate", G_CALLBACK(activate), &app);
  const int status = g_application_run(G_APPLICATION(application), argc, argv);
  g_object_unref(application);
  return status;
}
