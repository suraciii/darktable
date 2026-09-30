/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "mcp/mcp_params.h"
#include "mcp/dt_bridge.h"

#include "common/colorspaces.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "common/database.h"
#include "common/film.h"
#include "common/variables.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/datetime.h"
#include "common/colorlabels.h"
#include "common/ratings.h"
#include "views/view.h"
#include "common/introspection.h"
#include "common/iop_order.h"
#include "common/history.h"
#include "common/styles.h"
#include "common/usermanual_url.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"

#include <cairo/cairo.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

// when set, every tool that would change the library refuses instead
static gboolean _read_only = FALSE;

void dt_bridge_set_read_only(const gboolean on)
{
  _read_only = on;
}

static void _seterr(char **err, const char *fmt, ...)
{
  if(!err) return;
  va_list ap;
  va_start(ap, fmt);
  *err = g_strdup_vprintf(fmt, ap);
  va_end(ap);
}

static uint8_t *_hex_to_bytes(const char *hex, size_t *outlen)
{
  if(!hex) return NULL;
  const size_t n = strlen(hex);
  if(n % 2) return NULL;
  const size_t bl = n / 2;
  uint8_t *b = g_malloc(bl ? bl : 1);
  for(size_t i = 0; i < bl; i++)
  {
    const int hi = g_ascii_xdigit_value(hex[2 * i]);
    const int lo = g_ascii_xdigit_value(hex[2 * i + 1]);
    if(hi < 0 || lo < 0)
    {
      g_free(b);
      return NULL;
    }
    b[i] = (uint8_t)((hi << 4) | lo);
  }
  *outlen = bl;
  return b;
}

static char *_bytes_to_hex(const uint8_t *b, size_t n)
{
  char *s = g_malloc(2 * n + 1);
  static const char hexd[] = "0123456789abcdef";
  for(size_t i = 0; i < n; i++)
  {
    s[2 * i] = hexd[b[i] >> 4];
    s[2 * i + 1] = hexd[b[i] & 0xf];
  }
  s[2 * n] = '\0';
  return s;
}

static dt_iop_module_so_t *_find_so(const char *op)
{
  if(!op) return NULL;
  return dt_iop_get_module_so(op);
}

// full usermanual URL for a module op, or NULL (caller frees)
static char *_doc_url(const char *op)
{
  const char *topic = dt_get_help_url(op); // pointer into a static table; do not free
  return topic ? dt_get_manual_url(topic) : NULL;
}

static void _add_doc_url(JsonBuilder *b, const char *op)
{
  char *url = _doc_url(op);
  if(url)
  {
    json_builder_set_member_name(b, "doc_url");
    json_builder_add_string_value(b, url);
    g_free(url);
  }
}

static char *_builder_to_string(JsonBuilder *b)
{
  JsonNode *root = json_builder_get_root(b);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *out = json_generator_to_data(gen, NULL);
  g_object_unref(gen);
  json_node_unref(root);
  return out;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

char *dt_bridge_list_modules_json(void)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_array(b);
  for(GList *m = darktable.iop; m; m = g_list_next(m))
  {
    dt_iop_module_so_t *so = (dt_iop_module_so_t *)m->data;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "operation");
    json_builder_add_string_value(b, so->op);
    json_builder_set_member_name(b, "version");
    json_builder_add_int_value(b, so->version ? so->version() : -1);
    json_builder_set_member_name(b, "have_introspection");
    json_builder_add_boolean_value(b, so->have_introspection);
    _add_doc_url(b, so->op);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

char *dt_bridge_module_schema_json(const char *op, char **err)
{
  dt_iop_module_so_t *so = _find_so(op);
  if(!so)
  {
    _seterr(err, "unknown module operation '%s'", op ? op : "(null)");
    return NULL;
  }
  if(!so->have_introspection || !so->get_introspection
     || !so->get_introspection_linear)
  {
    _seterr(err, "module '%s' has no introspection", op);
    return NULL;
  }
  dt_introspection_t *intro = so->get_introspection();
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "operation");
  json_builder_add_string_value(b, so->op);
  json_builder_set_member_name(b, "params_version");
  json_builder_add_int_value(b, intro->params_version);
  json_builder_set_member_name(b, "params_size");
  json_builder_add_int_value(b, (gint64)intro->size);
  _add_doc_url(b, so->op);
  json_builder_set_member_name(b, "schema");
  dt_mcp_params_schema(b, intro->field);
  json_builder_end_object(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

// serialize the complete params tree, including nested arrays and structs
static void _write_fields_object(dt_iop_module_so_t *so, const void *blob,
                                 JsonBuilder *b)
{
  dt_introspection_t *intro = so->get_introspection();
  dt_mcp_params_values(b, intro->field, blob);
}



char *dt_bridge_decode_params_json(const char *op, const char *blob_hex,
                                   int params_version, char **err)
{
  dt_iop_module_so_t *so = _find_so(op);
  if(!so)
  {
    _seterr(err, "unknown module operation '%s'", op ? op : "(null)");
    return NULL;
  }
  if(!so->have_introspection || !so->get_introspection || !so->get_p)
  {
    _seterr(err, "module '%s' has no introspection", op);
    return NULL;
  }

  size_t blen = 0;
  uint8_t *blob = _hex_to_bytes(blob_hex, &blen);
  if(!blob) { _seterr(err, "invalid hex blob"); return NULL; }

  dt_introspection_t *intro = so->get_introspection();
  if(params_version <= 0 || params_version != intro->params_version)
  {
    _seterr(err, "module '%s' requires params_version %d, got %d",
            op, intro->params_version, params_version);
    g_free(blob);
    return NULL;
  }
  if(blen != intro->size)
  {
    _seterr(err, "blob size %zu != module '%s' params size %zu",
            blen, op, intro->size);
    g_free(blob);
    return NULL;
  }

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "operation");
  json_builder_add_string_value(b, so->op);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, intro->params_version);
  json_builder_set_member_name(b, "fields");
  _write_fields_object(so, blob, b);
  json_builder_end_object(b);

  char *out = _builder_to_string(b);
  g_object_unref(b);
  g_free(blob);
  return out;
}

// build a params blob from defaults, then validate and apply a complete JSON tree
static uint8_t *_seed_and_apply(dt_iop_module_so_t *so, const void *defaults,
                                JsonObject *fields, size_t *size, char **err)
{
  dt_introspection_t *intro = so->get_introspection();
  if(!defaults)
  {
    _seterr(err, "module '%s' has no initialized parameter block", so->op);
    return NULL;
  }
  uint8_t *blob = g_malloc(intro->size);
  memcpy(blob, defaults, intro->size);
  if(fields)
  {
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, fields);
    const gboolean ok = dt_mcp_params_apply(intro->field, blob, node, err);
    json_node_free(node);
    if(!ok) { g_free(blob); return NULL; }
  }
  *size = intro->size;
  return blob;
}

char *dt_bridge_encode_params_hex(const char *op, void *fields_jsonobject, char **err)
{
  dt_iop_module_so_t *so = _find_so(op);
  if(!so)
  {
    _seterr(err, "unknown module operation '%s'", op ? op : "(null)");
    return NULL;
  }
  if(!so->have_introspection || !so->get_introspection || !so->get_p || !so->get_f)
  {
    _seterr(err, "module '%s' has no introspection", op);
    return NULL;
  }

  dt_iop_module_t *module = calloc(1, sizeof(*module));
  if(dt_iop_load_module(module, so, NULL))
  {
    _seterr(err, "could not initialize module '%s'", op);
    return NULL;
  }
  size_t size = 0;
  uint8_t *blob = _seed_and_apply(so, module->default_params,
                                 (JsonObject *)fields_jsonobject, &size, err);
  dt_iop_cleanup_module(module);
  free(module);
  if(!blob) return NULL;
  char *hex = _bytes_to_hex(blob, size);
  g_free(blob);
  return hex;
}

// ---------------------------------------------------------------------------
// import + render
// ---------------------------------------------------------------------------

// the image cache hands back a blank entry for any positive id, so only the
// database can say whether an image is real
static gboolean _image_exists(dt_imgid_t imgid)
{
  sqlite3 *db = dt_database_get(darktable.db);
  sqlite3_stmt *st = NULL;
  gboolean found = FALSE;
  if(db && sqlite3_prepare_v2(db, "SELECT 1 FROM main.images WHERE id = ?1",
                              -1, &st, NULL) == SQLITE_OK)
  {
    sqlite3_bind_int(st, 1, imgid);
    found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
  }
  return found;
}

// every tool naming a single image goes through this: dt_is_valid_imgid() only
// rejects the sentinel, and the cache leaks a read lock on an unknown id
static gboolean _require_image(int imgid, const char *tool, char **err)
{
  if(dt_is_valid_imgid((dt_imgid_t)imgid) && _image_exists((dt_imgid_t)imgid))
    return TRUE;
  _seterr(err, "%s: no image with imgid %d", tool, imgid);
  return FALSE;
}

// is this film roll free of images?
static gboolean _film_is_empty(dt_filmid_t filmid)
{
  sqlite3 *db = dt_database_get(darktable.db);
  sqlite3_stmt *st = NULL;
  gboolean empty = FALSE;
  if(db && sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM main.images WHERE film_id = ?1", -1, &st, NULL)
     == SQLITE_OK)
  {
    sqlite3_bind_int(st, 1, filmid);
    if(sqlite3_step(st) == SQLITE_ROW) empty = sqlite3_column_int(st, 0) == 0;
    sqlite3_finalize(st);
  }
  return empty;
}

// the catalog matches on the literal folder string (image.c:2138) and
// dt_util_normalize_path() only resolves relative paths (utility.c:784), so
// "/dir/./a.raw" files a second roll and duplicates the image.
// not g_canonicalize_filename(): it is glib 2.58 and the tree caps
// GLIB_VERSION_MAX_ALLOWED at 2.56 (CMakeLists.txt:314). not grealpath.h's
// g_realpath(): it exit()s on failure, which a long-lived server cannot use
static gchar *_canonical_path(const char *path)
{
  gchar *norm = dt_util_normalize_path(path);   // handles file:// and relative
  if(!norm) return NULL;

  const char *rest = g_path_skip_root(norm);
  if(!rest) return norm;
  gchar *root = g_strndup(norm, rest - norm);

  gchar **parts = g_strsplit_set(rest, "/" G_DIR_SEPARATOR_S, -1);
  GPtrArray *keep = g_ptr_array_new();          // borrows the parts, frees none
  for(gchar **p = parts; *p; p++)
  {
    if(!**p || !g_strcmp0(*p, ".")) continue;
    if(!g_strcmp0(*p, ".."))
    {
      if(keep->len) g_ptr_array_remove_index(keep, keep->len - 1);
      continue;
    }
    g_ptr_array_add(keep, *p);
  }
  g_ptr_array_add(keep, NULL);
  gchar *tail = g_strjoinv(G_DIR_SEPARATOR_S, (gchar **)keep->pdata);
  gchar *out = g_strconcat(root, tail, NULL);
  g_free(tail);
  g_ptr_array_free(keep, TRUE);
  g_strfreev(parts);
  g_free(root);
  g_free(norm);

#ifndef _WIN32
  char buf[PATH_MAX] = { 0 };
  if(realpath(out, buf))                        // NULL when the file is absent
  {
    g_free(out);
    out = g_strdup(buf);
  }
#endif
  return out;
}

// the one place a file becomes an image row. import_images keeps the result;
// a render given input.path throws it away again (see _resolve_input)
static dt_imgid_t _import_file(const char *path, dt_filmid_t *created_film,
                               gboolean *created_image, GList **created_ids,
                               char **err)
{
  if(created_film) *created_film = NO_FILMID;
  if(created_image) *created_image = FALSE;
  if(created_ids) *created_ids = NULL;
  if(!path) { _seterr(err, "no input path or imgid provided"); return NO_IMGID; }

  // dt_image_import() normalizes but does not canonicalize, so the film roll
  // and the image row must both be keyed off the canonical string
  gchar *norm = _canonical_path(path);
  if(!norm)
  {
    _seterr(err, "could not resolve '%s'", path);
    return NO_IMGID;
  }
  path = norm;

  if(!g_file_test(path, G_FILE_TEST_IS_REGULAR))
  {
    _seterr(err, "'%s' is not a readable file", path);
    g_free(norm);
    return NO_IMGID;
  }

  gchar *dir = g_path_get_dirname(path);
  // whether the roll already existed decides if a scratch import may remove it
  const gboolean film_existed = dt_is_valid_filmid(dt_film_get_id(dir));
  dt_film_t film;
  dt_film_init(&film);  // holds a mutex; dt_film_new() does not set it up
  const dt_filmid_t filmid = dt_film_new(&film, dir);
  dt_film_cleanup(&film);
  g_free(dir);
  if(created_film && !film_existed) *created_film = filmid;
  if(!dt_is_valid_filmid(filmid))
  {
    _seterr(err, "could not create a film roll for '%s'", path);
    g_free(norm);
    return NO_IMGID;
  }

  const gboolean had_image = dt_is_valid_imgid(dt_image_get_id_full_path(path));
  const dt_imgid_t id = dt_image_import_no_sidecars(filmid, path);
  if(created_image) *created_image = dt_is_valid_imgid(id) && !had_image;
  if(created_ids && dt_is_valid_imgid(id) && !had_image)
    *created_ids = g_list_prepend(NULL, GINT_TO_POINTER(id));
  if(!dt_is_valid_imgid(id))
  {
    _seterr(err, "could not import '%s'", path);
    // the roll was created for this file alone, so a failed import would
    // otherwise leave an empty one behind
    if(!film_existed && _film_is_empty(filmid)) dt_film_remove(filmid);
    if(created_film) *created_film = NO_FILMID;
  }
  g_free(norm);
  return id;
}

// importing syncs a sidecar (image.c:_image_import_internal), so without muting
// this a scratch import leaves an .xmp beside the user's raw
typedef struct dt_mcp_xmp_mute_t
{
  gchar *saved;
  gboolean active;
} dt_mcp_xmp_mute_t;

static void _xmp_mute(dt_mcp_xmp_mute_t *m)
{
  const char *cur = dt_conf_get_string_const("write_sidecar_files");
  m->saved = g_strdup(cur ? cur : "on import");
  m->active = TRUE;
  dt_conf_set_string("write_sidecar_files", "never");
}

static void _xmp_unmute(dt_mcp_xmp_mute_t *m)
{
  if(!m->active) return;
  dt_conf_set_string("write_sidecar_files", m->saved);
  g_free(m->saved);
  m->saved = NULL;
  m->active = FALSE;
}

// what one request's throwaway import added to the catalog, so _drop_scratch()
// can take back exactly that and nothing else
typedef struct dt_mcp_scratch_t
{
  gboolean active;          // this request imported, so it owes a cleanup
  dt_filmid_t film;         // only when the request created the roll itself
  GList *imgids;            // every row the import filed, duplicates included
  dt_mcp_xmp_mute_t mute;
} dt_mcp_scratch_t;

// resolve a request's input to an imgid. a path already in the catalog is
// refused: reusing the row would make a stack's persistence invisible to the
// caller, and removing it afterwards would delete their image
static dt_imgid_t _resolve_input(const char *path, int imgid_in,
                                 dt_mcp_scratch_t *sc, char **err)
{
  sc->active = FALSE;
  sc->film = NO_FILMID;
  sc->imgids = NULL;
  if(imgid_in > 0)
  {
    // the image cache allocates a blank entry for any positive id and leaks its
    // read lock doing so, wedging the next writer
    if(!_image_exists((dt_imgid_t)imgid_in))
    {
      _seterr(err, "no image with imgid %d", imgid_in);
      return NO_IMGID;
    }
    return (dt_imgid_t)imgid_in;
  }
  if(!path) { _seterr(err, "no input path or imgid provided"); return NO_IMGID; }

  // probe with the string _import_file() files the image under: a relative
  // path, a file:// URI or a "/./" would miss this and the import would then
  // hand back a second row for the caller's own image
  gchar *norm = _canonical_path(path);
  const dt_imgid_t existing = dt_image_get_id_full_path(norm ? norm : path);
  g_free(norm);
  if(dt_is_valid_imgid(existing))
  {
    _seterr(err, "'%s' is already in the catalog as imgid %d: use input.imgid %d"
            " instead, where successful edits persist; add"
            " history_end 0 to that call to ignore its existing edits",
            path, existing, existing);
    return NO_IMGID;
  }

  _xmp_mute(&sc->mute);
  gboolean created = FALSE;
  const dt_imgid_t id = _import_file(path, &sc->film, &created, &sc->imgids, err);
  if(!dt_is_valid_imgid(id)) { _xmp_unmute(&sc->mute); return NO_IMGID; }

  // only a row this request created may be removed again
  if(!created)
  {
    // a roll made for this request has no image of its own to hold it
    if(dt_is_valid_filmid(sc->film) && _film_is_empty(sc->film))
      dt_film_remove(sc->film);
    sc->film = NO_FILMID;
    g_list_free(sc->imgids);
    sc->imgids = NULL;
    _xmp_unmute(&sc->mute);
    _seterr(err, "'%s' is already in the catalog as imgid %d: use input.imgid %d"
            " instead", path, id, id);
    return NO_IMGID;
  }
  sc->active = TRUE;
  return id;
}



// undo a scratch import: the rows go, and their history, tags and labels go
// with them through the foreign keys. the file on disk is never touched
static void _drop_scratch(dt_mcp_scratch_t *sc)
{
  sqlite3 *db = dt_database_get(darktable.db);
  for(GList *i = sc->imgids; i; i = g_list_next(i))
  {
    const dt_imgid_t imgid = GPOINTER_TO_INT(i->data);
    // main.module_order carries no foreign key (database.c:3535), so unlike
    // history and tags it does not go with the image
    sqlite3_stmt *st = NULL;
    if(db && sqlite3_prepare_v2(db, "DELETE FROM main.module_order WHERE imgid = ?1",
                                -1, &st, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int(st, 1, imgid);
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
    dt_image_remove(imgid);
  }
  g_list_free(sc->imgids);
  sc->imgids = NULL;

  // only the roll this request created, and only while empty:
  // dt_film_remove_empty() sweeps every empty roll and rmdirs each folder
  if(dt_is_valid_filmid(sc->film) && _film_is_empty(sc->film))
    dt_film_remove(sc->film);
  sc->film = NO_FILMID;

  sc->active = FALSE;
  _xmp_unmute(&sc->mute);
}

typedef struct dt_mcp_stage_t
{
  dt_imgid_t source;
  dt_imgid_t imgid;
  gboolean commit;
  dt_mcp_xmp_mute_t mute;
} dt_mcp_stage_t;

static gboolean _stage_sql(const char *sql, dt_imgid_t dest, dt_imgid_t source, char **err)
{
  sqlite3 *db = dt_database_get(darktable.db);
  sqlite3_stmt *stmt = NULL;
  gboolean ok = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK;
  if(ok)
  {
    sqlite3_bind_int(stmt, 1, dest);
    if(sqlite3_bind_parameter_count(stmt) > 1) sqlite3_bind_int(stmt, 2, source);
    ok = sqlite3_step(stmt) == SQLITE_DONE;
  }
  sqlite3_finalize(stmt);
  if(!ok && err && !*err) _seterr(err, "request catalog operation failed: %s", sqlite3_errmsg(db));
  return ok;
}

static gboolean _stage_copy_history(dt_imgid_t dest, dt_imgid_t source, char **err)
{
  const char *queries[] = {
    "DELETE FROM main.history WHERE imgid=?1",
    "INSERT INTO main.history (imgid,num,module,operation,op_params,enabled,blendop_params,"
      "blendop_version,multi_priority,multi_name,multi_name_hand_edited)"
      " SELECT ?1,num,module,operation,op_params,enabled,blendop_params,blendop_version,"
      "multi_priority,multi_name,multi_name_hand_edited FROM main.history WHERE imgid=?2",
    "DELETE FROM main.masks_history WHERE imgid=?1",
    "INSERT INTO main.masks_history (imgid,num,formid,form,name,version,points,points_count,source)"
      " SELECT ?1,num,formid,form,name,version,points,points_count,source"
      " FROM main.masks_history WHERE imgid=?2",
    "DELETE FROM main.module_order WHERE imgid=?1",
    "INSERT INTO main.module_order (imgid,version,iop_list)"
      " SELECT ?1,version,iop_list FROM main.module_order WHERE imgid=?2",
    "DELETE FROM main.history_hash WHERE imgid=?1",
    "INSERT INTO main.history_hash (imgid,current_hash)"
      " SELECT ?1,current_hash FROM main.history_hash WHERE imgid=?2"
  };
  for(size_t i = 0; i < G_N_ELEMENTS(queries); i++)
    if(!_stage_sql(queries[i], dest, source, err)) return FALSE;
  return TRUE;
}

static void _stage_discard(dt_mcp_stage_t *stage)
{
  if(dt_is_valid_imgid(stage->imgid))
  {
    // A native duplicate adjusts sibling versions and grouping. This row owns neither.
    dt_image_cache_remove(stage->imgid);
    dt_mipmap_cache_remove(stage->imgid);
    _stage_sql("DELETE FROM main.module_order WHERE imgid=?1", stage->imgid, 0, NULL);
    _stage_sql("DELETE FROM main.images WHERE id=?1", stage->imgid, 0, NULL);
    stage->imgid = NO_IMGID;
  }
  _xmp_unmute(&stage->mute);
}

static gboolean _stage_begin(dt_imgid_t source, gboolean scratch, const char *baseline,
                              gboolean edits, dt_mcp_stage_t *stage, char **err)
{
  *stage = (dt_mcp_stage_t){ .source = source, .imgid = NO_IMGID,
                            .commit = edits && !scratch && !_read_only };
  if(baseline && strcmp(baseline, "raw-development"))
  {
    _seterr(err, "baseline must be 'raw-development'");
    return FALSE;
  }
  if(_read_only && edits && !scratch)
  {
    _seterr(err, "server is --read-only: applying intent would modify image %d", source);
    return FALSE;
  }
  _xmp_mute(&stage->mute);
  sqlite3 *db = dt_database_get(darktable.db);
  if(sqlite3_exec(db, "SAVEPOINT mcp_stage", NULL, NULL, NULL) != SQLITE_OK)
  {
    _seterr(err, "could not start request catalog operation");
    _stage_discard(stage);
    return FALSE;
  }
  const char *clone =
    "INSERT INTO main.images (group_id,film_id,width,height,filename,"
      "maker_id,model_id,lens_id,camera_id,exposure,aperture,iso,focal_length,focus_distance,"
      "datetime_taken,flags,output_width,output_height,crop,raw_parameters,raw_black,raw_maximum,"
      "orientation,longitude,latitude,altitude,color_matrix,colorspace,version,max_version,"
      "history_end,position,aspect_ratio,exposure_bias,whitebalance_id,flash_id,"
      "exposure_program_id,metering_mode_id)"
    " SELECT NULL,film_id,width,height,filename,maker_id,model_id,lens_id,camera_id,"
      "exposure,aperture,iso,focal_length,focus_distance,datetime_taken,flags,0,0,crop,"
      "raw_parameters,raw_black,raw_maximum,orientation,longitude,latitude,altitude,color_matrix,"
      "colorspace,version,0,history_end,position,aspect_ratio,exposure_bias,whitebalance_id,"
      "flash_id,exposure_program_id,metering_mode_id FROM main.images WHERE id=?1";
  gboolean ok = _stage_sql(clone, source, 0, err);
  if(ok)
  {
    const sqlite3_int64 id = sqlite3_last_insert_rowid(db);
    ok = sqlite3_changes(db) == 1 && id > 0 && id <= INT_MAX;
    if(ok) stage->imgid = (dt_imgid_t)id;
    else _seterr(err, "could not create isolated request image");
  }
  if(ok)
  {
    char *flags = g_strdup_printf("UPDATE main.images SET group_id=id,flags=(flags|%d)&~%d,"
      "aspect_ratio=0,thumb_timestamp=-1,thumb_maxmip=0 WHERE id=?1",
      DT_IMAGE_AUTO_PRESETS_APPLIED | DT_IMAGE_NO_LEGACY_PRESETS,
      DT_IMAGE_LOCAL_COPY | DT_IMAGE_REMOVE);
    ok = _stage_sql(flags, stage->imgid, 0, err);
    g_free(flags);
  }
  if(ok && !baseline) ok = _stage_copy_history(stage->imgid, source, err);
  if(ok && baseline)
    ok = _stage_sql("UPDATE main.images SET history_end=0,version=0 WHERE id=?1",
                    stage->imgid, 0, err)
      && _stage_sql("INSERT INTO main.module_order (imgid,version,iop_list) VALUES (?1,?2,NULL)",
                     stage->imgid, DT_DEFAULT_IOP_ORDER_RAW, err);
  if(!ok) sqlite3_exec(db, "ROLLBACK TO mcp_stage", NULL, NULL, NULL);
  if(sqlite3_exec(db, "RELEASE mcp_stage", NULL, NULL, NULL) != SQLITE_OK) ok = FALSE;
  if(!ok)
  {
    stage->imgid = NO_IMGID;
    _stage_discard(stage);
  }
  return ok;
}

static gboolean _stage_commit(dt_mcp_stage_t *stage, const char *temporary,
                               const char *target, char **err)
{
  if(!stage->commit)
  {
    if(!temporary || !g_rename(temporary, target)) return TRUE;
    _seterr(err, "could not publish export output '%s'", target);
    return FALSE;
  }
  sqlite3 *db = dt_database_get(darktable.db);
  if(sqlite3_exec(db, "SAVEPOINT mcp_commit", NULL, NULL, NULL) != SQLITE_OK)
  {
    _seterr(err, "could not start history commit");
    return FALSE;
  }
  gboolean ok = _stage_copy_history(stage->source, stage->imgid, err);
  if(ok)
  {
    char *update = g_strdup_printf("UPDATE main.images SET history_end="
      "(SELECT history_end FROM main.images WHERE id=?2),flags=flags|%d,"
      "output_width=0,output_height=0,aspect_ratio=0,thumb_timestamp=-1,thumb_maxmip=0 WHERE id=?1",
      DT_IMAGE_AUTO_PRESETS_APPLIED | DT_IMAGE_NO_LEGACY_PRESETS);
    ok = _stage_sql(update, stage->source, stage->imgid, err);
    g_free(update);
  }
  if(ok && temporary && g_rename(temporary, target))
  {
    _seterr(err, "could not publish export output '%s'", target);
    ok = FALSE;
  }
  if(!ok) sqlite3_exec(db, "ROLLBACK TO mcp_commit", NULL, NULL, NULL);
  if(sqlite3_exec(db, "RELEASE mcp_commit", NULL, NULL, NULL) != SQLITE_OK) ok = FALSE;
  if(ok)
  {
    dt_image_cache_remove(stage->source);
    dt_mipmap_cache_remove(stage->source);
  }
  return ok;
}
char *dt_bridge_image_parameters_json(const char *path, int imgid_in, char **err)
{
  dt_mcp_scratch_t sc = { FALSE, NO_FILMID, NULL, { NULL, FALSE } };
  const dt_imgid_t source = _resolve_input(path, imgid_in, &sc, err);
  if(!dt_is_valid_imgid(source)) return NULL;
  dt_mcp_stage_t stage;
  if(!_stage_begin(source, sc.active, NULL, FALSE, &stage, err))
  {
    if(sc.active) _drop_scratch(&sc);
    return NULL;
  }
  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, stage.imgid);
  dt_dev_pop_history_items_ext(&dev, dev.history_end);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "imgid");
  json_builder_add_int_value(b, source);
  json_builder_set_member_name(b, "modules");
  json_builder_begin_array(b);
  for(GList *it = dev.iop; it; it = g_list_next(it))
  {
    dt_iop_module_t *mod = it->data;
    if(!mod || !mod->so || !mod->so->get_introspection
       || !mod->so->have_introspection) continue;
    dt_introspection_t *intro = mod->so->get_introspection();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "operation");
    json_builder_add_string_value(b, mod->so->op);
    json_builder_set_member_name(b, "params_version");
    json_builder_add_int_value(b, intro->params_version);
    json_builder_set_member_name(b, "multi_priority");
    json_builder_add_int_value(b, mod->multi_priority);
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, mod->enabled);
    json_builder_set_member_name(b, "iop_order");
    json_builder_add_int_value(b, mod->iop_order);
    if(*mod->multi_name)
    {
      json_builder_set_member_name(b, "multi_name");
      json_builder_add_string_value(b, mod->multi_name);
    }
    json_builder_set_member_name(b, "values");
    dt_mcp_params_values(b, intro->field, mod->params);
    json_builder_set_member_name(b, "defaults");
    dt_mcp_params_values(b, intro->field, mod->default_params);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  json_builder_end_object(b);
  dt_dev_cleanup(&dev);
  _stage_discard(&stage);
  if(sc.active) _drop_scratch(&sc);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

static gboolean _validate_stack(JsonArray *stack, char **err)
{
  for(guint i = 0; stack && i < json_array_get_length(stack); i++)
  {
    JsonNode *node = json_array_get_element(stack, i);
    if(!JSON_NODE_HOLDS_OBJECT(node))
    {
      _seterr(err, "stack[%u] must be an object", i);
      return FALSE;
    }
    JsonObject *entry = json_node_get_object(node);
    const char *known[] = { "operation", "params", "blob_hex", "params_version",
                            "multi_priority", "enabled", "before", "after" };
    GList *members = json_object_get_members(entry);
    for(GList *it = members; it; it = it->next)
    {
      gboolean found = FALSE;
      for(size_t j = 0; j < G_N_ELEMENTS(known); j++)
        if(!strcmp(it->data, known[j])) { found = TRUE; break; }
      if(!found)
      {
        _seterr(err, "stack[%u] unknown field '%s'", i, (const char *)it->data);
        g_list_free(members);
        return FALSE;
      }
    }
    g_list_free(members);
    JsonNode *op = json_object_get_member(entry, "operation");
    if(!op || !JSON_NODE_HOLDS_STRING(op) || !*json_node_get_string(op))
    {
      _seterr(err, "stack[%u] requires a nonempty operation string", i);
      return FALSE;
    }
    const char *strings[] = { "blob_hex", "before", "after" };
    for(size_t j = 0; j < G_N_ELEMENTS(strings); j++)
      if(json_object_has_member(entry, strings[j])
         && !JSON_NODE_HOLDS_STRING(json_object_get_member(entry, strings[j])))
      {
        _seterr(err, "stack[%u].%s must be a string", i, strings[j]);
        return FALSE;
      }
    const char *integers[] = { "params_version", "multi_priority" };
    for(size_t j = 0; j < G_N_ELEMENTS(integers); j++)
      if(json_object_has_member(entry, integers[j]))
      {
        JsonNode *value = json_object_get_member(entry, integers[j]);
        if(!JSON_NODE_HOLDS_INT(value) || json_node_get_int(value) < 0
           || json_node_get_int(value) > INT_MAX)
        {
          _seterr(err, "stack[%u].%s must be a nonnegative native integer", i, integers[j]);
          return FALSE;
        }
      }
    if(json_object_has_member(entry, "enabled")
       && !JSON_NODE_HOLDS_BOOLEAN(json_object_get_member(entry, "enabled")))
    {
      _seterr(err, "stack[%u].enabled must be boolean", i);
      return FALSE;
    }
    if(json_object_has_member(entry, "params")
       && !JSON_NODE_HOLDS_OBJECT(json_object_get_member(entry, "params")))
    {
      _seterr(err, "stack[%u].params must be an object", i);
      return FALSE;
    }
    if((json_object_has_member(entry, "params") && json_object_has_member(entry, "blob_hex"))
       || (json_object_has_member(entry, "before") && json_object_has_member(entry, "after"))
       || (json_object_has_member(entry, "blob_hex")
           && !json_object_has_member(entry, "params_version")))
    {
      _seterr(err, "stack[%u] has conflicting fields or an unversioned blob", i);
      return FALSE;
    }
  }
  return TRUE;
}

// apply one stack entry to a module instance in dev and snapshot it to history
static gboolean _apply_entry(dt_develop_t *dev, JsonObject *entry, char **err)
{
  JsonNode *op_node = json_object_get_member(entry, "operation");
  if(!op_node || !JSON_NODE_HOLDS_VALUE(op_node)
     || json_node_get_value_type(op_node) != G_TYPE_STRING)
  {
    _seterr(err, "stack entry requires string 'operation'");
    return FALSE;
  }
  const char *op = json_node_get_string(op_node);
  const gboolean has_params = json_object_has_member(entry, "params");
  const gboolean has_blob = json_object_has_member(entry, "blob_hex");
  if(has_params && has_blob)
  {
    _seterr(err, "stack entry '%s' cannot contain both 'params' and 'blob_hex'", op);
    return FALSE;
  }
  if(has_params && !JSON_NODE_HOLDS_OBJECT(json_object_get_member(entry, "params")))
  {
    _seterr(err, "stack entry '%s' params must be an object", op);
    return FALSE;
  }
  if(has_blob)
  {
    JsonNode *blob_node = json_object_get_member(entry, "blob_hex");
    if(!JSON_NODE_HOLDS_VALUE(blob_node)
       || json_node_get_value_type(blob_node) != G_TYPE_STRING)
    {
      _seterr(err, "stack entry '%s' blob_hex must be a string", op);
      return FALSE;
    }
  }
  if(json_object_has_member(entry, "multi_priority")
     && !JSON_NODE_HOLDS_INT(json_object_get_member(entry, "multi_priority")))
  {
    _seterr(err, "stack entry '%s' multi_priority must be an integer", op);
    return FALSE;
  }
  const int multi_priority = json_object_has_member(entry, "multi_priority")
                     ? (int)json_object_get_int_member(entry, "multi_priority") : 0;
  const gboolean enabled = json_object_has_member(entry, "enabled")
                     ? json_object_get_boolean_member(entry, "enabled") : TRUE;

  dt_iop_module_t *mod = dt_iop_get_module_by_op_priority(dev->iop, op, multi_priority);
  if(!mod && multi_priority > 0)
  {
    dt_iop_module_t *base = dt_iop_get_module_by_op_priority(dev->iop, op, -1);
    if(base && !(base->flags() & IOP_FLAGS_ONE_INSTANCE))
    {
      mod = calloc(1, sizeof(*mod));
      if(dt_iop_load_module(mod, base->so, dev)) mod = NULL;
      else
      {
        mod->instance = base->instance;
        dt_iop_update_multi_priority(mod, multi_priority);
        dt_ioppr_insert_module_instance(dev, mod);
        dev->iop = g_list_append(dev->iop, mod);
        dt_ioppr_resync_modules_order(dev);
        // reload_defaults can depend on whether this is the first ordered instance
        dt_iop_reload_defaults(mod);
      }
    }
  }
  if(!mod)
  {
    _seterr(err, "module '%s' instance %d is unavailable or cannot be created",
            op, multi_priority);
    return FALSE;
  }
  if(json_object_has_member(entry, "params_version")
     && (!mod->so->get_introspection || json_object_get_int_member(entry, "params_version")
         != mod->so->get_introspection()->params_version))
  {
    _seterr(err, "module '%s' parameter version is not current", op);
    return FALSE;
  }

  // relative placement: a raw iop_order is an opaque number a caller has no
  // way to choose sensibly
  if(json_object_has_member(entry, "before") && json_object_has_member(entry, "after"))
  {
    _seterr(err, "'%s': give 'before' or 'after', not both", op);
    return FALSE;
  }

  for(int pass = 0; pass < 2; pass++)
  {
    const char *key = pass ? "after" : "before";
    if(!json_object_has_member(entry, key)) continue;

    const char *target_op = json_object_get_string_member(entry, key);
    if(!target_op)
    {
      _seterr(err, "'%s' must name a module", key);
      return FALSE;
    }
    dt_iop_module_t *target =
      dt_iop_get_module_by_op_priority(dev->iop, target_op, -1);
    if(!target)
    {
      _seterr(err, "cannot place '%s' %s '%s': no such module in the pipe",
              op, key, target_op);
      return FALSE;
    }

    // imageop.c:1025 gates the same moves: without this, fences and iop_order
    // rules are bypassed and the illegal order is written as a custom one
    const gboolean allowed = pass
      ? dt_ioppr_check_can_move_after_iop(dev->iop, mod, target)
      : dt_ioppr_check_can_move_before_iop(dev->iop, mod, target);
    if(!allowed)
    {
      _seterr(err, "pipeline rules forbid placing '%s' %s '%s'",
              op, key, target_op);
      return FALSE;
    }

    const gboolean moved = pass
      ? dt_ioppr_move_iop_after(dev, mod, target)
      : dt_ioppr_move_iop_before(dev, mod, target);
    if(!moved)
    {
      _seterr(err, "pipeline refuses to place '%s' %s '%s'", op, key, target_op);
      return FALSE;
    }
  }

  size_t size = 0;
  uint8_t *blob = NULL;
  if(json_object_has_member(entry, "blob_hex"))
  {
    dt_introspection_t *intro = mod->so->get_introspection
      ? mod->so->get_introspection() : NULL;
    const int version = json_object_has_member(entry, "params_version")
      ? (int)json_object_get_int_member(entry, "params_version") : 0;
    if(!intro || version != intro->params_version)
    {
      _seterr(err, "module '%s' requires params_version %d for blob_hex, got %d",
              op, intro ? intro->params_version : -1, version);
      return FALSE;
    }
    blob = _hex_to_bytes(json_object_get_string_member(entry, "blob_hex"), &size);
    if(!blob) { _seterr(err, "invalid blob_hex for '%s'", op); return FALSE; }
  }
  else
  {
    JsonObject *fields = NULL;
    if(json_object_has_member(entry, "params"))
    {
      JsonNode *fn = json_object_get_member(entry, "params");
      if(JSON_NODE_HOLDS_OBJECT(fn)) fields = json_node_get_object(fn);
    }
    if(!mod->so->have_introspection)
    {
      _seterr(err, "module '%s' has no introspection for 'params'", op);
      return FALSE;
    }
    // the live block, so setting one field layers onto the image's current
    // values instead of resetting the rest to defaults
    const void *defaults = (mod->params_size == (int32_t)mod->so->get_introspection()->size)
      ? mod->params : NULL;
    blob = _seed_and_apply(mod->so, defaults, fields, &size, err);
    if(!blob) return FALSE;
  }

  if(size != (size_t)mod->params_size)
  {
    _seterr(err, "blob size %zu != module '%s' params size %d",
            size, op, mod->params_size);
    g_free(blob);
    return FALSE;
  }
  memcpy(mod->params, blob, size);
  g_free(blob);
  mod->enabled = enabled;
  dt_dev_add_history_item_ext(dev, mod, enabled, TRUE);
  return TRUE;
}

// in-memory 8-bit RGB export "format" module; we render through this instead of
// dt_imageio_preview() because that helper builds its surface via a GUI-only
// cairo wrapper (dereferences darktable.gui->ppd) and crashes headless
typedef struct
{
  dt_imageio_module_data_t head;
  int bpp;
  uint8_t *buf;
  uint32_t width, height;
} _mcp_fmt_t;

static int _mcp_write(dt_imageio_module_data_t *data, const char *filename,
                      const void *in,
                      const dt_colorspaces_color_profile_type_t over_type,
                      const char *over_filename, void *exif, const int exif_len,
                      const dt_imgid_t imgid, const int num, const int total,
                      dt_dev_pixelpipe_t *pipe, const gboolean export_masks)
{
  _mcp_fmt_t *d = (_mcp_fmt_t *)data;
  if(!in) return 1;

  // the export writes the real size into data before calling us; sizing this
  // from the requested box breaks once a dimension is left unconstrained
  const size_t bytes = sizeof(uint32_t) * (size_t)data->width * data->height;
  g_free(d->buf);
  d->buf = g_try_malloc(bytes);
  if(!d->buf) return 1;

  memcpy(d->buf, in, bytes);
  d->width = data->width;
  d->height = data->height;
  return 0;
}
static int _mcp_bpp(dt_imageio_module_data_t *d)
{
  return 8;
}
static int _mcp_levels(dt_imageio_module_data_t *d)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}
static const char *_mcp_mime(dt_imageio_module_data_t *d)
{
  return "memory";
}
static void _free_cb(void *p) { g_free(p); }

// render the committed history of `imgid` to a plain cairo RGB24 surface that
// owns its pixel buffer (freed when the surface is destroyed)
static cairo_surface_t *_render_to_surface(dt_imgid_t imgid, int w, int h,
                                           int history_end, char **err)
{
  // max_width/max_height is a bounding box and 0 means "no limit", so
  // defaulting an unset dimension would cap the one actually asked for
  if(w < 0) w = 0;
  if(h < 0) h = 0;
  if(w == 0 && h == 0)
  {
    w = 1024;
    h = 1024;
  }

  dt_imageio_module_format_t fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.mime = _mcp_mime;
  fmt.levels = _mcp_levels;
  fmt.bpp = _mcp_bpp;
  fmt.write_image = _mcp_write;

  _mcp_fmt_t dat;
  memset(&dat, 0, sizeof(dat));
  dat.head.max_width = w;
  dat.head.max_height = h;
  dat.head.width = w;
  dat.head.height = h;
  dat.head.style_append = TRUE;
  dat.bpp = 8;
  dat.buf = NULL; // allocated by _mcp_write once the real size is known

  // NB: dt_imageio_export_with_flags returns FALSE on success
  const gboolean failed = dt_imageio_export_with_flags(
      imgid, "memory", &fmt, (dt_imageio_module_data_t *)&dat,
      TRUE, TRUE, FALSE /*hq*/, TRUE /*upscale*/, FALSE /*is_scaling*/, 1.0,
      FALSE /*thumbnail*/, NULL /*filter*/, FALSE /*copy_meta*/, FALSE /*export_masks*/,
      DT_COLORSPACE_DISPLAY, NULL, DT_INTENT_LAST, NULL, NULL,
      1, 1, NULL /*metadata*/, history_end);

  if(failed || dat.width == 0 || dat.height == 0)
  {
    g_free(dat.buf);
    _seterr(err, "render failed (invalid image or pipeline)");
    return NULL;
  }

  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, dat.width);
  cairo_surface_t *surf =
    cairo_image_surface_create_for_data(dat.buf, CAIRO_FORMAT_RGB24, dat.width,
                                        dat.height, stride);
  if(cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(surf);
    g_free(dat.buf);
    _seterr(err, "cairo surface creation failed");
    return NULL;
  }
  static const cairo_user_data_key_t key;
  cairo_surface_set_user_data(surf, &key, dat.buf, _free_cb);
  return surf;
}

// Prepare only the isolated request row. Publication decides whether it commits.
static gboolean _prepare_stage(dt_mcp_stage_t *stage, const char *baseline,
                               JsonArray *stack, gboolean disable_tone_mappers,
                               int *history_end, char **err)
{
  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, stage->imgid);
  if(baseline && !dt_image_is_rawprepare_supported(&dev.image_storage))
  {
    _seterr(err, "raw-development requires a supported RAW input");
    dt_dev_cleanup(&dev);
    return FALSE;
  }
  dt_dev_pop_history_items_ext(&dev, *history_end == -1 ? dev.history_end
                                : MIN(*history_end, (int)g_list_length(dev.history)));
  if(baseline)
  {
    const char *allowed[] = { "rawprepare", "temperature", "highlights", "demosaic",
                              "flip", "exposure", "colorin", "colorout", "gamma" };
    for(GList *it = dev.iop; it; it = it->next)
    {
      dt_iop_module_t *module = it->data;
      gboolean keep = FALSE;
      for(size_t j = 0; j < G_N_ELEMENTS(allowed); j++)
        if(dt_iop_module_is(module, allowed[j])) { keep = TRUE; break; }
      if(!keep && module->enabled)
      {
        module->enabled = FALSE;
        dt_dev_add_history_item_ext(&dev, module, FALSE, TRUE);
      }
    }
  }
  if(disable_tone_mappers)
  {
    const char *names[] = { "sigmoid", "filmicrgb", "agx", "spektrafilm", "basecurve" };
    for(size_t j = 0; j < G_N_ELEMENTS(names); j++)
    {
      dt_iop_module_t *module = dt_iop_get_module_by_op_priority(dev.iop, names[j], 0);
      if(module && module->enabled)
      {
        module->enabled = FALSE;
        dt_dev_add_history_item_ext(&dev, module, FALSE, TRUE);
      }
    }
  }
  for(guint i = 0; stack && i < json_array_get_length(stack); i++)
    if(!_apply_entry(&dev, json_array_get_object_element(stack, i), err))
    {
      dt_dev_cleanup(&dev);
      return FALSE;
    }
  dt_dev_write_history_ext(&dev, stage->imgid);
  dt_dev_cleanup(&dev);
  *history_end = -1;
  return TRUE;
}

// import/resolve, develop, render, and undo a scratch import afterwards
static cairo_surface_t *_render_surface(const char *path, int imgid_in, int width,
                                        int height, const char *baseline, JsonArray *stack,
                                        gboolean disable_tone_mappers, int history_end,
                                        dt_mcp_stage_t *stage, dt_mcp_scratch_t *scratch,
                                        char **err)
{
  if(history_end < -1 || !_validate_stack(stack, err))
  {
    if(history_end < -1) _seterr(err, "history_end must be -1 (all) or >= 0");
    return NULL;
  }
  const dt_imgid_t source = _resolve_input(path, imgid_in, scratch, err);
  if(!dt_is_valid_imgid(source)) return NULL;
  const gboolean edits = baseline || disable_tone_mappers
                          || (stack && json_array_get_length(stack));
  cairo_surface_t *surface = NULL;
  if(_stage_begin(source, scratch->active, baseline, edits, stage, err)
     && _prepare_stage(stage, baseline, stack, disable_tone_mappers, &history_end, err))
    surface = _render_to_surface(stage->imgid, width, height, history_end, err);
  if(!surface)
  {
    _stage_discard(stage);
    if(scratch->active) _drop_scratch(scratch);
  }
  return surface;
}

static cairo_status_t _png_writer(void *closure, const unsigned char *data,
                                  unsigned int length)
{
  g_byte_array_append((GByteArray *)closure, data, length);
  return CAIRO_STATUS_SUCCESS;
}

gboolean dt_bridge_render_png(const char *path, int imgid_in, int width, int height,
                              const char *baseline, void *stack_jsonarray,
                              gboolean disable_tone_mappers, int history_end,
                              uint8_t **png_out, size_t *png_len, char **err)
{
  dt_mcp_stage_t stage;
  dt_mcp_scratch_t scratch = { FALSE, NO_FILMID, NULL, { NULL, FALSE } };
  cairo_surface_t *surf = _render_surface(path, imgid_in, width, height, baseline,
                                          (JsonArray *)stack_jsonarray,
                                          disable_tone_mappers, history_end,
                                          &stage, &scratch, err);
  gboolean ok = FALSE;
  if(surf)
  {
    GByteArray *buf = g_byte_array_new();
    if(cairo_surface_write_to_png_stream(surf, _png_writer, buf) == CAIRO_STATUS_SUCCESS)
    {
      if(_stage_commit(&stage, NULL, NULL, err))
      {
        *png_len = buf->len;
        *png_out = g_byte_array_free(buf, FALSE);
        ok = TRUE;
      }
      else g_byte_array_free(buf, TRUE);
    }
    else { g_byte_array_free(buf, TRUE); _seterr(err, "PNG encoding failed"); }
    cairo_surface_destroy(surf);
    _stage_discard(&stage);
    if(scratch.active) _drop_scratch(&scratch);
  }
  return ok;
}

char *dt_bridge_image_stats_json(const char *path, int imgid_in, int width, int height,
                                 const char *baseline, void *stack_jsonarray,
                                 gboolean disable_tone_mappers, int history_end, char **err)
{
  // stats only need a small render, but substituting the default per dimension
  // would cap whichever one the caller did ask for
  const gboolean unsized = width <= 0 && height <= 0;
  dt_mcp_stage_t stage;
  dt_mcp_scratch_t scratch = { FALSE, NO_FILMID, NULL, { NULL, FALSE } };
  cairo_surface_t *surf = _render_surface(path, imgid_in,
                                          unsized ? 512 : width,
                                          unsized ? 512 : height, baseline,
                                          (JsonArray *)stack_jsonarray,
                                          disable_tone_mappers, history_end,
                                          &stage, &scratch, err);
  if(!surf) return NULL;

  cairo_surface_flush(surf);
  const int w = cairo_image_surface_get_width(surf);
  const int h = cairo_image_surface_get_height(surf);
  const int stride = cairo_image_surface_get_stride(surf);
  const unsigned char *data = cairo_image_surface_get_data(surf);

  // per-channel 256-bin histograms (surface is 32bpp, native BGRA/RGB24)
  guint64 hist[3][256];
  memset(hist, 0, sizeof(hist));
  guint64 total = 0;
  for(int y = 0; y < h; y++)
  {
    const uint32_t *row = (const uint32_t *)(data + (size_t)y * stride);
    for(int x = 0; x < w; x++)
    {
      const uint32_t px = row[x];
      const int r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
      hist[0][r]++; hist[1][g]++; hist[2][b]++;
      total++;
    }
  }

  static const char *chan[3] = { "r", "g", "b" };
  JsonBuilder *jb = json_builder_new();
  json_builder_begin_object(jb);
  json_builder_set_member_name(jb, "width");
  json_builder_add_int_value(jb, w);
  json_builder_set_member_name(jb, "height");
  json_builder_add_int_value(jb, h);
  json_builder_set_member_name(jb, "channels");
  json_builder_begin_object(jb);
  for(int c = 0; c < 3; c++)
  {
    int minv = 255, maxv = 0;
    guint64 sum = 0;
    int p1 = 0, p50 = 0, p99 = 0;
    const guint64 t1 = total / 100, t50 = total / 2, t99 = (total * 99) / 100;
    guint64 acc = 0;
    gboolean g1 = FALSE, g50 = FALSE, g99 = FALSE;
    for(int v = 0; v < 256; v++)
    {
      const guint64 n = hist[c][v];
      if(n)
      {
        if(v < minv) minv = v;
        if(v > maxv) maxv = v;
        sum += (guint64)v * n;
      }
      acc += n;
      if(!g1 && acc >= t1)   { p1 = v;  g1 = TRUE; }
      if(!g50 && acc >= t50) { p50 = v; g50 = TRUE; }
      if(!g99 && acc >= t99) { p99 = v; g99 = TRUE; }
    }
    json_builder_set_member_name(jb, chan[c]);
    json_builder_begin_object(jb);
    json_builder_set_member_name(jb, "min");
    json_builder_add_int_value(jb, total ? minv : 0);
    json_builder_set_member_name(jb, "max");
    json_builder_add_int_value(jb, maxv);
    json_builder_set_member_name(jb, "mean");
    json_builder_add_double_value(jb, total ? (double)sum / total : 0.0);
    json_builder_set_member_name(jb, "p1");
    json_builder_add_int_value(jb, p1);
    json_builder_set_member_name(jb, "p50");
    json_builder_add_int_value(jb, p50);
    json_builder_set_member_name(jb, "p99");
    json_builder_add_int_value(jb, p99);
    json_builder_set_member_name(jb, "clip_lo");
    json_builder_add_int_value(jb, (gint64)hist[c][0]);
    json_builder_set_member_name(jb, "clip_hi");
    json_builder_add_int_value(jb, (gint64)hist[c][255]);
    json_builder_end_object(jb);
  }
  json_builder_end_object(jb);
  json_builder_end_object(jb);

  char *out = _builder_to_string(jb);
  g_object_unref(jb);
  cairo_surface_destroy(surf);
  if(!_stage_commit(&stage, NULL, NULL, err)) { g_free(out); out = NULL; }
  _stage_discard(&stage);
  if(scratch.active) _drop_scratch(&scratch);
  return out;
}

// ---------------------------------------------------------------------------
// library (catalog) tools
// ---------------------------------------------------------------------------

// a walked folder is filtered by dt_supported_image(), so sidecars are skipped
// quietly; an explicitly named path is always attempted, so its failure reports
static void _collect_folder(const char *folder, gboolean recursive, GPtrArray *out)
{
  GDir *d = g_dir_open(folder, 0, NULL);
  if(!d) return;
  const char *name;
  while((name = g_dir_read_name(d)))
  {
    gchar *full = g_build_filename(folder, name, NULL);
    if(g_file_test(full, G_FILE_TEST_IS_DIR))
    {
      // a symlinked directory can point at an ancestor, and the walk would
      // then never terminate
      if(recursive && !g_file_test(full, G_FILE_TEST_IS_SYMLINK))
        _collect_folder(full, TRUE, out);
      g_free(full);
    }
    else if(dt_supported_image(name))
      g_ptr_array_add(out, full);  // owned by the array
    else
      g_free(full);  // sidecars and everything else darktable cannot read
  }
  g_dir_close(d);
}

char *dt_bridge_import_images_json(void *paths_jsonarray, const char *folder,
                                   gboolean recursive, char **err)
{
  if(_read_only)
  {
    _seterr(err, "server is --read-only: cannot import into the library");
    return NULL;
  }

  JsonArray *paths = (JsonArray *)paths_jsonarray;
  const guint n_paths = paths ? json_array_get_length(paths) : 0;
  if(!n_paths && !(folder && *folder))
  {
    _seterr(err, "import_images: need 'paths' or 'folder'");
    return NULL;
  }

  GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
  for(guint i = 0; i < n_paths; i++)
  {
    const char *p = json_array_get_string_element(paths, i);
    if(p) g_ptr_array_add(files, g_strdup(p));
  }
  if(folder && *folder)
  {
    if(!g_file_test(folder, G_FILE_TEST_IS_DIR))
    {
      _seterr(err, "import_images: '%s' is not a directory", folder);
      g_ptr_array_free(files, TRUE);
      return NULL;
    }
    _collect_folder(folder, recursive, files);
  }

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  int n_new = 0, n_have = 0, n_fail = 0;

  json_builder_set_member_name(b, "images");
  json_builder_begin_array(b);
  for(guint i = 0; i < files->len; i++)
  {
    const char *path = g_ptr_array_index(files, i);
    gchar *pnorm = _canonical_path(path);
    const char *filed = pnorm ? pnorm : path;
    const dt_imgid_t have = dt_image_get_id_full_path(filed);
    dt_imgid_t id = have;
    const char *status = "already";
    char *ierr = NULL;

    if(!dt_is_valid_imgid(have))
    {
      // the import is authoritative: a path that only matches once normalized
      // would otherwise be reported as newly imported
      gboolean created = FALSE;
      id = _import_file(filed, NULL, &created, NULL, &ierr);
      status = !dt_is_valid_imgid(id) ? "failed" : created ? "imported" : "already";
    }

    json_builder_begin_object(b);
    // the canonical string, not the request's: without it a caller cannot tell
    // which catalog entry their path resolved to
    json_builder_set_member_name(b, "path");
    json_builder_add_string_value(b, filed);
    if(g_strcmp0(filed, path))
    {
      json_builder_set_member_name(b, "requested");
      json_builder_add_string_value(b, path);
    }
    json_builder_set_member_name(b, "status");
    json_builder_add_string_value(b, status);
    if(dt_is_valid_imgid(id))
    {
      json_builder_set_member_name(b, "imgid");
      json_builder_add_int_value(b, id);
    }
    else if(ierr)
    {
      json_builder_set_member_name(b, "error");
      json_builder_add_string_value(b, ierr);
    }
    json_builder_end_object(b);

    if(!g_strcmp0(status, "imported")) n_new++;
    else if(!g_strcmp0(status, "already")) n_have++;
    else n_fail++;
    g_free(ierr);
    g_free(pnorm);
  }
  json_builder_end_array(b);

  json_builder_set_member_name(b, "imported");
  json_builder_add_int_value(b, n_new);
  json_builder_set_member_name(b, "already");
  json_builder_add_int_value(b, n_have);
  json_builder_set_member_name(b, "failed");
  json_builder_add_int_value(b, n_fail);
  json_builder_end_object(b);

  g_ptr_array_free(files, TRUE);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

// one row per imported directory: what list_images can filter on
char *dt_bridge_list_film_rolls_json(void)
{
  sqlite3 *db = dt_database_get(darktable.db);
  JsonBuilder *b = json_builder_new();
  json_builder_begin_array(b);

  sqlite3_stmt *st = NULL;
  const char *q =
    "SELECT f.id, f.folder, COUNT(i.id)"
    "  FROM main.film_rolls f LEFT JOIN main.images i ON i.film_id = f.id"
    " GROUP BY f.id, f.folder ORDER BY f.folder";
  if(db && sqlite3_prepare_v2(db, q, -1, &st, NULL) == SQLITE_OK)
  {
    while(sqlite3_step(st) == SQLITE_ROW)
    {
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "film_roll");
      json_builder_add_int_value(b, sqlite3_column_int(st, 0));
      const char *fold = (const char *)sqlite3_column_text(st, 1);
      // the name darktable itself shows: the last show_folder_levels components
      // of the path, not the whole thing
      json_builder_set_member_name(b, "name");
      json_builder_add_string_value(b, fold ? dt_image_film_roll_name(fold) : "");
      json_builder_set_member_name(b, "folder");
      json_builder_add_string_value(b, fold);
      json_builder_set_member_name(b, "images");
      json_builder_add_int_value(b, sqlite3_column_int(st, 2));
      json_builder_end_object(b);
    }
    sqlite3_finalize(st);
  }

  json_builder_end_array(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

// escape the LIKE metacharacters in a caller-supplied substring
static gchar *_sql_like_escape(const char *in)
{
  GString *o = g_string_new(NULL);
  for(const char *p = in; p && *p; p++)
  {
    if(*p == '%' || *p == '_' || *p == '\\') g_string_append_c(o, '\\');
    g_string_append_c(o, *p);
  }
  return g_string_free(o, FALSE);
}

char *dt_bridge_list_images_json(int limit, const char *folder, int rating,
                                 const char *color, gboolean rejected_only,
                                 int film_roll, char **err)
{
  sqlite3 *db = dt_database_get(darktable.db);
  JsonBuilder *b = json_builder_new();
  json_builder_begin_array(b);

  // flags carries the star rating in its low 3 bits and DT_IMAGE_REJECTED (8)
  // separately, so a rejected image still holds whatever stars it had
  GString *q = g_string_new(
    "SELECT i.id, f.folder, i.filename, i.flags & 7, i.flags & 8,"
    "       (SELECT group_concat(c.color) FROM main.color_labels c WHERE c.imgid = i.id)"
    "  FROM main.images i JOIN main.film_rolls f ON i.film_id = f.id"
    " WHERE 1=1");

  if(folder && *folder)     g_string_append(q, " AND f.folder LIKE ?1 ESCAPE '\\'");
  if(film_roll > 0)         g_string_append(q, " AND f.id = ?4");
  if(rating >= 0)           g_string_append(q, " AND (i.flags & 7) >= ?2");
  if(rejected_only)         g_string_append(q, " AND (i.flags & 8) != 0");
  if(color && *color)
    g_string_append(q, " AND EXISTS (SELECT 1 FROM main.color_labels c"
                       "              WHERE c.imgid = i.id AND c.color = ?3)");
  g_string_append(q, " ORDER BY i.id");

  int color_idx = -1;
  for(int i = 0; color && dt_colorlabels_name[i]; i++)
    if(!g_ascii_strcasecmp(color, dt_colorlabels_name[i])) { color_idx = i; break; }
  // an unmatched name would bind -1 and return an empty list, which reads as
  // "no images carry that label" rather than "that is not a label"
  if(color && *color && color_idx < 0)
  {
    _seterr(err, "color must be one of red, yellow, green, blue, purple");
    g_string_free(q, TRUE);
    g_object_unref(b);
    return NULL;
  }

  sqlite3_stmt *st = NULL;
  if(db && sqlite3_prepare_v2(db, q->str, -1, &st, NULL) == SQLITE_OK)
  {
    if(folder && *folder)
    {
      // '_' and '%' would otherwise act as wildcards inside the caller's string
      gchar *esc = _sql_like_escape(folder);
      gchar *like = g_strdup_printf("%%%s%%", esc);
      sqlite3_bind_text(st, 1, like, -1, SQLITE_TRANSIENT);
      g_free(like);
      g_free(esc);
    }
    if(rating >= 0) sqlite3_bind_int(st, 2, rating);
    if(color && *color) sqlite3_bind_int(st, 3, color_idx);
    if(film_roll > 0) sqlite3_bind_int(st, 4, film_roll);

    int n = 0;
    while(sqlite3_step(st) == SQLITE_ROW && (limit <= 0 || n < limit))
    {
      const int id = sqlite3_column_int(st, 0);
      const char *fold = (const char *)sqlite3_column_text(st, 1);
      const char *fn = (const char *)sqlite3_column_text(st, 2);
      const int stars = sqlite3_column_int(st, 3);
      const int rej = sqlite3_column_int(st, 4);
      const char *labels = (const char *)sqlite3_column_text(st, 5);

      gchar *path = g_build_filename(fold ? fold : "", fn ? fn : "", NULL);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "imgid");
      json_builder_add_int_value(b, id);
      json_builder_set_member_name(b, "path");
      json_builder_add_string_value(b, path);
      json_builder_set_member_name(b, "rating");
      json_builder_add_int_value(b, stars);
      if(rej)
      {
        json_builder_set_member_name(b, "rejected");
        json_builder_add_boolean_value(b, TRUE);
      }
      if(labels && *labels)
      {
        json_builder_set_member_name(b, "color_labels");
        json_builder_begin_array(b);
        gchar **parts = g_strsplit(labels, ",", -1);
        for(int i = 0; parts[i]; i++)
        {
          const int c = atoi(parts[i]);
          json_builder_add_string_value(
            b, (c >= 0 && c < DT_COLORLABELS_LAST) ? dt_colorlabels_name[c] : "?");
        }
        g_strfreev(parts);
        json_builder_end_array(b);
      }
      json_builder_end_object(b);
      g_free(path);
      n++;
    }
    sqlite3_finalize(st);
  }
  g_string_free(q, TRUE);

  json_builder_end_array(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

char *dt_bridge_get_history_json(int imgid, char **err)
{
  // an empty array would otherwise read as "this image has no edits"
  if(!_require_image(imgid, "get_history", err)) return NULL;

  sqlite3 *db = dt_database_get(darktable.db);
  sqlite3_stmt *st = NULL;
  const char *q = "SELECT num, operation, op_params, module, enabled, multi_priority, multi_name"
                  " FROM main.history WHERE imgid = ?1 ORDER BY num";
  if(!db || sqlite3_prepare_v2(db, q, -1, &st, NULL) != SQLITE_OK)
  {
    _seterr(err, "could not query history");
    return NULL;
  }
  sqlite3_bind_int(st, 1, imgid);

  // where each module runs; the history's own `num` is the order edits were
  // made, a different thing entirely
  GList *order_list = dt_ioppr_get_iop_order_list((dt_imgid_t)imgid, FALSE);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_array(b);
  while(sqlite3_step(st) == SQLITE_ROW)
  {
    const int num = sqlite3_column_int(st, 0);
    const char *op = (const char *)sqlite3_column_text(st, 1);
    const void *blob = sqlite3_column_blob(st, 2);
    const int blen = sqlite3_column_bytes(st, 2);
    const int version = sqlite3_column_int(st, 3);
    const int enabled = sqlite3_column_int(st, 4);
    const int mp = sqlite3_column_int(st, 5);
    const char *mname = (const char *)sqlite3_column_text(st, 6);

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "num");
    json_builder_add_int_value(b, num);
    json_builder_set_member_name(b, "operation");
    json_builder_add_string_value(b, op ? op : "");
    json_builder_set_member_name(b, "version");
    json_builder_add_int_value(b, version);
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, enabled != 0);
    json_builder_set_member_name(b, "iop_order");
    json_builder_add_int_value(b, dt_ioppr_get_iop_order(order_list, op ? op : "", mp));
    json_builder_set_member_name(b, "multi_priority");
    json_builder_add_int_value(b, mp);
    if(mname && *mname)
    {
      json_builder_set_member_name(b, "multi_name");
      json_builder_add_string_value(b, mname);
    }

    // decode fields when the stored blob matches the module's current layout
    dt_iop_module_so_t *so = op ? dt_iop_get_module_so(op) : NULL;
    if(so && so->have_introspection && so->get_introspection && so->get_p)
    {
      dt_introspection_t *intro = so->get_introspection();
      if(blob && (size_t)blen == intro->size && version == intro->params_version)
      {
        json_builder_set_member_name(b, "fields");
        _write_fields_object(so, blob, b);
      }
    }
    json_builder_end_object(b);
  }
  sqlite3_finalize(st);
  dt_ioppr_iop_order_list_free(order_list);
  json_builder_end_array(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

// ---------------------------------------------------------------------------
// configuration (read only)
// ---------------------------------------------------------------------------

static const char *_confgen_type_name(const dt_confgen_type_t t)
{
  switch(t)
  {
    case DT_INT:    return "int";
    case DT_INT64:  return "int64";
    case DT_FLOAT:  return "float";
    case DT_BOOL:   return "bool";
    case DT_PATH:   return "path";
    case DT_ENUM:   return "enum";
    case DT_STRING:
    default:        return "string";
  }
}

// one entry: the live value plus whatever darktableconfig.xml.in declares about
// it, so a caller can tell a changed setting from a default and see what a key
// is allowed to hold
static void _conf_entry(JsonBuilder *b, const char *key)
{
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "key");
  json_builder_add_string_value(b, key);

  gchar *val = dt_conf_get_string(key);
  json_builder_set_member_name(b, "value");
  json_builder_add_string_value(b, val ? val : "");
  g_free(val);

  if(dt_confgen_exists(key))
  {
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, _confgen_type_name(dt_confgen_type(key)));

    const char *def = dt_confgen_get(key, DT_DEFAULT);
    if(def)
    {
      json_builder_set_member_name(b, "default");
      json_builder_add_string_value(b, def);
    }
    if(dt_confgen_value_exists(key, DT_MIN))
    {
      json_builder_set_member_name(b, "min");
      json_builder_add_string_value(b, dt_confgen_get(key, DT_MIN));
    }
    if(dt_confgen_value_exists(key, DT_MAX))
    {
      json_builder_set_member_name(b, "max");
      json_builder_add_string_value(b, dt_confgen_get(key, DT_MAX));
    }
    if(dt_confgen_type(key) == DT_ENUM)
    {
      // "[a][b][c]" as stored; split the same way the preferences dialog does
      // (gui/preferences.c), so a value containing ']' fails identically there
      const char *vals = dt_confgen_get(key, DT_VALUES);
      json_builder_set_member_name(b, "values");
      json_builder_begin_array(b);
      while(vals && *vals++ == '[' && *vals)
      {
        const char *end = strchr(vals, ']');
        if(!end) break;
        gchar *item = g_strndup(vals, end - vals);
        json_builder_add_string_value(b, item);
        g_free(item);
        vals = end + 1;
      }
      json_builder_end_array(b);
    }
  }
  json_builder_end_object(b);
}

char *dt_bridge_get_metadata_json(int imgid, char **err)
{
  if(!_require_image(imgid, "get_metadata", err)) return NULL;

  const dt_image_t *img = dt_image_cache_get((dt_imgid_t)imgid, 'r');
  if(!img)
  {
    _seterr(err, "get_metadata: no image %d", imgid);
    return NULL;
  }

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "imgid");
  json_builder_add_int_value(b, imgid);
  json_builder_set_member_name(b, "filename");
  json_builder_add_string_value(b, img->filename);

  json_builder_set_member_name(b, "camera");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "maker");
  json_builder_add_string_value(b, img->exif_maker);
  json_builder_set_member_name(b, "model");
  json_builder_add_string_value(b, img->exif_model);
  json_builder_set_member_name(b, "lens");
  json_builder_add_string_value(b, img->exif_lens);
  json_builder_end_object(b);

  json_builder_set_member_name(b, "exif");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "iso");
  json_builder_add_double_value(b, img->exif_iso);
  json_builder_set_member_name(b, "exposure");
  json_builder_add_double_value(b, img->exif_exposure);
  json_builder_set_member_name(b, "aperture");
  json_builder_add_double_value(b, img->exif_aperture);
  json_builder_set_member_name(b, "focal_length");
  json_builder_add_double_value(b, img->exif_focal_length);
  json_builder_set_member_name(b, "exposure_bias");
  json_builder_add_double_value(b, img->exif_exposure_bias);
  json_builder_set_member_name(b, "focus_distance");
  json_builder_add_double_value(b, img->exif_focus_distance);
  if(img->exif_datetime_taken)
  {
    char dt[DT_DATETIME_LENGTH] = { 0 };
    if(dt_datetime_gtimespan_to_exif(dt, sizeof(dt), img->exif_datetime_taken))
    {
      json_builder_set_member_name(b, "datetime_taken");
      json_builder_add_string_value(b, dt);
    }
  }
  if(*img->exif_whitebalance)
  {
    json_builder_set_member_name(b, "white_balance");
    json_builder_add_string_value(b, img->exif_whitebalance);
  }
  if(*img->exif_exposure_program)
  {
    json_builder_set_member_name(b, "exposure_program");
    json_builder_add_string_value(b, img->exif_exposure_program);
  }
  if(*img->exif_metering_mode)
  {
    json_builder_set_member_name(b, "metering_mode");
    json_builder_add_string_value(b, img->exif_metering_mode);
  }
  if(*img->exif_flash)
  {
    json_builder_set_member_name(b, "flash");
    json_builder_add_string_value(b, img->exif_flash);
  }
  json_builder_end_object(b);

  // rawprepare fills these on first load, so they are zero until the image has
  // been through the pipe once. omitted rather than reported as zero, which
  // would read as a real black point of 0 and a sensor with no range at all
  if(img->raw_white_point > 0)
  {
    json_builder_set_member_name(b, "raw");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "black_level");
    json_builder_add_int_value(b, img->raw_black_level);
    json_builder_set_member_name(b, "white_point");
    json_builder_add_int_value(b, img->raw_white_point);
    json_builder_set_member_name(b, "black_level_separate");
    json_builder_begin_array(b);
    for(int i = 0; i < 4; i++)
      json_builder_add_int_value(b, img->raw_black_level_separate[i]);
    json_builder_end_array(b);
    json_builder_end_object(b);
  }

  json_builder_set_member_name(b, "dimensions");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "width");
  json_builder_add_int_value(b, img->width);
  json_builder_set_member_name(b, "height");
  json_builder_add_int_value(b, img->height);
  json_builder_set_member_name(b, "final_width");
  json_builder_add_int_value(b, img->final_width);
  json_builder_set_member_name(b, "final_height");
  json_builder_add_int_value(b, img->final_height);
  json_builder_end_object(b);

  json_builder_end_object(b);
  dt_image_cache_read_release(img);

  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

char *dt_bridge_get_conf_json(const char *key, char **err)
{
  if(!key || !*key) { _seterr(err, "get_conf: need 'key'"); return NULL; }
  if(!dt_conf_key_exists(key) && !dt_confgen_exists(key))
  {
    _seterr(err, "get_conf: no such key '%s'", key);
    return NULL;
  }
  JsonBuilder *b = json_builder_new();
  _conf_entry(b, key);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

char *dt_bridge_list_conf_json(const char *prefix)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_array(b);

  // only the declared keys: darktablerc also holds window geometry and other
  // runtime scribble that tells a caller nothing
  if(darktable.conf && darktable.conf->x_confgen)
  {
    GList *keys = g_hash_table_get_keys(darktable.conf->x_confgen);
    keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
    for(GList *k = keys; k; k = g_list_next(k))
    {
      const char *name = (const char *)k->data;
      if(prefix && *prefix && !g_str_has_prefix(name, prefix)) continue;
      _conf_entry(b, name);
    }
    g_list_free(keys);
  }

  json_builder_end_array(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

char *dt_bridge_list_styles_json(const char *filter, int limit)
{
  // dt_styles_get_list() wraps the filter in '%...%' and matches name or
  // description (styles.c:1239), so this is a substring search, not a prefix
  GList *styles = dt_styles_get_list(filter ? filter : "");
  const guint total = g_list_length(styles);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  // a catalog of several hundred styles runs to tens of kilobytes, so say how
  // many matched: a truncated array is otherwise indistinguishable from a
  // complete one
  json_builder_set_member_name(b, "total");
  json_builder_add_int_value(b, total);
  json_builder_set_member_name(b, "styles");
  json_builder_begin_array(b);
  int n = 0;
  for(GList *s = styles; s; s = g_list_next(s))
  {
    if(limit > 0 && n++ >= limit) break;
    dt_style_t *st = (dt_style_t *)s->data;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, st->name ? st->name : "");
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, st->description ? st->description : "");
    json_builder_end_object(b);
  }
  g_list_free_full(styles, dt_style_free);
  json_builder_end_array(b);
  json_builder_end_object(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

// ---------------------------------------------------------------------------
// selection: rating and color labels
// ---------------------------------------------------------------------------

// JSON array of image ids -> GList, rejecting anything the library does not hold
static GList *_imgids_from_json(JsonArray *arr, char **err)
{
  if(!arr || json_array_get_length(arr) == 0)
  {
    _seterr(err, "need a non-empty 'imgids' array");
    return NULL;
  }
  GList *l = NULL;
  const guint n = json_array_get_length(arr);
  for(guint i = 0; i < n; i++)
  {
    const dt_imgid_t id = (dt_imgid_t)json_array_get_int_element(arr, i);
    // must exist, not merely look plausible: the image cache hands back a blank
    // entry for any positive id and leaks its read lock doing so
    if(!dt_is_valid_imgid(id) || !_image_exists(id))
    {
      _seterr(err, "imgids[%u]: no image with imgid %d", i, id);
      g_list_free(l);
      return NULL;
    }
    l = g_list_prepend(l, GINT_TO_POINTER(id));
  }
  return g_list_reverse(l);
}

gboolean dt_bridge_set_rating(void *imgids_jsonarray, int rating,
                              gboolean reject, char **err)
{
  if(_read_only)
  {
    _seterr(err, "server is --read-only: cannot set a rating");
    return FALSE;
  }
  if(!reject && (rating < 0 || rating > 5))
  {
    _seterr(err, "rating must be 0-5; pass reject:true to reject");
    return FALSE;
  }

  GList *l = _imgids_from_json((JsonArray *)imgids_jsonarray, err);
  if(!l) return FALSE;

  // reject is a flag beside the stars, not a rating value: DT_VIEW_REJECT is
  // what _ratings_apply_to_image() tests for, and it leaves the star count alone
  const int target = reject ? DT_VIEW_REJECT : rating;

  // ratings.c:_ratings_apply() toggles reject and one star back off when every
  // image already holds them, a keyboard affordance no API caller asked for.
  // dropping those images makes the call idempotent and stops the test passing
  GList *todo = NULL;
  for(GList *i = l; i; i = g_list_next(i))
    if(dt_ratings_get(GPOINTER_TO_INT(i->data)) != target)
      todo = g_list_prepend(todo, i->data);
  g_list_free(l);

  if(todo)
  {
    dt_ratings_apply_on_list(todo, target, FALSE);
    for(GList *i = todo; i; i = g_list_next(i))
      dt_image_write_sidecar_file(GPOINTER_TO_INT(i->data));
    g_list_free(todo);
  }
  return TRUE;
}

gboolean dt_bridge_set_color_label(void *imgids_jsonarray, const char *color,
                                   gboolean toggle, char **err)
{
  if(_read_only)
  {
    _seterr(err, "server is --read-only: cannot set a color label");
    return FALSE;
  }

  int idx = -1;
  for(int i = 0; color && dt_colorlabels_name[i]; i++)
    if(!g_ascii_strcasecmp(color, dt_colorlabels_name[i])) { idx = i; break; }
  if(idx < 0)
  {
    _seterr(err, "color must be one of red, yellow, green, blue, purple");
    return FALSE;
  }

  GList *l = _imgids_from_json((JsonArray *)imgids_jsonarray, err);
  if(!l) return FALSE;

  // the two take the color differently: toggle wants the index and shifts it
  // itself, set wants a ready-made bitmask
  if(toggle)
    dt_colorlabels_toggle_label_on_list(l, idx, FALSE);
  else
    dt_colorlabels_set_labels(l, 1 << idx, FALSE, FALSE);

  for(GList *i = l; i; i = g_list_next(i))
    dt_image_write_sidecar_file(GPOINTER_TO_INT(i->data));
  g_list_free(l);
  return TRUE;
}

gboolean dt_bridge_reset_history(int imgid, char **err)
{
  if(_read_only)
  {
    _seterr(err, "server is --read-only: cannot reset history");
    return FALSE;
  }

  if(!_require_image(imgid, "reset_history", err)) return FALSE;

  // an empty stack takes the no-edits path and renders unchanged, so
  // clearing needs its own verb
  dt_history_delete_on_image((dt_imgid_t)imgid);
  dt_image_write_sidecar_file((dt_imgid_t)imgid);
  return TRUE;
}

gboolean dt_bridge_apply_style(const char *name, int imgid, gboolean overwrite,
                               void *imgids_jsonarray, char **err)
{
  if(_read_only)
  {
    _seterr(err, "server is --read-only: cannot apply a style");
    return FALSE;
  }
  if(!name)
  {
    _seterr(err, "apply_style: need 'name'");
    return FALSE;
  }
  // _styles_apply_to_image_ext() just returns for an unknown name, so without
  // this the call reports success having changed nothing
  if(!dt_styles_exists(name))
  {
    _seterr(err, "apply_style: no style named '%s'", name);
    return FALSE;
  }

  // one imgid or a list; dt_styles_apply_to_image is single-image either way,
  // so the loop lives here rather than costing the caller a round trip each
  GList *l = NULL;
  if(imgids_jsonarray && json_array_get_length((JsonArray *)imgids_jsonarray) > 0)
  {
    l = _imgids_from_json((JsonArray *)imgids_jsonarray, err);
    if(!l) return FALSE;
  }
  else if(dt_is_valid_imgid((dt_imgid_t)imgid))
  {
    if(!_require_image(imgid, "apply_style", err)) return FALSE;
    l = g_list_prepend(NULL, GINT_TO_POINTER(imgid));
  }
  else
  {
    _seterr(err, "apply_style: need 'imgid' or a non-empty 'imgids'");
    return FALSE;
  }

  for(GList *i = l; i; i = g_list_next(i))
  {
    const dt_imgid_t id = GPOINTER_TO_INT(i->data);
    dt_styles_apply_to_image(name, FALSE, overwrite, id);
    dt_image_write_sidecar_file(id);
  }
  g_list_free(l);
  return TRUE;
}

gboolean dt_bridge_save_style(const char *name, const char *description,
                              int imgid, char **err)
{
  if(_read_only)
  {
    _seterr(err, "server is --read-only: cannot save a style");
    return FALSE;
  }

  if(!name)
  {
    _seterr(err, "save_style: need 'name'");
    return FALSE;
  }
  if(!_require_image(imgid, "save_style", err)) return FALSE;
  if(!dt_styles_create_from_image(name, description ? description : "",
                                  (dt_imgid_t)imgid, NULL, TRUE))
  {
    _seterr(err, "could not create style '%s' (already exists?)", name);
    return FALSE;
  }
  return TRUE;
}

gboolean dt_bridge_import_style(const char *path, char **err)
{
  if(_read_only)
  {
    _seterr(err, "server is --read-only: cannot import a style");
    return FALSE;
  }

  if(!path) { _seterr(err, "import_style: need 'path'"); return FALSE; }

  // dt_styles_import_from_file() is void and logs failures rather than
  // returning them, so without these checks every call answers "ok"
  if(!g_file_test(path, G_FILE_TEST_IS_REGULAR))
  {
    _seterr(err, "import_style: '%s' is not a readable file", path);
    return FALSE;
  }

  GList *before = dt_styles_get_list("");
  const guint n_before = g_list_length(before);
  g_list_free_full(before, dt_style_free);

  dt_styles_import_from_file(path);

  GList *after = dt_styles_get_list("");
  const guint n_after = g_list_length(after);
  g_list_free_full(after, dt_style_free);

  if(n_after == n_before)
  {
    _seterr(err, "import_style: '%s' added no style; malformed, or a style of"
                 " that name already exists", path);
    return FALSE;
  }
  return TRUE;
}

// where darktable itself would put an export when no target is named:
// plugins/imageio/storage/disk/file_directory, defaulting to a
// darktable_exported/ folder beside each source file
static gchar *_default_out_path(dt_imgid_t imgid, int seq, const char *ext)
{
  char full[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, full, sizeof(full), &from_cache);
  if(!*full) return NULL;

  const char *conf =
    dt_conf_get_string_const("plugins/imageio/storage/disk/file_directory");
  gchar *pattern = g_strdup(conf && *conf
                            ? conf
                            : "$(FILE_FOLDER)/darktable_exported/$(FILE_NAME)");
  dt_variables_params_t *vp = NULL;
  dt_variables_params_init(&vp);
  vp->filename = full;
  vp->jobcode = "export";
  vp->imgid = imgid;
  vp->sequence = seq;
  gchar *base = dt_variables_expand_path(vp, pattern, TRUE);
  dt_variables_params_destroy(vp);
  g_free(pattern);

  if(!base) return NULL;
  gchar *out = g_strconcat(base, ".", ext, NULL);
  g_free(base);
  return out;
}

// darktable's configured conflict policy; split out so the name generation
// below stays a pure function the unit tests can drive
static int _conflict_action(void)
{
  return dt_conf_get_int("plugins/imageio/storage/disk/overwrite");
}

// apply plugins/imageio/storage/disk/overwrite, default a free "_01" name
// rather than clobber (disk.c:451). NULL means no name: *skipped separates the
// policy leaving a file alone from every candidate being taken. the policy is
// about files on disk before the export, so a target `claimed` by this same
// call is never overwritten or skipped, or the batch would lose an image
static gchar *_resolve_conflict(const char *file, const int action,
                                GHashTable *claimed, gboolean *skipped)
{
  *skipped = FALSE;
  const gboolean taken = claimed && g_hash_table_contains(claimed, file);
  if(!taken && !g_file_test(file, G_FILE_TEST_EXISTS)) return g_strdup(file);

  if(!taken)
  {
    if(action == 1 || action == 2) return g_strdup(file);   // overwrite
    if(action == 3) { *skipped = TRUE; return NULL; }       // skip
  }

  // split on the basename: a directory containing a dot would otherwise send
  // the retry into a different, newly created folder
  gchar *bn = g_path_get_basename(file);
  gchar *bdot = g_strrstr(bn, ".");
  const size_t stem_len = bdot ? (size_t)(strlen(file) - strlen(bdot)) : strlen(file);
  gchar *stem = g_strndup(file, stem_len);
  gchar *ext = g_strdup(bdot ? bdot + 1 : "");
  g_free(bn);
  gchar *out = NULL;
  gboolean free_name = FALSE;
  for(int seq = 1; seq < 10000; seq++)
  {
    g_free(out);
    out = *ext ? g_strdup_printf("%s_%.2d.%s", stem, seq, ext)
               : g_strdup_printf("%s_%.2d", stem, seq);
    if(!g_file_test(out, G_FILE_TEST_EXISTS)
       && !(claimed && g_hash_table_contains(claimed, out)))
    {
      free_name = TRUE;
      break;
    }
  }
  g_free(stem);
  g_free(ext);

  // every candidate was taken: returning the last one would overwrite it
  if(!free_name) { g_free(out); return NULL; }
  return out;
}

// the single decision about where one exported image goes:
//   out_path  the caller named the file, used exactly as given
//   out_dir   <out_dir>/<source basename>.<ext>
//   neither   darktable's configured pattern
// the conflict policy applies only to a name we chose, never the caller's.
// *skipped is the path it left alone, NULL otherwise (caller frees)
static gchar *_export_target(dt_imgid_t imgid, int seq, const char *out_path,
                             const char *out_dir, const char *ext,
                             GHashTable *claimed, gchar **skipped, char **err)
{
  *skipped = NULL;
  if(out_path && *out_path) return g_strdup(out_path);

  gchar *candidate = NULL;
  if(out_dir && *out_dir)
  {
    char full[PATH_MAX] = { 0 };
    gboolean from_cache = FALSE;
    dt_image_full_path(imgid, full, sizeof(full), &from_cache);
    gchar *base = g_path_get_basename(full);
    gchar *dot = g_strrstr(base, ".");
    if(dot) *dot = '\0';
    gchar *name = g_strconcat(base, ".", ext, NULL);
    candidate = g_build_filename(out_dir, name, NULL);
    g_free(name);
    g_free(base);
  }
  else
    candidate = _default_out_path(imgid, seq, ext);

  if(!candidate)
  {
    _seterr(err, "export_images: could not work out a target for image %d", imgid);
    return NULL;
  }
  gboolean skip = FALSE;
  gchar *final = _resolve_conflict(candidate, _conflict_action(), claimed, &skip);
  if(skip)
    *skipped = g_strdup(candidate);
  else if(!final)
    _seterr(err, "export_images: no free filename for '%s'", candidate);
  g_free(candidate);
  return final;
}


// output must never replace the source or an external metadata file
static gboolean _export_target_safe(dt_imgid_t imgid, const char *target, char **err)
{
  char source[PATH_MAX] = { 0 };
  dt_image_full_path(imgid, source, sizeof(source), NULL);
  gchar *input = _canonical_path(source);
  gchar *output = _canonical_path(target);
  const char *extension = output ? strrchr(output, '.') : NULL;
  GStatBuf in_stat, out_stat;
  const gboolean aliases = g_stat(source, &in_stat) == 0 && g_stat(target, &out_stat) == 0
    && in_stat.st_dev == out_stat.st_dev && in_stat.st_ino == out_stat.st_ino;
  const gboolean safe = input && output && strcmp(input, output) && !aliases
    && !dt_is_valid_imgid(dt_image_get_id_full_path(output))
    && !(extension && !g_ascii_strcasecmp(extension, ".xmp"));
  g_free(input);
  g_free(output);
  if(!safe) _seterr(err, "export_images: output must not replace an original or XMP sidecar");
  return safe;
}

// create the directory an export is about to be written into
static gboolean _ensure_parent_dir(const char *file, char **err)
{
  gchar *dir = g_path_get_dirname(file);
  const gboolean ok = (g_mkdir_with_parents(dir, 0755) == 0);
  if(!ok) _seterr(err, "export_images: could not create directory '%s'", dir);
  g_free(dir);
  return ok;
}

// write one developed image through a real format module, so every format
// darktable can write is available rather than cairo's PNG alone
static gboolean _write_export(dt_imgid_t imgid, const char *filename,
                              dt_imageio_module_format_t *fmt, int quality,
                              int width, int height, int history_end,
                              gboolean upscale, gboolean high_quality,
                              int requested_bpp, const char *icc_file,
                              gboolean scene_linear, char **err)
{
  // format settings live in the module's own conf keys; quality is the one a
  // caller reasonably varies per request, so set it around get_params
  gchar *qkey = NULL;
  int qsaved = 0;
  const gboolean set_q = quality > 0;
  if(set_q)
  {
    qkey = g_strdup_printf("plugins/imageio/format/%s/quality", fmt->plugin_name);
    if(dt_conf_key_exists(qkey))
    {
      qsaved = dt_conf_get_int(qkey);
      dt_conf_set_int(qkey, quality);
    }
    else
    {
      g_free(qkey);
      qkey = NULL;
    }
  }
  int tiff_bpp_saved = 0;
  gboolean set_tiff_bpp = !g_strcmp0(fmt->plugin_name, "tiff")
                          && (scene_linear || requested_bpp > 0)
                          && dt_conf_key_exists("plugins/imageio/format/tiff/bpp");
  if(set_tiff_bpp)
  {
    tiff_bpp_saved = dt_conf_get_int("plugins/imageio/format/tiff/bpp");
    dt_conf_set_int("plugins/imageio/format/tiff/bpp",
                    scene_linear ? 32 : requested_bpp);
  }
  dt_imageio_module_data_t *fdata = fmt->get_params(fmt);
  if(set_tiff_bpp) dt_conf_set_int("plugins/imageio/format/tiff/bpp", tiff_bpp_saved);
  if(qkey) { dt_conf_set_int(qkey, qsaved); g_free(qkey); }
  if(!fdata)
  {
    _seterr(err, "export_images: '%s' returned no format parameters",
            fmt->plugin_name);
    return FALSE;
  }

  fdata->max_width = width;
  fdata->max_height = height;
  fdata->style[0] = '\0';
  fdata->style_append = TRUE;

  if(!_ensure_parent_dir(filename, err))
  {
    fmt->free_params(fmt, fdata);
    return FALSE;
  }

  // the same output profile settings darktable's own export reads
  // (libs/export.c:417); -1 on either means the image's own choice stands
  const dt_colorspaces_color_profile_type_t icc_type = icc_file
    ? DT_COLORSPACE_FILE
    : (scene_linear ? DT_COLORSPACE_LIN_REC2020
                    : dt_conf_get_int("plugins/lighttable/export/icctype"));
  gchar *icc_filename = icc_file ? g_strdup(icc_file)
                                 : dt_conf_get_string("plugins/lighttable/export/iccprofile");
  const dt_iop_color_intent_t icc_intent =
    dt_conf_get_int("plugins/lighttable/export/iccintent");

  // NB: returns FALSE on success
  const gboolean failed = dt_imageio_export_with_flags(
      imgid, filename, fmt, fdata,
      FALSE /*ignore_exif*/, FALSE /*display_byteorder*/, high_quality, upscale,
      FALSE /*is_scaling*/, 1.0, FALSE /*thumbnail*/, NULL /*filter*/,
      TRUE /*copy_metadata*/, FALSE /*export_masks*/,
      icc_type, icc_filename, icc_intent, NULL, NULL,
      1, 1, NULL /*metadata*/, history_end);

  g_free(icc_filename);
  fmt->free_params(fmt, fdata);
  if(failed) _seterr(err, "export_images: could not write '%s'", filename);
  return !failed;
}

// pick the format module: an explicit name, else the extension the caller put
// on out_path, else darktable's configured export format
static dt_imageio_module_format_t *_pick_format(const char *format_name,
                                                const char *out_path, char **err)
{
  gchar *guess = NULL;
  const char *want = format_name;
  if(!want && out_path)
  {
    const char *dot = strrchr(out_path, '.');
    if(dot && dot[1]) want = guess = g_ascii_strdown(dot + 1, -1);
  }
  if(!want)
  {
    const char *conf = dt_conf_get_string_const("plugins/lighttable/export/format_name");
    want = (conf && *conf) ? conf : "jpeg";
  }

  // scene-linear-tiff is a bridge contract, not a format plugin name
  if(!g_strcmp0(want, "scene-linear-tiff")) want = "tiff";
  if(!g_strcmp0(want, "jpg")) want = "jpeg";
  if(!g_strcmp0(want, "tif")) want = "tiff";
  dt_imageio_module_format_t *fmt = dt_imageio_get_format_by_name(want);
  if(!fmt) _seterr(err, "export_images: unknown format '%s'", want);
  g_free(guess);
  return fmt;
}

gboolean dt_bridge_export_images(const char *in_path, int imgid_in,
                                 int width, int height,
                                 int history_end, const char *out_path,
                                 void *imgids_jsonarray, const char *out_dir,
                                 const char *format_name, int quality,
                                 int bpp, const char *icc_file,
                                 const char *baseline, void *stack_jsonarray,
                                 gboolean upscale, gboolean high_quality,
                                 void *written_paths, void *skipped_paths,
                                 char **err)
{
  GPtrArray *written = (GPtrArray *)written_paths;
  GPtrArray *skipped_out = (GPtrArray *)skipped_paths;
  JsonArray *ids = (JsonArray *)imgids_jsonarray;
  const gboolean batch = ids && json_array_get_length(ids) > 0;
  JsonArray *stack = (JsonArray *)stack_jsonarray;
  if(!_validate_stack(stack, err)) return FALSE;

  if(history_end < -1)
  {
    _seterr(err, "history_end must be -1 (all) or >= 0");
    return FALSE;
  }
  if(out_path && out_dir)
  {
    _seterr(err, "export_images: give 'out_path' or 'out_dir', not both");
    return FALSE;
  }
  // out_path names one file, so it cannot describe a batch
  if(batch && out_path)
  {
    _seterr(err, "export_images: 'out_path' names a single file; use 'out_dir'"
                 " with 'imgids'");
    return FALSE;
  }
  if(!batch && !in_path && imgid_in <= 0)
  {
    _seterr(err, "export_images: need input.path/imgid, or 'imgids'");
    return FALSE;
  }
  if(format_name && !g_strcmp0(format_name, "scene-linear-tiff") && !icc_file)
  {
    _seterr(err, "scene-linear-tiff requires an icc_file");
    return FALSE;
  }
  if(icc_file && !dt_colorspaces_get_profile(DT_COLORSPACE_FILE, icc_file,
                                             DT_PROFILE_DIRECTION_OUT))
  {
    _seterr(err, "export_images: icc_file must name a registered output profile in configdir/color/out");
    return FALSE;
  }

  dt_imageio_module_format_t *fmt = _pick_format(format_name, out_path, err);
  if(!fmt) return FALSE;

  // the extension the module itself appends, so the name matches the bytes
  dt_imageio_module_data_t *probe = fmt->get_params(fmt);
  gchar *ext = g_strdup(probe ? fmt->extension(probe) : "png");
  if(probe) fmt->free_params(fmt, probe);

  // resolve the input up front so a path-addressed image has a real imgid
  // before its target is worked out: it then lands where an imgid-addressed
  // one would, rather than somewhere of its own
  GList *l = NULL;
  dt_mcp_scratch_t sc = { FALSE, NO_FILMID, NULL, { NULL, FALSE } };

  if(batch)
    l = _imgids_from_json(ids, err);
  else
  {
    const dt_imgid_t work = _resolve_input(in_path, imgid_in, &sc, err);
    if(dt_is_valid_imgid(work)) l = g_list_prepend(NULL, GINT_TO_POINTER(work));
  }
  if(!l) { g_free(ext); return FALSE; }

  // the targets handed out so far: _resolve_conflict() can only see the
  // filesystem, and a file is not there yet when the next image of the batch
  // resolves its own name against it
  GHashTable *claimed = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
  gboolean all_ok = TRUE;
  int seq = 1;
  GList *untried = NULL;
  for(GList *i = l; i; i = g_list_next(i), seq++)
  {
    const dt_imgid_t id = GPOINTER_TO_INT(i->data);
    gchar *skipped = NULL;
    gchar *target = _export_target(id, seq, out_path, out_dir, ext, claimed,
                                   &skipped, err);
    // the conflict policy says leave it: report it, or an export that wrote
    // nothing is indistinguishable from one that had nothing to do
    if(skipped)
    {
      if(skipped_out) g_ptr_array_add(skipped_out, skipped);
      else g_free(skipped);
      continue;
    }
    if(!target || !_export_target_safe(id, target, err))
    {
      all_ok = FALSE;
      untried = g_list_next(i);
      g_free(target);
      break;
    }
    g_hash_table_add(claimed, g_strdup(target));

    dt_mcp_stage_t stage;
    int prepared_history_end = history_end;
    const gboolean edits = baseline || (stack && json_array_get_length(stack));
    all_ok = _stage_begin(id, sc.active, baseline, edits, &stage, err);
    gchar *temporary = NULL;
    if(all_ok) all_ok = _prepare_stage(&stage, baseline, stack, FALSE,
                                      &prepared_history_end, err);
    if(all_ok && _ensure_parent_dir(target, err))
    {
      temporary = g_strconcat(target, ".mcp-XXXXXX", NULL);
      const int fd = g_mkstemp(temporary);
      if(fd < 0) { _seterr(err, "could not create private export output"); all_ok = FALSE; }
      else close(fd);
    }
    else all_ok = FALSE;
    if(all_ok) all_ok = _write_export(stage.imgid, temporary, fmt, quality, width, height,
                                      prepared_history_end, upscale, high_quality, bpp,
                                      icc_file, !g_strcmp0(format_name, "scene-linear-tiff"), err);
    if(all_ok) all_ok = _stage_commit(&stage, temporary, target, err);
    if(temporary) { g_unlink(temporary); g_free(temporary); }
    _stage_discard(&stage);
    if(!all_ok) { untried = g_list_next(i); g_free(target); break; }
    if(written) g_ptr_array_add(written, g_strdup(target));
    g_free(target);
  }

  // the batch stops at the first failure, and the caller is told only which
  // files were written; without this the images after it go unmentioned and
  // read as if they had been exported
  if(!all_ok && untried && err && *err)
  {
    GString *m = g_string_new(*err);
    g_string_append(m, "; not attempted:");
    for(GList *j = untried; j; j = g_list_next(j))
      g_string_append_printf(m, " %d", GPOINTER_TO_INT(j->data));
    g_free(*err);
    *err = g_string_free(m, FALSE);
  }

  if(sc.active) _drop_scratch(&sc);
  g_hash_table_destroy(claimed);
  g_list_free(l);
  g_free(ext);
  return all_ok;
}
