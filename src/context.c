/**
 * context.c
 *
 * Copyright (c) 2017 endaaman
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "context.h"
#include "app.h"
#include "builtin.h"
#include "property.h"
#include "command.h"


typedef void (*TymCommandFunc)(Context* context);
typedef struct {
  unsigned key;
  GdkModifierType mod;
  TymCommandFunc func;
} KeyPair;

typedef struct {
  const char* name;
  TymCommandFunc func;
} SignalDefinition;

static const char* TYM_MODULE_NAME = "tym";
static const char* TYM_DEFAULT_NOTIFICATION_TITLE = "tym";

static KeyPair DEFAULT_KEY_PAIRS[] = {
  { GDK_KEY_c    , GDK_CONTROL_MASK | GDK_SHIFT_MASK, command_copy_clipboard      },
  { GDK_KEY_v    , GDK_CONTROL_MASK | GDK_SHIFT_MASK, command_paste_clipboard     },
  { GDK_KEY_r    , GDK_CONTROL_MASK | GDK_SHIFT_MASK, command_reload              },
  {},
};

static SignalDefinition SIGNALS[] = {
  { "ReloadTheme", command_reload_theme },
  {},
};

char* context_acquire_config_path(Context* context)
{
  char* path = option_get_config_path(context->option);
  if (is_none(path)) {
    return NULL;
  }
  if (!path) {
    return g_build_path(
      G_DIR_SEPARATOR_S,
      g_get_user_config_dir(),
      TYM_CONFIG_DIR_NAME,
      TYM_CONFIG_FILE_NAME,
      NULL
    );
  }

  if (g_path_is_absolute(path)) {
    return g_strdup(path);
  }
  char* cwd = g_get_current_dir();
  path = g_build_path(G_DIR_SEPARATOR_S, cwd, path, NULL);
  g_free(cwd);
  return path;
}

char* context_acquire_theme_path(Context* context)
{
  char* path = option_get_theme_path(context->option);
  if (is_none(path)) {
    return NULL;
  }

  if (!path) {
    return g_build_path(
      G_DIR_SEPARATOR_S,
      g_get_user_config_dir(),
      TYM_CONFIG_DIR_NAME,
      TYM_THEME_FILE_NAME,
      NULL
    );
  }
  if (g_path_is_absolute(path)) {
    return g_strdup(path);
  }

  char* cwd = g_get_current_dir();
  path = g_build_path(G_DIR_SEPARATOR_S, cwd, path, NULL);
  g_free(cwd);
  return path;
}

void context_load_lua_context(Context* context)
{
  if (option_get_nolua(context->option)) {
    g_message("Lua context is not loaded");
    return;
  }

  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  luaX_requirec(L, TYM_MODULE_NAME, builtin_register_module, true, context);
  lua_pop(L, 1);
  context->lua = L;
}

Context* context_init()
{
  dd("init");
  Context* context = g_malloc0(sizeof(Context));
  context->state.initializing = true;
  context->meta = meta_init();
  context->option = option_init(context->meta);
  context->config = config_init(context->meta);
  context->keymap = keymap_init();
  context->hook = hook_init();
  context->layout = layout_init();
  context->app = G_APPLICATION(gtk_application_new(
    TYM_APP_ID,
    G_APPLICATION_NON_UNIQUE | G_APPLICATION_HANDLES_COMMAND_LINE)
  );
  return context;
}

void context_close(Context* context)
{
  dd("close");
  meta_close(context->meta);
  option_close(context->option);
  config_close(context->config);
  keymap_close(context->keymap);
  hook_close(context->hook);
  layout_close(context->layout);
  g_object_unref(context->app);
  if (context->lua) {
    lua_close(context->lua);
  }
  g_free(context);
}

int context_start(Context* context, int argc, char** argv)
{
  GApplication* app = context->app;
  g_application_add_main_option_entries(app, context->option->entries);

  g_signal_connect(app, "activate", G_CALLBACK(on_activate), context);
  g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), context);
  return g_application_run(app, argc, argv);
}

void context_load_device(Context* context)
{
  GdkDisplay* display = gdk_display_get_default();
#ifdef TYM_USE_GDK_SEAT
  GdkSeat* seat = gdk_display_get_default_seat(display);
  context->device = gdk_seat_get_keyboard(seat);
#else
  GdkDeviceManager* manager = gdk_display_get_device_manager(display);
  GList* devices = gdk_device_manager_list_devices(manager, GDK_DEVICE_TYPE_MASTER);
  for (GList* li = devices; li != NULL; li = li->next) {
  GdkDevice* d = (GdkDevice*)li->data;
    if (gdk_device_get_source(d) == GDK_SOURCE_KEYBOARD) {
      context->device = d;
      break;
    }
  }
  g_list_free(devices);
#endif
}

static void context_on_lua_error(Context* context, const char* error)
{
  char* message = g_strdup_printf("%s", error);
  g_message("%s", message);
  context_notify(context, error, "tym: lua error");
  g_free(message);
}

void context_restore_default(Context* context)
{
  for (GList* li = context->meta->list; li != NULL; li = li->next) {
    MetaEntry* e = (MetaEntry*)li->data;
    char* target = NULL;
    if (strncmp("color_", e->name, 6) == 0) {
      g_ascii_strtoull(&e->name[6], &target, 10);
      if (&e->name[6] != target) {
        // skip loading `color_%d` in this loop
        continue;
      }
    }
    char* key = e->name;
    switch (e->type) {
      case META_ENTRY_TYPE_STRING: {
        context_set_str(context, key, e->default_value);
        break;
      }
      case META_ENTRY_TYPE_INTEGER: {
        context_set_int(context, key, *(int*)e->default_value);
        break;
      }
      case META_ENTRY_TYPE_BOOLEAN: {
        context_set_bool(context, key, *(bool*)e->default_value);
        break;
      }
      case META_ENTRY_TYPE_NONE:
        break;
    }
  }
  // set colors here
  GdkRGBA* palette = g_new0(GdkRGBA, 16);
  unsigned i = 0;
  while (i < 16) {
    char s[10] = {};
    g_snprintf(s, 10, "color_%d", i);
    MetaEntry* e = meta_get_entry(context->meta, s);
    assert(gdk_rgba_parse(&palette[i], e->default_value));
    i += 1;
  }
  vte_terminal_set_colors(context->layout->vte, NULL, NULL, palette, 16);
}

void context_override_by_option(Context* context)
{
  for (GList* li = context->meta->list; li != NULL; li = li->next) {
    MetaEntry* e = (MetaEntry*)li->data;
    char* key = e->name;
    switch (e->type) {
      case META_ENTRY_TYPE_STRING: {
        const char* v = NULL;
        bool has_value = option_get_str_value(context->option, key, &v);
        if (has_value) {
          context_set_str(context, key, v);
        }
        break;
      }
      case META_ENTRY_TYPE_INTEGER: {
        int v = 0;
        bool has_value = option_get_int_value(context->option, key, &v);
        if (has_value) {
          context_set_int(context, key, v);
        }
        break;
      }
      case META_ENTRY_TYPE_BOOLEAN: {
        bool v = false;
        bool has_value = option_get_bool_value(context->option, key, &v);
        if (has_value) {
          context_set_bool(context, key, v);
        }
        break;
      }
      case META_ENTRY_TYPE_NONE:
        break;
    }
  }
}

void context_load_config(Context* context)
{
  df();
  if (!context->lua) {
    g_message("Skipped loading config because Lua context is not loaded.");
    return;
  }

  if (context->state.loading) {
    g_message("Tried to load config recursively. Ignoring loading.");
    return;
  }

  context->state.loading = true;

  char* config_path = context_acquire_config_path(context);
  dd("config path: `%s`", config_path);
  if (!config_path) {
    g_message("Skipped config loading.");
    goto EXIT;
  }

  if (!g_file_test(config_path, G_FILE_TEST_EXISTS)) {
    g_message("Config file (`%s`) does not exist. Skipped config loading.", config_path);
    goto EXIT;
  }

  lua_State* L = context->lua;
  int result = luaL_dofile(L, config_path);
  if (result != LUA_OK) {
    const char* error = lua_tostring(L, -1);
    lua_pop(L, 1);
    context_on_lua_error(context, error);
    goto EXIT;
  }

EXIT:
  context->state.loading = false;
  if (config_path) {
    g_free(config_path);
  }
  dd("load config end");
}

void context_load_theme(Context* context)
{
  df();
  if (!context->lua) {
    g_message("Skipped loading theme because Lua context is not loaded.");
    return;
  }

  char* theme_path = context_acquire_theme_path(context);
  dd("theme path: `%s`", theme_path);
  if (!theme_path) {
    g_message("Skipped theme loading.");
    goto EXIT;
  }

  if (!g_file_test(theme_path, G_FILE_TEST_EXISTS)) {
    // do not warn
    g_message("Theme file (`%s`) does not exist. Skiped theme loading.", theme_path);
    goto EXIT;
  }

  lua_State* L = context->lua;
  int result = luaL_loadfile(L, theme_path);
  if (result != LUA_OK) {
    g_warning("Could not load `%s`.", theme_path);
    goto EXIT;
  }

  if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
    const char* error = lua_tostring(L, -1);
    g_message("Got error excuting theme script. Stopped theme loading.");
    context_on_lua_error(context, error);
    goto EXIT;
  }

  if (!lua_istable(L, -1)) {
    g_message("Theme script must return a table (got %s). Skiped theme assignment.", lua_typename(L, lua_type(L, -1)));
    goto EXIT;
  }

  lua_pushnil(L);
  while (lua_next(L, -2)) {
    lua_pushvalue(L, -2);
    const char* key = lua_tostring(L, -1);
    const char* value = lua_tostring(L, -2);
    lua_pop(L, 2);
    if (!value || strncmp("color_", key, 6) != 0) {
      g_warning("%s: Invalid color key in theme: `%s`", theme_path, key);
      continue;
    }
    context_set_str(context, key, value);
  }
  lua_pop(L, 1);

EXIT:
  if (theme_path) {
    g_free(theme_path);
  }
  dd("load theme end");
}

static bool context_perform_default(Context* context, unsigned key, GdkModifierType mod)
{
  unsigned i = 0;
  while (DEFAULT_KEY_PAIRS[i].func) {
    KeyPair* pair = &DEFAULT_KEY_PAIRS[i];
    if ((key == pair->key) && !(~mod & pair->mod)) {
      pair->func(context);
      return true;
    }
    i++;
  }
  return false;
}

bool context_perform_keymap(Context* context, unsigned key, GdkModifierType mod)
{
  if (context->lua) {
    bool result = false;
    char* error = NULL;
    if (keymap_perform(context->keymap, context->lua, key, mod, &result, &error)) {
      // if the keymap func is normally excuted,  default action will be canceled.
      // if `return true` in the keymap func, default action will be performed.
      if (!result) {
        return true;
      }
    } else {
      if (error) {
        context_on_lua_error(context, error);
        g_free(error);
        // if the keymap func has error, default action will be canceled.
        return true;
      }
    }
  }
  if (context_get_bool(context, "ignore_default_keymap")) {
    return false;
  }
  return context_perform_default(context, key, mod);
}

void context_handle_signal(Context* context, const char* signal_name, GVariant* parameters)
{
  UNUSED(parameters);
  dd("receive signal: %s", signal_name);
  unsigned i = 0;
  while (SIGNALS[i].func) {
    SignalDefinition* def = &SIGNALS[i];
    if (0 == g_strcmp0(def->name, signal_name)) {
      def->func(context);
      return;
    }
    i++;
  }
}

GdkWindow* context_get_gdk_window(Context* context)
{
  return gtk_widget_get_window(GTK_WIDGET(context->layout->window));
}

void context_notify(Context* context, const char* body, const char* title)
{
  GNotification* notification = g_notification_new(title ? title : TYM_DEFAULT_NOTIFICATION_TITLE);
  GIcon* icon = g_themed_icon_new_with_default_fallbacks(config_get_str(context->config, "icon"));

  g_notification_set_icon(notification, G_ICON(icon));
  g_notification_set_body(notification, body);
  g_notification_set_priority(notification, G_NOTIFICATION_PRIORITY_URGENT);
  g_application_send_notification(context->app, TYM_APP_ID, notification);

  g_object_unref(notification);
  g_object_unref(icon);
}

void context_launch_uri(Context* context, const char* uri)
{
  dd("launch: `%s`", uri);
  GError* error = NULL;
  GdkDisplay* display = gdk_display_get_default();
  GdkAppLaunchContext* ctx = gdk_display_get_app_launch_context(display);
  gdk_app_launch_context_set_screen(ctx, gdk_screen_get_default());
  /* gdk_app_launch_context_set_timestamp(ctx, event->time); */
  if (!g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(ctx), &error)) {
    char* message = g_strdup_printf("Failed to launch uri: %s", error->message);
    context_notify(context, message, NULL);
    g_message("%s", message);
    g_free(message);
    g_error_free(error);
  }
}

const char* context_get_str(Context* context, const char* key)
{
  MetaEntry* e = meta_get_entry(context->meta, key);
  if (e->getter) {
    return ((PropertyStrGetter)e->getter)(context, key);
  }
  return config_get_str(context->config, key);
}

int context_get_int(Context* context, const char* key)
{
  MetaEntry* e = meta_get_entry(context->meta, key);
  if (e->getter) {
    return ((PropertyIntGetter)e->getter)(context, key);
  }
  return config_get_int(context->config, key);
}

bool context_get_bool(Context* context, const char* key)
{
  MetaEntry* e = meta_get_entry(context->meta, key);
  if (e->getter) {
    return ((PropertyBoolGetter)e->getter)(context, key);
  }
  return config_get_bool(context->config, key);
}

void context_set_str(Context* context, const char* key, const char* value)
{
  MetaEntry* e = meta_get_entry(context->meta, key);
  if (e->setter) {
    ((PropertyStrSetter)e->setter)(context, key, value);
    return;
  }
  if (!e->getter) {
    config_set_str(context->config, key, value);
    return;
  }
  dd("`%s`: setter is not provided but getter is provided", key);
}

void context_set_int(Context* context, const char* key, int value)
{
  MetaEntry* e = meta_get_entry(context->meta, key);
  if (e->setter) {
    ((PropertyIntSetter)e->setter)(context, key, value);
    return;
  }
  if (!e->getter) {
    config_set_int(context->config, key, value);
    return;
  }
  dd("`%s`: setter is not provided but getter is provided", key);
}

void context_set_bool(Context* context, const char* key, bool value)
{
  MetaEntry* e = meta_get_entry(context->meta, key);
  if (e->setter) {
    ((PropertyBoolSetter)e->setter)(context, key, value);
    return;
  }
  if (!e->getter) {
    config_set_bool(context->config, key, value);
    return;
  }
  dd("`%s`: setter is not provided but getter is provided", key);
}
