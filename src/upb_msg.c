/*
 * upb - a minimalist implementation of protocol buffers.
 *
 * Copyright (c) 2010 Google Inc.  See LICENSE for details.
 * Author: Josh Haberman <jhaberman@gmail.com>
 *
 * Data structure for storing a message of protobuf data.
 */

#include "upb_msg.h"

static uint32_t upb_round_up_pow2(uint32_t v) {
  // http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v++;
  return v;
}

static void upb_elem_free(upb_value v, upb_fielddef *f) {
  switch(f->type) {
    case UPB_TYPE(MESSAGE):
    case UPB_TYPE(GROUP):
      _upb_msg_free(upb_value_getmsg(v), upb_downcast_msgdef(f->def));
      break;
    case UPB_TYPE(STRING):
    case UPB_TYPE(BYTES):
      _upb_string_free(upb_value_getstr(v));
      break;
    default:
      abort();
  }
}

static void upb_elem_unref(upb_value v, upb_fielddef *f) {
  assert(upb_elem_ismm(f));
  upb_atomic_refcount_t *refcount = upb_value_getrefcount(v);
  if (refcount && upb_atomic_unref(refcount))
    upb_elem_free(v, f);
}

static void upb_field_free(upb_value v, upb_fielddef *f) {
  if (upb_isarray(f)) {
    _upb_array_free(upb_value_getarr(v), f);
  } else {
    upb_elem_free(v, f);
  }
}

static void upb_field_unref(upb_value v, upb_fielddef *f) {
  assert(upb_field_ismm(f));
  upb_atomic_refcount_t *refcount = upb_value_getrefcount(v);
  if (refcount && upb_atomic_unref(refcount))
    upb_field_free(v, f);
}


/* upb_array ******************************************************************/

upb_array *upb_array_new(void) {
  upb_array *arr = malloc(sizeof(*arr));
  upb_atomic_refcount_init(&arr->refcount, 1);
  arr->size = 0;
  arr->len = 0;
  arr->ptr = NULL;
  return arr;
}

void upb_array_recycle(upb_array **_arr, upb_fielddef *f) {
  upb_array *arr = *_arr;
  if(arr && upb_atomic_only(&arr->refcount)) {
    arr->len = 0;
  } else {
    upb_array_unref(arr, f);
    *_arr = upb_array_new();
  }
}

void _upb_array_free(upb_array *arr, upb_fielddef *f) {
  if (upb_elem_ismm(f)) {
    // Need to release refs on sub-objects.
    upb_valuetype_t type = upb_elem_valuetype(f);
    for (upb_arraylen_t i = 0; i < arr->size; i++) {
      upb_valueptr p = _upb_array_getptr(arr, f, i);
      upb_elem_unref(upb_value_read(p, type), f);
    }
  }
  free(arr->ptr);
  free(arr);
}

void upb_array_resize(upb_array *arr, upb_fielddef *f, upb_arraylen_t len) {
  size_t type_size = upb_types[f->type].size;
  upb_arraylen_t old_size = arr->size;
  if (old_size < len) {
    // Need to resize.
    size_t new_size = upb_round_up_pow2(len);
    arr->ptr = realloc(arr->ptr, new_size * type_size);
    arr->size = new_size;
    memset(arr->ptr + (old_size * type_size), 0,
           (new_size - old_size) * type_size);
  }
  arr->len = len;
}


/* upb_msg ********************************************************************/

upb_msg *upb_msg_new(upb_msgdef *md) {
  upb_msg *msg = malloc(md->size);
  // Clear all set bits and cached pointers.
  memset(msg, 0, md->size);
  upb_atomic_refcount_init(&msg->refcount, 1);
  return msg;
}

void _upb_msg_free(upb_msg *msg, upb_msgdef *md) {
  // Need to release refs on all sub-objects.
  upb_msg_iter i;
  for(i = upb_msg_begin(md); !upb_msg_done(i); i = upb_msg_next(md, i)) {
    upb_fielddef *f = upb_msg_iter_field(i);
    upb_valueptr p = _upb_msg_getptr(msg, f);
    upb_valuetype_t type = upb_field_valuetype(f);
    if (upb_field_ismm(f)) upb_field_unref(upb_value_read(p, type), f);
  }
  free(msg);
}

void upb_msg_recycle(upb_msg **_msg, upb_msgdef *msgdef) {
  upb_msg *msg = *_msg;
  if(msg && upb_atomic_only(&msg->refcount)) {
    upb_msg_clear(msg, msgdef);
  } else {
    upb_msg_unref(msg, msgdef);
    *_msg = upb_msg_new(msgdef);
  }
}

INLINE void upb_msg_sethas(upb_msg *msg, upb_fielddef *f) {
  msg->data[f->set_bit_offset] |= f->set_bit_mask;
}

void upb_msg_set(upb_msg *msg, upb_fielddef *f, upb_value val) {
  assert(val.type == upb_types[upb_field_valuetype(f)].inmemory_type);
  upb_valueptr ptr = _upb_msg_getptr(msg, f);
  if (upb_field_ismm(f)) {
    // Unref any previous value we may have had there.
    upb_value oldval = upb_value_read(ptr, upb_field_valuetype(f));
    upb_field_unref(oldval, f);

    // Ref the new value.
    upb_atomic_refcount_t *refcount = upb_value_getrefcount(val);
    if (refcount) upb_atomic_ref(refcount);
  }
  upb_msg_sethas(msg, f);
  return upb_value_write(ptr, val, upb_field_valuetype(f));
}

upb_value upb_msg_get(upb_msg *msg, upb_fielddef *f) {
  if (!upb_msg_has(msg, f)) {
    upb_value val = f->default_value;
    if (upb_issubmsg(f)) {
      // TODO: handle arrays also, which must be treated similarly.
      upb_msgdef *md = upb_downcast_msgdef(f->def);
      upb_msg *m = upb_msg_new(md);
      // Copy all set bits and values, except the refcount.
      memcpy(m , upb_value_getmsg(val), md->size);
      upb_atomic_refcount_init(&m->refcount, 0); // The msg will take a ref.
      upb_value_setmsg(&val, m);
    }
    upb_msg_set(msg, f, val);
    return val;
  } else {
    return upb_value_read(_upb_msg_getptr(msg, f), upb_field_valuetype(f));
  }
}

static upb_flow_t upb_msg_dispatch(upb_msg *msg, upb_msgdef *md,
                                   upb_dispatcher *d);

static upb_flow_t upb_msg_pushval(upb_value val, upb_fielddef *f,
                                  upb_dispatcher *d, upb_handlers_fieldent *hf) {
#define CHECK_FLOW(x) do { \
  upb_flow_t flow = x; if (flow != UPB_CONTINUE) return flow; \
  } while(0)

// For when a SKIP can be implemented just through an early return.
#define CHECK_FLOW_LOCAL(x) do { \
  upb_flow_t flow = x; \
  if (flow != UPB_CONTINUE) { \
    if (flow == UPB_SKIPSUBMSG) flow = UPB_CONTINUE; \
    return flow; \
  } \
} while (0)
  if (upb_issubmsg(f)) {
    upb_msg *msg = upb_value_getmsg(val);
    CHECK_FLOW_LOCAL(upb_dispatch_startsubmsg(d, hf, 0));
    CHECK_FLOW_LOCAL(upb_msg_dispatch(msg, upb_downcast_msgdef(f->def), d));
    CHECK_FLOW(upb_dispatch_endsubmsg(d));
  } else {
    CHECK_FLOW(upb_dispatch_value(d, hf, val));
  }
  return UPB_CONTINUE;
}

static upb_flow_t upb_msg_dispatch(upb_msg *msg, upb_msgdef *md,
                                   upb_dispatcher *d) {
  upb_msg_iter i;
  for(i = upb_msg_begin(md); !upb_msg_done(i); i = upb_msg_next(md, i)) {
    upb_fielddef *f = upb_msg_iter_field(i);
    if (!upb_msg_has(msg, f)) continue;
    upb_handlers_fieldent *hf = upb_dispatcher_lookup(d, f->number);
    if (!hf) continue;
    upb_value val = upb_msg_get(msg, f);
    if (upb_isarray(f)) {
      upb_array *arr = upb_value_getarr(val);
      for (uint32_t j = 0; j < upb_array_len(arr); ++j) {
        CHECK_FLOW_LOCAL(upb_msg_pushval(upb_array_get(arr, f, j), f, d, hf));
      }
    } else {
      CHECK_FLOW_LOCAL(upb_msg_pushval(val, f, d, hf));
    }
  }
  return UPB_CONTINUE;
#undef CHECK_FLOW
#undef CHECK_FLOW_LOCAL
}

void upb_msg_runhandlers(upb_msg *msg, upb_msgdef *md, upb_handlers *h,
                         void *closure, upb_status *status) {
  upb_dispatcher d;
  upb_dispatcher_init(&d, h);
  upb_dispatcher_reset(&d, closure, 0);

  upb_dispatch_startmsg(&d);
  upb_msg_dispatch(msg, md, &d);
  upb_dispatch_endmsg(&d, status);

  upb_dispatcher_uninit(&d);
}

static upb_valueptr upb_msg_getappendptr(upb_msg *msg, upb_fielddef *f) {
  upb_valueptr p = _upb_msg_getptr(msg, f);
  if (upb_isarray(f)) {
    // Create/recycle/resize the array if necessary, and find a pointer to
    // a newly-appended element.
    if (!upb_msg_has(msg, f)) {
      upb_array_recycle(p.arr, f);
      upb_msg_sethas(msg, f);
    }
    assert(*p.arr != NULL);
    upb_arraylen_t oldlen = upb_array_len(*p.arr);
    upb_array_resize(*p.arr, f, oldlen + 1);
    p = _upb_array_getptr(*p.arr, f, oldlen);
  }
  return p;
}

static void upb_msg_appendval(upb_msg *msg, upb_fielddef *f, upb_value val) {
  upb_valueptr p = upb_msg_getappendptr(msg, f);
  if (upb_isstring(f)) {
    // We do:
    //  - upb_string_recycle(), upb_string_substr() instead of
    //  - upb_string_unref(), upb_string_getref()
    // because we can conveniently cache these upb_string objects in the
    // upb_msg, whereas the upb_src who is sending us these strings may not
    // have a good way of caching them.  This saves the upb_src from allocating
    // new upb_strings all the time to give us.
    //
    // If you were using this to copy one upb_msg to another this would
    // allocate string objects whereas a upb_string_getref could have avoided
    // those allocations completely; if this is an issue, we could make it an
    // option of the upb_msgsink which behavior is desired.
    upb_string *src = upb_value_getstr(val);
    upb_string_recycle(p.str);
    upb_string_substr(*p.str, src, 0, upb_string_len(src));
  } else {
    upb_value_write(p, val, f->type);
  }
  upb_msg_sethas(msg, f);
}

upb_msg *upb_msg_appendmsg(upb_msg *msg, upb_fielddef *f, upb_msgdef *msgdef) {
  upb_valueptr p = upb_msg_getappendptr(msg, f);
  if (upb_isarray(f) || !upb_msg_has(msg, f)) {
    upb_msg_recycle(p.msg, msgdef);
    upb_msg_sethas(msg, f);
  }
  return *p.msg;
}


/* upb_msg dynhandlers  *******************************************************/

static upb_flow_t upb_dmsgsink_value(void *_m, upb_value fval, upb_value val) {
  upb_msg *m = _m;
  upb_fielddef *f = upb_value_getfielddef(fval);
  if (upb_isstring(f)) {
    //fprintf(stderr, "dmsg_value!  this=%p f=%p name=" UPB_STRFMT ",
    //        " UPB_STRFMT "  %p\n", m, f, UPB_STRARG(f->name), UPB_STRARG(val.val.str));
  } else {
    //fprintf(stderr, "dmsg_value!  this=%p f=%p name=" UPB_STRFMT ",
    //        %llu\n", m, f, UPB_STRARG(f->name), val.val.uint64);
  }
  upb_msg_appendval(m, f, val);
  return UPB_CONTINUE;
}

static upb_sflow_t upb_dmsgsink_startsubmsg(void *_m, upb_value fval) {
  upb_msg *m = _m;
  upb_fielddef *f = upb_value_getfielddef(fval);
  //fprintf(stderr, "dmsg_startsubmsg!  " UPB_STRFMT " %p\n", UPB_STRARG(fval.val.fielddef->name), f);
  upb_msgdef *msgdef = upb_downcast_msgdef(f->def);
  void *p = upb_msg_appendmsg(m, f, msgdef);
  //printf("Continuing with: %p\n", p);
  return UPB_CONTINUE_WITH(p);
}

void upb_msg_regdhandlers(upb_handlers *h) {
  upb_register_all(h,
      NULL,  // startmsg
      NULL,  // endmsg
      &upb_dmsgsink_value,
      &upb_dmsgsink_startsubmsg,
      NULL,  // endsubmsg
      NULL); // unknown
}