/*
 * Lua binding for the schema-driven compression engine.
 *
 * The Schema is a plain struct with no pointers into C++ lifetimes except its
 * name, so Lua builds one with ffi.new and hands it straight back. That makes a
 * "compiled" schema literally a piece of cdata rather than a handle into a
 * registry, and decoding is an FFI call with the loop on the Lua side - the
 * direction that is both traced and safe (an FFI-called C function that
 * re-enters the VM segfaults once the calling loop is hot).
 */

#include "lua/compressionffi.h"

#include <stdint.h>

#include "support/compression-schema.h"

using namespace PCSX::Compression;

namespace {

void compressionStatsInit(DecodeStats* st) { stats_init(st); }

int compressionDecode(const Schema* s, const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstcap, size_t outlen,
                      size_t* consumed, long* items, DecodeStats* st) {
    return schema_decode(s, src, srclen, dst, dstcap, outlen, consumed, items, st);
}

int compressionValidate(const Schema* s) { return schema_validate(s); }

int compressionEncode(const Schema* s, const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstcap, int policy) {
    return schema_encode(s, src, srclen, dst, dstcap, policy);
}

/* Keep in step with the `SIZEOF` table in compressionffi.lua. The Lua side
   checks every one of these against ffi.sizeof at load time and refuses to
   publish anything if they disagree, because a struct that drifts between the
   header and the cdef produces plausible-looking wrong output rather than an
   error. */
uint32_t compressionSizeof(int which) {
    switch (which) {
        case 0:
            return sizeof(Term);
        case 1:
            return sizeof(Field);
        case 2:
            return sizeof(Guard);
        case 3:
            return sizeof(Production);
        case 4:
            return sizeof(Schema);
        case 5:
            return sizeof(DecodeStats);
        default:
            return 0;
    }
}

template <typename T, size_t S>
void registerSymbol(PCSX::Lua L, const char (&name)[S], const T ptr) {
    L.push<S>(name);
    L.push((void*)ptr);
    L.settable();
}

#define REGISTER(L, s) registerSymbol(L, #s, s)

void registerAllSymbols(PCSX::Lua L) {
    L.getfieldtable("_CLIBS", LUA_REGISTRYINDEX);
    L.push("SUPPORT_COMPRESSION");
    L.newtable();
    REGISTER(L, compressionStatsInit);
    REGISTER(L, compressionDecode);
    REGISTER(L, compressionEncode);
    REGISTER(L, compressionValidate);
    REGISTER(L, compressionSizeof);
    L.settable();
    L.pop();
}

}  // namespace

void PCSX::LuaFFI::open_compression(Lua L) {
    static int lualoader = 2;
    static const char* compressionFFICDef = (
#include "lua/compressionffi-cdef.lua"
    );
    static const char* compressionFFI = (
#include "lua/compressionffi.lua"
    );
    registerAllSymbols(L);
    L.load(compressionFFICDef, "src:lua/compressionffi-cdef.lua");
    L.load(compressionFFI, "src:lua/compressionffi.lua");
}
