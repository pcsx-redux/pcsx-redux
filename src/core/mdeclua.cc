/***************************************************************************
 *   Copyright (C) 2026 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

// PCSX.MDEC: BS frames to 24-bit RGB through the emulator's own MDEC decoder, and
// back through supportpsx's STR frame encoder. This lives in core rather than
// supportpsx because the decoder is GPL (see core/mdec-decoder.h).

#include "core/mdeclua.h"

#include <stdint.h>

#include <exception>
#include <new>
#include <string>
#include <vector>

#include "core/mdec-decoder.h"
#include "supportpsx/dct.h"
#include "supportpsx/str.h"

namespace {

// Null on success, or why the stream does not decode to a `width` x `height`
// frame. Both are multiples of 16; `rgb` takes width * height * 3 bytes, rows
// packed. Macroblocks are column-major, as retail STR and STR::encode lay them.
const char* mdecDecodeBS(const uint8_t* bs, uint32_t size, uint32_t width, uint32_t height, uint8_t* rgb) {
    if (width == 0 || height == 0 || (width % 16) || (height % 16)) return "width and height are multiples of 16";
    std::vector<uint16_t> rl;
    PCSX::DCT::ContainerResult r;
    try {
        r = PCSX::DCT::fromContainer({bs, size}, PCSX::DCT::Container::Bs, rl);
    } catch (const std::bad_alloc&) {
        return "out of memory";
    }
    if (r.failed) return r.error;
    if (r.blocks != (width / 16) * (height / 16) * 6) return "the stream's block count does not match the frame size";
    // As tools/mdec loads them: setStandard() leaves the prescaled quant tables alone.
    PCSX::MDECDecoder::Tables tables;
    tables.setQuant(PCSX::DCT::standardQuantTable(), PCSX::DCT::standardQuantTable());
    tables.setScale(PCSX::MDECDecoder::standardScaleTable());
    const auto frame =
        PCSX::MDECDecoder::decodeFrame(rl.data(), static_cast<unsigned>(rl.size()), tables, width, height, true, rgb);
    switch (frame.status.anomaly) {
        case PCSX::MDECDecoder::Anomaly::None:
            return nullptr;
        case PCSX::MDECDecoder::Anomaly::Exhausted:
            return "the stream ends before the frame does";
        case PCSX::MDECDecoder::Anomaly::RunPast63:
            return "a run goes past coefficient 63";
    }
    return "the decoder stopped early";
}

struct Encoded {
    std::vector<uint8_t> bs;
    std::string error;
    int qScale = 0;
};

// Codes `width` x `height` 24-bit RGB, `stride` bytes a row, at the lowest q_scale
// whose BS frame is at most `budget` bytes, 0 for no budget, and that a player
// holding STR::Settings' default rlHalfwords decodes. Always returns an object;
// its error is empty on success.
Encoded* mdecEncodeBS(const uint8_t* rgb, uint32_t width, uint32_t height, uint32_t stride, uint32_t budget) {
    auto encoded = new Encoded();
    if (width == 0 || height == 0 || stride < width * 3) {
        encoded->error = "the frame is empty or its stride is shorter than a row";
        return encoded;
    }
    // Exceptions can't cross the ffi boundary.
    try {
        const PCSX::STR::Settings settings;
        PCSX::STR::Planes planes;
        planes.fromRGB(rgb, width, height, stride);
        PCSX::DCT::Encoder encoder(1, settings.transform);
        auto result = PCSX::STR::encodeFrame(encoder, planes, settings, budget, encoded->bs);
        if (result.failed) {
            encoded->bs.clear();
            encoded->error = std::move(result.error);
        }
        encoded->qScale = result.qScale;
    } catch (const std::exception& e) {
        encoded->bs.clear();
        encoded->error = e.what();
    }
    return encoded;
}
void mdecEncodedDestroy(Encoded* encoded) { delete encoded; }
const char* mdecEncodedError(const Encoded* encoded) {
    return encoded->error.empty() ? nullptr : encoded->error.c_str();
}
const uint8_t* mdecEncodedData(const Encoded* encoded) { return encoded->bs.data(); }
uint32_t mdecEncodedSize(const Encoded* encoded) { return static_cast<uint32_t>(encoded->bs.size()); }
int mdecEncodedQScale(const Encoded* encoded) { return encoded->qScale; }

template <typename T, size_t S>
void registerSymbol(PCSX::Lua L, const char (&name)[S], const T ptr) {
    L.push<S>(name);
    L.push((void*)ptr);
    L.settable();
}

#define REGISTER(L, s) registerSymbol(L, #s, s)

void registerAllSymbols(PCSX::Lua L) {
    L.getfieldtable("_CLIBS", LUA_REGISTRYINDEX);
    L.push("CORE_MDEC");
    L.newtable();
    REGISTER(L, mdecDecodeBS);
    REGISTER(L, mdecEncodeBS);
    REGISTER(L, mdecEncodedDestroy);
    REGISTER(L, mdecEncodedError);
    REGISTER(L, mdecEncodedData);
    REGISTER(L, mdecEncodedSize);
    REGISTER(L, mdecEncodedQScale);
    L.settable();
    L.pop();
}

}  // namespace

void PCSX::LuaFFI::open_mdec(Lua L) {
    static int lualoader = 1;
    static const char* mdecFFI = (
#include "core/mdecffi.lua"
    );
    registerAllSymbols(L);
    L.load(mdecFFI, "src:core/mdecffi.lua");
}
