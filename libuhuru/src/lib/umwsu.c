#include <libumwsu/module.h>
#include <libumwsu/scan.h>
#include "alert.h"
#include "dir.h"
#include "modulep.h"
#include "quarantine.h"
#include "remote.h"
#include "statusp.h"
#include "watchp.h"

#include <assert.h>
#include <glib.h>
#include <magic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct umwsu {
  int verbosity;
  int is_remote;
  GPtrArray *modules;

  GHashTable *mime_types_table;
  magic_t magic;
  /* for now... */
  struct umwsu_watch *watch;
};

static struct umwsu *umwsu_new(int is_remote)
{
  struct umwsu *u = (struct umwsu *)malloc(sizeof(struct umwsu));

  assert(u != NULL);

  u->verbosity = 0;
  u->is_remote = is_remote;

  u->modules = g_ptr_array_new();

  u->magic = magic_open(MAGIC_MIME_TYPE);
  magic_load(u->magic, NULL);

  u->mime_types_table = g_hash_table_new(g_str_hash, g_str_equal);

  u->watch = NULL;

  return u;
}

static void umwsu_add_module(struct umwsu *u, struct umwsu_module *mod)
{
  g_ptr_array_add(u->modules, mod);
}

static void load_entry(const char *full_path, enum dir_entry_flag flags, int errno, void *data)
{
  struct umwsu *u = (struct umwsu *)data;
  const char *t = magic_file(u->magic, full_path);
  struct umwsu_module *mod;

  if (strcmp("application/x-sharedlib", t))
    return;

  if (u->verbosity >= 1)
    printf("UMWSU: loading module object: %s\n", full_path);

  mod = module_new(full_path);

  umwsu_add_module(u, mod);
}

static int umwsu_module_load_directory(struct umwsu *u, const char *directory)
{
  return dir_map(directory, 0, load_entry, u);
}

static void umwsu_module_load_all(struct umwsu *u)
{
  if (!u->is_remote)
    umwsu_module_load_directory(u, LIBUMWSU_MODULES_PATH);

  if (!u->is_remote) {
    umwsu_add_module(u, &umwsu_mod_alert);
    umwsu_add_module(u, &umwsu_mod_quarantine);
  }

  umwsu_add_module(u, &umwsu_mod_remote);
}

static void umwsu_module_conf_all(struct umwsu *u)
{
  int i;

  for (i = 0; i < u->modules->len; i++) {
    struct umwsu_module *mod = (struct umwsu_module *)g_ptr_array_index(u->modules, i);

    conf_load(mod);
  }
}

static void umwsu_add_mime_type(struct umwsu *u, const char *mime_type, struct umwsu_module *mod)
{
  GPtrArray *mod_array;

  mod_array = (GPtrArray *)g_hash_table_lookup(u->mime_types_table, mime_type);

  if (mod_array == NULL) {
    mod_array = g_ptr_array_new();

    g_hash_table_insert(u->mime_types_table, (gpointer)mime_type, mod_array);
  }

  g_ptr_array_add(mod_array, mod);
}

static void uwmsu_module_init_all(struct umwsu *u)
{
  int i;

  for (i = 0; i < u->modules->len; i++) {
    struct umwsu_module *mod = (struct umwsu_module *)g_ptr_array_index(u->modules, i);

    if (mod->init != NULL)
      (*mod->init)(&mod->data);

    if (mod->mime_types != NULL) {
      const char **p;

      for(p = mod->mime_types; *p != NULL; p++)
	umwsu_add_mime_type(u, *p, mod);
    }
  }
}

static void mod_print(gpointer data, gpointer user_data)
{
  module_print((struct umwsu_module *)data, stderr);
}

static void uwm_module_print_all(struct umwsu *u)
{
  g_ptr_array_foreach(u->modules, mod_print, NULL);
}

struct umwsu *umwsu_open(int is_remote)
{
  struct umwsu *u = umwsu_new(is_remote);

  umwsu_module_load_all(u);

  umwsu_module_conf_all(u);

  uwmsu_module_init_all(u);

  /* uwm_module_print_all(u); */

  return u;
}

int umwsu_is_remote(struct umwsu *u)
{
  return u->is_remote;
}

void umwsu_set_verbose(struct umwsu *u, int verbosity)
{
  u->verbosity = verbosity;
}

int umwsu_get_verbose(struct umwsu *u)
{
  return u->verbosity;
}

static void mod_print_name(gpointer data, gpointer user_data)
{
  struct umwsu_module *mod = (struct umwsu_module *)data;
  printf("%s ", mod->name);
}

static void print_mime_type_entry(gpointer key, gpointer value, gpointer user_data)
{
  printf("UMWSU: MIME type: %s handled by module: ", (char *)key);
  g_ptr_array_foreach((GPtrArray *)value, mod_print_name, NULL);
  printf("\n");
}

void umwsu_print(struct umwsu *u)
{
  printf("UMWSU: modules loaded: ");
  g_ptr_array_foreach(u->modules, mod_print_name, NULL);
  printf("\n");

  g_hash_table_foreach(u->mime_types_table, print_mime_type_entry, NULL);
}

struct umwsu_module *umwsu_get_module_by_name(struct umwsu *u, const char *name)
{
  int i;

  for (i = 0; i < u->modules->len; i++) {
    struct umwsu_module *mod = (struct umwsu_module *)g_ptr_array_index(u->modules, i);

    if (!strcmp(mod->name, name))
      return mod;
  }

  return NULL;
}

GPtrArray *umwsu_get_applicable_modules(struct umwsu *u, magic_t magic, const char *path, char **p_mime_type)
{
  GPtrArray *modules;

  if (magic == NULL)
    magic = u->magic;

  *p_mime_type = strdup(magic_file(magic, path));

  modules = (GPtrArray *)g_hash_table_lookup(u->mime_types_table, *p_mime_type);

  if (modules != NULL)
    return modules;

  modules = (GPtrArray *)g_hash_table_lookup(u->mime_types_table, "*");

  return modules;
}

void umwsu_close(struct umwsu *u)
{
  magic_close(u->magic);

  /* must close all modules */
}

void umwsu_watch(struct umwsu *u, const char *dir)
{
  if (u->watch == NULL)
    u->watch = umwsu_watch_new();

  umwsu_watch_add(u->watch, dir);
}

int umwsu_watch_next_event(struct umwsu *u, struct umwsu_watch_event *event)
{
  return umwsu_watch_wait(u->watch, event);
}
