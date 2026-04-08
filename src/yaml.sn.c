/*
 * yaml/yaml.sn.c — YAML Encoder/Decoder
 *
 * Encoder: builds YAML strings directly with a growable buffer (no library).
 *          Emits block style — nested objects/arrays indent by 2 spaces per
 *          level. Strings are always double-quoted with JSON-style escaping
 *          so roundtrip is safe.
 *
 * Decoder: recursive-descent parser into a lightweight node tree, then
 *          vtable methods do key/index lookups. Only handles the subset of
 *          YAML that the encoder emits (closed-loop roundtrip format).
 *
 * Zero external dependencies. All memory is managed explicitly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

/* =========================================================================
 * Growable Buffer
 * ========================================================================= */

typedef struct {
    char  *data;
    size_t length;
    size_t capacity;
} YamlBuffer;

/* Allocate a new buffer with the given initial capacity (minimum 64 bytes). */
static YamlBuffer *yaml_buffer_new(size_t initial_capacity) {
    YamlBuffer *buf = (YamlBuffer *)calloc(1, sizeof(YamlBuffer));
    buf->capacity = initial_capacity > 64 ? initial_capacity : 64;
    buf->data = (char *)malloc(buf->capacity);
    buf->data[0] = '\0';
    return buf;
}

/* Ensure the buffer has room for at least `needed` more bytes. */
static void yaml_buffer_ensure(YamlBuffer *buf, size_t needed) {
    if (buf->length + needed >= buf->capacity) {
        buf->capacity = (buf->capacity + needed) * 2;
        buf->data = (char *)realloc(buf->data, buf->capacity);
    }
}

/* Append a single character. */
static void yaml_buffer_append_char(YamlBuffer *buf, char c) {
    yaml_buffer_ensure(buf, 1);
    buf->data[buf->length++] = c;
    buf->data[buf->length] = '\0';
}

/* Append `n` bytes from a raw string. */
static void yaml_buffer_append_raw(YamlBuffer *buf, const char *str, size_t n) {
    yaml_buffer_ensure(buf, n);
    memcpy(buf->data + buf->length, str, n);
    buf->length += n;
    buf->data[buf->length] = '\0';
}

/* Append a null-terminated string. */
static void yaml_buffer_append_str(YamlBuffer *buf, const char *str) {
    yaml_buffer_append_raw(buf, str, strlen(str));
}

/* Append `n` spaces (for block-style indentation). */
static void yaml_buffer_append_spaces(YamlBuffer *buf, int n) {
    yaml_buffer_ensure(buf, (size_t)n);
    for (int i = 0; i < n; i++) buf->data[buf->length++] = ' ';
    buf->data[buf->length] = '\0';
}

/* Append a double-quoted JSON-escaped string (includes surrounding quotes).
 * Scans for runs of safe characters and copies them in bulk. */
static void yaml_buffer_append_quoted_string(YamlBuffer *buf, const char *str) {
    yaml_buffer_append_char(buf, '"');
    if (!str) { yaml_buffer_append_char(buf, '"'); return; }
    const char *p = str;
    while (*p) {
        /* Scan for a run of characters that don't need escaping. */
        const char *run_start = p;
        while (*p && (unsigned char)*p >= 0x20 && *p != '"' && *p != '\\') p++;
        if (p > run_start) {
            yaml_buffer_append_raw(buf, run_start, p - run_start);
        }
        if (!*p) break;
        /* Handle the special character. */
        switch (*p) {
            case '"':  yaml_buffer_append_raw(buf, "\\\"", 2); break;
            case '\\': yaml_buffer_append_raw(buf, "\\\\", 2); break;
            case '\n': yaml_buffer_append_raw(buf, "\\n", 2);  break;
            case '\r': yaml_buffer_append_raw(buf, "\\r", 2);  break;
            case '\t': yaml_buffer_append_raw(buf, "\\t", 2);  break;
            case '\b': yaml_buffer_append_raw(buf, "\\b", 2);  break;
            case '\f': yaml_buffer_append_raw(buf, "\\f", 2);  break;
            default: {
                /* Control character < 0x20: emit \uXXXX directly */
                char escaped[7];
                unsigned char c = (unsigned char)*p;
                escaped[0] = '\\'; escaped[1] = 'u'; escaped[2] = '0'; escaped[3] = '0';
                escaped[4] = "0123456789abcdef"[c >> 4];
                escaped[5] = "0123456789abcdef"[c & 0x0f];
                escaped[6] = '\0';
                yaml_buffer_append_raw(buf, escaped, 6);
                break;
            }
        }
        p++;
    }
    yaml_buffer_append_char(buf, '"');
}

/* Free a buffer and its data. */
static void yaml_buffer_free(YamlBuffer *buf) {
    if (buf) { free(buf->data); free(buf); }
}

/* Fast integer-to-buffer: write a long long directly without snprintf. */
static void yaml_buffer_append_int(YamlBuffer *buf, long long val) {
    char tmp[21]; /* enough for -9223372036854775808 */
    char *end = tmp + sizeof(tmp);
    char *p = end;
    int negative = 0;

    if (val < 0) {
        negative = 1;
        /* Handle LLONG_MIN safely: negate in unsigned */
        unsigned long long uval = (unsigned long long)(-(val + 1)) + 1;
        do { *--p = '0' + (char)(uval % 10); uval /= 10; } while (uval);
    } else {
        unsigned long long uval = (unsigned long long)val;
        do { *--p = '0' + (char)(uval % 10); uval /= 10; } while (uval);
    }
    if (negative) *--p = '-';
    yaml_buffer_append_raw(buf, p, end - p);
}

/* Append a double value (whole numbers fast-pathed as int, NaN/Inf → null). */
static void yaml_buffer_append_double(YamlBuffer *buf, double val) {
    if (!isfinite(val)) {
        yaml_buffer_append_raw(buf, "null", 4);
        return;
    }
    if (val == (long long)val && fabs(val) < 1e15) {
        yaml_buffer_append_int(buf, (long long)val);
    } else {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%.17g", val);
        yaml_buffer_append_raw(buf, tmp, n);
    }
}

/* =========================================================================
 * YAML Encoder
 * ========================================================================= */

/*
 * Encoder context uses a nesting stack instead of heap-allocating
 * sub-encoders. Each nesting level tracks indent, array-or-object state,
 * and whether we're inside an array item (affects first-field emission).
 * Sub-encoder __sn__Encoder structs are pre-allocated inside the context.
 */
#define YAML_ENCODER_MAX_DEPTH 64

typedef struct {
    YamlBuffer    *buffer;
    int            depth;
    /* entry_count[d]: number of keys/items already written at this level.
     * Used to detect "first field in an array item body" (entry_count == 0
     * means we append on the same line as the "- " marker instead of on a
     * fresh indented line). */
    int            entry_count[YAML_ENCODER_MAX_DEPTH];
    int            is_array[YAML_ENCODER_MAX_DEPTH];
    int            indent[YAML_ENCODER_MAX_DEPTH];
    /* in_array_item[d]: this level is an object that was pushed as an
     * array element. Its first field shares the "- " line; subsequent
     * fields indent one level deeper. */
    int            in_array_item[YAML_ENCODER_MAX_DEPTH];
    /* empty_placeholder[d]: byte offset in buffer where we reserved space
     * for a "{}"/"[]" placeholder when pushing an empty block. If end() is
     * reached with entry_count == 0, we patch that in. Otherwise ignored. */
    long           empty_marker_pos[YAML_ENCODER_MAX_DEPTH];
    __sn__Encoder  subs[YAML_ENCODER_MAX_DEPTH]; /* pre-allocated sub-encoders */
} YamlEncoderCtx;

static __sn__EncoderVTable encoder_vtable; /* forward decl */

/* Emit the line prefix for the next entry at the current depth. For a
 * regular object field, this is "\n<indent>". For an array item, this is
 * "\n<indent>- ". At depth 0, the first field has no leading newline.
 *
 * Special case: when in_array_item[depth] is set and entry_count[depth]
 * is 0, the caller is writing the first field of an object that was
 * pushed as an array element. That field goes on the same line as the
 * "- " marker already emitted by appendObject(), so we emit nothing
 * (no newline, no indent) — just a single space.
 */
static void encoder_emit_prefix(YamlEncoderCtx *ctx) {
    int d = ctx->depth;

    if (ctx->in_array_item[d] && ctx->entry_count[d] == 0) {
        /* First field of an array-item object: inline after "- ". */
        ctx->entry_count[d] = 1;
        return;
    }

    if (d == 0 && ctx->entry_count[d] == 0) {
        /* First field at root: no leading newline. */
        ctx->entry_count[d] = 1;
    } else {
        yaml_buffer_append_char(ctx->buffer, '\n');
        yaml_buffer_append_spaces(ctx->buffer, ctx->indent[d]);
        ctx->entry_count[d]++;
    }

    if (ctx->is_array[d]) {
        yaml_buffer_append_raw(ctx->buffer, "- ", 2);
    }
}

/* Write an object key: "key: " (with trailing space). Assumes prefix already emitted. */
static void encoder_write_key(YamlEncoderCtx *ctx, const char *key) {
    yaml_buffer_append_str(ctx->buffer, key);
    yaml_buffer_append_raw(ctx->buffer, ": ", 2);
}

/* Push a new nesting level and return the pre-allocated sub-encoder. */
static __sn__Encoder *encoder_push(YamlEncoderCtx *ctx, int is_array, int in_array_item) {
    ctx->depth++;
    int d = ctx->depth;
    ctx->entry_count[d] = 0;
    ctx->is_array[d] = is_array;
    ctx->indent[d] = ctx->indent[d - 1] + 2;
    ctx->in_array_item[d] = in_array_item;
    /* Reserve the current buffer position so end() can patch in "{}"/"[]"
     * if this block turns out to be empty. We emit a single space as a
     * placeholder that we can overwrite. */
    ctx->empty_marker_pos[d] = (long)ctx->buffer->length;
    ctx->subs[d].__sn__vt = &encoder_vtable;
    ctx->subs[d].__sn__ctx = ctx;
    ctx->subs[d].__sn__cleanup = NULL;
    return &ctx->subs[d];
}

/* --- Keyed writers (for object fields) --- */

static void encoder_write_str(__sn__Encoder *self, const char *key, const char *val) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    encoder_write_key(ctx, key);
    yaml_buffer_append_quoted_string(ctx->buffer, val);
}

static void encoder_write_int(__sn__Encoder *self, const char *key, long long val) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    encoder_write_key(ctx, key);
    yaml_buffer_append_int(ctx->buffer, val);
}

static void encoder_write_double(__sn__Encoder *self, const char *key, double val) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    encoder_write_key(ctx, key);
    yaml_buffer_append_double(ctx->buffer, val);
}

static void encoder_write_bool(__sn__Encoder *self, const char *key, long long val) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    encoder_write_key(ctx, key);
    if (val) yaml_buffer_append_raw(ctx->buffer, "true", 4);
    else     yaml_buffer_append_raw(ctx->buffer, "false", 5);
}

static void encoder_write_null(__sn__Encoder *self, const char *key) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    encoder_write_key(ctx, key);
    yaml_buffer_append_raw(ctx->buffer, "null", 4);
}

/* --- Nested structure writers --- */

static __sn__Encoder *encoder_begin_object(__sn__Encoder *self, const char *key) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    /* "key:" with no trailing space — children will emit a newline before
     * their first entry so the value appears on the next line. */
    yaml_buffer_append_str(ctx->buffer, key);
    yaml_buffer_append_char(ctx->buffer, ':');
    return encoder_push(ctx, 0, 0);
}

static __sn__Encoder *encoder_begin_array(__sn__Encoder *self, const char *key) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    yaml_buffer_append_str(ctx->buffer, key);
    yaml_buffer_append_char(ctx->buffer, ':');
    return encoder_push(ctx, 1, 0);
}

static void encoder_end(__sn__Encoder *self) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    int d = ctx->depth;
    if (ctx->entry_count[d] == 0) {
        /* Empty block — patch a " {}" or " []" marker at the recorded
         * position. The buffer currently ends at empty_marker_pos, with
         * the preceding "key:" already written, so we just append.  */
        yaml_buffer_append_char(ctx->buffer, ' ');
        if (ctx->is_array[d]) yaml_buffer_append_raw(ctx->buffer, "[]", 2);
        else                  yaml_buffer_append_raw(ctx->buffer, "{}", 2);
    }
    ctx->depth--;
}

/* --- Array element appenders (for array encoders) --- */

static void encoder_append_str(__sn__Encoder *self, const char *val) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    yaml_buffer_append_quoted_string(ctx->buffer, val);
}

static void encoder_append_int(__sn__Encoder *self, long long val) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    yaml_buffer_append_int(ctx->buffer, val);
}

static void encoder_append_double(__sn__Encoder *self, double val) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    yaml_buffer_append_double(ctx->buffer, val);
}

static void encoder_append_bool(__sn__Encoder *self, long long val) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    yaml_buffer_append_str(ctx->buffer, val ? "true" : "false");
}

static __sn__Encoder *encoder_append_object(__sn__Encoder *self) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    encoder_emit_prefix(ctx);
    /* An object appended as an array element: its first field shares the
     * "- " line ("- name: ..."), subsequent fields indent to align with
     * the first field (parent indent + 2). */
    return encoder_push(ctx, 0, 1);
}

/* Finalize the top-level encoder and return the YAML string. */
static char *encoder_result(__sn__Encoder *self) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    int d = ctx->depth;
    if (ctx->entry_count[d] == 0) {
        /* Completely empty root — emit "{}" or "[]" on its own. */
        if (ctx->is_array[d]) yaml_buffer_append_raw(ctx->buffer, "[]", 2);
        else                  yaml_buffer_append_raw(ctx->buffer, "{}", 2);
    }
    char *result = strdup(ctx->buffer->data);
    yaml_buffer_free(ctx->buffer);
    free(ctx);
    self->__sn__ctx = NULL;
    return result;
}

/* Encoder vtable — wired into every encoder instance. */
static __sn__EncoderVTable encoder_vtable = {
    .writeStr     = encoder_write_str,
    .writeInt     = encoder_write_int,
    .writeDouble  = encoder_write_double,
    .writeBool    = encoder_write_bool,
    .writeNull    = encoder_write_null,
    .beginObject  = encoder_begin_object,
    .beginArray   = encoder_begin_array,
    .end          = encoder_end,
    .appendStr    = encoder_append_str,
    .appendInt    = encoder_append_int,
    .appendDouble = encoder_append_double,
    .appendBool   = encoder_append_bool,
    .appendObject = encoder_append_object,
    .result       = encoder_result,
};

/* Cleanup handler for the root encoder (frees buffer + context). */
static void encoder_cleanup(__sn__Encoder *self) {
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)self->__sn__ctx;
    if (ctx) {
        yaml_buffer_free(ctx->buffer);
        free(ctx);
        self->__sn__ctx = NULL;
    }
}

/* Initialize an encoder context at depth 0. */
static void encoder_init_ctx(YamlEncoderCtx *ctx, YamlBuffer *buffer, int is_array) {
    ctx->buffer = buffer;
    ctx->depth = 0;
    ctx->entry_count[0] = 0;
    ctx->is_array[0] = is_array;
    ctx->indent[0] = 0;
    ctx->in_array_item[0] = 0;
    ctx->empty_marker_pos[0] = 0;
}

/* Public: create a root object encoder. */
__sn__Encoder *sn_yaml_encoder(void) {
    YamlBuffer *buffer = yaml_buffer_new(4096);
    __sn__Encoder *enc = (__sn__Encoder *)calloc(1, sizeof(__sn__Encoder));
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)calloc(1, sizeof(YamlEncoderCtx));
    encoder_init_ctx(ctx, buffer, 0);
    enc->__sn__vt = &encoder_vtable;
    enc->__sn__ctx = ctx;
    enc->__sn__cleanup = encoder_cleanup;
    return enc;
}

/* Public: create a root array encoder. */
__sn__Encoder *sn_yaml_array_encoder(void) {
    YamlBuffer *buffer = yaml_buffer_new(65536);
    __sn__Encoder *enc = (__sn__Encoder *)calloc(1, sizeof(__sn__Encoder));
    YamlEncoderCtx *ctx = (YamlEncoderCtx *)calloc(1, sizeof(YamlEncoderCtx));
    encoder_init_ctx(ctx, buffer, 1);
    enc->__sn__vt = &encoder_vtable;
    enc->__sn__ctx = ctx;
    enc->__sn__cleanup = encoder_cleanup;
    return enc;
}

/* =========================================================================
 * YAML Decoder — Node Tree Types
 * ========================================================================= */

/*
 * Parses YAML into a lightweight node tree. Nodes are allocated individually.
 * The tree is freed when the root decoder is cleaned up.
 */

typedef enum {
    YAML_NODE_OBJECT,
    YAML_NODE_ARRAY,
    YAML_NODE_STRING,
    YAML_NODE_INT,
    YAML_NODE_DOUBLE,
    YAML_NODE_BOOL,
    YAML_NODE_NULL
} YamlNodeType;

typedef struct YamlNode YamlNode;

typedef struct {
    char     *key;
    YamlNode *value;
} YamlKeyValue;

struct YamlNode {
    YamlNodeType type;
    union {
        struct { YamlKeyValue *entries; int count; int capacity; } object;
        struct { YamlNode    **items;   int count; int capacity; } array;
        char      *string_value;
        long long  int_value;
        double     double_value;
        int        bool_value;
    };
};

/* Recursively free a node tree. */
static void yaml_node_free(YamlNode *node) {
    if (!node) return;
    switch (node->type) {
        case YAML_NODE_OBJECT:
            for (int i = 0; i < node->object.count; i++) {
                free(node->object.entries[i].key);
                yaml_node_free(node->object.entries[i].value);
            }
            free(node->object.entries);
            break;
        case YAML_NODE_ARRAY:
            for (int i = 0; i < node->array.count; i++) {
                yaml_node_free(node->array.items[i]);
            }
            free(node->array.items);
            break;
        case YAML_NODE_STRING:
            free(node->string_value);
            break;
        default:
            break;
    }
    free(node);
}

/* Look up a key in an object node. Returns NULL if not found. */
static YamlNode *yaml_object_get(YamlNode *node, const char *key) {
    if (!node || node->type != YAML_NODE_OBJECT) return NULL;
    for (int i = 0; i < node->object.count; i++) {
        if (strcmp(node->object.entries[i].key, key) == 0)
            return node->object.entries[i].value;
    }
    return NULL;
}

/* Allocate a new empty object node. */
static YamlNode *yaml_node_new_object(void) {
    YamlNode *n = (YamlNode *)calloc(1, sizeof(YamlNode));
    n->type = YAML_NODE_OBJECT;
    n->object.capacity = 8;
    n->object.entries = (YamlKeyValue *)malloc(sizeof(YamlKeyValue) * n->object.capacity);
    n->object.count = 0;
    return n;
}

/* Allocate a new empty array node. */
static YamlNode *yaml_node_new_array(void) {
    YamlNode *n = (YamlNode *)calloc(1, sizeof(YamlNode));
    n->type = YAML_NODE_ARRAY;
    n->array.capacity = 8;
    n->array.items = (YamlNode **)malloc(sizeof(YamlNode *) * n->array.capacity);
    n->array.count = 0;
    return n;
}

/* Allocate a null node (used for missing/unparseable values). */
static YamlNode *yaml_node_new_null(void) {
    YamlNode *n = (YamlNode *)calloc(1, sizeof(YamlNode));
    n->type = YAML_NODE_NULL;
    return n;
}

/* Append a key/value to an object node (grows as needed). */
static void yaml_object_append(YamlNode *obj, char *key, YamlNode *value) {
    if (obj->object.count >= obj->object.capacity) {
        obj->object.capacity *= 2;
        obj->object.entries = (YamlKeyValue *)realloc(
            obj->object.entries, sizeof(YamlKeyValue) * obj->object.capacity);
    }
    obj->object.entries[obj->object.count++] = (YamlKeyValue){ key, value };
}

/* Append an item to an array node (grows as needed). */
static void yaml_array_append(YamlNode *arr, YamlNode *item) {
    if (arr->array.count >= arr->array.capacity) {
        arr->array.capacity *= 2;
        arr->array.items = (YamlNode **)realloc(
            arr->array.items, sizeof(YamlNode *) * arr->array.capacity);
    }
    arr->array.items[arr->array.count++] = item;
}

/* =========================================================================
 * YAML Decoder — Parser
 * =========================================================================
 *
 * Strategy: line-oriented cursor. Each "line" is a slice of the input from
 * the start of a physical line (past leading spaces) up to the next newline.
 * parse_block(indent) consumes lines at exactly that indent level, dispatching
 * to object or array parsing based on the first token ("- " vs "key:").
 */

typedef struct {
    const char *p;      /* current position in input */
    const char *end;    /* one past the last byte */
} YamlCursor;

/* Count leading spaces on the current line (from cur->p, which should be
 * positioned at the start of a line). */
static int yaml_line_indent(const YamlCursor *cur) {
    int n = 0;
    const char *q = cur->p;
    while (q < cur->end && *q == ' ') { q++; n++; }
    return n;
}

/* True if the cursor is positioned at the start of a line that is blank
 * (only whitespace before the newline or end). */
static int yaml_line_is_blank(const YamlCursor *cur) {
    const char *q = cur->p;
    while (q < cur->end && *q != '\n') {
        if (*q != ' ' && *q != '\t' && *q != '\r') return 0;
        q++;
    }
    return 1;
}

/* Advance the cursor past the current line's newline (if any). */
static void yaml_advance_line(YamlCursor *cur) {
    while (cur->p < cur->end && *cur->p != '\n') cur->p++;
    if (cur->p < cur->end && *cur->p == '\n') cur->p++;
}

/* Skip any blank lines at the current cursor position. */
static void yaml_skip_blank_lines(YamlCursor *cur) {
    while (cur->p < cur->end && yaml_line_is_blank(cur)) {
        yaml_advance_line(cur);
    }
}

/* True if cursor is at EOF (past the end of input). */
static int yaml_at_eof(const YamlCursor *cur) {
    return cur->p >= cur->end;
}

/* Parse a double-quoted string starting at **p. Consumes the closing quote.
 * Returns a heap copy of the decoded string content. Handles JSON-style
 * escapes (\" \\ \/ \n \r \t \b \f \uXXXX). */
static char *yaml_parse_quoted_string(const char **p, const char *end) {
    if (*p >= end || **p != '"') return strdup("");
    (*p)++;  /* skip opening quote */

    YamlBuffer *buf = yaml_buffer_new(64);
    while (*p < end && **p != '"') {
        if (**p == '\\' && *p + 1 < end) {
            (*p)++;
            switch (**p) {
                case '"':  yaml_buffer_append_char(buf, '"');  break;
                case '\\': yaml_buffer_append_char(buf, '\\'); break;
                case '/':  yaml_buffer_append_char(buf, '/');  break;
                case 'n':  yaml_buffer_append_char(buf, '\n'); break;
                case 'r':  yaml_buffer_append_char(buf, '\r'); break;
                case 't':  yaml_buffer_append_char(buf, '\t'); break;
                case 'b':  yaml_buffer_append_char(buf, '\b'); break;
                case 'f':  yaml_buffer_append_char(buf, '\f'); break;
                case 'u': {
                    unsigned codepoint = 0;
                    for (int i = 0; i < 4 && (*p + 1) < end; i++) {
                        (*p)++;
                        char c = **p;
                        codepoint <<= 4;
                        if (c >= '0' && c <= '9') codepoint |= c - '0';
                        else if (c >= 'a' && c <= 'f') codepoint |= 10 + c - 'a';
                        else if (c >= 'A' && c <= 'F') codepoint |= 10 + c - 'A';
                    }
                    if (codepoint < 128) yaml_buffer_append_char(buf, (char)codepoint);
                    else yaml_buffer_append_char(buf, '?');
                    break;
                }
                default: yaml_buffer_append_char(buf, **p); break;
            }
        } else {
            yaml_buffer_append_char(buf, **p);
        }
        (*p)++;
    }
    if (*p < end && **p == '"') (*p)++;

    char *result = strdup(buf->data);
    yaml_buffer_free(buf);
    return result;
}

/* Parse a "scalar value" token (quoted string, number, bool, null, or
 * inline empty markers). Consumes trailing spaces on the same line but
 * NOT the newline. Allocates a new YamlNode. */
static YamlNode *yaml_parse_scalar(const char **p, const char *end) {
    /* Skip leading spaces on the value side. */
    while (*p < end && **p == ' ') (*p)++;

    YamlNode *node = (YamlNode *)calloc(1, sizeof(YamlNode));

    if (*p >= end || **p == '\n') {
        /* No value on this line — null. (Could also signal a following
         * block; callers check for that before invoking parse_scalar.) */
        node->type = YAML_NODE_NULL;
        return node;
    }

    if (**p == '"') {
        node->type = YAML_NODE_STRING;
        node->string_value = yaml_parse_quoted_string(p, end);
        return node;
    }

    /* Inline empty markers */
    if (**p == '{' && (*p + 1) < end && (*p)[1] == '}') {
        node->type = YAML_NODE_OBJECT;
        node->object.capacity = 0;
        node->object.entries = NULL;
        node->object.count = 0;
        *p += 2;
        return node;
    }
    if (**p == '[' && (*p + 1) < end && (*p)[1] == ']') {
        node->type = YAML_NODE_ARRAY;
        node->array.capacity = 0;
        node->array.items = NULL;
        node->array.count = 0;
        *p += 2;
        return node;
    }

    /* Capture the unquoted token up to end-of-line. */
    const char *tok_start = *p;
    while (*p < end && **p != '\n' && **p != '\r') (*p)++;
    const char *tok_end = *p;
    /* Trim trailing spaces. */
    while (tok_end > tok_start && tok_end[-1] == ' ') tok_end--;

    size_t tlen = (size_t)(tok_end - tok_start);

    if (tlen == 4 && memcmp(tok_start, "true", 4) == 0) {
        node->type = YAML_NODE_BOOL;
        node->bool_value = 1;
        return node;
    }
    if (tlen == 5 && memcmp(tok_start, "false", 5) == 0) {
        node->type = YAML_NODE_BOOL;
        node->bool_value = 0;
        return node;
    }
    if (tlen == 4 && memcmp(tok_start, "null", 4) == 0) {
        node->type = YAML_NODE_NULL;
        return node;
    }
    if (tlen == 1 && tok_start[0] == '~') {
        node->type = YAML_NODE_NULL;
        return node;
    }

    /* Try to parse as a number. We use a stack buffer since tokens are
     * guaranteed to be short scalars on a single line. */
    if (tlen > 0 && tlen < 64) {
        char tmp[64];
        memcpy(tmp, tok_start, tlen);
        tmp[tlen] = '\0';
        char *nend;
        double d = strtod(tmp, &nend);
        if (nend != tmp && (size_t)(nend - tmp) == tlen) {
            int is_integer = 1;
            for (size_t i = 0; i < tlen; i++) {
                if (tmp[i] == '.' || tmp[i] == 'e' || tmp[i] == 'E') {
                    is_integer = 0;
                    break;
                }
            }
            if (is_integer) {
                node->type = YAML_NODE_INT;
                node->int_value = (long long)d;
            } else {
                node->type = YAML_NODE_DOUBLE;
                node->double_value = d;
            }
            return node;
        }
    }

    /* Fallback: treat as a bare (unquoted) string. */
    node->type = YAML_NODE_STRING;
    node->string_value = (char *)malloc(tlen + 1);
    memcpy(node->string_value, tok_start, tlen);
    node->string_value[tlen] = '\0';
    return node;
}

/* Forward declaration for recursion. */
static YamlNode *yaml_parse_block(YamlCursor *cur, int min_indent);

/* Parse a "key: value" line, or "key:" followed by a nested block.
 * On entry, cur->p points at the start of the line (including leading spaces).
 * The caller has already verified the indent matches expectations.
 * Returns a newly-allocated (key, value) pair; the caller appends it to
 * an object node. The value may be a scalar (consumed from the same line)
 * or a nested block (parsed at indent + 2 from the next non-blank line). */
static void yaml_parse_object_entry(YamlCursor *cur, int entry_indent, YamlKeyValue *out) {
    /* Skip leading spaces. */
    const char *p = cur->p + entry_indent;
    const char *end = cur->end;

    /* Parse the key: either a quoted string or a bare identifier up to ':'. */
    char *key = NULL;
    if (p < end && *p == '"') {
        key = yaml_parse_quoted_string(&p, end);
    } else {
        const char *k_start = p;
        while (p < end && *p != ':' && *p != '\n') p++;
        /* Trim trailing spaces from bare key. */
        const char *k_end = p;
        while (k_end > k_start && k_end[-1] == ' ') k_end--;
        size_t klen = (size_t)(k_end - k_start);
        key = (char *)malloc(klen + 1);
        memcpy(key, k_start, klen);
        key[klen] = '\0';
    }

    /* Expect ':'. */
    if (p < end && *p == ':') p++;

    /* Skip spaces after the colon. */
    while (p < end && *p == ' ') p++;

    YamlNode *value = NULL;

    if (p >= end || *p == '\n' || *p == '\r') {
        /* No inline value — advance to next line and parse a nested block. */
        cur->p = p;
        yaml_advance_line(cur);
        yaml_skip_blank_lines(cur);

        if (!yaml_at_eof(cur) && yaml_line_indent(cur) > entry_indent) {
            value = yaml_parse_block(cur, yaml_line_indent(cur));
        } else {
            /* No nested block and no inline value — null. */
            value = yaml_node_new_null();
        }
    } else {
        /* Inline scalar value. */
        value = yaml_parse_scalar(&p, end);
        cur->p = p;
        yaml_advance_line(cur);
    }

    out->key = key;
    out->value = value;
}

/* Parse a block (object or array) starting at the current cursor position.
 * All lines at exactly `min_indent` belong to this block. A line at a
 * greater indent is a child's content; a lesser indent terminates the block.
 *
 * The first line's content determines the block type:
 *   "- ..."    → array
 *   "key: ..." → object
 */
static YamlNode *yaml_parse_block(YamlCursor *cur, int min_indent) {
    yaml_skip_blank_lines(cur);
    if (yaml_at_eof(cur)) return yaml_node_new_null();

    int indent = yaml_line_indent(cur);
    if (indent != min_indent) {
        /* Shouldn't happen if caller does its job, but be defensive. */
        return yaml_node_new_null();
    }

    const char *first_content = cur->p + indent;
    if (first_content >= cur->end) return yaml_node_new_null();

    /* Dispatch: array starts with "- ", object starts with "key:". */
    if (*first_content == '-' &&
        (first_content + 1 >= cur->end || first_content[1] == ' ' || first_content[1] == '\n' || first_content[1] == '\r')) {
        /* --- Array block --- */
        YamlNode *arr = yaml_node_new_array();

        while (!yaml_at_eof(cur)) {
            yaml_skip_blank_lines(cur);
            if (yaml_at_eof(cur)) break;
            int this_indent = yaml_line_indent(cur);
            if (this_indent < min_indent) break;
            if (this_indent > min_indent) break;  /* deeper content is a child's */

            const char *p = cur->p + this_indent;
            if (p >= cur->end || *p != '-') break;
            /* Consume the "- " marker. */
            p++;
            /* Allow "-\n" (empty marker with nested block below) or "- value". */
            int had_space = 0;
            while (p < cur->end && *p == ' ') { p++; had_space = 1; }

            YamlNode *item = NULL;
            if (p >= cur->end || *p == '\n' || *p == '\r') {
                /* "- " followed by nothing — look for nested block on next line. */
                cur->p = p;
                yaml_advance_line(cur);
                yaml_skip_blank_lines(cur);
                if (!yaml_at_eof(cur) && yaml_line_indent(cur) > min_indent) {
                    item = yaml_parse_block(cur, yaml_line_indent(cur));
                } else {
                    item = yaml_node_new_null();
                }
            } else if (had_space) {
                /* "- value" — the value might be a scalar on this line, OR
                 * it might be the start of an inline object ("- key: value")
                 * whose subsequent fields are indented under the dash column
                 * plus 2. We detect inline-object form by scanning for a
                 * ':' on this line that isn't inside quotes. */
                const char *scan = p;
                int in_quote = 0;
                const char *colon = NULL;
                while (scan < cur->end && *scan != '\n' && *scan != '\r') {
                    if (*scan == '"') in_quote = !in_quote;
                    else if (*scan == ':' && !in_quote) { colon = scan; break; }
                    scan++;
                }

                if (colon != NULL) {
                    /* Inline object starting at `p`. The first field sits
                     * at column (min_indent + 2), and subsequent fields
                     * will be at the same column on their own lines. We
                     * synthesize a new cursor position at `p`, treating
                     * (min_indent + 2) as the entry indent for all fields
                     * of this object. */
                    YamlNode *obj = yaml_node_new_object();
                    int obj_indent = min_indent + 2;

                    /* Parse the first field from `p` directly. We set
                     * cur->p to the start of the current line so the
                     * parse_object_entry helper can use `entry_indent` to
                     * skip past the "- " marker and any extra spaces. The
                     * actual column of `p` is min_indent + 2 (dash + space). */
                    /* Temporarily position cur->p so that cur->p + obj_indent == p. */
                    cur->p = p - obj_indent;
                    YamlKeyValue kv;
                    yaml_parse_object_entry(cur, obj_indent, &kv);
                    yaml_object_append(obj, kv.key, kv.value);

                    /* Consume any further fields at exactly obj_indent. */
                    while (!yaml_at_eof(cur)) {
                        yaml_skip_blank_lines(cur);
                        if (yaml_at_eof(cur)) break;
                        int li = yaml_line_indent(cur);
                        if (li != obj_indent) break;
                        /* Make sure this line doesn't start a new array item. */
                        const char *lp = cur->p + li;
                        if (lp < cur->end && *lp == '-' &&
                            (lp + 1 >= cur->end || lp[1] == ' ' || lp[1] == '\n' || lp[1] == '\r')) {
                            break;
                        }
                        YamlKeyValue kv2;
                        yaml_parse_object_entry(cur, obj_indent, &kv2);
                        yaml_object_append(obj, kv2.key, kv2.value);
                    }
                    item = obj;
                } else {
                    /* Pure scalar item. */
                    item = yaml_parse_scalar(&p, cur->end);
                    cur->p = p;
                    yaml_advance_line(cur);
                }
            } else {
                /* "-" with no space — malformed-ish. Advance and null. */
                cur->p = p;
                yaml_advance_line(cur);
                item = yaml_node_new_null();
            }

            yaml_array_append(arr, item);
        }
        return arr;

    } else {
        /* --- Object block --- */
        YamlNode *obj = yaml_node_new_object();

        while (!yaml_at_eof(cur)) {
            yaml_skip_blank_lines(cur);
            if (yaml_at_eof(cur)) break;
            int this_indent = yaml_line_indent(cur);
            if (this_indent < min_indent) break;
            if (this_indent > min_indent) break;

            const char *p = cur->p + this_indent;
            /* Don't consume an array marker as an object field. */
            if (p < cur->end && *p == '-' &&
                (p + 1 >= cur->end || p[1] == ' ' || p[1] == '\n' || p[1] == '\r')) {
                break;
            }

            YamlKeyValue kv;
            yaml_parse_object_entry(cur, this_indent, &kv);
            yaml_object_append(obj, kv.key, kv.value);
        }
        return obj;
    }
}

/* Top-level parse: figure out whether the input begins with an array or
 * object (or scalar), and return the root node. */
static YamlNode *yaml_parse_root(const char *input) {
    YamlCursor cur;
    cur.p = input;
    cur.end = input + strlen(input);

    yaml_skip_blank_lines(&cur);
    if (yaml_at_eof(&cur)) return yaml_node_new_null();

    int indent = yaml_line_indent(&cur);
    const char *first = cur.p + indent;
    if (first >= cur.end) return yaml_node_new_null();

    /* A scalar at the root (e.g. "{}" or "42") that isn't "- ..." or "key:". */
    if (*first == '{' || *first == '[') {
        /* Inline empty root object/array. */
        const char *p = first;
        return yaml_parse_scalar(&p, cur.end);
    }

    return yaml_parse_block(&cur, indent);
}

/* =========================================================================
 * YAML Decoder — Sindarin Vtable Implementation
 * ========================================================================= */

typedef struct {
    YamlNode *node;   /* borrowed reference into the parse tree */
    YamlNode *root;   /* root node — only non-NULL for the root decoder (owns the tree) */
} YamlDecoderCtx;

static __sn__Decoder *decoder_create(YamlNode *node, YamlNode *root);

/* --- Keyed readers (for object fields) --- */

static char *decoder_read_str(__sn__Decoder *self, const char *key) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    YamlNode *val = yaml_object_get(ctx->node, key);
    return (val && val->type == YAML_NODE_STRING) ? strdup(val->string_value) : strdup("");
}

static long long decoder_read_int(__sn__Decoder *self, const char *key) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    YamlNode *val = yaml_object_get(ctx->node, key);
    return (val && val->type == YAML_NODE_INT) ? val->int_value : 0;
}

static double decoder_read_double(__sn__Decoder *self, const char *key) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    YamlNode *val = yaml_object_get(ctx->node, key);
    if (val && val->type == YAML_NODE_DOUBLE) return val->double_value;
    if (val && val->type == YAML_NODE_INT)    return (double)val->int_value;
    return 0.0;
}

static long long decoder_read_bool(__sn__Decoder *self, const char *key) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    YamlNode *val = yaml_object_get(ctx->node, key);
    return (val && val->type == YAML_NODE_BOOL) ? val->bool_value : 0;
}

static long long decoder_has_key(__sn__Decoder *self, const char *key) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    return yaml_object_get(ctx->node, key) != NULL;
}

static __sn__Decoder *decoder_read_object(__sn__Decoder *self, const char *key) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    return decoder_create(yaml_object_get(ctx->node, key), NULL);
}

static __sn__Decoder *decoder_read_array(__sn__Decoder *self, const char *key) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    return decoder_create(yaml_object_get(ctx->node, key), NULL);
}

/* --- Array accessors --- */

static long long decoder_length(__sn__Decoder *self) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == YAML_NODE_ARRAY) return ctx->node->array.count;
    return 0;
}

static __sn__Decoder *decoder_at(__sn__Decoder *self, long long index) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == YAML_NODE_ARRAY && index < ctx->node->array.count)
        return decoder_create(ctx->node->array.items[index], NULL);
    return decoder_create(NULL, NULL);
}

static char *decoder_at_str(__sn__Decoder *self, long long index) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == YAML_NODE_ARRAY && index < ctx->node->array.count) {
        YamlNode *val = ctx->node->array.items[index];
        if (val->type == YAML_NODE_STRING) return strdup(val->string_value);
    }
    return strdup("");
}

static long long decoder_at_int(__sn__Decoder *self, long long index) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == YAML_NODE_ARRAY && index < ctx->node->array.count) {
        YamlNode *val = ctx->node->array.items[index];
        if (val->type == YAML_NODE_INT) return val->int_value;
    }
    return 0;
}

static double decoder_at_double(__sn__Decoder *self, long long index) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == YAML_NODE_ARRAY && index < ctx->node->array.count) {
        YamlNode *val = ctx->node->array.items[index];
        if (val->type == YAML_NODE_DOUBLE) return val->double_value;
        if (val->type == YAML_NODE_INT)    return (double)val->int_value;
    }
    return 0.0;
}

static long long decoder_at_bool(__sn__Decoder *self, long long index) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    if (ctx->node && ctx->node->type == YAML_NODE_ARRAY && index < ctx->node->array.count) {
        YamlNode *val = ctx->node->array.items[index];
        if (val->type == YAML_NODE_BOOL) return val->bool_value;
    }
    return 0;
}

/* Decoder vtable — wired into every decoder instance. */
static __sn__DecoderVTable decoder_vtable = {
    .readStr    = decoder_read_str,
    .readInt    = decoder_read_int,
    .readDouble = decoder_read_double,
    .readBool   = decoder_read_bool,
    .hasKey     = decoder_has_key,
    .readObject = decoder_read_object,
    .readArray  = decoder_read_array,
    .length     = decoder_length,
    .at         = decoder_at,
    .atStr      = decoder_at_str,
    .atInt      = decoder_at_int,
    .atDouble   = decoder_at_double,
    .atBool     = decoder_at_bool,
};

/* Cleanup handler for the root decoder (frees the entire parse tree). */
static void decoder_cleanup(__sn__Decoder *self) {
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)self->__sn__ctx;
    if (ctx) {
        if (ctx->root) yaml_node_free(ctx->root);
        free(ctx);
        self->__sn__ctx = NULL;
    }
}

/* Create a decoder wrapping a node. If `root` is non-NULL, this decoder owns the tree. */
static __sn__Decoder *decoder_create(YamlNode *node, YamlNode *root) {
    __sn__Decoder *dec = (__sn__Decoder *)calloc(1, sizeof(__sn__Decoder));
    YamlDecoderCtx *ctx = (YamlDecoderCtx *)calloc(1, sizeof(YamlDecoderCtx));
    ctx->node = node;
    ctx->root = root;
    dec->__sn__vt = &decoder_vtable;
    dec->__sn__ctx = ctx;
    dec->__sn__cleanup = decoder_cleanup;
    return dec;
}

/* Public: parse a YAML string and return a root decoder. */
__sn__Decoder *sn_yaml_decoder(const char *input) {
    YamlNode *root = yaml_parse_root(input);
    return decoder_create(root, root);
}
