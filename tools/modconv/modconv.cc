/***************************************************************************
 *   Copyright (C) 2024 PCSX-Redux authors                                 *
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

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "args/args.hxx"
#include "fmt/format.h"
#include "support/binstruct.h"
#include "support/file.h"
#include "support/tool-args.h"
#include "support/typestring-wrapper.h"
#include "supportpsx/adpcm.h"
#include "tools/modconv/layout.h"
#include "tools/modconv/stretch.h"

typedef PCSX::BinStruct::Field<PCSX::BinStruct::CString<20>, TYPESTRING("Title")> ModTitle;

typedef PCSX::BinStruct::Field<PCSX::BinStruct::CString<22>, TYPESTRING("Name")> SampleName;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::BEUInt16, TYPESTRING("Length")> SampleLength;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt8, TYPESTRING("FineTune")> SampleFineTune;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt8, TYPESTRING("Volume")> SampleVolume;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::BEUInt16, TYPESTRING("LoopStart")> SampleLoopStart;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::BEUInt16, TYPESTRING("LoopLength")> SampleLoopLength;

typedef PCSX::BinStruct::Struct<TYPESTRING("ModSample"), SampleName, SampleLength, SampleFineTune, SampleVolume,
                                SampleLoopStart, SampleLoopLength>
    ModSample;
typedef PCSX::BinStruct::RepeatedStruct<ModSample, TYPESTRING("ModSamples"), 31> ModSamples;

typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt8, TYPESTRING("Positions")> Positions;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt8, TYPESTRING("RestartPosition")> RestartPosition;
typedef PCSX::BinStruct::RepeatedField<PCSX::BinStruct::UInt8, TYPESTRING("PatternTable"), 128> PatternTable;

typedef PCSX::BinStruct::Field<PCSX::BinStruct::CString<4>, TYPESTRING("Signature")> Signature;

typedef PCSX::BinStruct::Struct<TYPESTRING("ModFile"), ModTitle, ModSamples, Positions, RestartPosition, PatternTable,
                                Signature>
    ModFile;

int main(int argc, char** argv) {
    args::ArgumentParser parser("");
    args::ValueFlag<std::string> outputFlag(parser, "output", "", {"o"});
    args::ValueFlag<std::string> samplesFlag(parser, "samples", "", {"s"});
    args::ValueFlag<unsigned> amplificationFlag(parser, "amplification", "", {"a"});
    args::ValueFlag<std::string> stretchFlag(parser, "stretch", "", {"stretch"});
    args::Flag helpFlag(parser, "help", "", {"h"});
    args::PositionalList<std::string> inputsList(parser, "inputs", "");
    PCSX::ToolArgs::parse(parser, argc, argv);
    const auto output = PCSX::ToolArgs::get(outputFlag);

    fmt::print(R"(
modconv by Nicolas "Pixel" Noble
https://github.com/pcsx-redux/pcsx-redux/tree/main/tools/modconv/

)");

    const auto inputs = args::get(inputsList);
    const bool asksForHelp = args::get(helpFlag);
    const bool hasOutput = output.has_value();
    const bool oneInput = inputs.size() == 1;
    const auto samplesFile = PCSX::ToolArgs::get(samplesFlag);
    const auto amplification = PCSX::ToolArgs::get(amplificationFlag).value_or(175);
    const auto stretchList = PCSX::ToolArgs::get(stretchFlag);
    if (asksForHelp || !oneInput || !hasOutput) {
        fmt::print(R"(
Usage: {} input.mod [-h] [-s output.smp] [-a amp] [--stretch list] -o output.hit
  input.mod         mandatory: specify the input mod file
  -o output.hit     mandatory: name of the output hit file.
  -h                displays this help information and exit.
  -s output.smp     optional: name of the output sample file.
  -a amplification  optional: value of sample amplification. Defaults to 175.
  --stretch list    optional: comma separated sample numbers, or "all", whose
                    loops are resampled to whole SPU blocks instead of rounded.

If the -s option is specified, the .hit file will only contain the pattern data,
and the .smp file will contain the sample data which can be loaded into the SPU
memory separately. If the -s option is not specified, the .hit file will contain
both the pattern and sample data.

The SPU loops on whole blocks of 28 samples, so by default loop lengths are
rounded to the nearest block, which detunes the looped part of the sample.
A stretched sample is instead resampled so that its loop is a whole number of
blocks, and the notes playing it are transposed and its finetune adjusted to
play it at the same pitch. A sample that can't be stretched, because its notes
would move past the period table, it would get too long, or stretching isn't
closer than rounding, is rounded, and modconv says so.
)",
                   argv[0]);
        return -1;
    }

    const auto& input = inputs[0];
    PCSX::IO<PCSX::File> file(new PCSX::PosixFile(input));
    if (file->failed()) {
        fmt::print("Unable to open file: {}\n", input);
        return -1;
    }

    ModFile modFile;
    modFile.deserialize(file);

    std::string_view signature(modFile.get<Signature>().value, 4);

    unsigned channels = 0;
    if (signature == "M.K." || signature == "M!K!") {
        channels = 4;
    } else if (std::isdigit(signature[0]) && (signature[1] == 'C') && (signature[2] == 'H') && (signature[3] == 'N')) {
        channels = signature[0] - '0';
    } else if (std::isdigit(signature[0]) && std::isdigit(signature[1]) && (signature[2] == 'C') &&
               (signature[3] == 'H')) {
        channels = (signature[0] - '0') * 10 + signature[1] - '0';
    }

    if (channels == 0) {
        fmt::print("{} doesn't have a recognized MOD file format.\n", input);
        return -1;
    }

    if (channels > 24) {
        fmt::print("{} has too many channels ({}). The maximum is 24.\n", input, channels);
        return -1;
    }

    unsigned maxPatternID = 0;
    for (unsigned i = 0; i < 128; i++) {
        maxPatternID = std::max(maxPatternID, unsigned(modFile.get<PatternTable>()[i]));
    }

    auto patternSlice = file->read(channels * (maxPatternID + 1) * 256);
    std::vector<uint8_t> patternData(patternSlice.data<uint8_t>(), patternSlice.data<uint8_t>() + patternSlice.size());

    std::vector<bool> stretchRequested(32, false);
    if (stretchList.has_value()) {
        if (stretchList.value().empty()) {
            fmt::print("--stretch needs a list of sample numbers, or \"all\".\n");
            return -1;
        }
        std::stringstream list(stretchList.value());
        std::string item;
        while (std::getline(list, item, ',')) {
            if (item == "all") {
                std::fill(stretchRequested.begin(), stretchRequested.end(), true);
                continue;
            }
            char* end = nullptr;
            const unsigned long n = std::strtoul(item.c_str(), &end, 10);
            if (item.empty() || *end || n < 1 || n > 31) {
                fmt::print("Invalid sample number in --stretch: {}\n", item);
                return -1;
            }
            stretchRequested[n] = true;
        }
    }

    fmt::print("Title:     {}\n", modFile.get<ModTitle>().value);
    fmt::print("Channels:  {}\n", channels);
    fmt::print("Positions: {}\n", modFile.get<Positions>().value);
    fmt::print("Patterns:  {}\n", maxPatternID + 1);
    fmt::print("Converting samples...\n");

    PCSX::IO<PCSX::File> encodedSamples =
        samplesFile.has_value()
            ? static_cast<PCSX::File*>(new PCSX::PosixFile(samplesFile.value().c_str(), PCSX::FileOps::TRUNCATE))
            : static_cast<PCSX::File*>(new PCSX::BufferFile(PCSX::FileOps::READWRITE));

    constexpr uint8_t silentLoopBlock[16] = {0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    constexpr unsigned spuMemory = 512 * 1024 - 0x1010;

    std::string loopWarnings;
    std::string stretchReport;
    uint8_t orders[128];
    for (unsigned i = 0; i < 128; i++) orders[i] = modFile.get<PatternTable>()[i];
    const auto cellMap = PCSX::ModConv::mapCells(patternData, channels, orders,
                                                 std::min(128u, unsigned(modFile.get<Positions>().value)));
    const auto notesUsed = PCSX::ModConv::notesPerSample(patternData, cellMap);
    std::vector<std::optional<PCSX::ModConv::Stretch>> stretches(32);
    std::unique_ptr<PCSX::ADPCM::Encoder> encoder(new PCSX::ADPCM::Encoder);
    for (unsigned i = 0; i < 31; i++) {
        encoder->reset();
        auto& sample = modFile.get<ModSamples>()[i];
        fmt::print("Sample {:2} [{:22}] - ", i + 1, sample.get<SampleName>().value);
        auto length = sample.get<SampleLength>().value;
        auto layout = PCSX::ModConv::computeLayout(length, sample.get<SampleLoopStart>().value,
                                                   sample.get<SampleLoopLength>().value);
        if (stretchRequested[i + 1] && layout.hasLoop && !layout.exact) {
            stretches[i + 1] = PCSX::ModConv::chooseStretch(
                layout.loopLength, PCSX::ModConv::finetuneFromNibble(sample.get<SampleFineTune>().value),
                notesUsed[i + 1]);
            const auto& st = *stretches[i + 1];
            const unsigned newLoopStart = unsigned(std::lround(layout.loopStart * st.ratio() / 2.0)) * 2;
            if (st.offTable != 0) {
                // Those notes would be clamped to the top of the table and play flat.
                stretchReport += fmt::format(
                    "  Sample {:2}: {} notes would transpose past the table, rounded instead\n", i + 1, st.offTable);
                stretches[i + 1].reset();
            } else if (st.worstCents >= st.roundedCents) {
                stretchReport += fmt::format("  Sample {:2}: rounding is as close, not stretched\n", i + 1);
                stretches[i + 1].reset();
            } else if (PCSX::ModConv::computeLayout((newLoopStart + st.newLoopLength) / 2, newLoopStart / 2,
                                                    st.newLoopLength / 2)
                           .encodedLength >= 65536) {
                stretchReport += fmt::format("  Sample {:2}: too long to stretch, rounded instead\n", i + 1);
                stretches[i + 1].reset();
            }
        }
        if (length == 0) {
            fmt::print("Empty\n");
            continue;
        }
        int16_t input[28];
        uint8_t spuBlock[16];
        unsigned encodedLength = 0;
        if (!layout.hasLoop) {
            file->skip<uint16_t>();
            length--;
            length *= 2;
            while (length >= 28) {
                for (unsigned j = 0; j < 28; j++) {
                    input[j] = int16_t(file->read<int8_t>()) * amplification;
                }
                length -= 28;
                encoder->processSPUBlock(input, spuBlock, PCSX::ADPCM::Encoder::BlockAttribute::OneShot);
                spuBlock[1] = length == 0 ? 1 : 0;
                encodedSamples->write(spuBlock, 16);
                encodedLength += 16;
            }
            if (length != 0) {
                for (unsigned j = 0; j < length; j++) {
                    input[j] = int16_t(file->read<int8_t>()) * amplification;
                }
                for (unsigned j = length; j < 28; j++) {
                    input[j] = 0;
                }
                encoder->processSPUBlock(input, spuBlock, PCSX::ADPCM::Encoder::BlockAttribute::OneShot);
                spuBlock[1] = 0;
                encodedSamples->write(spuBlock, 16);
                encodedLength += 16;
            }
            encodedSamples->write(silentLoopBlock, 16);
            encodedLength += 16;
            fmt::print("Size {} -> {}\n", sample.get<SampleLength>().value * 2 - 2, encodedLength);
        } else {
            std::vector<int16_t> source(length * 2);
            for (auto& s : source) s = int16_t(file->read<int8_t>()) * amplification;
            if (stretches[i + 1]) {
                const auto& st = *stretches[i + 1];
                unsigned newLoopStart;
                source = PCSX::ModConv::resampleLoop(source, layout.loopStart, layout.loopLength, st.newLoopLength,
                                                     newLoopStart);
                layout = PCSX::ModConv::computeLayout(source.size() / 2, newLoopStart / 2, st.newLoopLength / 2);
                sample.get<SampleFineTune>().value = PCSX::ModConv::finetuneToNibble(st.finetune);
                stretchReport += fmt::format(
                    "  Sample {:2} [{}]: loop {} -> {} samples, {:+} semitones, finetune {:+} (was {:+}), worst "
                    "loop pitch error {:.1f} cents over {} notes ({:.1f} rounded)\n",
                    i + 1, sample.get<SampleName>().value, st.loopLength, st.newLoopLength, st.semitones, st.finetune,
                    st.oldFinetune, st.worstCents, notesUsed[i + 1].size(), st.roundedCents);
            }
            std::vector<int16_t> pcm(layout.pad, 0);
            pcm.insert(pcm.end(), source.begin() + layout.skip, source.begin() + layout.loopStart);
            const unsigned loopBlock = pcm.size() / 28;
            for (unsigned j = 0; j < layout.body; j++) {
                pcm.push_back(source[layout.loopStart + j % layout.loopLength]);
            }
            const unsigned blocks = pcm.size() / 28;
            for (unsigned b = 0; b < blocks; b++) {
                // The loop start block is forced to filter 0, so it decodes the same whether the SPU
                // reaches it from the previous block or jumps to it from the loop end.
                encoder->processSPUBlock(pcm.data() + b * 28, spuBlock, PCSX::ADPCM::Encoder::BlockAttribute::OneShot,
                                         b == loopBlock);
                uint8_t blockAttribute = 0;
                if (b >= loopBlock) blockAttribute |= 2;
                if (b == loopBlock) blockAttribute |= 4;
                if (b == (blocks - 1)) blockAttribute |= 1;
                spuBlock[1] = blockAttribute;
                encodedSamples->write(spuBlock, 16);
                encodedLength += 16;
            }
            fmt::print("Size {} -> {}, loop {}+{} bytes, pad {}{}\n", sample.get<SampleLength>().value * 2,
                       encodedLength, layout.loopStart, layout.loopLength, layout.pad,
                       layout.exact ? "" : ", loop period rounded to a whole block");
            if (!layout.exact)
                loopWarnings += PCSX::ModConv::loopWarning(i + 1, sample.get<SampleName>().value, layout);
        }
        sample.get<SampleLength>().value = encodedLength;
        // Write back the loop that was actually encoded, after clamping, so that the player's 9xx
        // offsets wrap against the same loop.
        sample.get<SampleLoopStart>().value = layout.hasLoop ? layout.loopStart / 2 : 0;
        sample.get<SampleLoopLength>().value = layout.hasLoop ? layout.loopLength / 2 : 1;
        if (encodedLength >= 65536) {
            fmt::print("Sample too big.\n");
            return -1;
        }
    }

    if (!stretchReport.empty()) {
        fmt::print("Stretched loops:\n{}", stretchReport);
        for (auto& w : PCSX::ModConv::rewritePatterns(patternData, cellMap, stretches)) {
            fmt::print("Warning: {}\n", w);
        }
    }

    if (!loopWarnings.empty()) {
        fmt::print("{}{}{}", PCSX::ModConv::c_loopWarningHeader, loopWarnings, PCSX::ModConv::c_loopWarningFooter);
    }

    if (channels >= 10) {
        modFile.get<Signature>().value[0] = 'H';
        modFile.get<Signature>().value[1] = 'M';
        modFile.get<Signature>().value[2] = (channels / 10) + '0';
        modFile.get<Signature>().value[3] = (channels % 10) + '0';
    } else {
        modFile.get<Signature>().value[0] = 'H';
        modFile.get<Signature>().value[1] = 'I';
        modFile.get<Signature>().value[2] = 'T';
        modFile.get<Signature>().value[3] = channels + '0';
    }

    unsigned fullLength = 0;
    for (unsigned i = 0; i < 31; i++) {
        auto& sample = modFile.get<ModSamples>()[i];
        fullLength += sample.get<SampleLength>().value;
    }

    if (fullLength >= spuMemory) {
        fmt::print("Not enough SPU memory to store all samples; {} bytes required but only {} available.\n", fullLength,
                   spuMemory);
        return -1;
    } else {
        fmt::print("Used {} bytes of SPU memory, {} still available.\n", fullLength, spuMemory - fullLength);
    }

    PCSX::IO<PCSX::File> out(new PCSX::PosixFile(output.value().c_str(), PCSX::FileOps::TRUNCATE));
    modFile.serialize(out);
    out->write(patternData.data(), patternData.size());
    if (!samplesFile.has_value()) {
        out->write(std::move(encodedSamples.asA<PCSX::BufferFile>()->borrow()));
    }

    out->close();
    encodedSamples->close();
    if (samplesFile.has_value()) {
        fmt::print("All done, files {} and {} written out.\n", output.value(), samplesFile.value());
    } else {
        fmt::print("All done, file {} written out.\n", output.value());
    }

    return 0;
}
