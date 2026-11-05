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

#include "supportpsx/sf2.h"

#include <string.h>

#include <algorithm>

namespace {

// Generator operators, SoundFont 2.04 section 8.1.2.
constexpr uint16_t c_genInstrument = 41;
constexpr uint16_t c_genSampleID = 53;
constexpr uint16_t c_genSampleModes = 54;
// Zero points the specification requires after each sample.
constexpr uint32_t c_samplePad = 46;
// Points the specification requires between the loop end and the sample end.
constexpr uint32_t c_loopTail = 8;

class Buffer {
  public:
    void u8(uint8_t v) { m_data.push_back(v); }
    void u16(uint16_t v) {
        u8(v & 0xff);
        u8(v >> 8);
    }
    void u32(uint32_t v) {
        u16(v & 0xffff);
        u16(v >> 16);
    }
    void fourcc(const char* id) { bytes(id, 4); }
    void bytes(const void* data, size_t size) {
        auto p = reinterpret_cast<const uint8_t*>(data);
        m_data.insert(m_data.end(), p, p + size);
    }
    void name(const std::string& str) {
        char buf[20] = {};
        memcpy(buf, str.data(), std::min<size_t>(str.size(), 19));
        bytes(buf, 20);
    }
    void chunk(const char* id, const Buffer& body) {
        fourcc(id);
        u32(uint32_t(body.m_data.size()));
        bytes(body.m_data.data(), body.m_data.size());
        if (body.m_data.size() & 1) u8(0);
    }
    void list(const char* type, const Buffer& body) {
        fourcc("LIST");
        u32(uint32_t(body.m_data.size() + 4));
        fourcc(type);
        bytes(body.m_data.data(), body.m_data.size());
    }
    std::vector<uint8_t> m_data;
};

Buffer zstr(const std::string& str) {
    Buffer b;
    b.bytes(str.data(), str.size());
    b.u8(0);
    if (b.m_data.size() & 1) b.u8(0);
    return b;
}

Buffer infoList(const std::string& bankName, const std::string& comment) {
    Buffer info;
    Buffer ifil;
    ifil.u16(2);
    ifil.u16(1);
    info.chunk("ifil", ifil);
    info.chunk("isng", zstr("EMU8000"));
    info.chunk("INAM", zstr(bankName));
    if (!comment.empty()) info.chunk("ICMT", zstr(comment));
    info.chunk("ISFT", zstr("PCSX-Redux"));
    return info;
}

// Appends a sample's points to smpl and returns how many points past the end of its data were
// added. A loop running to the very end of the data gets the samples that follow its end when
// played, which are the first ones of the loop.
uint32_t appendPoints(Buffer& smpl, const PCSX::SF2::Sample& s) {
    for (auto v : s.pcm) smpl.u16(uint16_t(v));
    uint32_t tail = 0;
    if (s.loops && s.loopEnd > s.loopStart && s.loopEnd + c_loopTail > s.pcm.size()) {
        tail = s.loopEnd + c_loopTail - uint32_t(s.pcm.size());
        const uint32_t length = s.loopEnd - s.loopStart;
        const uint32_t phase = uint32_t(s.pcm.size()) - s.loopEnd;
        for (uint32_t k = 0; k < tail; k++) smpl.u16(uint16_t(s.pcm[s.loopStart + (phase + k) % length]));
    }
    for (uint32_t p = 0; p < c_samplePad; p++) smpl.u16(0);
    return tail;
}

void presetHeader(Buffer& phdr, const std::string& name, uint16_t preset, uint16_t bank, uint16_t bagIndex) {
    phdr.name(name);
    phdr.u16(preset);
    phdr.u16(bank);
    phdr.u16(bagIndex);
    phdr.u32(0);
    phdr.u32(0);
    phdr.u32(0);
}

void bag(Buffer& buffer, uint16_t genIndex) {
    buffer.u16(genIndex);
    buffer.u16(0);
}

// One preset, one instrument and one sample per input sample, all sharing its index.
class Hydra {
  public:
    void add(const PCSX::SF2::Sample& s, uint16_t i, uint32_t size) {
        presetHeader(phdr, s.name, i % 128, i / 128, i);
        bag(pbag, m_pgenCount);
        pgen.u16(c_genInstrument);
        pgen.u16(i);
        m_pgenCount++;

        inst.name(s.name);
        inst.u16(i);
        bag(ibag, m_igenCount);
        if (s.loops) {
            igen.u16(c_genSampleModes);
            igen.u16(1);
            m_igenCount++;
        }
        // sampleID has to be the last generator of an instrument zone.
        igen.u16(c_genSampleID);
        igen.u16(i);
        m_igenCount++;

        shdr.name(s.name);
        shdr.u32(m_offset);
        shdr.u32(m_offset + size);
        shdr.u32(m_offset + (s.loops ? s.loopStart : 0));
        shdr.u32(m_offset + (s.loops ? s.loopEnd : uint32_t(s.pcm.size())));
        shdr.u32(s.sampleRate);
        shdr.u8(s.rootKey);
        shdr.u8(uint8_t(s.correction));
        shdr.u16(0);
        shdr.u16(1);  // mono
        m_offset += size + c_samplePad;
    }
    void terminate(uint16_t count) {
        presetHeader(phdr, "EOP", 0, 0, count);
        bag(pbag, m_pgenCount);
        for (unsigned i = 0; i < 10; i++) pmod.u8(0);
        pgen.u32(0);
        inst.name("EOI");
        inst.u16(count);
        bag(ibag, m_igenCount);
        for (unsigned i = 0; i < 10; i++) imod.u8(0);
        igen.u32(0);
        shdr.name("EOS");
        for (unsigned i = 0; i < 26; i++) shdr.u8(0);
    }
    Buffer list() const {
        Buffer pdta;
        pdta.chunk("phdr", phdr);
        pdta.chunk("pbag", pbag);
        pdta.chunk("pmod", pmod);
        pdta.chunk("pgen", pgen);
        pdta.chunk("inst", inst);
        pdta.chunk("ibag", ibag);
        pdta.chunk("imod", imod);
        pdta.chunk("igen", igen);
        pdta.chunk("shdr", shdr);
        return pdta;
    }

  private:
    Buffer phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
    uint32_t m_offset = 0;
    uint16_t m_pgenCount = 0;
    uint16_t m_igenCount = 0;
};

}  // namespace

std::vector<uint8_t> PCSX::SF2::write(const std::vector<Sample>& samples, const std::string& bankName,
                                      const std::string& comment) {
    Buffer smpl;
    Hydra hydra;
    for (uint16_t i = 0; i < samples.size(); i++) {
        const uint32_t tail = appendPoints(smpl, samples[i]);
        hydra.add(samples[i], i, uint32_t(samples[i].pcm.size()) + tail);
    }
    hydra.terminate(uint16_t(samples.size()));

    Buffer sdta;
    sdta.chunk("smpl", smpl);

    Buffer body;
    body.fourcc("sfbk");
    body.list("INFO", infoList(bankName, comment));
    body.list("sdta", sdta);
    body.list("pdta", hydra.list());

    Buffer riff;
    riff.fourcc("RIFF");
    riff.u32(uint32_t(body.m_data.size()));
    riff.bytes(body.m_data.data(), body.m_data.size());
    return std::move(riff.m_data);
}

namespace {

struct Chunk {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

uint16_t rd16(const uint8_t* p) { return p[0] | p[1] << 8; }
uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

// Finds the chunk `id` in a run of chunks; when `listType` is set, the LIST of that type.
Chunk find(const uint8_t* data, uint32_t size, const char* id, const char* listType = nullptr) {
    uint32_t pos = 0;
    while (pos + 8 <= size) {
        const uint32_t len = rd32(data + pos + 4);
        if (len > size - pos - 8) break;
        const uint8_t* body = data + pos + 8;
        if (listType) {
            if (memcmp(data + pos, "LIST", 4) == 0 && len >= 4 && memcmp(body, listType, 4) == 0) {
                return {body + 4, len - 4};
            }
        } else if (memcmp(data + pos, id, 4) == 0) {
            return {body, len};
        }
        pos += 8 + len + (len & 1);
    }
    return {};
}

}  // namespace

std::vector<PCSX::SF2::Sample> PCSX::SF2::read(const uint8_t* data, size_t size) {
    std::vector<Sample> ret;
    if (size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "sfbk", 4) != 0) return ret;
    const uint32_t riffSize = std::min<uint32_t>(rd32(data + 4), uint32_t(size - 8));
    if (riffSize < 4) return ret;
    const uint8_t* body = data + 12;
    const uint32_t bodySize = riffSize - 4;
    const Chunk sdta = find(body, bodySize, nullptr, "sdta");
    const Chunk pdta = find(body, bodySize, nullptr, "pdta");
    if (!sdta.data || !pdta.data) return ret;
    const Chunk smpl = find(sdta.data, sdta.size, "smpl");
    const Chunk shdr = find(pdta.data, pdta.size, "shdr");
    const Chunk ibag = find(pdta.data, pdta.size, "ibag");
    const Chunk igen = find(pdta.data, pdta.size, "igen");
    if (!smpl.data || !shdr.data || shdr.size < 46) return ret;
    const uint32_t points = smpl.size / 2;

    // The last record is the terminal one.
    const unsigned count = shdr.size / 46 - 1;
    std::vector<bool> loops(count, false);
    if (ibag.data && igen.data) {
        // The generators of one zone run from its ibag index to the next one's. Every zone
        // that names a sample decides whether that sample loops.
        const unsigned bags = ibag.size / 4;
        const unsigned gens = igen.size / 4;
        for (unsigned b = 0; b + 1 < bags; b++) {
            const unsigned first = rd16(ibag.data + b * 4);
            const unsigned last = std::min(unsigned(rd16(ibag.data + (b + 1) * 4)), gens);
            int mode = 0;
            int sampleID = -1;
            for (unsigned g = first; g < last; g++) {
                const uint16_t oper = rd16(igen.data + g * 4);
                const uint16_t amount = rd16(igen.data + g * 4 + 2);
                if (oper == c_genSampleModes) mode = amount & 3;
                if (oper == c_genSampleID) sampleID = amount;
            }
            if (sampleID >= 0 && unsigned(sampleID) < count && (mode == 1 || mode == 3)) loops[sampleID] = true;
        }
    }

    for (unsigned i = 0; i < count; i++) {
        const uint8_t* h = shdr.data + i * 46;
        const uint32_t start = rd32(h + 20);
        const uint32_t end = rd32(h + 24);
        const uint32_t startLoop = rd32(h + 28);
        const uint32_t endLoop = rd32(h + 32);
        if (start > end || end > points) continue;
        Sample& s = ret.emplace_back();
        s.name.assign(reinterpret_cast<const char*>(h), strnlen(reinterpret_cast<const char*>(h), 20));
        s.pcm.reserve(end - start);
        for (uint32_t p = start; p < end; p++) s.pcm.push_back(int16_t(rd16(smpl.data + p * 2)));
        s.sampleRate = rd32(h + 36);
        s.rootKey = h[40] > 127 ? 60 : h[40];
        s.correction = int8_t(h[41]);
        if (loops[i] && startLoop >= start && startLoop < endLoop && endLoop <= end) {
            s.loops = true;
            s.loopStart = startLoop - start;
            s.loopEnd = endLoop - start;
        }
    }
    return ret;
}
