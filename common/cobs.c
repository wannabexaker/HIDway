#include "cobs.h"

size_t hidway_cobs_encode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t rd = 0, wr = 0;
    size_t code_pos = wr++; /* reserve slot for the first code byte */
    uint8_t code = 1;

    while (rd < len) {
        uint8_t b = in[rd++];
        if (b != 0) {
            out[wr++] = b;
            code++;
            if (code != 0xFF)
                continue;
        }
        /* Close the current run: write its code, open a new one. */
        out[code_pos] = code;
        code_pos = wr++;
        code = 1;
    }
    out[code_pos] = code;
    return wr;
}

size_t hidway_cobs_decode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t rd = 0, wr = 0;

    while (rd < len) {
        uint8_t code = in[rd++];
        if (code == 0)
            return 0; /* 0x00 must not appear inside encoded data */
        for (uint8_t i = 1; i < code; i++) {
            if (rd >= len)
                return 0; /* run overruns the input */
            out[wr++] = in[rd++];
        }
        if (code != 0xFF && rd < len)
            out[wr++] = 0; /* implicit zero between runs (not after the last) */
    }
    return wr;
}
