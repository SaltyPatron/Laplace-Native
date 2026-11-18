/* JSON, the one reader and the one writer every Laplace program uses (RFC 8259): the configuration, model headers and
 * request bodies it reads, the answers and descriptions it writes.
 *
 * Reading: lp_json_parse takes the whole text and either refuses it (malformed, trailing bytes, nesting deeper than
 * LP_JSON_DEPTH) or gives a document whose values are nodes, numbered from 0 (the root). An object's members keep their
 * order and a repeated key is kept (lp_json_get answers the first). Strings are decoded once (escapes, \u surrogate
 * pairs; a lone surrogate in its generalized 3-byte form, as Laplace writes every scalar) and stay NUL-terminated.
 *   lp_json *j = lp_json_parse(body, len);
 *   uint32_t text = lp_json_get(j, LP_JSON_ROOT, "text");      // LP_JSON_NONE when absent or the root is no object
 *   const char *s = lp_json_string(j, text, &n);               // NULL unless a string
 *   lp_json_free(j);
 * Writing: lp_json_str appends one string, quoted and escaped. */
#ifndef LAPLACE_JSON_H
#define LAPLACE_JSON_H
#include "laplace/laplace.h"

#define LP_JSON_ROOT  0u
#define LP_JSON_NONE  UINT32_MAX
#define LP_JSON_DEPTH 256

typedef enum { LP_JSON_NULL, LP_JSON_FALSE, LP_JSON_TRUE, LP_JSON_NUMBER, LP_JSON_STRING, LP_JSON_ARRAY, LP_JSON_OBJECT } lp_json_type;
typedef struct lp_json lp_json;

LP_API lp_json     *lp_json_parse(const char *text, size_t len);     /* NULL: not one well-formed JSON value */
LP_API void         lp_json_free(lp_json *);
LP_API lp_json_type lp_json_kind(const lp_json *, uint32_t node);     /* LP_JSON_NULL for LP_JSON_NONE */
LP_API uint32_t     lp_json_count(const lp_json *, uint32_t node);    /* an array's elements, an object's members; else 0 */
LP_API uint32_t     lp_json_item(const lp_json *, uint32_t node, uint32_t i);   /* the i-th element or member value */
LP_API const char  *lp_json_key(const lp_json *, uint32_t object, uint32_t i);  /* the i-th member's key */
LP_API uint32_t     lp_json_get(const lp_json *, uint32_t object, const char *key);
LP_API const char  *lp_json_string(const lp_json *, uint32_t node, size_t *len);   /* len may be NULL */
LP_API bool         lp_json_number(const lp_json *, uint32_t node, double *out);
LP_API bool         lp_json_integer(const lp_json *, uint32_t node, int64_t *out); /* a number written without fraction
                                                                                      or exponent that int64 holds */
LP_API bool         lp_json_bool(const lp_json *, uint32_t node, bool *out);
/* A value's own bytes in the text it was parsed from (which the caller keeps): to pass a member on as it came. */
LP_API const char  *lp_json_text(const lp_json *, uint32_t node, size_t *len);

/* s (len bytes) as a JSON string, quoted, onto b: " and \ escaped, \n \r \t \b \f by name, every other control
 * character as \u00XX; every other byte as it is. */
LP_API void lp_json_str(lp_buf *b, const char *s, size_t len);

#endif
