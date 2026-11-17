#include "mpx_id.h"

#include <string.h>

const char mpx_id_alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* Crockford base32 value of one symbol, or -1. The alphabet has no I, L, O or
 * U, so those are simply invalid rather than aliased: aliasing is for typed
 * input, and this runs on wire bytes. */
static int symbol_value(char c)
{
    const char *pos;

    if (c == '\0') {
        return -1;
    }
    pos = strchr(mpx_id_alphabet, c);
    if (pos == NULL) {
        return -1;
    }
    return (int)(pos - mpx_id_alphabet);
}

static bool has_prefix(const char *id)
{
    const bool mpx = id[0] == 'M' && id[1] == 'P' && id[2] == 'X';
    const bool dev = id[0] == 'D' && id[1] == 'E' && id[2] == 'V';
    return mpx || dev;
}

int mpx_id_check_symbol(const char *id)
{
    int factor = 2;
    int total = 0;

    if (id == NULL || strlen(id) != (size_t)MPX_ID_STR_LEN) {
        return -1;
    }
    if (!has_prefix(id) || id[3] != '-') {
        return -1;
    }

    /* Luhn mod 32 running right to left over the body. The doubled value is
     * folded with `a / 32 + a % 32` rather than subtracted twice: that is what
     * makes a substitution at the doubling position detectable too, which plain
     * Luhn mod 32 misses for delta 16. See tools/mpx_id.py, which is the
     * reference implementation and the source of the test vectors. */
    for (int i = MPX_ID_CHECK_OFF - 1; i >= MPX_ID_BODY_OFF; i--) {
        int a = symbol_value(id[i]);
        if (a < 0) {
            return -1;
        }
        a *= factor;
        factor = (factor == 2) ? 1 : 2;
        total += a / 32 + a % 32;
    }

    return (int)(unsigned char)mpx_id_alphabet[(32 - total % 32) % 32];
}

bool mpx_id_valid(const char *id)
{
    int expected;

    if (id == NULL) {
        return false;
    }
    expected = mpx_id_check_symbol(id);
    if (expected < 0) {
        return false;
    }
    if (symbol_value(id[MPX_ID_CHECK_OFF]) < 0) {
        return false;
    }
    return id[MPX_ID_CHECK_OFF] == (char)expected;
}

bool mpx_id_legacy_valid(const char *id)
{
    size_t len;

    if (id == NULL) {
        return false;
    }
    len = strlen(id);
    if (len == 0 || len > (size_t)MPX_ID_STR_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)id[i];
        /* Printable ASCII without spaces: the field is raw bytes on the wire
         * and NUL is its terminator, so anything else would be a frame the
         * cloud mangles. */
        if (c < 0x21 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

bool mpx_id_valid_or_legacy(const char *id)
{
    return mpx_id_valid(id) || mpx_id_legacy_valid(id);
}

int mpx_id_generate(mpx_id_random_fn rng, char out[MPX_ID_STR_LEN + 1])
{
    uint8_t raw[MPX_ID_BODY_LEN];
    int check;

    if (rng == NULL || out == NULL) {
        return -1;
    }

    rng(raw, sizeof raw);

    memcpy(out, "MPX-", 4);
    for (size_t i = 0; i < (size_t)MPX_ID_BODY_LEN; i++) {
        out[MPX_ID_BODY_OFF + i] = mpx_id_alphabet[raw[i] & 0x1f];
    }

    /* A placeholder first: mpx_id_check_symbol() requires a 16-character
     * string, and it never reads the check position itself (that is the whole
     * point -- it computes what should be there). */
    out[MPX_ID_CHECK_OFF] = mpx_id_alphabet[0];
    out[MPX_ID_STR_LEN] = '\0';

    check = mpx_id_check_symbol(out);
    if (check < 0) {
        return -1;
    }
    out[MPX_ID_CHECK_OFF] = (char)check;
    return 0;
}

bool mpx_id_to_frame(uint8_t out[MPX_ID_STR_LEN], const char *id)
{
    size_t len;

    if (out == NULL || id == NULL) {
        return false;
    }
    len = strlen(id);
    if (len == 0 || len > (size_t)MPX_ID_STR_LEN) {
        return false;
    }

    /* Zero-filled then copied: a 10-character legacy ID and a 16-character
     * production ID both become the same 16 bytes the old
     * `memset` + `strncpy(..., 16)` produced, so nothing on the wire moves. */
    memset(out, 0, MPX_ID_STR_LEN);
    memcpy(out, id, len);
    return true;
}
