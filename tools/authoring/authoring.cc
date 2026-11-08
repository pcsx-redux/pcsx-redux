/***************************************************************************
 *   Copyright (C) 2025 PCSX-Redux authors                                 *
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

#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>

#include "args/args.hxx"
#include "fmt/format.h"
#include "json.hpp"
#include "mips-common/util/bitfield.hh"
#include "support/container-file.h"
#include "support/djbhash.h"
#include "support/file.h"
#include "support/mem4g.h"
#include "support/polyfills.h"
#include "support/tool-args.h"
#include "supportpsx/binloader.h"
#include "supportpsx/iec-60908b.h"
#include "supportpsx/iso9660-builder.h"
#include "supportpsx/iso9660-lowlevel.h"
#include "supportpsx/ps1-packer.h"
#include "supportpsx/ucl-utils.h"
#include "ucl/ucl.h"

template <PCSX::PolyFill::IntegralConcept T, std::endian endianess = std::endian::little>
void writeToBuffer(void* buffer_, T v) {
    if constexpr (endianess != std::endian::native) {
        v = PCSX::PolyFill::byteSwap(v);
    }
    uint8_t* buffer = reinterpret_cast<uint8_t*>(buffer_);
    for (unsigned i = 0; i < sizeof(T); i++) {
        buffer[i] = v & 0xff;
        v >>= 8;
    }
}

static constexpr unsigned c_maximumSectorCount = (99 * 60 + 59) * 75 + 74 - 150;

union IndexEntry {
    enum class Method : uint32_t {
        NONE = 0,
        UCL_NRV2E = 1,
        LZ4 = 2,
        COUNT = 3,
    };
    typedef Utilities::BitSpan<uint32_t, 21> DecompSizeField;
    typedef Utilities::BitSpan<uint32_t, 11> PaddingField;
    typedef Utilities::BitSpan<uint32_t, 19> SectorOffsetField;
    typedef Utilities::BitSpan<uint32_t, 10> CompressedSizeField;
    typedef Utilities::BitSpan<Method, 3> MethodField;
    typedef Utilities::BitField<DecompSizeField, PaddingField, SectorOffsetField, CompressedSizeField, MethodField>
        CompressedEntry;
    uint32_t getDecompSize() const { return entry.get<DecompSizeField>(); }
    uint32_t getPadding() const { return entry.get<PaddingField>(); }
    uint32_t getSectorOffset() const { return entry.get<SectorOffsetField>(); }
    uint32_t getCompressedSize() const { return entry.get<CompressedSizeField>(); }
    Method getCompressionMethod() const { return entry.get<MethodField>(); }
    void setDecompSize(uint32_t v) { entry.set<DecompSizeField>(v); }
    void setPadding(uint32_t v) { entry.set<PaddingField>(v); }
    void setSectorOffset(uint32_t v) { entry.set<SectorOffsetField>(v); }
    void setCompressedSize(uint32_t v) { entry.set<CompressedSizeField>(v); }
    void setMethod(Method v) { entry.set<MethodField>(v); }
    uint32_t asArray[4];
    struct {
        uint64_t hash;
        CompressedEntry entry;
    };
};

static_assert(sizeof(IndexEntry) == 16);

int main(int argc, char** argv) {
    args::ArgumentParser parser("");
    args::ValueFlag<std::string> outputFlag(parser, "output", "", {"o"});
    args::ValueFlag<std::string> basedirFlag(parser, "basedir", "", {"basedir"});
    args::ValueFlag<std::string> licenseFlag(parser, "license", "", {"license"});
    args::ValueFlag<unsigned> threadsFlag(parser, "threads", "", {"threads"});
    args::Flag quietFlag(parser, "quiet", "", {"q"});
    args::Flag helpFlag(parser, "help", "", {"h"});
    args::PositionalList<std::string> inputsList(parser, "inputs", "");
    PCSX::ToolArgs::parse(parser, argc, argv);
    const auto output = PCSX::ToolArgs::get(outputFlag);
    const auto inputs = args::get(inputsList);
    const auto license = PCSX::ToolArgs::get(licenseFlag);
    const bool asksForHelp = args::get(helpFlag);
    const bool quiet = args::get(quietFlag);
    const bool hasOutput = output.has_value();
    const bool hasExactlyOneInput = inputs.size() == 1;

    if (asksForHelp || !hasExactlyOneInput || !hasOutput) {
        fmt::print(R"(
Usage: {} input.json [-h] -o output.bin
  input.json        mandatory: specify the input JSON file.
  -o output.bin     mandatory: name of the output file.
  -basedir path     optional: base directory for the input files.
  -license file     optional: use this license file.
  -threads count    optional: number of threads to use for compression.
  -q                optional: only print errors.
  -h                displays this help information and exit.
)",
                   argv[0]);
        return -1;
    }

    auto input = inputs[0];
    const std::filesystem::path basePath =
        PCSX::ToolArgs::get(basedirFlag).value_or(std::filesystem::path(input).parent_path().string());
    PCSX::IO<PCSX::File> indexFile(new PCSX::PosixFile(input));
    if (indexFile->failed()) {
        fmt::print("Unable to open file: {}\n", input);
        return -1;
    }
    PCSX::FileAsContainer container(indexFile);
    auto indexData = nlohmann::json::parse(container.begin(), container.end(), nullptr, false, true);
    if (indexData.is_discarded()) {
        fmt::print("Unable to parse JSON file: {}\n", input);
        return -1;
    }
    if (indexData.is_null()) {
        fmt::print("Unable to parse JSON file: {}\n", input);
        return -1;
    }

    if (!indexData.is_object()) {
        fmt::print("Invalid JSON file: {}\n", input);
        return -1;
    }

    if (!indexData.contains("executable") || !indexData["executable"].is_string()) {
        fmt::print("Invalid JSON file: {}\n", input);
        return -1;
    }

    if (!indexData.contains("files") || !indexData["files"].is_array()) {
        fmt::print("Invalid JSON file: {}\n", input);
        return -1;
    }

    PCSX::IO<PCSX::File> out(new PCSX::PosixFile(output.value(), PCSX::FileOps::TRUNCATE));
    if (out->failed()) {
        fmt::print("Error opening output file {}\n", output.value());
        return -1;
    }
    PCSX::ISO9660Builder builder(out);

    PCSX::IO<PCSX::File> licenseFile(new PCSX::FailedFile);
    if (license.has_value()) {
        licenseFile.setFile(new PCSX::PosixFile(license.value()));
        if (licenseFile->failed()) {
            fmt::print("Error opening license file {}\n", license.value());
            return -1;
        }
    }

    const unsigned threadCount = PCSX::ToolArgs::get(threadsFlag).value_or(std::thread::hardware_concurrency());

    nlohmann::json pvdData = nlohmann::json::object();
    if (indexData.contains("pvd") && indexData["pvd"].is_object()) {
        pvdData = indexData["pvd"];
    }

    auto executablePath = indexData["executable"].get<std::string>();
    PCSX::IO<PCSX::File> executableFile(new PCSX::PosixFile(basePath / executablePath));
    if (executableFile->failed()) {
        fmt::print("Unable to open file: {}\n", executablePath);
        return -1;
    }

    builder.writeLicense(licenseFile);

    PCSX::BinaryLoader::Info info;
    PCSX::IO<PCSX::Mem4G> memory(new PCSX::Mem4G());
    std::map<uint32_t, std::string> symbols;
    bool success = PCSX::BinaryLoader::load(executableFile, memory, info, symbols);
    if (!success) {
        fmt::print("Unable to load file: {}\n", executablePath);
        return -1;
    }
    if (!info.pc.has_value()) {
        fmt::print("File {} is invalid.\n", executablePath);
        return -1;
    }

    const unsigned filesCount = indexData["files"].size();
    const unsigned indexSectorsCount = ((filesCount + 1) * sizeof(IndexEntry) + 2047) / 2048;

    if (filesCount > c_maximumSectorCount) {
        fmt::print("Too many files specified ({}), max allowed is {}\n", filesCount, c_maximumSectorCount);
        return -1;
    }
    if (!quiet) {
        fmt::print("Index size: {}\n", indexSectorsCount * 2048);
    }

    PCSX::PS1Packer::Options options;
    options.booty = false;
    options.raw = false;
    options.rom = false;
    options.cpe = false;
    options.shell = false;
    options.nokernel = true;
    options.tload = false;
    options.nopad = false;
    PCSX::IO<PCSX::File> compressedExecutable(new PCSX::BufferFile(PCSX::FileOps::READWRITE));
    PCSX::PS1Packer::pack(new PCSX::SubFile(memory, memory->lowestAddress(), memory->actualSize()),
                          compressedExecutable, memory->lowestAddress(), info.pc.value_or(0), info.gp.value_or(0),
                          info.sp.value_or(0), options);

    if (compressedExecutable->size() % 2048 != 0) {
        fmt::print("Executable size is not a multiple of 2048\n");
        return -1;
    }
    if (!quiet) {
        fmt::print("Executable size: {}\n", compressedExecutable->size());
        fmt::print("Executable location: {}\n", 23 + indexSectorsCount);
    }

    const unsigned executableSectorsCount = compressedExecutable->size() / 2048;
    unsigned currentSector = 23 + indexSectorsCount;
    std::atomic<uint32_t> maxMargin;

    for (unsigned i = 0; i < executableSectorsCount; i++) {
        auto sector = compressedExecutable.asA<PCSX::BufferFile>()->borrow(i * 2048);
        builder.writeSectorAt(sector.data<uint8_t>(), PCSX::IEC60908b::MSF{150 + currentSector++},
                              PCSX::IEC60908b::SectorMode::M2_FORM1);
    }

    std::unique_ptr<uint8_t[]> indexEntryDataBuffer(new uint8_t[indexSectorsCount * 2048]);
    memset(indexEntryDataBuffer.get(), 0, indexSectorsCount * 2048);
    std::span<IndexEntry> indexEntryData = {reinterpret_cast<IndexEntry*>(indexEntryDataBuffer.get()) + 1, filesCount};

    struct WorkUnit {
        WorkUnit() : semaphore(0), failed(false) {}
        std::binary_semaphore semaphore;
        std::vector<uint8_t> sectorData;
        nlohmann::json fileInfo;
        uint32_t margin = 0;
        bool failed;
    };
    static WorkUnit work[c_maximumSectorCount];
    for (unsigned i = 0; i < filesCount; i++) {
        auto& fileInfo = indexData["files"][i];
        if (!fileInfo.is_object()) {
            fmt::print("Invalid JSON file: {}\n", input);
            return -1;
        }
        if (!fileInfo.contains("path") || !fileInfo["path"].is_string()) {
            fmt::print("Invalid JSON file: {}\n", input);
            return -1;
        }
        work[i].fileInfo = fileInfo;
    }
    auto createSectorHeader = [](uint8_t sector[2352]) {
        memset(sector + 1, 0xff, 10);
        sector[15] = 2;
        sector[18] = sector[22] = 8;
    };

    std::atomic<unsigned> currentWorkUnit = 0;
    for (unsigned i = 0; i < threadCount; i++) {
        std::thread t([&]() {
            while (1) {
                std::atomic_thread_fence(std::memory_order_acq_rel);
                unsigned workUnitIndex = currentWorkUnit.fetch_add(1);
                if (workUnitIndex >= filesCount) return;
                auto& workUnit = work[workUnitIndex];
                auto filePath = workUnit.fileInfo["path"].get<std::string>();
                PCSX::IO<PCSX::File> file(new PCSX::PosixFile(basePath / filePath));
                if (file->failed()) {
                    workUnit.failed = true;
                    workUnit.semaphore.release();
                    continue;
                }
                unsigned size = file->size();
                if (size >= 2 * 1024 * 1024) {
                    workUnit.failed = true;
                    workUnit.semaphore.release();
                    continue;
                }
                unsigned originalSectorsCount = (size + 2047) / 2048;
                std::vector<uint8_t> dataIn;
                dataIn.resize(originalSectorsCount * 2048);
                file->read(dataIn.data(), dataIn.size());

                // "compress": false keeps a file stored, so it can be read sector by sector.
                const bool store = workUnit.fileInfo.contains("compress") &&
                                   workUnit.fileInfo["compress"].is_boolean() &&
                                   !workUnit.fileInfo["compress"].get<bool>();

                std::vector<uint8_t> dataOut;
                ucl_uint outSize = 0;
                unsigned compressedSectorsCount = originalSectorsCount;
                if (!store) {
                    dataOut.resize(dataIn.size() * 1.2 + 2064 + 2048);
                    int r = ucl_nrv2e_99_compress(dataIn.data(), size, dataOut.data() + 2048, &outSize, nullptr, 10,
                                                  nullptr, nullptr);
                    if (r != UCL_E_OK) {
                        workUnit.failed = true;
                        workUnit.semaphore.release();
                        continue;
                    }
                    compressedSectorsCount = (outSize + 2047) / 2048;
                }
                const bool compressed = compressedSectorsCount < originalSectorsCount;
                // The index holds a size of at most 1023 sectors; a stored file of 1024 would read as 0.
                if (!compressed && originalSectorsCount > 1023) {
                    workUnit.failed = true;
                    workUnit.semaphore.release();
                    continue;
                }

                // Only a compressed entry is decompressed in place, so only it needs a margin.
                if (compressed) {
                    ssize_t margin = PCSX::UCLUtils::inPlaceOverlapMargin(dataOut.data() + 2048, outSize, size);
                    ssize_t relMargin = margin - size + outSize;
                    workUnit.margin = relMargin > 0 ? static_cast<uint32_t>(relMargin) : 0;
                    do {
                        auto oldOverlap = maxMargin.load();
                        if (workUnit.margin <= oldOverlap) break;
                        if (maxMargin.compare_exchange_weak(oldOverlap, workUnit.margin)) break;
                    } while (1);
                }

                IndexEntry* entry = &indexEntryData[workUnitIndex];

                if (workUnit.fileInfo["name"].is_string()) {
                    entry->hash = PCSX::djb::hash(workUnit.fileInfo["name"].get<std::string>());
                } else {
                    entry->hash = PCSX::djb::hash(filePath);
                }
                entry->setDecompSize(size);
                std::span<uint8_t> source;
                unsigned sectorCount = 0;
                if (compressed) {
                    entry->setCompressedSize(compressedSectorsCount);
                    entry->setMethod(IndexEntry::Method::UCL_NRV2E);
                    unsigned padding = outSize % 2048;
                    if (padding > 0) {
                        padding = 2048 - padding;
                    }
                    entry->setPadding(padding);
                    sectorCount = compressedSectorsCount;
                    source = {reinterpret_cast<uint8_t*>(dataOut.data()) - padding + 2048, sectorCount * 2048};
                } else {
                    entry->setCompressedSize(originalSectorsCount);
                    entry->setMethod(IndexEntry::Method::NONE);
                    entry->setPadding(0);
                    sectorCount = originalSectorsCount;
                    source = {reinterpret_cast<uint8_t*>(dataIn.data()), sectorCount * 2048};
                }
                workUnit.sectorData.resize(sectorCount * 2352);
                for (unsigned sector = 0; sector < sectorCount; sector++) {
                    uint8_t* dest = workUnit.sectorData.data() + sector * 2352;
                    createSectorHeader(dest);
                    memcpy(dest + 24, source.data() + sector * 2048, 2048);
                    PCSX::IEC60908b::computeEDCECC(dest);
                }
                workUnit.semaphore.release();
            }
        });
        t.detach();
    }

    auto putSectorLBA = [](uint8_t sector[2352], uint32_t lba) {
        PCSX::IEC60908b::MSF time(lba + 150);
        time.toBCD(sector + 12);
    };

    for (unsigned workUnitIndex = 0; workUnitIndex < filesCount; workUnitIndex++) {
        auto& workUnit = work[workUnitIndex];
        workUnit.semaphore.acquire();
        std::atomic_thread_fence(std::memory_order_acq_rel);
        if (workUnit.failed) {
            fmt::print("Error processing file: {}\n", workUnit.fileInfo["path"].get<std::string>());
            return -1;
        }
        IndexEntry* entry = &indexEntryData[workUnitIndex];
        if (!quiet) {
            fmt::print("Processed file: {}\n", workUnit.fileInfo["path"].get<std::string>());
            fmt::print("  Original size: {}\n", entry->getDecompSize());
            fmt::print("  Compressed size: {}\n", entry->getCompressedSize() * 2048);
            fmt::print("  Compression method: {}\n", static_cast<uint32_t>(entry->getCompressionMethod()));
            fmt::print("  Sector offset: {}\n", currentSector);
            fmt::print("  Margin: {}\n", workUnit.margin);
        }
        entry->setSectorOffset(currentSector);
        unsigned sectorCount = entry->getCompressedSize();
        for (unsigned sector = 0; sector < sectorCount; sector++) {
            uint8_t* dest = workUnit.sectorData.data() + sector * 2352;
            putSectorLBA(dest, currentSector);
            builder.writeSectorAt(dest, PCSX::IEC60908b::MSF{150 + currentSector++}, PCSX::IEC60908b::SectorMode::RAW);
        }
    }

    if (!quiet) {
        fmt::print("Processed {} files.\n", filesCount);
        fmt::print("Maximum margin: {}\n", maxMargin.load());
    }

    uint8_t empty[2048] = {0};
    for (unsigned i = 0; i < 9000; i++) {
        builder.writeSectorAt(empty, PCSX::IEC60908b::MSF{150 + currentSector++},
                              PCSX::IEC60908b::SectorMode::M2_FORM1);
    }

    const unsigned totalSectorCount = currentSector;

    indexEntryDataBuffer[0] = 'P';
    indexEntryDataBuffer[1] = 'S';
    indexEntryDataBuffer[2] = 'X';
    indexEntryDataBuffer[3] = '-';
    indexEntryDataBuffer[4] = 'A';
    indexEntryDataBuffer[5] = 'R';
    indexEntryDataBuffer[6] = 'C';
    indexEntryDataBuffer[7] = '1';
    // The reader allows (byte + 1) * 16 bytes, so 4095 is the largest margin the byte holds.
    if (maxMargin.load() >= 4096) {
        fmt::print("Decompression margin of {} bytes does not fit the archive header (4095 max).\n",
                   maxMargin.load());
        return -1;
    }
    uint32_t maxMarginValue = maxMargin.load() >> 4;
    writeToBuffer(indexEntryDataBuffer.get() + 8, filesCount);
    writeToBuffer<uint8_t>(indexEntryDataBuffer.get() + 11, maxMarginValue);
    writeToBuffer(indexEntryDataBuffer.get() + 12, totalSectorCount);
    std::sort(indexEntryData.begin(), indexEntryData.end(),
              [](const IndexEntry& a, const IndexEntry& b) { return a.hash < b.hash; });

    for (unsigned i = 0; i < indexSectorsCount; i++) {
        auto sector = indexEntryDataBuffer.get() + i * 2048;
        builder.writeSectorAt(sector, PCSX::IEC60908b::MSF{150 + i + 23}, PCSX::IEC60908b::SectorMode::M2_FORM1);
    }

    PCSX::IO<PCSX::File> pvdSector(new PCSX::BufferFile(PCSX::FileOps::READWRITE));
    PCSX::ISO9660LowLevel::PVD pvd;
    pvd.reset();
    pvd.get<PCSX::ISO9660LowLevel::PVD_TypeCode>().value = 1;
    pvd.get<PCSX::ISO9660LowLevel::PVD_StdIdent>().set("CD001");
    pvd.get<PCSX::ISO9660LowLevel::PVD_Version>().value = 1;
    auto systemIdent = pvdData["system_id"].is_string() ? pvdData["system_id"].get<std::string>() : "PLAYSTATION";
    pvd.get<PCSX::ISO9660LowLevel::PVD_SystemIdent>().set(systemIdent, ' ');
    auto volumeIdent = pvdData["volume_id"].is_string() ? pvdData["volume_id"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeIdent>().set(volumeIdent, ' ');
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSpaceSize>().value = totalSectorCount;
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSpaceSizeBE>().value = totalSectorCount;
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSetSize>().value = 1;
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSetSizeBE>().value = 1;
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSequenceNumber>().value = 1;
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolumeSequenceNumberBE>().value = 1;
    pvd.get<PCSX::ISO9660LowLevel::PVD_LogicalBlockSize>().value = 2048;
    pvd.get<PCSX::ISO9660LowLevel::PVD_LogicalBlockSizeBE>().value = 2048;
    pvd.get<PCSX::ISO9660LowLevel::PVD_PathTableSize>().value = 10;
    pvd.get<PCSX::ISO9660LowLevel::PVD_PathTableSizeBE>().value = 10;
    pvd.get<PCSX::ISO9660LowLevel::PVD_LPathTableLocation>().value = 18;
    pvd.get<PCSX::ISO9660LowLevel::PVD_LPathTableOptLocation>().value = 19;
    pvd.get<PCSX::ISO9660LowLevel::PVD_MPathTableLocation>().value = 20;
    pvd.get<PCSX::ISO9660LowLevel::PVD_MPathTableOptLocation>().value = 21;
    auto& root = pvd.get<PCSX::ISO9660LowLevel::PVD_RootDir>();
    root.get<PCSX::ISO9660LowLevel::DirEntry_Length>().value = 34;
    root.get<PCSX::ISO9660LowLevel::DirEntry_ExtLength>().value = 0;
    root.get<PCSX::ISO9660LowLevel::DirEntry_LBA>().value = 22;
    root.get<PCSX::ISO9660LowLevel::DirEntry_LBABE>().value = 22;
    root.get<PCSX::ISO9660LowLevel::DirEntry_Size>().value = 2048;
    root.get<PCSX::ISO9660LowLevel::DirEntry_SizeBE>().value = 2048;
    root.get<PCSX::ISO9660LowLevel::DirEntry_Flags>().value = 2;
    root.get<PCSX::ISO9660LowLevel::DirEntry_VolSeqNo>().value = 1;
    root.get<PCSX::ISO9660LowLevel::DirEntry_VolSeqNoBE>().value = 1;
    root.get<PCSX::ISO9660LowLevel::DirEntry_Filename>().value.resize(1);
    auto volumeSetIdent = pvdData["volume_set_id"].is_string() ? pvdData["volume_set_id"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_VolSetIdent>().set(volumeSetIdent, ' ');
    auto publisherIdent = pvdData["publisher"].is_string() ? pvdData["publisher"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_PublisherIdent>().set(publisherIdent, ' ');
    auto dataPreparerIdent = pvdData["preparer"].is_string() ? pvdData["preparer"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_DataPreparerIdent>().set(dataPreparerIdent, ' ');
    auto applicationIdent = pvdData["application_id"].is_string() ? pvdData["application_id"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_ApplicationIdent>().set(applicationIdent, ' ');
    auto copyrightFileIdent = pvdData["copyright"].is_string() ? pvdData["copyright"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_CopyrightFileIdent>().set(copyrightFileIdent, ' ');
    auto abstractFileIdent = pvdData["abstract"].is_string() ? pvdData["abstract"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_AbstractFileIdent>().set(abstractFileIdent, ' ');
    auto bibliographicFileIdent =
        pvdData["bibliographic"].is_string() ? pvdData["bibliographic"].get<std::string>() : "";
    pvd.get<PCSX::ISO9660LowLevel::PVD_BibliographicFileIdent>().set(bibliographicFileIdent, ' ');
    pvd.get<PCSX::ISO9660LowLevel::PVD_FileStructureVersion>().value = 1;

    pvd.serialize(pvdSector);
    while (pvdSector->size() < 2048) {
        pvdSector->write<uint8_t>(0);
    }
    builder.writeSectorAt(pvdSector.asA<PCSX::BufferFile>()->borrow(0).data<uint8_t>(), {0, 2, 16},
                          PCSX::IEC60908b::SectorMode::M2_FORM1);

    uint8_t sector[2048];
    memset(sector, 0, sizeof(sector));
    sector[0] = 0xff;
    sector[1] = 'C';
    sector[2] = 'D';
    sector[3] = '0';
    sector[4] = '0';
    sector[5] = '1';
    builder.writeSectorAt(sector, {0, 2, 17}, PCSX::IEC60908b::SectorMode::M2_FORM1);

    memset(sector, 0, sizeof(sector));
    sector[0] = 1;
    sector[2] = 22;
    sector[6] = 1;
    builder.writeSectorAt(sector, {0, 2, 18}, PCSX::IEC60908b::SectorMode::M2_FORM1);
    builder.writeSectorAt(sector, {0, 2, 19}, PCSX::IEC60908b::SectorMode::M2_FORM1);

    memset(sector, 0, sizeof(sector));
    sector[0] = 1;
    sector[5] = 22;
    sector[7] = 1;
    builder.writeSectorAt(sector, {0, 2, 20}, PCSX::IEC60908b::SectorMode::M2_FORM1);
    builder.writeSectorAt(sector, {0, 2, 21}, PCSX::IEC60908b::SectorMode::M2_FORM1);

    uint8_t rootSector[2048] = {
        0x22, 0x00, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01,
        0x01, 0x00, 0x22, 0x00, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x08, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00,
        0x00, 0x01, 0x01, 0x01, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x01, 0x09, 0x50, 0x53, 0x58, 0x2e, 0x45, 0x58, 0x45, 0x3b, 0x31,
    };
    writeToBuffer<uint32_t, std::endian::little>(rootSector + 70, indexSectorsCount + 23);
    writeToBuffer<uint32_t, std::endian::big>(rootSector + 74, indexSectorsCount + 23);
    writeToBuffer<uint32_t, std::endian::little>(rootSector + 78, executableSectorsCount * 2048);
    writeToBuffer<uint32_t, std::endian::big>(rootSector + 82, executableSectorsCount * 2048);
    builder.writeSectorAt(rootSector, {0, 2, 22}, PCSX::IEC60908b::SectorMode::M2_FORM1);

    return 0;
}
