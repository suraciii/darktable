/*
    This file is part of darktable.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "mcp_params.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static const char *_type_name(dt_introspection_type_t type)
{
  switch(type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT: return "float";
    case DT_INTROSPECTION_TYPE_DOUBLE: return "double";
    case DT_INTROSPECTION_TYPE_CHAR: return "char";
    case DT_INTROSPECTION_TYPE_INT8: return "int8";
    case DT_INTROSPECTION_TYPE_UINT8: return "uint8";
    case DT_INTROSPECTION_TYPE_SHORT: return "short";
    case DT_INTROSPECTION_TYPE_USHORT: return "ushort";
    case DT_INTROSPECTION_TYPE_INT: return "int";
    case DT_INTROSPECTION_TYPE_UINT: return "uint";
    case DT_INTROSPECTION_TYPE_LONG: return "long";
    case DT_INTROSPECTION_TYPE_ULONG: return "ulong";
    case DT_INTROSPECTION_TYPE_BOOL: return "boolean";
    case DT_INTROSPECTION_TYPE_ENUM: return "enum";
    case DT_INTROSPECTION_TYPE_ARRAY: return "array";
    case DT_INTROSPECTION_TYPE_STRUCT: return "object";
    case DT_INTROSPECTION_TYPE_UNION: return "union";
    case DT_INTROSPECTION_TYPE_FLOATCOMPLEX: return "float-complex";
    case DT_INTROSPECTION_TYPE_OPAQUE: return "opaque";
    default: return "unsupported";
  }
}

static void _description(JsonBuilder *b, const char *description)
{
  if(description && *description)
  {
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, description);
  }
}

static gboolean _is_integer(dt_introspection_type_t t)
{
  return t == DT_INTROSPECTION_TYPE_CHAR || t == DT_INTROSPECTION_TYPE_INT8
      || t == DT_INTROSPECTION_TYPE_UINT8 || t == DT_INTROSPECTION_TYPE_SHORT
      || t == DT_INTROSPECTION_TYPE_USHORT || t == DT_INTROSPECTION_TYPE_INT
      || t == DT_INTROSPECTION_TYPE_UINT || t == DT_INTROSPECTION_TYPE_LONG
      || t == DT_INTROSPECTION_TYPE_ULONG;
}

static void _schema(JsonBuilder *b, dt_introspection_field_t *f)
{
  const dt_introspection_type_t t = f->header.type;
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b,
                                t == DT_INTROSPECTION_TYPE_ARRAY
                                && f->Array.type == DT_INTROSPECTION_TYPE_CHAR
                                  ? "string" : _type_name(t));
  _description(b, f->header.description);

  if(t == DT_INTROSPECTION_TYPE_ARRAY && f->Array.field
     && f->Array.type == DT_INTROSPECTION_TYPE_CHAR)
  {
    json_builder_set_member_name(b, "capacity");
    json_builder_add_int_value(b, (gint64)f->Array.count);
    json_builder_set_member_name(b, "encoding");
    json_builder_add_string_value(b, "utf-8");
  }
  else if(t == DT_INTROSPECTION_TYPE_ARRAY)
  {
    json_builder_set_member_name(b, "count");
    json_builder_add_int_value(b, (gint64)f->Array.count);
    json_builder_set_member_name(b, "dimensions");
    json_builder_begin_array(b);
    dt_introspection_field_t *element = f;
    while(element && element->header.type == DT_INTROSPECTION_TYPE_ARRAY)
    {
      json_builder_add_int_value(b, (gint64)element->Array.count);
      element = element->Array.field;
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "element");
    if(f->Array.field) _schema(b, f->Array.field);
    else
    {
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "type");
      json_builder_add_string_value(b, "unsupported");
      json_builder_end_object(b);
    }
  }
  else if(t == DT_INTROSPECTION_TYPE_STRUCT || t == DT_INTROSPECTION_TYPE_UNION)
  {
    json_builder_set_member_name(b, "fields");
    json_builder_begin_array(b);
    dt_introspection_field_t **fields = t == DT_INTROSPECTION_TYPE_STRUCT
      ? f->Struct.fields : f->Union.fields;
    if(fields)
      for(size_t i = 0; fields[i]; i++)
      {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, fields[i]->header.field_name);
        json_builder_set_member_name(b, "schema");
        _schema(b, fields[i]);
        json_builder_end_object(b);
      }
    json_builder_end_array(b);
    if(t == DT_INTROSPECTION_TYPE_UNION)
    {
      json_builder_set_member_name(b, "supported");
      json_builder_add_boolean_value(b, FALSE);
    }
  }
  else if(t == DT_INTROSPECTION_TYPE_ENUM)
  {
    json_builder_set_member_name(b, "default");
    json_builder_add_int_value(b, f->Enum.Default);
    json_builder_set_member_name(b, "symbols");
    json_builder_begin_array(b);
    for(dt_introspection_type_enum_tuple_t *e = f->Enum.values; e && e->name; e++)
    {
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "symbol");
      json_builder_add_string_value(b, e->name);
      json_builder_set_member_name(b, "value");
      json_builder_add_int_value(b, e->value);
      _description(b, e->description);
      json_builder_end_object(b);
    }
    json_builder_end_array(b);
  }
  else if(t == DT_INTROSPECTION_TYPE_FLOAT)
  {
    json_builder_set_member_name(b, "min"); json_builder_add_double_value(b, f->Float.Min);
    json_builder_set_member_name(b, "max"); json_builder_add_double_value(b, f->Float.Max);
    json_builder_set_member_name(b, "default"); json_builder_add_double_value(b, f->Float.Default);
  }
  else if(t == DT_INTROSPECTION_TYPE_DOUBLE)
  {
    json_builder_set_member_name(b, "min"); json_builder_add_double_value(b, f->Double.Min);
    json_builder_set_member_name(b, "max"); json_builder_add_double_value(b, f->Double.Max);
    json_builder_set_member_name(b, "default"); json_builder_add_double_value(b, f->Double.Default);
  }
  else if(_is_integer(t))
  {
    gint64 min = 0, max = 0, def = 0;
    switch(t)
    {
      case DT_INTROSPECTION_TYPE_CHAR: min=f->Char.Min; max=f->Char.Max; def=f->Char.Default; break;
      case DT_INTROSPECTION_TYPE_INT8: min=f->Int8.Min; max=f->Int8.Max; def=f->Int8.Default; break;
      case DT_INTROSPECTION_TYPE_UINT8: min=f->UInt8.Min; max=f->UInt8.Max; def=f->UInt8.Default; break;
      case DT_INTROSPECTION_TYPE_SHORT: min=f->Short.Min; max=f->Short.Max; def=f->Short.Default; break;
      case DT_INTROSPECTION_TYPE_USHORT: min=f->UShort.Min; max=f->UShort.Max; def=f->UShort.Default; break;
      case DT_INTROSPECTION_TYPE_INT: min=f->Int.Min; max=f->Int.Max; def=f->Int.Default; break;
      case DT_INTROSPECTION_TYPE_UINT: min=f->UInt.Min; max=f->UInt.Max; def=f->UInt.Default; break;
      case DT_INTROSPECTION_TYPE_LONG: min=f->Long.Min; max=f->Long.Max; def=f->Long.Default; break;
      case DT_INTROSPECTION_TYPE_ULONG: max=(gint64)f->ULong.Max; def=(gint64)f->ULong.Default; break;
      default: break;
    }
    json_builder_set_member_name(b, "min"); json_builder_add_int_value(b, min);
    json_builder_set_member_name(b, "max"); json_builder_add_int_value(b, max);
    json_builder_set_member_name(b, "default"); json_builder_add_int_value(b, def);
  }
  else if(t == DT_INTROSPECTION_TYPE_BOOL)
  {
    json_builder_set_member_name(b, "default");
    json_builder_add_boolean_value(b, f->Bool.Default != 0);
  }
  else
  {
    json_builder_set_member_name(b, "supported");
    json_builder_add_boolean_value(b, FALSE);
    json_builder_set_member_name(b, "reason");
    json_builder_add_string_value(b, "parameter kind is not supported by MCP");
  }
  json_builder_end_object(b);
}

void dt_mcp_params_schema(JsonBuilder *b, dt_introspection_field_t *field)
{
  if(field) _schema(b, field);
}

static void _values(JsonBuilder *b, dt_introspection_field_t *f, const void *p)
{
  const dt_introspection_type_t t = f->header.type;
  if(t == DT_INTROSPECTION_TYPE_STRUCT || t == DT_INTROSPECTION_TYPE_UNION)
  {
    json_builder_begin_object(b);
    if(t == DT_INTROSPECTION_TYPE_STRUCT && f->Struct.fields)
      for(size_t i=0; f->Struct.fields[i]; i++)
      {
        dt_introspection_field_t *child = NULL;
        void *cp = dt_introspection_get_child(f, (void *)p,
                                              f->Struct.fields[i]->header.field_name, &child);
        if(cp && child)
        {
          json_builder_set_member_name(b, child->header.field_name);
          _values(b, child, cp);
        }
      }
    json_builder_end_object(b);
    return;
  }
  if(t == DT_INTROSPECTION_TYPE_ARRAY)
  {
    if(f->Array.type == DT_INTROSPECTION_TYPE_CHAR)
    {
      const char *nul = memchr(p, '\0', f->Array.count);
      size_t n = nul ? (size_t)(nul - (const char *)p) : f->Array.count;
      char *text = g_strndup((const char *)p, n);
      json_builder_add_string_value(b, text);
      g_free(text);
      return;
    }
    json_builder_begin_array(b);
    for(size_t i=0; i<f->Array.count; i++)
    {
      dt_introspection_field_t *child = NULL;
      void *ep = dt_introspection_access_array(f, (void *)p, i, &child);
      if(ep && child) _values(b, child, ep);
    }
    json_builder_end_array(b);
    return;
  }
  switch(t)
  {
    case DT_INTROSPECTION_TYPE_FLOAT: json_builder_add_double_value(b, *(const float *)p); break;
    case DT_INTROSPECTION_TYPE_DOUBLE: json_builder_add_double_value(b, *(const double *)p); break;
    case DT_INTROSPECTION_TYPE_CHAR: json_builder_add_int_value(b, *(const char *)p); break;
    case DT_INTROSPECTION_TYPE_INT8: json_builder_add_int_value(b, *(const int8_t *)p); break;
    case DT_INTROSPECTION_TYPE_UINT8: json_builder_add_int_value(b, *(const uint8_t *)p); break;
    case DT_INTROSPECTION_TYPE_SHORT: json_builder_add_int_value(b, *(const short *)p); break;
    case DT_INTROSPECTION_TYPE_USHORT: json_builder_add_int_value(b, *(const unsigned short *)p); break;
    case DT_INTROSPECTION_TYPE_INT: json_builder_add_int_value(b, *(const int *)p); break;
    case DT_INTROSPECTION_TYPE_UINT: json_builder_add_int_value(b, *(const unsigned int *)p); break;
    case DT_INTROSPECTION_TYPE_LONG: json_builder_add_int_value(b, *(const long *)p); break;
    case DT_INTROSPECTION_TYPE_ULONG: json_builder_add_int_value(b, *(const unsigned long *)p); break;
    case DT_INTROSPECTION_TYPE_BOOL: json_builder_add_boolean_value(b, *(const gboolean *)p != 0); break;
    case DT_INTROSPECTION_TYPE_ENUM:
    {
      const char *name = dt_introspection_get_enum_name(f, *(const int *)p);
      if(name) json_builder_add_string_value(b, name);
      else json_builder_add_int_value(b, *(const int *)p);
      break;
    }
    default: json_builder_add_null_value(b); break;
  }
}

void dt_mcp_params_values(JsonBuilder *b, dt_introspection_field_t *field, const void *data)
{
  if(field && data) _values(b, field, data);
}

static void _error(char **err, const char *path, const char *code, const char *detail)
{
  if(err && !*err) *err = g_strdup_printf("%s: %s (%s)", path, code, detail);
}

static gboolean _number(JsonNode *v, gboolean integer, double *num, char **err, const char *path)
{
  if(!JSON_NODE_HOLDS_INT(v) && !JSON_NODE_HOLDS_DOUBLE(v))
  { _error(err, path, "invalid_type", "number required"); return FALSE; }
  *num = JSON_NODE_HOLDS_INT(v) ? (double)json_node_get_int(v) : json_node_get_double(v);
  if(!isfinite(*num)) { _error(err, path, "invalid_number", "non-finite value"); return FALSE; }
  if(integer && trunc(*num) != *num) { _error(err, path, "invalid_integer", "fraction is not allowed"); return FALSE; }
  return TRUE;
}

static gboolean _range(dt_introspection_field_t *f, double n)
{
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT: return n >= f->Float.Min && n <= f->Float.Max;
    case DT_INTROSPECTION_TYPE_DOUBLE: return n >= f->Double.Min && n <= f->Double.Max;
    case DT_INTROSPECTION_TYPE_CHAR: return n >= f->Char.Min && n <= f->Char.Max;
    case DT_INTROSPECTION_TYPE_INT8: return n >= f->Int8.Min && n <= f->Int8.Max;
    case DT_INTROSPECTION_TYPE_UINT8: return n >= f->UInt8.Min && n <= f->UInt8.Max;
    case DT_INTROSPECTION_TYPE_SHORT: return n >= f->Short.Min && n <= f->Short.Max;
    case DT_INTROSPECTION_TYPE_USHORT: return n >= f->UShort.Min && n <= f->UShort.Max;
    case DT_INTROSPECTION_TYPE_INT: return n >= f->Int.Min && n <= f->Int.Max;
    case DT_INTROSPECTION_TYPE_UINT: return n >= f->UInt.Min && n <= f->UInt.Max;
    case DT_INTROSPECTION_TYPE_LONG: return n >= f->Long.Min && n <= f->Long.Max;
    case DT_INTROSPECTION_TYPE_ULONG: return n >= 0 && (long double)n <= (long double)f->ULong.Max;
    default: return TRUE;
  }
}

static void _store_number(dt_introspection_field_t *f, void *p, double n)
{
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT: *(float *)p=n; break; case DT_INTROSPECTION_TYPE_DOUBLE: *(double *)p=n; break;
    case DT_INTROSPECTION_TYPE_CHAR: *(char *)p=n; break; case DT_INTROSPECTION_TYPE_INT8: *(int8_t *)p=n; break;
    case DT_INTROSPECTION_TYPE_UINT8: *(uint8_t *)p=n; break; case DT_INTROSPECTION_TYPE_SHORT: *(short *)p=n; break;
    case DT_INTROSPECTION_TYPE_USHORT: *(unsigned short *)p=n; break; case DT_INTROSPECTION_TYPE_INT: *(int *)p=n; break;
    case DT_INTROSPECTION_TYPE_UINT: *(unsigned int *)p=n; break; case DT_INTROSPECTION_TYPE_LONG: *(long *)p=n; break;
    case DT_INTROSPECTION_TYPE_ULONG: *(unsigned long *)p=n; break; default: break;
  }
}
static void _store_integer(dt_introspection_field_t *f, void *p, gint64 n)
{
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_CHAR: *(char *)p=(char)n; break;
    case DT_INTROSPECTION_TYPE_INT8: *(int8_t *)p=(int8_t)n; break;
    case DT_INTROSPECTION_TYPE_UINT8: *(uint8_t *)p=(uint8_t)n; break;
    case DT_INTROSPECTION_TYPE_SHORT: *(short *)p=(short)n; break;
    case DT_INTROSPECTION_TYPE_USHORT: *(unsigned short *)p=(unsigned short)n; break;
    case DT_INTROSPECTION_TYPE_INT: *(int *)p=(int)n; break;
    case DT_INTROSPECTION_TYPE_UINT: *(unsigned int *)p=(unsigned int)n; break;
    case DT_INTROSPECTION_TYPE_LONG: *(long *)p=(long)n; break;
    case DT_INTROSPECTION_TYPE_ULONG: *(unsigned long *)p=(unsigned long)n; break;
    default: break;
  }
}
static gboolean _integer_in_range(dt_introspection_field_t *f, gint64 n)
{
  const long double v = n;
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_CHAR: return v >= f->Char.Min && v <= f->Char.Max;
    case DT_INTROSPECTION_TYPE_INT8: return v >= f->Int8.Min && v <= f->Int8.Max;
    case DT_INTROSPECTION_TYPE_UINT8: return v >= f->UInt8.Min && v <= f->UInt8.Max;
    case DT_INTROSPECTION_TYPE_SHORT: return v >= f->Short.Min && v <= f->Short.Max;
    case DT_INTROSPECTION_TYPE_USHORT: return v >= f->UShort.Min && v <= f->UShort.Max;
    case DT_INTROSPECTION_TYPE_INT: return v >= f->Int.Min && v <= f->Int.Max;
    case DT_INTROSPECTION_TYPE_UINT: return v >= f->UInt.Min && v <= f->UInt.Max;
    case DT_INTROSPECTION_TYPE_LONG: return v >= f->Long.Min && v <= f->Long.Max;
    case DT_INTROSPECTION_TYPE_ULONG: return v >= 0 && v <= f->ULong.Max;
    default: return FALSE;
  }
}

static gboolean _apply(dt_introspection_field_t *f, void *p, JsonNode *v, char **err, const char *path)
{
  const dt_introspection_type_t t = f->header.type;
  if(t == DT_INTROSPECTION_TYPE_STRUCT)
  {
    if(!JSON_NODE_HOLDS_OBJECT(v)) { _error(err,path,"invalid_type","object required"); return FALSE; }
    JsonObject *o = json_node_get_object(v);
    GList *members = json_object_get_members(o);
    for(GList *it=members; it; it=it->next)
    {
      const char *name=it->data; dt_introspection_field_t *child=NULL;
      void *cp=dt_introspection_get_child(f,p,name,&child);
      if(!cp || !child) { _error(err,path,"unknown_field",name); g_list_free(members); return FALSE; }
      char *childpath=g_strdup_printf("%s.%s",path,name);
      gboolean ok=_apply(child,cp,json_object_get_member(o,name),err,childpath); g_free(childpath);
      if(!ok) { g_list_free(members); return FALSE; }
    }
    g_list_free(members); return TRUE;
  }
  if(t == DT_INTROSPECTION_TYPE_ARRAY)
  {
    if(f->Array.type == DT_INTROSPECTION_TYPE_CHAR)
    {
      if(!JSON_NODE_HOLDS_STRING(v)) { _error(err,path,"invalid_type","string required"); return FALSE; }
      const char *s=json_node_get_string(v); gsize len=strlen(s);
      if(!g_utf8_validate(s, (gssize)len, NULL)) { _error(err,path,"invalid_encoding","string must be valid UTF-8"); return FALSE; }
      if(len >= f->Array.count) { _error(err,path,"string_too_long","capacity includes terminator"); return FALSE; }
      memset(p,0,f->Array.count); memcpy(p,s,len); return TRUE;
    }
    if(!JSON_NODE_HOLDS_ARRAY(v)) { _error(err,path,"invalid_type","array required"); return FALSE; }
    JsonArray *a=json_node_get_array(v);
    if(json_array_get_length(a) != f->Array.count) { _error(err,path,"invalid_shape","array length differs from schema"); return FALSE; }
    for(guint i=0;i<f->Array.count;i++)
    {
      dt_introspection_field_t *child=NULL; void *ep=dt_introspection_access_array(f,p,i,&child);
      char *epath=g_strdup_printf("%s[%u]",path,i); gboolean ok=_apply(child,ep,json_array_get_element(a,i),err,epath); g_free(epath);
      if(!ok) return FALSE;
    }
    return TRUE;
  }
  if(t == DT_INTROSPECTION_TYPE_BOOL)
  {
    if(!JSON_NODE_HOLDS_BOOLEAN(v)) { _error(err,path,"invalid_type","boolean required"); return FALSE; }
    *(gboolean *)p=json_node_get_boolean(v); return TRUE;
  }
  if(t == DT_INTROSPECTION_TYPE_ENUM)
  {
    int n; gboolean found=FALSE;
    if(JSON_NODE_HOLDS_STRING(v))
      found=dt_introspection_get_enum_value(f,json_node_get_string(v),&n);
    else if(JSON_NODE_HOLDS_INT(v))
    {
      const gint64 candidate=json_node_get_int(v);
      for(dt_introspection_type_enum_tuple_t *e=f->Enum.values; e&&e->name; e++)
        if((gint64)e->value == candidate) { n=e->value; found=TRUE; break; }
    }
    else { _error(err,path,"invalid_type","enum symbol or integer required"); return FALSE; }
    if(!found) { _error(err,path,"invalid_enum","unknown enum value"); return FALSE; }
    *(int *)p=n; return TRUE;
  }
  if(_is_integer(t))
  {
    if(JSON_NODE_HOLDS_INT(v))
    {
      const gint64 n=json_node_get_int(v);
      if(!_integer_in_range(f,n)) { _error(err,path,"out_of_range","value is outside declared range"); return FALSE; }
      _store_integer(f,p,n); return TRUE;
    }
    double n;
    if(!_number(v,TRUE,&n,err,path)) return FALSE;
    if(fabs(n) > 9007199254740992.0 || !_range(f,n))
    { _error(err,path,"invalid_integer","value is not exactly representable"); return FALSE; }
    _store_number(f,p,n); return TRUE;
  }
  if(t == DT_INTROSPECTION_TYPE_FLOAT || t == DT_INTROSPECTION_TYPE_DOUBLE)
  {
    double n; if(!_number(v,FALSE,&n,err,path)) return FALSE;
    if(!_range(f,n)) { _error(err,path,"out_of_range","value is outside declared range"); return FALSE; }
    _store_number(f,p,n); return TRUE;
  }
  _error(err,path,"unsupported_type","parameter kind cannot be applied"); return FALSE;
}

gboolean dt_mcp_params_apply(dt_introspection_field_t *field, void *data, JsonNode *value, char **err)
{
  if(err) *err=NULL;
  if(!field || !data || !value) { _error(err, "$", "invalid_argument", "missing field, data, or value"); return FALSE; }
  const size_t size=field->header.size;
  if(size == 0) { _error(err,"$","invalid_schema","zero-sized field"); return FALSE; }
  void *copy=g_malloc(size); memcpy(copy,data,size);
  gboolean ok=_apply(field,copy,value,err,field->header.field_name && *field->header.field_name ? field->header.field_name : "$");
  if(ok) memcpy(data,copy,size);
  g_free(copy);
  return ok;
}
