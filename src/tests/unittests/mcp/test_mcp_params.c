/*
    This file is part of darktable.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <setjmp.h>
#include <stddef.h>
#include <string.h>

#include <cmocka.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include "common/introspection.h"
#include "mcp/mcp_params.h"

typedef struct
{
  int gain;
  gboolean enabled;
  int mode;
  char label[16];
} nested_params_t;

typedef struct
{
  nested_params_t nested;
  int curve[3][2];
} params_blob_t;

static dt_introspection_field_t gain_field = {
  .Int = { .header = { .type = DT_INTROSPECTION_TYPE_INT, .field_name = "gain", .size = sizeof(int), .offset = offsetof(params_blob_t, nested) + offsetof(nested_params_t, gain) },
           .Min = -4, .Max = 4, .Default = 1 }
};
static dt_introspection_field_t enabled_field = {
  .Bool = { .header = { .type = DT_INTROSPECTION_TYPE_BOOL, .field_name = "enabled", .size = sizeof(gboolean), .offset = offsetof(params_blob_t, nested) + offsetof(nested_params_t, enabled) },
            .Default = FALSE }
};
static dt_introspection_type_enum_tuple_t mode_values[] = {
  { "off", 0, "disabled" }, { "on", 2, "enabled" }, { NULL, 0, NULL }
};
static dt_introspection_field_t mode_field = {
  .Enum = { .header = { .type = DT_INTROSPECTION_TYPE_ENUM, .field_name = "mode", .size = sizeof(int), .offset = offsetof(params_blob_t, nested) + offsetof(nested_params_t, mode) },
            .entries = 2, .values = mode_values, .Default = 0 }
};
static dt_introspection_field_t label_element_field = {
  .Char = { .header = { .type = DT_INTROSPECTION_TYPE_CHAR, .field_name = "label_element", .size = sizeof(char), .offset = 0 },
            .Min = 0, .Max = 127, .Default = 0 }
};
static dt_introspection_field_t label_field = {
  .Array = { .header = { .type = DT_INTROSPECTION_TYPE_ARRAY, .field_name = "label", .size = sizeof(((nested_params_t *)0)->label), .offset = offsetof(params_blob_t, nested) + offsetof(nested_params_t, label) },
             .count = 16, .type = DT_INTROSPECTION_TYPE_CHAR, .field = &label_element_field }
};
static dt_introspection_field_t curve_element_field = {
  .Int = { .header = { .type = DT_INTROSPECTION_TYPE_INT, .field_name = "curve_element", .size = sizeof(int), .offset = 0 },
           .Min = -8, .Max = 8, .Default = 0 }
};
static dt_introspection_field_t curve_row_field = {
  .Array = { .header = { .type = DT_INTROSPECTION_TYPE_ARRAY, .field_name = "curve_row", .size = sizeof(int[2]), .offset = offsetof(params_blob_t, curve) },
             .count = 2, .type = DT_INTROSPECTION_TYPE_INT, .field = &curve_element_field }
};
static dt_introspection_field_t curve_field = {
  .Array = { .header = { .type = DT_INTROSPECTION_TYPE_ARRAY, .field_name = "curve", .size = sizeof(((params_blob_t *)0)->curve), .offset = offsetof(params_blob_t, curve) },
             .count = 3, .type = DT_INTROSPECTION_TYPE_ARRAY, .field = &curve_row_field }
};
static dt_introspection_field_t *nested_fields[] = {
  &gain_field, &enabled_field, &mode_field, &label_field, NULL
};
static dt_introspection_field_t nested_field = {
  .Struct = { .header = { .type = DT_INTROSPECTION_TYPE_STRUCT, .field_name = "nested", .size = sizeof(nested_params_t), .offset = offsetof(params_blob_t, nested) },
              .entries = 4, .fields = nested_fields }
};
static dt_introspection_field_t *root_fields[] = { &nested_field, &curve_field, NULL };
static dt_introspection_field_t root_field = {
  .Struct = { .header = { .type = DT_INTROSPECTION_TYPE_STRUCT, .field_name = "params", .size = sizeof(params_blob_t), .offset = 0 },
              .entries = 2, .fields = root_fields }
};

static JsonNode *_parse(const char *text)
{
  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, text, -1, NULL));
  JsonNode *copy = json_node_copy(json_parser_get_root(parser));
  g_object_unref(parser);
  return copy;
}

static JsonNode *_encode_schema(void)
{
  JsonBuilder *builder = json_builder_new();
  dt_mcp_params_schema(builder, &root_field);
  JsonNode *root = json_node_copy(json_builder_get_root(builder));
  g_object_unref(builder);
  return root;
}

static JsonNode *_encode_values(const params_blob_t *blob)
{
  JsonBuilder *builder = json_builder_new();
  dt_mcp_params_values(builder, &root_field, blob);
  JsonNode *root = json_node_copy(json_builder_get_root(builder));
  g_object_unref(builder);
  return root;
}

static void test_recursive_schema_and_values(void **state)
{
  (void)state;
  params_blob_t blob = { .nested = { .gain = -4, .enabled = TRUE, .mode = 2, .label = "caf\xc3\xa9" },
                         .curve = { { -8, 0 }, { 1, 8 }, { 2, -3 } } };
  JsonNode *schema = _encode_schema();
  JsonObject *schema_object = json_node_get_object(schema);
  JsonArray *fields = json_object_get_array_member(schema_object, "fields");
  assert_int_equal(json_array_get_length(fields), 2);
  JsonObject *nested = json_array_get_object_element(fields, 0);
  JsonObject *nested_schema = json_object_get_object_member(nested, "schema");
  JsonObject *curve = json_array_get_object_element(fields, 1);
  JsonObject *curve_schema = json_object_get_object_member(curve, "schema");
  assert_int_equal(json_object_get_int_member(curve_schema, "count"), 3);
  assert_int_equal(json_array_get_length(json_object_get_array_member(curve_schema, "dimensions")), 2);
  assert_string_equal(json_object_get_string_member(json_array_get_object_element(
                        json_object_get_array_member(nested_schema, "fields"), 2), "name"), "mode");

  JsonNode *values = _encode_values(&blob);
  JsonObject *values_object = json_node_get_object(values);
  JsonObject *nested_values = json_object_get_object_member(values_object, "nested");
  assert_int_equal(json_object_get_int_member(nested_values, "gain"), -4);
  assert_true(json_object_get_boolean_member(nested_values, "enabled"));
  assert_string_equal(json_object_get_string_member(nested_values, "mode"), "on");
  assert_string_equal(json_object_get_string_member(nested_values, "label"), "café");
  JsonArray *rows = json_object_get_array_member(values_object, "curve");
  assert_int_equal(json_array_get_int_element(json_array_get_array_element(rows, 0), 0), -8);
  assert_int_equal(json_array_get_int_element(json_array_get_array_element(rows, 2), 1), -3);
  json_node_unref(schema);
  json_node_unref(values);
}

static void test_apply_round_trip_and_omissions(void **state)
{
  (void)state;
  params_blob_t expected = { .nested = { .gain = -4, .enabled = TRUE, .mode = 2, .label = "caf\xc3\xa9" },
                             .curve = { { -8, 0 }, { 1, 8 }, { 2, -3 } } };
  params_blob_t actual = { .nested = { .gain = 1, .enabled = FALSE, .mode = 0, .label = "keep" },
                           .curve = { { 7, 7 }, { 7, 7 }, { 7, 7 } } };
  JsonNode *full = _parse("{\"nested\":{\"gain\":-4,\"enabled\":true,\"mode\":\"on\",\"label\":\"caf\\u00e9\"},\"curve\":[[-8,0],[1,8],[2,-3]]}");
  char *error = NULL;
  assert_true(dt_mcp_params_apply(&root_field, &actual, full, &error));
  assert_null(error);
  assert_memory_equal(&actual, &expected, sizeof(actual));
  json_node_unref(full);

  params_blob_t preserved = actual;
  JsonNode *partial = _parse("{\"nested\":{\"gain\":4}}");
  assert_true(dt_mcp_params_apply(&root_field, &preserved, partial, &error));
  assert_int_equal(preserved.nested.gain, 4);
  assert_true(preserved.nested.enabled);
  assert_int_equal(preserved.nested.mode, 2);
  assert_memory_equal(preserved.nested.label, expected.nested.label, sizeof(expected.nested.label));
  assert_memory_equal(preserved.curve, expected.curve, sizeof(expected.curve));
  json_node_unref(partial);
}

static void _assert_rejected_unchanged(const char *json, const params_blob_t *before)
{
  params_blob_t actual = *before;
  JsonNode *value = _parse(json);
  char *error = NULL;
  assert_false(dt_mcp_params_apply(&root_field, &actual, value, &error));
  assert_non_null(error);
  assert_memory_equal(&actual, before, sizeof(actual));
  g_free(error);
  json_node_unref(value);
}

static void test_apply_strictness_and_atomicity(void **state)
{
  (void)state;
  const params_blob_t before = { .nested = { .gain = 2, .enabled = TRUE, .mode = 2, .label = "original" },
                                 .curve = { { 1, 2 }, { 3, 4 }, { 5, 6 } } };
  _assert_rejected_unchanged("{\"nested\":{\"gain\":1.5}}", &before);
  _assert_rejected_unchanged("{\"nested\":{\"gain\":5}}", &before);
  _assert_rejected_unchanged("{\"nested\":{\"mode\":\"missing\"}}", &before);
  _assert_rejected_unchanged("{\"nested\":{\"enabled\":null}}", &before);
  _assert_rejected_unchanged("{\"nested\":{\"label\":7}}", &before);
  _assert_rejected_unchanged("{\"nested\":{\"label\":\"this string is too long\"}}", &before);
  _assert_rejected_unchanged("{\"nested\":{\"unknown\":1}}", &before);
  _assert_rejected_unchanged("{\"curve\":[[1,2],[3,4]]}", &before);
  _assert_rejected_unchanged("{\"nested\":{\"gain\":-4,\"mode\":\"on\",\"label\":\"ok\"},\"curve\":[[-8,0],[1,8],[2,9]]}", &before);

  params_blob_t boundary = before;
  JsonNode *value = _parse("{\"nested\":{\"gain\":-4},\"curve\":[[-8,-8],[8,8],[-8,8]]}");
  char *error = NULL;
  assert_true(dt_mcp_params_apply(&root_field, &boundary, value, &error));
  assert_null(error);
  assert_int_equal(boundary.nested.gain, -4);
  assert_int_equal(boundary.curve[0][0], -8);
  assert_int_equal(boundary.curve[1][1], 8);
  json_node_unref(value);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_recursive_schema_and_values),
    cmocka_unit_test(test_apply_round_trip_and_omissions),
    cmocka_unit_test(test_apply_strictness_and_atomicity),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
