#include "support/compression-schema.h"

#include <string.h>

#include <vector>

namespace PCSX {
namespace Compression {

static inline unsigned sh(unsigned v, int s) { return s > 0 ? (v << s) : (v >> (-s)); }

static inline int field_raw(const Field* f, const uint8_t* tok) {
    int v = 0;
    for (int i = 0; i < f->nterms; i++)
        v |= (int)sh((unsigned)(tok[f->terms[i].byte] & f->terms[i].mask), f->terms[i].shift);
    return v;
}

/* THE decoder's production selector. The encoder calls this on its own candidate
   tokens rather than re-deriving the rule, so the two can never disagree about
   which encodings a guarded production has stolen from an unguarded one. */
const Production* schema_select(const Schema* s, const uint8_t* tok, size_t avail) {
    for (int pi = 0; pi < s->nprods; pi++) {
        const Production* C = &s->prods[pi];
        if ((size_t)C->nbytes > avail) continue;
        int pass = 1;
        for (int g = 0; g < C->nguards && pass; g++) {
            int raw = field_raw(&C->guards[g].f, tok);
            if (C->guards[g].op == G_EQ)
                pass = (raw == C->guards[g].value);
            else if (C->guards[g].op == G_NE)
                pass = (raw != C->guards[g].value);
        }
        if (pass) return C;
    }
    return 0;
}

static int field_ok(const Field* f, int nbytes) {
    if (f->nterms < 0 || f->nterms > 3) return 0;
    for (int i = 0; i < f->nterms; i++)
        if (f->terms[i].byte < 0 || f->terms[i].byte >= nbytes) return 0;
    return 1;
}

int schema_validate(const Schema* s) {
    if (s->flag_unit_bytes != 1 && s->flag_unit_bytes != 2) return SCH_ERR_BAD_SCHEMA;
    if (s->flags_per_group < 1 || s->flags_per_group > 8) return SCH_ERR_BAD_SCHEMA;
    if (s->nprods < 0 || s->nprods > 4) return SCH_ERR_BAD_SCHEMA;
    if (s->offset_base == OB_WINDOW_END && s->max_dist <= 0) return SCH_ERR_BAD_SCHEMA;
    for (int i = 0; i < s->nprods; i++) {
        const Production* P = &s->prods[i];
        if (P->nbytes < 1 || P->nbytes > 4) return SCH_ERR_BAD_SCHEMA;
        if (P->nguards < 0 || P->nguards > 2) return SCH_ERR_BAD_SCHEMA;
        for (int g = 0; g < P->nguards; g++)
            if (!field_ok(&P->guards[g].f, P->nbytes)) return SCH_ERR_BAD_SCHEMA;
        for (int f = 0; f < F_NFIELDS; f++)
            if (!field_ok(&P->fields[f], P->nbytes)) return SCH_ERR_BAD_SCHEMA;
    }
    return 0;
}

void stats_init(DecodeStats* st) {
    memset(st, 0, sizeof(*st));
    st->dist_min = st->len_min = st->run_min = 1 << 30;
    st->dist_max = st->len_max = st->run_max = -(1 << 30);
}
#define UPD(mn, mx, v)                  \
    do {                                \
        if ((v) < st->mn) st->mn = (v); \
        if ((v) > st->mx) st->mx = (v); \
    } while (0)

int schema_decode(const Schema* s, const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstcap, size_t outlen,
                  size_t* consumed, long* items_out, DecodeStats* st) {
    int bad = schema_validate(s);
    if (bad) return bad;
    size_t sp = 0, op = 0;
    long items = 0;

    /* A format with an E_END production runs until that token, with no
       destination bound of its own; one without runs to outlen. Model whichever
       the grammar declares; dstcap stays a safety cap so a missing terminator
       cannot run away in-process. */
    int has_end = 0;
    for (int i = 0; i < s->nprods; i++)
        if (s->prods[i].emit == E_END) has_end = 1;
    size_t limit = has_end ? dstcap : outlen;

    while (op < limit) {
        /* fetch a flag group */
        if (sp + (size_t)s->flag_unit_bytes > srclen) return SCH_ERR_SRC_EOF;
        unsigned flags = src[sp];
        sp += s->flag_unit_bytes; /* the 2nd byte of a 16-bit unit is decorative */
        if (s->flag_reverse_bits) {
            flags = ((flags >> 1) & 0x55) | ((flags << 1) & 0xaa);
            flags = ((flags >> 2) & 0x33) | ((flags << 2) & 0xcc);
            flags = ((flags >> 4) & 0x0f) | ((flags << 4) & 0xf0);
        }

        for (int k = 0; k < s->flags_per_group && op < limit; k++) {
            int bit = s->flag_lsb_first ? (flags & 1) : ((flags >> 7) & 1);
            if (s->flag_lsb_first)
                flags >>= 1;
            else
                flags = (flags << 1) & 0xff;
            items++;

            int is_match = s->flag_one_means_match ? bit : !bit;
            if (!is_match) {
                if (st) {
                    st->tokens_literal++;
                    if (sp & 1) st->lit_at_odd_off++;
                }
                for (int n = 0; n < s->flag_unit_bytes; n++) {
                    if (sp >= srclen) return SCH_ERR_SRC_EOF;
                    /* the encoder pads a short tail out to a whole unit */
                    if (!has_end && op >= outlen) {
                        sp++;
                        continue;
                    }
                    if (op >= dstcap) return SCH_ERR_DST_FULL;
                    dst[op++] = src[sp++];
                }
                continue;
            }

            /* pick the production: first whose guards all pass */
            uint8_t tok[4] = {0, 0, 0, 0};
            size_t avail = srclen - sp;
            if (avail > 4) avail = 4;
            memcpy(tok, src + sp, avail);
            const Production* P = schema_select(s, tok, avail);
            if (!P) {
                /* a production skipped only for want of bytes means the input
                   was cut short, not that the token is malformed */
                for (int pi = 0; pi < s->nprods; pi++)
                    if ((size_t)s->prods[pi].nbytes > avail) return SCH_ERR_SRC_EOF;
                return SCH_ERR_NO_PROD;
            }
            memcpy(tok, src + sp, (size_t)P->nbytes);
            size_t tok_off = sp;
            sp += P->nbytes;

            if (P->emit == E_END) {
                if (consumed) *consumed = sp;
                if (items_out) *items_out = items;
                return (int)op;
            }
            if (P->emit == E_FILL) {
                if (st) {
                    st->tokens_fill++;
                    if (tok_off & 1) st->fill_at_odd_off++;
                }
                int run = field_raw(&P->fields[F_RUN], tok) + P->fields[F_RUN].bias;
                uint8_t v = (uint8_t)(field_raw(&P->fields[F_VAL], tok) + P->fields[F_VAL].bias);
                if (st) UPD(run_min, run_max, run);
                for (int n = 0; n < run; n++) {
                    if (op >= dstcap) return SCH_ERR_DST_FULL;
                    dst[op++] = v;
                }
            } else {
                int len = field_raw(&P->fields[F_LEN], tok) + P->fields[F_LEN].bias;
                int dist = field_raw(&P->fields[F_DIST], tok) + P->fields[F_DIST].bias + s->dist_bias;
                long from;
                switch (s->offset_base) {
                    case OB_WINDOW_END:
                        from = (long)op - s->max_dist - 1 + dist - s->window_start;
                        break;
                    case OB_ABSOLUTE:
                        from = dist - s->window_start;
                        break;
                    default:
                        from = (long)op - dist;
                        break;
                }
                if (st) {
                    st->tokens_match++;
                    if (tok_off & 1) st->match_at_odd_off++;
                    UPD(dist_min, dist_max, dist);
                    UPD(len_min, len_max, len);
                    if (dist < len) st->overlap_tokens++;
                    if (from < 0) st->negative_refs++;
                }
                if (from >= (long)op) return SCH_ERR_BAD_REF;
                /* without a terminator, a match stops mid-token on reaching the
                   target length; a fill does not. */
                for (int n = 0; n < len; n++, from++) {
                    if (op >= dstcap) return SCH_ERR_DST_FULL;
                    dst[op] = (from < 0) ? s->fill_byte : dst[from];
                    op++;
                    if (!has_end && op >= outlen) break;
                }
            }
        }
    }

    if (consumed) *consumed = sp;
    if (items_out) *items_out = items;
    return (int)op;
}

/* ---- encoder ------------------------------------------------------------ */

static inline void field_pack(const Field* f, int value, uint8_t* tok) {
    for (int i = 0; i < f->nterms; i++)
        tok[f->terms[i].byte] |= (uint8_t)(sh((unsigned)value, -f->terms[i].shift) & f->terms[i].mask);
}

/* capability probe: what does this schema's MATCH production support? */
struct MatchCaps {
    int min_len, max_len, min_dist, max_dist, nbytes;
    const Production* p;
};
static void match_caps(const Schema* s, MatchCaps* c) {
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < s->nprods; i++) {
        const Production* P = &s->prods[i];
        if (P->emit != E_MATCH || P->nguards != 0) continue; /* the unguarded match arm */
        const Field* L = &P->fields[F_LEN];
        const Field* D = &P->fields[F_DIST];
        int lmax = 0, dmax = 0;
        for (int t = 0; t < L->nterms; t++) lmax |= (int)sh(L->terms[t].mask, L->terms[t].shift);
        for (int t = 0; t < D->nterms; t++) dmax |= (int)sh(D->terms[t].mask, D->terms[t].shift);
        /* a length below 1 would not advance the encoder, and a distance below the
           field's bias would pack a negative raw value */
        c->min_len = L->bias > 1 ? L->bias : 1;
        c->max_len = lmax + L->bias;
        c->min_dist = D->bias + s->dist_bias > 1 ? D->bias + s->dist_bias : 1;
        c->max_dist = dmax + D->bias + s->dist_bias;
        c->nbytes = P->nbytes;
        c->p = P;
        return;
    }
}

/* The same probe for every FILL production the schema declares. A schema may
   declare several (e.g. a short and a long form) and nothing in the IR says
   which one to prefer: that falls out of coverage-per-emitted-byte, so a schema
   can grow another fill form without the encoder learning about it. */
struct FillCaps {
    const Production* p;
    int min_run, max_run, nbytes;
};
static int fill_caps(const Schema* s, FillCaps* out, int maxout) {
    int n = 0;
    for (int i = 0; i < s->nprods && n < maxout; i++) {
        const Production* P = &s->prods[i];
        if (P->emit != E_FILL) continue;
        const Field* R = &P->fields[F_RUN];
        int rmax = 0;
        for (int t = 0; t < R->nterms; t++) rmax |= (int)sh(R->terms[t].mask, R->terms[t].shift);
        out[n].p = P;
        out[n].nbytes = P->nbytes;
        out[n].min_run = R->bias;
        out[n].max_run = rmax + R->bias;
        n++;
    }
    return n;
}

/* Build a fill token: the guards' fixed values first (that is what puts the
   escape nibble in place), then the payload. Then ask the DECODER'S selector
   whether the result actually lands on P. A guarded production that shadows
   part of the match arm can also shadow part of another fill's encoding space,
   and asking is how neither rule gets written down a second time. Pass the full
   4-byte window the decoder uses, not nbytes - a short token judged with
   avail==2 hides every longer production that would have won. */
static int pack_fill(const Schema* s, const FillCaps* fc, int run, uint8_t v, uint8_t* tok) {
    memset(tok, 0, 4);
    for (int g = 0; g < fc->p->nguards; g++)
        if (fc->p->guards[g].op == G_EQ) field_pack(&fc->p->guards[g].f, fc->p->guards[g].value, tok);
    field_pack(&fc->p->fields[F_RUN], run - fc->p->fields[F_RUN].bias, tok);
    field_pack(&fc->p->fields[F_VAL], (int)v - fc->p->fields[F_VAL].bias, tok);
    return schema_select(s, tok, 4) == fc->p;
}

/* a > b as coverage per emitted byte, ties broken on raw coverage */
static inline int denser(int cov_a, int cost_a, int cov_b, int cost_b) {
    long l = (long)cov_a * cost_b, r = (long)cov_b * cost_a;
    return l != r ? (l > r) : (cov_a > cov_b);
}

#define HBITS 15
#define HSIZE (1 << HBITS)
static inline uint32_t hash3(const uint8_t* p) { return (uint32_t)(((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & (HSIZE - 1)); }

int schema_encode(const Schema* s, const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstcap, int policy) {
    int bad = schema_validate(s);
    if (bad) return bad;
    if (s->offset_base != OB_RELATIVE) return SCH_ERR_UNSUPPORTED;
    MatchCaps mc;
    match_caps(s, &mc);
    if (!mc.p) return SCH_ERR_NO_PROD;
    /* the ring bound: a match may not reach further back than ring - max_len,
       or the runtime's circular window overwrites the bytes it is copying. */
    if (s->ring_size) {
        int cap = s->ring_size - mc.max_len;
        if (cap < mc.max_dist) mc.max_dist = cap;
    }

    FillCaps fc[4];
    int nfc = fill_caps(s, fc, 4);

    std::vector<int32_t> head(HSIZE, -1), prev(srclen ? srclen : 1, -1);

    size_t op = 0, pos = 0;
    size_t flagpos = 0;
    int inGroup = 0;
    unsigned flags = 0;

    /* Allocate a flag byte LAZILY, when a group's first item is emitted. The
       eager version had to guess whether another item was coming, and guessed
       wrong exactly when the stream ended on a group boundary and the terminator
       still needed a bit. */
#define ENSURE_FLAG()                                                      \
    do {                                                                   \
        if (inGroup == 0) {                                                \
            if (op + s->flag_unit_bytes > dstcap) return SCH_ERR_ENC_FULL; \
            flagpos = op;                                                  \
            op += s->flag_unit_bytes;                                      \
        }                                                                  \
    } while (0)
#define FLAGBIT (s->flag_lsb_first ? (1u << inGroup) : (0x80u >> inGroup))
#define FLUSH_FLAG()                                       \
    do {                                                   \
        unsigned f_ = flags;                               \
        if (s->flag_reverse_bits) {                        \
            f_ = ((f_ >> 1) & 0x55) | ((f_ << 1) & 0xaa);  \
            f_ = ((f_ >> 2) & 0x33) | ((f_ << 2) & 0xcc);  \
            f_ = ((f_ >> 4) & 0x0f) | ((f_ << 4) & 0xf0);  \
        }                                                  \
        dst[flagpos] = (uint8_t)f_;                        \
        if (s->flag_unit_bytes == 2) dst[flagpos + 1] = 0; \
        flags = 0;                                         \
        inGroup = 0;                                       \
    } while (0)

    while (pos < srclen) {
        ENSURE_FLAG();
        /* --- find the longest match in range --- */
        int best_len = 0;
        size_t best_dist = 0;
        if (pos + 2 < srclen) {
            uint32_t h = hash3(src + pos);
            int cand = head[h];
            int guard = 4096;
            while (cand >= 0 && guard--) {
                size_t d = pos - (size_t)cand;
                if (d > (size_t)mc.max_dist) break;
                if (d < (size_t)mc.min_dist) {
                    cand = prev[cand];
                    continue;
                }
                int l = 0, maxl = (int)(srclen - pos);
                if (maxl > mc.max_len) maxl = mc.max_len;
                if (!s->allow_overlap && maxl > (int)d) maxl = (int)d;
                while (l < maxl && src[cand + l] == src[pos + l]) l++;
                if (l > best_len) {
                    best_len = l;
                    best_dist = d;
                    if (l >= mc.max_len) break;
                }
                cand = prev[cand];
            }
        }

        /* --- find the best fill over the run of identical bytes starting here.
           A fill carries its own value, so it needs no history: it is available
           at position 0, and it is the only item that can cover a run longer
           than the match arm's ceiling. Which fill form wins is decided by
           density, not by a per-format rule. --- */
        int fill_cover = 0, fill_cost = 0;
        uint8_t fill_tok[4] = {0, 0, 0, 0};
        if (nfc) {
            size_t runlen = 1;
            while (pos + runlen < srclen && src[pos + runlen] == src[pos]) runlen++;
            for (int i = 0; i < nfc; i++) {
                if ((int)runlen < fc[i].min_run) continue;
                int want = (int)runlen;
                if (want > fc[i].max_run) want = fc[i].max_run;
                uint8_t t[4];
                while (want >= fc[i].min_run && !pack_fill(s, &fc[i], want, src[pos], t)) want--;
                if (want < fc[i].min_run) continue;
                if (!fill_cover || denser(want, fc[i].nbytes, fill_cover, fill_cost)) {
                    fill_cover = want;
                    fill_cost = fc[i].nbytes;
                    memcpy(fill_tok, t, 4);
                }
            }
        }

        int emit_match = (best_len >= mc.min_len);
        int emit_fill = fill_cover && (!emit_match || denser(fill_cover, fill_cost, best_len, mc.nbytes));

        if (emit_match && !emit_fill && policy == PP_LAZY && pos + 1 < srclen && best_len < mc.max_len) {
            /* peek: is the next position strictly better? then emit a literal here. */
            int nb = 0;
            if (pos + 3 < srclen) {
                uint32_t h2 = hash3(src + pos + 1);
                int c2 = head[h2];
                int g2 = 512;
                while (c2 >= 0 && g2--) {
                    size_t d2 = (pos + 1) - (size_t)c2;
                    if (d2 > (size_t)mc.max_dist) break;
                    if (d2 < (size_t)mc.min_dist) {
                        c2 = prev[c2];
                        continue;
                    }
                    int l2 = 0, m2 = (int)(srclen - pos - 1);
                    if (m2 > mc.max_len) m2 = mc.max_len;
                    if (!s->allow_overlap && m2 > (int)d2) m2 = (int)d2;
                    while (l2 < m2 && src[c2 + l2] == src[pos + 1 + l2]) l2++;
                    if (l2 > nb) nb = l2;
                    c2 = prev[c2];
                }
            }
            if (nb > best_len) emit_match = 0;
        }

        int consumed = 0, done = 0;
        if (emit_fill) {
            if (op + fill_cost > dstcap) return SCH_ERR_ENC_FULL;
            memcpy(dst + op, fill_tok, (size_t)fill_cost);
            op += fill_cost;
            if (s->flag_one_means_match) flags |= FLAGBIT;
            consumed = fill_cover;
            done = 1;
        } else if (emit_match) {
            uint8_t tok[4] = {0, 0, 0, 0};
            /* A guarded production can shadow part of the match arm's encoding
               space (e.g. a length value reserved as a fill escape makes that
               length inexpressible as a match). Shorten until the token really
               does select the match arm, asking the decoder's own selector. */
            while (best_len >= mc.min_len) {
                memset(tok, 0, sizeof(tok));
                field_pack(&mc.p->fields[F_LEN], best_len - mc.p->fields[F_LEN].bias, tok);
                field_pack(&mc.p->fields[F_DIST], (int)best_dist - mc.p->fields[F_DIST].bias - s->dist_bias, tok);
                if (schema_select(s, tok, 4) == mc.p) break;
                best_len--;
            }
            if (best_len >= mc.min_len) {
                if (op + mc.nbytes > dstcap) return SCH_ERR_ENC_FULL;
                memcpy(dst + op, tok, mc.nbytes);
                op += mc.nbytes;
                if (s->flag_one_means_match) flags |= FLAGBIT;
                consumed = best_len;
                done = 1;
            }
        }
        if (!done) {
            /* the decoder copies one control unit per literal; a short tail is padded */
            if (op + s->flag_unit_bytes > dstcap) return SCH_ERR_ENC_FULL;
            consumed = 0;
            for (int n = 0; n < s->flag_unit_bytes; n++) {
                if (pos + n < srclen) {
                    dst[op++] = src[pos + n];
                    consumed++;
                } else {
                    dst[op++] = 0;
                }
            }
            if (s->flag_one_means_match == 0) flags |= FLAGBIT;
        }

        /* index every position we just covered */
        for (int n = 0; n < consumed; n++) {
            size_t q = pos + n;
            if (q + 2 < srclen) {
                uint32_t h = hash3(src + q);
                prev[q] = head[h];
                head[h] = (int32_t)q;
            }
        }
        pos += consumed;
        inGroup++;

        if (inGroup == s->flags_per_group) FLUSH_FLAG();
    }

    /* terminator, emitted as an ordinary item so it takes a flag bit and, if the
       group was exactly full, forces a fresh flag byte. A variable-size
       trailer falls out of that; it is not a special case. */
    if (s->emit_terminator) {
        const Production* T = 0;
        for (int i = 0; i < s->nprods; i++)
            if (s->prods[i].emit == E_END) {
                T = &s->prods[i];
                break;
            }
        if (T) {
            ENSURE_FLAG();
            uint8_t tok[4] = {0, 0, 0, 0};
            for (int g = 0; g < T->nguards; g++)
                if (T->guards[g].op == G_EQ) field_pack(&T->guards[g].f, T->guards[g].value, tok);
            if (op + T->nbytes > dstcap) return SCH_ERR_ENC_FULL;
            memcpy(dst + op, tok, T->nbytes);
            op += T->nbytes;
            if (s->flag_one_means_match) flags |= (s->flag_lsb_first ? (1u << inGroup) : (0x80u >> inGroup));
            inGroup++;
        }
    }

    if (inGroup) FLUSH_FLAG();
#undef ENSURE_FLAG
#undef FLUSH_FLAG
#undef FLAGBIT

    return (int)op;
}

}  // namespace Compression
}  // namespace PCSX
