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

#include "core/spdwriter.h"

#include <array>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"

using PCSX::SPD::RegWrite;

namespace {

struct Packet {
    uint8_t type;
    std::vector<uint32_t> payload;
};

std::vector<Packet> parse(const std::vector<uint8_t>& data) {
    std::vector<Packet> packets;
    EXPECT_GE(data.size(), 16u);
    EXPECT_EQ(memcmp(data.data(), "PSXSPUDUMPv1r1\0\0", 16), 0);
    EXPECT_EQ(data.size() % 4, 0u);
    size_t pos = 16;
    auto word = [&](size_t at) {
        return uint32_t(data[at]) | (uint32_t(data[at + 1]) << 8) | (uint32_t(data[at + 2]) << 16) |
               (uint32_t(data[at + 3]) << 24);
    };
    while (pos + 4 <= data.size()) {
        uint32_t header = word(pos);
        Packet p{uint8_t(header >> 24), {}};
        uint32_t len = header & 0xffffff;
        pos += 4;
        EXPECT_LE(pos + len * 4, data.size());
        for (uint32_t i = 0; i < len; i++) p.payload.push_back(word(pos + i * 4));
        pos += len * 4;
        packets.push_back(std::move(p));
    }
    EXPECT_EQ(pos, data.size());
    return packets;
}

const Packet* find(const std::vector<Packet>& packets, uint8_t type) {
    for (auto& p : packets) {
        if (p.type == type) return &p;
    }
    return nullptr;
}

// Index of the first packet after the trace begin marker.
size_t liveStart(const std::vector<Packet>& packets) {
    for (size_t i = 0; i < packets.size(); i++) {
        if (packets[i].type == 0x04) return i + 1;
    }
    ADD_FAILURE() << "no trace begin packet";
    return packets.size();
}

struct Fixture {
    std::array<uint16_t, 0x100> baseline{};
    std::vector<uint8_t> ram = std::vector<uint8_t>(PCSX::SPD::c_spuRamSize, 0);
    PCSX::SPD::Options options;

    Fixture() {
        // 100 ticks per second at a 1 MHz clock: one tick every 10000 cycles.
        options.tickRate = 100 << 16;
        options.clock = 1000000;
    }

    // A sample at start spanning blocks 16-byte blocks, end flag on the last.
    void sample(uint32_t start, uint32_t blocks) {
        for (uint32_t i = 0; i < blocks; i++) {
            ram[start + i * 16 + 1] = (i == blocks - 1) ? 1 : 0;
            ram[start + i * 16 + 2] = uint8_t(0x40 + i);
        }
    }

    PCSX::SPD::Result write(const std::vector<RegWrite>& writes) {
        return PCSX::SPD::writeCapture(baseline, writes, ram.data(), options);
    }
};

}  // namespace

TEST(SPDWriter, RefusesAnEmptyCapture) {
    Fixture f;
    auto result = f.write({});
    EXPECT_FALSE(result.error.empty());
    EXPECT_TRUE(result.data.empty());
}

TEST(SPDWriter, HeaderAndStructure) {
    Fixture f;
    auto result = f.write({{0, 0x180, 0x3fff}});
    ASSERT_TRUE(result.error.empty());
    auto packets = parse(result.data);

    auto* rate = find(packets, 0x30);
    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->payload, std::vector<uint32_t>{100 << 16});

    // spdplayer only starts a stream reachable from an order table, so a capture carries one
    // non-looping order pointing at a single pattern.
    auto* order = find(packets, 0x10);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->payload, (std::vector<uint32_t>{0, 0, 0}));

    auto* header = find(packets, 0x11);
    ASSERT_NE(header, nullptr);
    ASSERT_EQ(header->payload.size(), 1u);
    // The offset lands exactly on the first packet after the pattern header.
    size_t pos = 16;
    size_t i = 0;
    for (; i < packets.size() && &packets[i] != header; i++) pos += 4 + packets[i].payload.size() * 4;
    pos += 4 + header->payload.size() * 4;
    EXPECT_EQ(header->payload[0], pos);
    EXPECT_EQ(packets[i + 1].type, 0x00);

    EXPECT_EQ(packets.back().type, 0x02);
}

TEST(SPDWriter, BaselineSkipsEdgeTriggeredAndReadOnlyRegisters) {
    Fixture f;
    for (unsigned i = 0; i < 0x100; i++) f.baseline[i] = uint16_t(0x1000 + i);
    f.baseline[0x1aa / 2] = 0xc0f0;
    auto result = f.write({{0, 0x180, 0x3fff}});
    ASSERT_TRUE(result.error.empty());
    auto packets = parse(result.data);
    size_t live = liveStart(packets);
    ASSERT_GE(live, 2u);
    auto& restore = packets[live - 2];
    ASSERT_EQ(restore.type, 0x00);

    std::array<int, 0x100> seen{};
    for (uint32_t w : restore.payload) {
        uint16_t reg = w >> 16;
        ASSERT_LT(reg, 0x200);
        seen[reg / 2]++;
        if (reg == 0x1aa) {
            // Transfer mode and IRQ enable cleared, everything else kept.
            EXPECT_EQ(w & 0xffff, 0xc080u);
        } else {
            EXPECT_EQ(w & 0xffff, 0x1000u + reg / 2);
        }
    }
    for (uint16_t reg : {0x188, 0x18a, 0x18c, 0x18e, 0x19c, 0x1a6, 0x1a8, 0x1ae, 0x0c, 0x17c}) {
        EXPECT_EQ(seen[reg / 2], 0) << std::hex << reg;
    }
    for (uint16_t reg : {0x00, 0x04, 0x06, 0x0e, 0x176, 0x180, 0x1a2, 0x1aa, 0x1c0, 0x1fe}) {
        EXPECT_EQ(seen[reg / 2], 1) << std::hex << reg;
    }
    // SPUCNT goes last, so the restored state is complete before the SPU is switched over.
    EXPECT_EQ(restore.payload.back() >> 16, 0x1aau);
}

TEST(SPDWriter, GroupsWritesByTickWithWaitsBetween) {
    Fixture f;
    auto result = f.write({
        {5000, 0x184, 1},        // tick 0
        {6000, 0x186, 2},        // tick 0, 0.1 tick after the first
        {35000, 0x180, 3},       // tick 3
        {35000, 0x1a8, 0x1234},  // dropped: transfer FIFO
        {36000, 0x182, 4},       // tick 3
    });
    ASSERT_TRUE(result.error.empty());
    EXPECT_EQ(result.writes, 4u);
    EXPECT_EQ(result.ticks, 3u);
    auto packets = parse(result.data);
    size_t live = liveStart(packets);
    ASSERT_EQ(packets.size(), live + 3);
    EXPECT_EQ(packets[live].payload, (std::vector<uint32_t>{0x01840001, 0x01860002, 0xefff0003}));
    EXPECT_EQ(packets[live + 1].payload, (std::vector<uint32_t>{0x01800003, 0x01820004}));
    EXPECT_EQ(packets[live + 2].type, 0x02);
}

TEST(SPDWriter, TicksDoNotAccumulateRoundingError) {
    Fixture f;
    // A write every 0.4 tick: per-step rounding would freeze at zero, absolute rounding walks.
    std::vector<RegWrite> writes;
    for (unsigned i = 0; i <= 50; i++) writes.push_back({uint64_t(i) * 4000, 0x180, uint16_t(i)});
    auto result = f.write(writes);
    ASSERT_TRUE(result.error.empty());
    EXPECT_EQ(result.ticks, 20u);
    auto packets = parse(result.data);
    uint64_t waited = 0;
    for (size_t i = liveStart(packets); i < packets.size(); i++) {
        for (uint32_t w : packets[i].payload) {
            if ((w >> 16) == 0xefff) waited += w & 0xffff;
        }
    }
    EXPECT_EQ(waited, 20u);
}

TEST(SPDWriter, CycleStampsGoingBackwardsKeepOrder) {
    Fixture f;
    // A guest call restores the cycle counter, so a later write can carry an earlier stamp.
    auto result =
        f.write({{50000, 0x180, 1}, {80000, 0x180, 2}, {60000, 0x180, 3}, {40000, 0x180, 4}, {100000, 0x180, 5}});
    ASSERT_TRUE(result.error.empty());
    EXPECT_EQ(result.ticks, 5u);
    auto packets = parse(result.data);
    size_t live = liveStart(packets);
    ASSERT_EQ(packets.size(), live + 4);
    EXPECT_EQ(packets[live].payload, (std::vector<uint32_t>{0x01800001, 0xefff0003}));
    EXPECT_EQ(packets[live + 1].payload, (std::vector<uint32_t>{0x01800002, 0x01800003, 0x01800004, 0xefff0002}));
    EXPECT_EQ(packets[live + 2].payload, (std::vector<uint32_t>{0x01800005}));
}

TEST(SPDWriter, LongWaitsAreChunked) {
    Fixture f;
    // 70000 ticks.
    auto result = f.write({{0, 0x180, 1}, {700000000, 0x180, 2}});
    ASSERT_TRUE(result.error.empty());
    auto packets = parse(result.data);
    size_t live = liveStart(packets);
    EXPECT_EQ(packets[live].payload, (std::vector<uint32_t>{0x01800001, 0xefffffff, 0xefff1171}));
}

TEST(SPDWriter, SampleDataCoversKeyedVoicesOnly) {
    Fixture f;
    f.sample(0x2000, 4);  // voice 1, keyed
    f.sample(0x3000, 2);  // voice 17, keyed
    f.sample(0x8000, 8);  // voice 2, start set but never keyed
    f.baseline[(1 * 0x10 + 6) / 2] = 0x2000 >> 3;
    f.baseline[(2 * 0x10 + 6) / 2] = 0x8000 >> 3;
    auto result = f.write({
        {0, 0x188, 1 << 1},
        {10, 0x116, 0x3000 >> 3},  // voice 17 start, written during the capture
        {20, 0x18a, 1 << 1},
    });
    ASSERT_TRUE(result.error.empty());
    EXPECT_EQ(result.sampleBase, 0x2000u);
    EXPECT_EQ(result.sampleSize, 0x1020u);
    auto packets = parse(result.data);
    auto* data = find(packets, 0x21);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->payload[0], 0x2000u >> 3);
    ASSERT_EQ(data->payload.size(), 1 + 0x1020u / 4);
    std::vector<uint8_t> bytes(0x1020);
    memcpy(bytes.data(), data->payload.data() + 1, bytes.size());
    EXPECT_EQ(0, memcmp(bytes.data(), f.ram.data() + 0x2000, bytes.size()));
}

TEST(SPDWriter, SampleDataCoversRepeatAddresses) {
    Fixture f;
    f.sample(0x2000, 4);  // voice 1 start
    f.sample(0x6000, 2);  // voice 1 repeat, set before the key on
    f.sample(0x9000, 2);  // voice 1 repeat, rewritten while it plays
    f.sample(0xc000, 2);  // voice 2 repeat, never keyed
    f.baseline[(1 * 0x10 + 6) / 2] = 0x2000 >> 3;
    f.baseline[(1 * 0x10 + 0xe) / 2] = 0x6000 >> 3;
    f.baseline[(2 * 0x10 + 0xe) / 2] = 0xc000 >> 3;
    auto result = f.write({
        {0, 0x188, 1 << 1},
        {10, 0x01e, 0x9000 >> 3},
        {20, 0x02e, 0xc000 >> 3},
    });
    ASSERT_TRUE(result.error.empty());
    EXPECT_EQ(result.sampleBase, 0x2000u);
    EXPECT_EQ(result.sampleSize, 0x9020u - 0x2000u);
}

TEST(SPDWriter, NoKeyOnsMeansNoSampleData) {
    Fixture f;
    f.sample(0x2000, 4);
    f.baseline[6 / 2] = 0x2000 >> 3;
    auto result = f.write({{0, 0x180, 1}});
    ASSERT_TRUE(result.error.empty());
    EXPECT_EQ(result.sampleSize, 0u);
    EXPECT_EQ(find(parse(result.data), 0x21), nullptr);
}

TEST(SPDWriter, FullRamUploadsPastTheCaptureBuffers) {
    Fixture f;
    f.options.fullRam = true;
    auto result = f.write({{0, 0x180, 1}});
    ASSERT_TRUE(result.error.empty());
    EXPECT_EQ(result.sampleBase, 0x1000u);
    EXPECT_EQ(result.sampleSize, PCSX::SPD::c_spuRamSize - 0x1000u);
}
