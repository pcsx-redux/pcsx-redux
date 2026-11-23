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

// The read LUT maps the RAM window the guest's RAM_SIZE register asks the DRAM controller
// for, with the RAM behind it mirrored, and leaves the pages past it unmapped. The loader
// bound asks those same LUTs where a binary may go.

#include <cstdint>
#include <memory>
#include <string>

#include "core/arguments.h"
#include "core/commandline.h"
#include "core/psxemulator.h"
#include "core/psxhw.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "gtest/gtest.h"

namespace {

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

constexpr uint32_t c_1MB = 0x100000;

class MemoryRamSize : public ::testing::Test {
  protected:
    void SetUp() override {
        m_oldSystem = PCSX::g_system;
        m_oldEmulator = PCSX::g_emulator;
        PCSX::g_system = &m_system;
        m_emulator = std::make_unique<PCSX::Emulator>();
        PCSX::g_emulator = m_emulator.get();
        m_emulator->settings.get<PCSX::Emulator::SettingDynarec>().value = false;
        ASSERT_NE(m_emulator->m_mem->init(), -1);
        // HW::reset reaches into the counters, which need the CPU.
        ASSERT_TRUE(PCSX::R3000Acpu::psxInit());
        m_emulator->m_cpu->m_regs.cycle = 0;
        m_mem = m_emulator->m_mem.get();
    }
    void TearDown() override {
        if (m_emulator) {
            if (m_emulator->m_cpu) m_emulator->m_cpu->psxShutdown();
            m_emulator->m_mem->shutdown();
            m_emulator.reset();
        }
        PCSX::g_emulator = m_oldEmulator;
        PCSX::g_system = m_oldSystem;
    }
    void setRamSize(uint32_t value) { m_emulator->m_hw->write32(0x1f801060, value); }
    void* page(uint32_t p) { return m_mem->m_readLUT[p]; }
    // The last page of the window is mapped and the next one isn't, in all three segments.
    void expectWindow(uint32_t window) {
        const uint32_t last = (window >> 16) - 1;
        for (uint32_t segment : {0x0000u, 0x8000u, 0xa000u}) {
            EXPECT_NE(page(segment + last), nullptr) << std::hex << segment + last;
            if (window < 0x800000) EXPECT_EQ(page(segment + last + 1), nullptr) << std::hex << segment + last + 1;
        }
        EXPECT_EQ(page(0x0080), nullptr);
        EXPECT_EQ(page(0x8080), nullptr);
        EXPECT_EQ(page(0xa080), nullptr);
    }

    TestSystem m_system;
    PCSX::System* m_oldSystem = nullptr;
    PCSX::Emulator* m_oldEmulator = nullptr;
    std::unique_ptr<PCSX::Emulator> m_emulator;
    PCSX::Memory* m_mem = nullptr;
};

}  // namespace

TEST(MemoryRamWindow, DocumentedBankSizes) {
    // Bits 11-8 of RAM_SIZE, two banks of the same size.
    EXPECT_EQ(PCSX::Memory::ramWindow(0x0000), 1 * c_1MB);
    EXPECT_EQ(PCSX::Memory::ramWindow(0x0300), 4 * c_1MB);
    EXPECT_EQ(PCSX::Memory::ramWindow(0x0400), 2 * c_1MB);
    EXPECT_EQ(PCSX::Memory::ramWindow(0x0700), 8 * c_1MB);
    EXPECT_EQ(PCSX::Memory::ramWindow(0x0800), 2 * c_1MB);
    EXPECT_EQ(PCSX::Memory::ramWindow(0x0b00), 8 * c_1MB);
    EXPECT_EQ(PCSX::Memory::ramWindow(0x0c00), 4 * c_1MB);
    EXPECT_EQ(PCSX::Memory::ramWindow(0x0f00), 16 * c_1MB);
    // Only bits 8-11 count.
    EXPECT_EQ(PCSX::Memory::ramWindow(0x0b88), 8 * c_1MB);
    EXPECT_EQ(PCSX::Memory::ramWindow(0x0888), 2 * c_1MB);
    EXPECT_EQ(PCSX::Memory::ramWindow(0xfffff0ff), 1 * c_1MB);
}

TEST_F(MemoryRamSize, ResetValueKeepsTheWholeWindow) { expectWindow(8 * c_1MB); }

TEST_F(MemoryRamSize, Retail0B88MapsEightMegabytesOfMirrors) {
    setRamSize(0xb88);
    expectWindow(8 * c_1MB);
    // 2MB of RAM behind the window, so every 2MB repeats.
    EXPECT_EQ(page(0x0020), page(0x0000));
    EXPECT_EQ(page(0x0060), page(0x0000));
    EXPECT_EQ(page(0x807f), page(0x001f));
    EXPECT_NE(page(0x0001), page(0x0000));
}

TEST_F(MemoryRamSize, Value0888MapsTwoMegabytes) {
    setRamSize(0x888);
    expectWindow(2 * c_1MB);
}

TEST_F(MemoryRamSize, OtherDocumentedWindows) {
    setRamSize(0x088);
    expectWindow(1 * c_1MB);
    setRamSize(0x388);
    expectWindow(4 * c_1MB);
    EXPECT_EQ(page(0x0020), page(0x0000));
    setRamSize(0x488);
    expectWindow(2 * c_1MB);
    setRamSize(0xc88);
    expectWindow(4 * c_1MB);
    // 16MB is past what the LUTs cover; the first 8MB stay mapped.
    setRamSize(0xf88);
    expectWindow(8 * c_1MB);
}

TEST_F(MemoryRamSize, EightMegabytesBehindTheWindow) {
    m_emulator->settings.get<PCSX::Emulator::Setting8MB>().value = true;
    setRamSize(0xb88);
    expectWindow(8 * c_1MB);
    EXPECT_NE(page(0x0020), page(0x0000));
    EXPECT_EQ(m_mem->ramSize(), 8 * c_1MB);
    // Bit 8 clear asks for 2MB, whatever the setting says.
    setRamSize(0xa88);
    EXPECT_EQ(m_mem->ramSize(), 2 * c_1MB);
}

TEST_F(MemoryRamSize, HardwareResetRestoresTheWholeWindow) {
    setRamSize(0x888);
    expectWindow(2 * c_1MB);
    m_emulator->m_hw->reset();
    expectWindow(8 * c_1MB);
}

TEST_F(MemoryRamSize, WriteLUTFollowsTheReadLUT) {
    m_mem->setBIU(0x1e988);
    setRamSize(0x888);
    EXPECT_NE(m_mem->m_writeLUT[0x801f], nullptr);
    EXPECT_EQ(m_mem->m_writeLUT[0x8020], nullptr);
}

TEST_F(MemoryRamSize, LoadSpansInTheRetailWindow) {
    setRamSize(0xb88);
    EXPECT_FALSE(m_mem->checkLoadSpan(0x80010000, 0x800).has_value());
    EXPECT_FALSE(m_mem->checkLoadSpan(0x80600000, 0x800).has_value());
    EXPECT_FALSE(m_mem->checkLoadSpan(0x807ff800, 0x800).has_value());
    EXPECT_TRUE(m_mem->checkLoadSpan(0x807ff800, 0x804).has_value());
    EXPECT_FALSE(m_mem->checkLoadSpan(0x80000000, 2 * c_1MB).has_value());
    EXPECT_TRUE(m_mem->checkLoadSpan(0x80000000, 2 * c_1MB + 1).has_value());
    EXPECT_TRUE(m_mem->checkLoadSpan(0x80010000, 0x40000000).has_value());
    EXPECT_FALSE(m_mem->checkLoadSpan(0xbfc00000, 0x80000).has_value());
    EXPECT_TRUE(m_mem->checkLoadSpan(0xbfc7f800, 0x804).has_value());
    EXPECT_TRUE(m_mem->checkLoadSpan(0x1f800000, 0x100).has_value());
    EXPECT_TRUE(m_mem->checkLoadSpan(0xc0000000, 0x800).has_value());
    EXPECT_TRUE(m_mem->checkLoadSpan(0xfffff000, 0x2000).has_value());
}

TEST_F(MemoryRamSize, LoadSpansFollowTheWindow) {
    setRamSize(0x888);
    EXPECT_FALSE(m_mem->checkLoadSpan(0x801ff800, 0x800).has_value());
    EXPECT_TRUE(m_mem->checkLoadSpan(0x801ff800, 0x804).has_value());
    EXPECT_TRUE(m_mem->checkLoadSpan(0x80600000, 0x800).has_value());
}
