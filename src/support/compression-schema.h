/*
 * schema: an IR for LZSS-family compressed stream formats.
 *
 * A plain mask/shift table cannot express that the variation between these
 * formats is in the GRAMMAR, not just the bit layout. So the IR is a list of
 * guarded productions, plus control-stream shape, plus window semantics, plus a
 * trailer rule. A table would have to hardcode or bolt on all four.
 */
#ifndef SCHEMA_H
#define SCHEMA_H

#include <stddef.h>
#include <stdint.h>

namespace PCSX {
namespace Compression {

enum FieldId { F_NONE = 0, F_LEN, F_DIST, F_RUN, F_VAL, F_NFIELDS };

/* A field is OR'd out of up to 3 (byte, mask, shift) terms. shift > 0 = left. */
struct Term {
    int8_t byte;
    uint8_t mask;
    int8_t shift;
};
struct Field {
    Term terms[3];
    int nterms;
    int bias; /* added after assembly */
};

/* E_END is a real production, not a trailer int. A format that terminates on,
   say, a zero-distance token expresses that as a guarded production, so the
   size of the trailer (one extra flag unit when the last group is full) is
   EMERGENT - the terminator consumes a flag bit like any other item. */
enum EmitKind { E_MATCH, E_FILL, E_END };

enum GuardOp { G_NONE = 0, G_EQ, G_NE };
/* A guard carries its OWN extractor rather than sharing the emit fields: a
   production may need to TEST one bit range (e.g. an escape nibble) while
   EMITTING a value computed from a different one, and one slot cannot be both. */
struct Guard {
    Field f;
    GuardOp op;
    int value;
}; /* tested on RAW (pre-bias) value */

struct Production {
    int nbytes; /* token bytes to read, INCLUDING the ones the guard reads */
    Guard guards[2];
    int nguards;
    EmitKind emit;
    Field fields[F_NFIELDS]; /* indexed by FieldId; nterms==0 means unused */
};

enum OffsetBase {
    OB_RELATIVE = 0, /* out_pos - dist */
    OB_WINDOW_END,   /* out_pos - maxdist - 1 + dist - wstart */
    OB_ABSOLUTE,     /* dist - wstart */
};

struct Schema {
    const char* name;

    /* control stream */
    int flag_unit_bytes;      /* 1, or 2 for 16-bit control units */
    int flags_per_group;      /* 8 */
    int flag_lsb_first;       /* 1 = consume bit 0 then shift right */
    int flag_one_means_match; /* 1 = a set bit selects a production, 0 = literal */
    int flag_reverse_bits;    /* bit-reverse each control byte before use */

    /* window */
    OffsetBase offset_base;
    int window_start;
    int max_dist;      /* window size; required for OB_WINDOW_END */
    int dist_bias;     /* added to every decoded distance */
    uint8_t fill_byte; /* what a reference below the start of output yields */
    int allow_overlap;

    /* grammar: first production whose guards all pass, wins */
    Production prods[4];
    int nprods;

    /* EMIT CONSTRAINTS. Deliberately separate from what the grammar can DECODE:
       a prober must accept everything the format can express, a packer must only
       emit what the target runtime is known to survive, which is not derivable
       from field widths. */
    int ring_size;           /* runtime window ring; caps dist at ring_size - max_len. 0 = no cap */
    int emit_allow_negative; /* may the encoder reference before the start of output? */

    int emit_terminator; /* encoder must append the E_END production */
};

/* Observed-token statistics. The point is to derive what the ORIGINAL encoder
   emitted, which is the best evidence of what the target's own decoder
   tolerates. This decoder is deliberately permissive and proves nothing. */
struct DecodeStats {
    long tokens_match, tokens_fill, tokens_literal;
    int dist_min, dist_max, len_min, len_max, run_min, run_max;
    long overlap_tokens;   /* dist < len : source overlaps destination */
    long negative_refs;    /* read before the start of output */
    long match_at_odd_off; /* token began at an odd offset in the stream */
    long fill_at_odd_off;
    long lit_at_odd_off;
};
void stats_init(DecodeStats* st);

/* Decode. Returns bytes written, or negative on error. */
enum {
    SCH_ERR_SRC_EOF = -3,
    SCH_ERR_DST_FULL = -4,
    SCH_ERR_NO_PROD = -6,
    SCH_ERR_BAD_REF = -8,     /* a match points at or past the output position */
    SCH_ERR_BAD_SCHEMA = -9,  /* see schema_validate */
    SCH_ERR_UNSUPPORTED = -10, /* the encoder only packs OB_RELATIVE windows */
};

/* Checks every value the codec uses as an index or a loop step: token bytes 1..4,
   term bytes inside their token, control unit 1 or 2, 1..8 flags per group, and a
   max_dist for OB_WINDOW_END. Returns 0 or SCH_ERR_BAD_SCHEMA. Decode and encode
   both run it first. */
int schema_validate(const Schema* s);
int schema_decode(const Schema* s, const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstcap, size_t outlen,
                  size_t* consumed, long* items_out, DecodeStats* st = 0);

/* Encode. Returns bytes written to dst, or negative. `policy` selects the parse. */
enum ParsePolicy { PP_GREEDY = 0, PP_LAZY = 1 };
enum { SCH_ERR_ENC_FULL = -7 };
int schema_encode(const Schema* s, const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstcap, int policy);

/* The decoder's own production selector, exposed because the encoder asks it
   which production a candidate token actually lands on rather than re-deriving
   the guard rules. Anything building a schema from outside wants it for the
   same reason. */
const Production* schema_select(const Schema* s, const uint8_t* tok, size_t avail);

}  // namespace Compression
}  // namespace PCSX

#endif
