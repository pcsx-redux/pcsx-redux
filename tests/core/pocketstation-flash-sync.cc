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

// A PocketStation docked in slot 1 keeps its own copy of the card image. Sectors the device
// writes have to reach the card image (and the card file), and edits of the card image on the
// host side have to reach the device. Both directions are driven through the SIO registers, so
// every byte goes through MemoryCard::transceive into the device's COM port, with OpenPSK as
// the kernel. Build it with `make -C src/pocketstation/openpsk`, or point
// PCSX_POCKETSTATION_KERNEL at another 16 KiB image; without one the tests are skipped.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/arguments.h"
#include "core/commandline.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/sio.h"
#include "core/system.h"
#include "gtest/gtest.h"
#include "pocketstation/pocketstation.h"

namespace {

constexpr size_t c_sector = 128;
constexpr size_t c_cardSize = 1024 * c_sector;
// PSX cycles per byte, about one byte time at the BIOS's 250 kHz card clock.
constexpr uint64_t c_byteCycles = 1100;

const char* const c_argv[] = {"pcsx-redux", "-testmode"};

class TestSystem final : public PCSX::System {
  public:
    TestSystem() : m_commandLine(2, c_argv), m_args(m_commandLine) {}
    void softReset() override {}
    void hardReset() override {}
    void biosPutc(int) override {}
    const PCSX::Arguments& getArgs() const override { return m_args; }
    void printf(std::string&&) override {}
    void log(PCSX::LogClass, std::string&&) override {}
    void message(std::string&&) override {}
    void luaMessage(const std::string&, bool) override {}
    void update(bool) override {}
    void close() override {}
    void purgeAllEvents() override {}
    void testQuit(int) override {}

  private:
    PCSX::CommandLine m_commandLine;
    PCSX::Arguments m_args;
};

std::filesystem::path findKernel() {
    if (const char* env = std::getenv("PCSX_POCKETSTATION_KERNEL")) return env;
    std::filesystem::path dir = std::filesystem::current_path();
    while (true) {
        auto maybe = dir / "src" / "pocketstation" / "openpsk" / "openpsk.bin";
        if (std::filesystem::exists(maybe)) return maybe;
        if (!dir.has_parent_path() || dir.parent_path() == dir) return {};
        dir = dir.parent_path();
    }
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> pattern(uint8_t seed, size_t size) {
    std::vector<uint8_t> v(size);
    uint32_t x = 0x12345678u ^ seed;
    for (auto& b : v) {
        x = x * 1103515245u + 12345u;
        b = uint8_t(x >> 16);
    }
    return v;
}

class PocketStationFlashSync : public ::testing::Test {
  protected:
    void SetUp() override {
        m_kernel = findKernel();
        if (m_kernel.empty()) GTEST_SKIP() << "no PocketStation kernel image; build src/pocketstation/openpsk";
        std::error_code ec;
        ASSERT_EQ(std::filesystem::file_size(m_kernel, ec), 16384u) << m_kernel << " is not a 16 KiB kernel image";
        m_dir = std::filesystem::temp_directory_path() /
                ("pcsx-redux-psk-sync-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(m_dir);
        m_cardPath = m_dir / "card.mcd";
        m_initial = pattern(1, c_cardSize);
        {
            std::ofstream out(m_cardPath, std::ios::binary);
            out.write(reinterpret_cast<const char*>(m_initial.data()), m_initial.size());
        }

        m_oldSystem = PCSX::g_system;
        m_oldEmulator = PCSX::g_emulator;
        PCSX::g_system = &m_system;
        // Torn down by hand in TearDown: the destructor closes a Lua state this fixture never opens.
        m_emulator = new PCSX::Emulator();
        PCSX::g_emulator = m_emulator;
        auto& settings = m_emulator->settings;
        settings.get<PCSX::Emulator::SettingDynarec>().value = false;
        settings.get<PCSX::Emulator::SettingMcd1>().value = m_cardPath.u8string();
        settings.get<PCSX::Emulator::SettingMcd2>().value = (m_dir / "card2.mcd").u8string();
        settings.get<PCSX::Emulator::SettingMcd1Pocketstation>().value = true;
        settings.get<PCSX::Emulator::SettingPocketstationBios>().value = m_kernel.u8string();
        ASSERT_NE(m_emulator->m_mem->init(), -1);
        ASSERT_TRUE(PCSX::R3000Acpu::psxInit());
        m_emulator->m_cpu->m_regs.cycle = 0;
        m_sio = m_emulator->m_sio.get();
        m_sio->loadMcds(m_cardPath.u8string(), (m_dir / "card2.mcd").u8string());
        m_sio->togglePocketstationMode();
        m_card = m_sio->getMemoryCard(0);
        ASSERT_NE(m_card->getPocketstation(), nullptr);

        // Boot for a second; the device docks itself once its kernel arms the dock interrupt.
        run(33868800);
        ASSERT_TRUE(m_card->isPocketstationDocked());
    }

    void TearDown() override {
        if (m_emulator) {
            m_card = nullptr;
            m_emulator->m_sio->getMemoryCard(0)->disablePocketstation();
            PCSX::g_emulator = m_oldEmulator;
            PCSX::g_system = m_oldSystem;
        }
        std::error_code ec;
        if (!m_dir.empty()) std::filesystem::remove_all(m_dir, ec);
    }

    // Advance the R3000A clock and let the SIO clock the device by as much, as branchTest does.
    void run(uint64_t cycles) {
        constexpr uint64_t step = 10000;
        while (cycles) {
            const uint64_t n = std::min(cycles, step);
            m_emulator->m_cpu->m_regs.cycle += n;
            m_sio->stepPocketstation();
            cycles -= n;
        }
    }

    // One card transaction on port 1: /SEL, every byte through the SIO data register, /SEL off.
    std::vector<uint8_t> transaction(const std::vector<uint8_t>& tx) {
        m_sio->writeCtrl16(0x0007);  // TX enable, /SEL, RX enable, port 1
        run(2000);
        std::vector<uint8_t> rx;
        for (uint8_t b : tx) {
            m_sio->write8(b);
            rx.push_back(m_sio->read8());
            run(c_byteCycles);
        }
        m_sio->writeCtrl16(0x0000);
        run(100000);
        return rx;
    }

    std::vector<uint8_t> writeSector(unsigned sector, const uint8_t* data) {
        std::vector<uint8_t> tx = {0x81, 0x57, 0x00, 0x00, uint8_t(sector >> 8), uint8_t(sector)};
        uint8_t chk = uint8_t(sector >> 8) ^ uint8_t(sector);
        for (size_t i = 0; i < c_sector; i++) {
            tx.push_back(data[i]);
            chk ^= data[i];
        }
        tx.push_back(chk);
        tx.insert(tx.end(), {0x00, 0x00, 0x00, 0x00});
        return transaction(tx);
    }

    // Returns the 128 data bytes of the reply, located from the address echo, or nothing.
    std::vector<uint8_t> readSector(unsigned sector, std::vector<uint8_t>* raw) {
        std::vector<uint8_t> tx = {0x81, 0x52, 0x00, 0x00, uint8_t(sector >> 8), uint8_t(sector)};
        tx.resize(142, 0x00);
        *raw = transaction(tx);
        for (size_t i = 6; i + 2 + c_sector < raw->size(); i++) {
            if ((*raw)[i] == 0x5d && (*raw)[i + 1] == uint8_t(sector >> 8) && (*raw)[i + 2] == uint8_t(sector)) {
                return {raw->begin() + i + 3, raw->begin() + i + 3 + c_sector};
            }
        }
        return {};
    }

    static std::string hex(const std::vector<uint8_t>& v) {
        std::string s;
        char buf[4];
        for (uint8_t b : v) {
            snprintf(buf, sizeof(buf), "%02X ", b);
            s += buf;
        }
        return s;
    }

    const uint8_t* cardSector(unsigned sector) {
        return reinterpret_cast<const uint8_t*>(m_sio->getMcdData(1)) + sector * c_sector;
    }
    const uint8_t* flashSector(unsigned sector) { return m_card->getPocketstation()->flashData() + sector * c_sector; }

    TestSystem m_system;
    PCSX::System* m_oldSystem = nullptr;
    PCSX::Emulator* m_oldEmulator = nullptr;
    PCSX::Emulator* m_emulator = nullptr;
    PCSX::SIO* m_sio = nullptr;
    PCSX::MemoryCard* m_card = nullptr;
    std::filesystem::path m_kernel, m_dir, m_cardPath;
    std::vector<uint8_t> m_initial;
};

}  // namespace

TEST_F(PocketStationFlashSync, DeviceWriteReachesCardImageAndFile) {
    constexpr unsigned sector = 9;
    const auto data = pattern(2, c_sector);
    ASSERT_NE(memcmp(data.data(), m_initial.data() + sector * c_sector, c_sector), 0);

    const auto rx = writeSector(sector, data.data());
    // The device took the write: its flash holds the new sector. Without this the checks below
    // could fail for a write the kernel never accepted.
    ASSERT_EQ(memcmp(flashSector(sector), data.data(), c_sector), 0) << "write reply: " << hex(rx);

    EXPECT_EQ(memcmp(cardSector(sector), data.data(), c_sector), 0) << "card image did not get the device's write";
    const auto file = readFile(m_cardPath);
    ASSERT_EQ(file.size(), c_cardSize);
    EXPECT_EQ(memcmp(file.data() + sector * c_sector, data.data(), c_sector), 0)
        << "card file did not get the device's write";
    // Nothing else moved.
    EXPECT_EQ(memcmp(file.data(), m_initial.data(), sector * c_sector), 0);
}

TEST_F(PocketStationFlashSync, CardImageEditReachesDevice) {
    constexpr unsigned sector = 5;
    std::vector<uint8_t> raw;
    // Control: before any edit the device answers with the card's own sector.
    const auto before = readSector(sector, &raw);
    ASSERT_EQ(before.size(), c_sector) << "read reply: " << hex(raw);
    ASSERT_EQ(memcmp(before.data(), m_initial.data() + sector * c_sector, c_sector), 0) << hex(raw);

    // A host-side edit of the card image, as the memory card manager makes.
    const auto data = pattern(3, c_sector);
    memcpy(m_sio->getMcdData(1) + sector * c_sector, data.data(), c_sector);

    const auto after = readSector(sector, &raw);
    ASSERT_EQ(after.size(), c_sector) << "read reply: " << hex(raw);
    EXPECT_EQ(memcmp(after.data(), data.data(), c_sector), 0) << "device did not see the card image edit: " << hex(raw);
    EXPECT_EQ(memcmp(flashSector(sector), data.data(), c_sector), 0);
}
