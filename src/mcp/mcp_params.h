/*
    This file is part of darktable.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include <json-glib/json-glib.h>
#include <glib.h>

#include "common/introspection.h"


#ifndef JSON_NODE_HOLDS_INT
#define JSON_NODE_HOLDS_INT(n) \
  (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64)
#endif
#ifndef JSON_NODE_HOLDS_DOUBLE
#define JSON_NODE_HOLDS_DOUBLE(n) \
  (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_DOUBLE)
#endif
#ifndef JSON_NODE_HOLDS_STRING
#define JSON_NODE_HOLDS_STRING(n) \
  (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
#endif
#ifndef JSON_NODE_HOLDS_BOOLEAN
#define JSON_NODE_HOLDS_BOOLEAN(n) \
  (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_BOOLEAN)
#endif
/** Append the complete recursive schema represented by field. */
void dt_mcp_params_schema(JsonBuilder *b, dt_introspection_field_t *field);

/** Append the value represented by field at data. */
void dt_mcp_params_values(JsonBuilder *b, dt_introspection_field_t *field,
                          const void *data);

/** Validate and apply value to data. Omitted struct members are preserved. */
gboolean dt_mcp_params_apply(dt_introspection_field_t *field, void *data,
                             JsonNode *value, char **err);
