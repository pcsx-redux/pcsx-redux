-- lualoader, R"EOF(--

ffi.cdef [[

enum CompressionFieldId { COMPRESSION_F_NONE = 0, COMPRESSION_F_LEN, COMPRESSION_F_DIST, COMPRESSION_F_RUN, COMPRESSION_F_VAL, COMPRESSION_F_NFIELDS };
enum CompressionEmitKind { COMPRESSION_E_MATCH, COMPRESSION_E_FILL, COMPRESSION_E_END };
enum CompressionGuardOp { COMPRESSION_G_NONE = 0, COMPRESSION_G_EQ, COMPRESSION_G_NE };
enum CompressionOffsetBase { COMPRESSION_OB_RELATIVE = 0, COMPRESSION_OB_WINDOW_END, COMPRESSION_OB_ABSOLUTE };
enum CompressionParsePolicy { COMPRESSION_PP_GREEDY = 0, COMPRESSION_PP_LAZY = 1 };

typedef struct { int8_t byte; uint8_t mask; int8_t shift; } CompressionTerm;
typedef struct { CompressionTerm terms[3]; int nterms; int bias; } CompressionField;
typedef struct { CompressionField f; enum CompressionGuardOp op; int value; } CompressionGuard;

typedef struct {
    int nbytes;
    CompressionGuard guards[2];
    int nguards;
    enum CompressionEmitKind emit;
    CompressionField fields[5];
} CompressionProduction;

typedef struct {
    const char* name;
    int flag_unit_bytes;
    int flags_per_group;
    int flag_lsb_first;
    int flag_one_means_match;
    int flag_reverse_bits;
    enum CompressionOffsetBase offset_base;
    int window_start;
    int max_dist;
    int dist_bias;
    uint8_t fill_byte;
    int allow_overlap;
    CompressionProduction prods[4];
    int nprods;
    int ring_size;
    int emit_allow_negative;
    int emit_terminator;
} CompressionSchema;

typedef struct {
    long tokens_match, tokens_fill, tokens_literal;
    int dist_min, dist_max, len_min, len_max, run_min, run_max;
    long overlap_tokens;
    long negative_refs;
    long match_at_odd_off;
    long fill_at_odd_off;
    long lit_at_odd_off;
} CompressionDecodeStats;

void compressionStatsInit(CompressionDecodeStats* st);
int compressionDecode(const CompressionSchema* s, const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstcap,
                      size_t outlen, size_t* consumed, long* items, CompressionDecodeStats* st);
int compressionValidate(const CompressionSchema* s);
int compressionEncode(const CompressionSchema* s, const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstcap, int policy);

/* Layout witnesses. The CompressionSchema above is hand-transcribed from
   support/compression-schema.h, so a field added on one side and not the other
   would otherwise show up as garbage decodes rather than as an error. */
uint32_t compressionSizeof(int which);

]]

-- )EOF"
