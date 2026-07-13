#ifndef CLAW_UTF8_H
#define CLAW_UTF8_H

#include <stddef.h>

/*
 * UTF-8 boundary helpers used wherever string content is truncated before
 * being embedded in JSON or forwarded to a remote API.  Raw byte truncation
 * can leave a dangling lead byte or a continuation byte orphaned at the cut
 * point, producing "Invalid UTF-8 middle byte" errors from strict parsers.
 *
 * Both functions handle all four UTF-8 sequence lengths (1-4 bytes) and
 * correctly distinguish a COMPLETE final multi-byte character (keep it) from
 * a CLIPPED one (drop the dangling lead byte).
 */

/**
 * Walk back from buf[len] to the nearest valid UTF-8 character boundary.
 * Returns a length <= len such that buf[0..result) contains only complete
 * UTF-8 sequences.  Never reads outside buf[0..len).
 */
size_t claw_utf8_safe_len(const char *buf, size_t len);

/**
 * Copy src into dst (dst_size bytes), truncating at a UTF-8 character
 * boundary and appending CLAW_UTF8_TRUNCATION_NOTICE when content was cut.
 * Always NUL-terminates dst (unless dst_size == 0).
 */
void claw_utf8_truncate_copy(char *dst, size_t dst_size, const char *src);

/**
 * Scan the entire NUL-terminated string `s` in place and replace every byte
 * that is not part of a well-formed UTF-8 sequence with '?'.  Unlike
 * claw_utf8_safe_len (which only trims a clipped sequence at the END), this
 * repairs invalid bytes ANYWHERE in the string — e.g. a lead byte left
 * dangling by a byte-wise truncation buried mid-string, such as Lua's
 * luaO_chunkid cutting a multi-byte character inside a "[string \"...\"]"
 * error message.  Length is preserved (each bad byte -> one '?'), so byte
 * offsets are unchanged.  Also rejects overlong encodings and surrogates.
 * Returns the (unchanged) string length.  Safe on NULL (returns 0).
 *
 * Use at any boundary where bytes of unknown provenance (tool output, file
 * reads, sensor dumps) are embedded into JSON forwarded to a strict remote
 * parser that rejects the whole request on the first invalid byte.
 */
size_t claw_utf8_sanitize_inplace(char *s);

/* Notice appended by claw_utf8_truncate_copy when content is cut. */
#define CLAW_UTF8_TRUNCATION_NOTICE  "...[The information was truncated.]"

#endif /* CLAW_UTF8_H */
