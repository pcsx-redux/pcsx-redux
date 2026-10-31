/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
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

#include "core/sstate.h"

#include <algorithm>
#include <iterator>

#include "core/alloc-tracker.h"
#include "core/callstacks.h"
#include "core/cdrom.h"
#include "core/gpu.h"
#include "core/mdec.h"
#include "core/pad.h"
#include "core/psxcounters.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/sio.h"
#include "spu/interface.h"

namespace {

void serializePad(PCSX::SaveStates::PadTransfer& dst, PCSX::Pads::Port port) {
    using namespace PCSX::SaveStates;
    PCSX::Pads::TransferState pad;
    PCSX::g_emulator->m_pads->getTransferState(port, pad);
    dst.get<PadPresent>().value = true;
    dst.get<PadCmd>().value = pad.cmd;
    dst.get<PadConfigMode>().value = pad.configMode;
    dst.get<PadAnalogMode>().value = pad.analogMode;
    dst.get<PadBufferLen>().value = pad.bufferLen;
    dst.get<PadCurrentByte>().value = pad.currentByte;
    dst.get<PadBuffer>().copyFrom(pad.buf);
    dst.get<PadMotorMapping>().copyFrom(pad.motorMapping);
    dst.get<PadConfigCommandsUsed>().value = pad.configCommandsUsed;
    dst.get<PadOldMethodLatch>().value = pad.oldMethodLatch;
}

// A state from before the pads were serialised has no pad message at all, and the pads then keep
// whatever they had, as they always did.
void deserializePad(const PCSX::SaveStates::PadTransfer& src, PCSX::Pads::Port port) {
    using namespace PCSX::SaveStates;
    if (!src.get<PadPresent>().value) return;
    PCSX::Pads::TransferState pad;
    pad.cmd = src.get<PadCmd>().value;
    pad.configMode = src.get<PadConfigMode>().value;
    pad.analogMode = src.get<PadAnalogMode>().value;
    pad.bufferLen = src.get<PadBufferLen>().value;
    pad.currentByte = src.get<PadCurrentByte>().value;
    src.get<PadBuffer>().copyTo(pad.buf);
    src.get<PadMotorMapping>().copyTo(pad.motorMapping);
    pad.configCommandsUsed = src.get<PadConfigCommandsUsed>().value;
    pad.oldMethodLatch = src.get<PadOldMethodLatch>().value;
    PCSX::g_emulator->m_pads->setTransferState(port, pad);
}

void serializePads(PCSX::SaveStates::SaveState& state) {
    using namespace PCSX::SaveStates;
    auto& sio = state.get<SIOField>();
    serializePad(sio.get<SIOPad1>(), PCSX::Pads::Port::Port1);
    serializePad(sio.get<SIOPad2>(), PCSX::Pads::Port::Port2);
}

void deserializePads(const PCSX::SaveStates::SaveState& state) {
    using namespace PCSX::SaveStates;
    const auto& sio = state.get<SIOField>();
    deserializePad(sio.get<SIOPad1>(), PCSX::Pads::Port::Port1);
    deserializePad(sio.get<SIOPad2>(), PCSX::Pads::Port::Port2);
}

}  // namespace

PCSX::SaveStates::SaveState PCSX::SaveStates::constructSaveState() { return Access::constructSaveState(); }

PCSX::SaveStates::SaveState PCSX::SaveStates::Access::constructSaveState() {
    auto* cd = g_emulator->m_cdrom.get();
    return SaveState{
        SaveStateInfo{
            VersionString{},
            Version{},
        },
        Thumbnail{},
        Memory{
            RAM{g_emulator->m_mem->m_wram},
            ROM{g_emulator->m_mem->m_bios},
            EXP1{g_emulator->m_mem->m_exp1},
            HardwareMemory{g_emulator->m_mem->m_hard},
            SRAM{g_emulator->m_mem->m_sram},
            BIU{},
            HasBIU{},
        },
        Registers{
            GPR{g_emulator->m_cpu->m_regs.GPR.r},
            CP0{g_emulator->m_cpu->m_regs.CP0.r},
            CP2D{g_emulator->m_cpu->m_regs.CP2D.r},
            CP2C{g_emulator->m_cpu->m_regs.CP2C.r},
            PC{g_emulator->m_cpu->m_regs.pc.raw()},
            Code{g_emulator->m_cpu->m_regs.code},
            Cycle{g_emulator->m_cpu->m_regs.cycle},
            ScheduleMask{g_emulator->m_cpu->m_regs.scheduleMask},
            ICacheAddr{g_emulator->m_cpu->m_regs.iCacheAddr},
            ICacheCode{g_emulator->m_cpu->m_regs.iCacheCode},
            NextIsDelaySlot{g_emulator->m_cpu->m_nextIsDelaySlot},
            DelaySlotInfo1{
                DelaySlotIndex{g_emulator->m_cpu->m_delayedLoadInfo[0].index},
                DelaySlotValue{g_emulator->m_cpu->m_delayedLoadInfo[0].value},
                DelaySlotMask{g_emulator->m_cpu->m_delayedLoadInfo[0].mask},
                DelaySlotPcValue{g_emulator->m_cpu->m_delayedLoadInfo[0].pcValue},
                DelaySlotActive{g_emulator->m_cpu->m_delayedLoadInfo[0].active},
                DelaySlotPcActive{g_emulator->m_cpu->m_delayedLoadInfo[0].pcActive},
                DelaySlotFromLink{g_emulator->m_cpu->m_delayedLoadInfo[0].fromLink},
            },
            DelaySlotInfo2{
                DelaySlotIndex{g_emulator->m_cpu->m_delayedLoadInfo[1].index},
                DelaySlotValue{g_emulator->m_cpu->m_delayedLoadInfo[1].value},
                DelaySlotMask{g_emulator->m_cpu->m_delayedLoadInfo[1].mask},
                DelaySlotPcValue{g_emulator->m_cpu->m_delayedLoadInfo[1].pcValue},
                DelaySlotActive{g_emulator->m_cpu->m_delayedLoadInfo[1].active},
                DelaySlotPcActive{g_emulator->m_cpu->m_delayedLoadInfo[1].pcActive},
                DelaySlotFromLink{g_emulator->m_cpu->m_delayedLoadInfo[1].fromLink},
            },
            CurrentDelayedLoad{g_emulator->m_cpu->m_currentDelayedLoad},
            ScheduleTargetsField{g_emulator->m_cpu->m_regs.scheduleTargets},
            InISR{g_emulator->m_cpu->m_inISR},
            MulDivReady{g_emulator->m_cpu->m_regs.muldivReady},
            GteReady{g_emulator->m_cpu->m_regs.gteReady},
            GtePending1{
                GtePendingReg{g_emulator->m_cpu->m_gtePending[0].reg},
                GtePendingValue{g_emulator->m_cpu->m_gtePending[0].value},
                GtePendingCtrl{g_emulator->m_cpu->m_gtePending[0].ctrl},
                GtePendingCountdown{g_emulator->m_cpu->m_gtePending[0].countdown},
            },
            GtePending2{
                GtePendingReg{g_emulator->m_cpu->m_gtePending[1].reg},
                GtePendingValue{g_emulator->m_cpu->m_gtePending[1].value},
                GtePendingCtrl{g_emulator->m_cpu->m_gtePending[1].ctrl},
                GtePendingCountdown{g_emulator->m_cpu->m_gtePending[1].countdown},
            },
            GtePending3{
                GtePendingReg{g_emulator->m_cpu->m_gtePending[2].reg},
                GtePendingValue{g_emulator->m_cpu->m_gtePending[2].value},
                GtePendingCtrl{g_emulator->m_cpu->m_gtePending[2].ctrl},
                GtePendingCountdown{g_emulator->m_cpu->m_gtePending[2].countdown},
            },
            GtePendingCount{g_emulator->m_cpu->m_gtePendingCount},
            NextIsBranchSlot{g_emulator->m_cpu->m_nextIsBranchSlot},
            NextSlotBranchPC{g_emulator->m_cpu->m_nextSlotBranchPC},
        },
        GPU{},
        SPU{},
        SIO{
            SIOBuffer{g_emulator->m_sio->m_buffer},
            SIOStatusReg{g_emulator->m_sio->m_regs.status},
            SIOModeReg{g_emulator->m_sio->m_regs.mode},
            SIOCtrlReg{g_emulator->m_sio->m_regs.control},
            SIOBaudReg{g_emulator->m_sio->m_regs.baud},
            SIOBufferMaxIndex{g_emulator->m_sio->m_maxBufferIndex},
            SIOBufferIndex{g_emulator->m_sio->m_bufferIndex},
            SIOPadState{g_emulator->m_sio->m_padState},
            SIOCurrentDevice{g_emulator->m_sio->m_currentDevice},
            SIOPadAddress{g_emulator->m_sio->m_padAddress},
            SIOMCD1TempBuffer{g_emulator->m_sio->m_memoryCard[0].m_tempBuffer},
            SIOMCD1DirectoryFlag{g_emulator->m_sio->m_memoryCard[0].m_directoryFlag},
            SIOMCD1ChecksumIn{g_emulator->m_sio->m_memoryCard[0].m_checksumIn},
            SIOMCD1ChecksumOut{g_emulator->m_sio->m_memoryCard[0].m_checksumOut},
            SIOMCD1CommandTicks{g_emulator->m_sio->m_memoryCard[0].m_commandTicks},
            SIOMCD1CurrentCommand{g_emulator->m_sio->m_memoryCard[0].m_currentCommand},
            SIOMCD1Sector{g_emulator->m_sio->m_memoryCard[0].m_sector},
            SIOMCD1DataOffset{g_emulator->m_sio->m_memoryCard[0].m_dataOffset},
            SIOMCD2TempBuffer{g_emulator->m_sio->m_memoryCard[1].m_tempBuffer},
            SIOMCD2DirectoryFlag{g_emulator->m_sio->m_memoryCard[1].m_directoryFlag},
            SIOMCD2ChecksumIn{g_emulator->m_sio->m_memoryCard[1].m_checksumIn},
            SIOMCD2ChecksumOut{g_emulator->m_sio->m_memoryCard[1].m_checksumOut},
            SIOMCD2CommandTicks{g_emulator->m_sio->m_memoryCard[1].m_commandTicks},
            SIOMCD2CurrentCommand{g_emulator->m_sio->m_memoryCard[1].m_currentCommand},
            SIOMCD2Sector{g_emulator->m_sio->m_memoryCard[1].m_sector},
            SIOMCD2DataOffset{g_emulator->m_sio->m_memoryCard[1].m_dataOffset},
            SIOMCD1BTempBuffer{g_emulator->m_sio->m_tapMemoryCard[0].m_tempBuffer},
            SIOMCD1BDirectoryFlag{g_emulator->m_sio->m_tapMemoryCard[0].m_directoryFlag},
            SIOMCD1BChecksumIn{g_emulator->m_sio->m_tapMemoryCard[0].m_checksumIn},
            SIOMCD1BChecksumOut{g_emulator->m_sio->m_tapMemoryCard[0].m_checksumOut},
            SIOMCD1BCommandTicks{g_emulator->m_sio->m_tapMemoryCard[0].m_commandTicks},
            SIOMCD1BCurrentCommand{g_emulator->m_sio->m_tapMemoryCard[0].m_currentCommand},
            SIOMCD1BSector{g_emulator->m_sio->m_tapMemoryCard[0].m_sector},
            SIOMCD1BDataOffset{g_emulator->m_sio->m_tapMemoryCard[0].m_dataOffset},
            SIOMCD1CTempBuffer{g_emulator->m_sio->m_tapMemoryCard[1].m_tempBuffer},
            SIOMCD1CDirectoryFlag{g_emulator->m_sio->m_tapMemoryCard[1].m_directoryFlag},
            SIOMCD1CChecksumIn{g_emulator->m_sio->m_tapMemoryCard[1].m_checksumIn},
            SIOMCD1CChecksumOut{g_emulator->m_sio->m_tapMemoryCard[1].m_checksumOut},
            SIOMCD1CCommandTicks{g_emulator->m_sio->m_tapMemoryCard[1].m_commandTicks},
            SIOMCD1CCurrentCommand{g_emulator->m_sio->m_tapMemoryCard[1].m_currentCommand},
            SIOMCD1CSector{g_emulator->m_sio->m_tapMemoryCard[1].m_sector},
            SIOMCD1CDataOffset{g_emulator->m_sio->m_tapMemoryCard[1].m_dataOffset},
            SIOMCD1DTempBuffer{g_emulator->m_sio->m_tapMemoryCard[2].m_tempBuffer},
            SIOMCD1DDirectoryFlag{g_emulator->m_sio->m_tapMemoryCard[2].m_directoryFlag},
            SIOMCD1DChecksumIn{g_emulator->m_sio->m_tapMemoryCard[2].m_checksumIn},
            SIOMCD1DChecksumOut{g_emulator->m_sio->m_tapMemoryCard[2].m_checksumOut},
            SIOMCD1DCommandTicks{g_emulator->m_sio->m_tapMemoryCard[2].m_commandTicks},
            SIOMCD1DCurrentCommand{g_emulator->m_sio->m_tapMemoryCard[2].m_currentCommand},
            SIOMCD1DSector{g_emulator->m_sio->m_tapMemoryCard[2].m_sector},
            SIOMCD1DDataOffset{g_emulator->m_sio->m_tapMemoryCard[2].m_dataOffset},
            SIOMCD2BTempBuffer{g_emulator->m_sio->m_tapMemoryCard[3].m_tempBuffer},
            SIOMCD2BDirectoryFlag{g_emulator->m_sio->m_tapMemoryCard[3].m_directoryFlag},
            SIOMCD2BChecksumIn{g_emulator->m_sio->m_tapMemoryCard[3].m_checksumIn},
            SIOMCD2BChecksumOut{g_emulator->m_sio->m_tapMemoryCard[3].m_checksumOut},
            SIOMCD2BCommandTicks{g_emulator->m_sio->m_tapMemoryCard[3].m_commandTicks},
            SIOMCD2BCurrentCommand{g_emulator->m_sio->m_tapMemoryCard[3].m_currentCommand},
            SIOMCD2BSector{g_emulator->m_sio->m_tapMemoryCard[3].m_sector},
            SIOMCD2BDataOffset{g_emulator->m_sio->m_tapMemoryCard[3].m_dataOffset},
            SIOMCD2CTempBuffer{g_emulator->m_sio->m_tapMemoryCard[4].m_tempBuffer},
            SIOMCD2CDirectoryFlag{g_emulator->m_sio->m_tapMemoryCard[4].m_directoryFlag},
            SIOMCD2CChecksumIn{g_emulator->m_sio->m_tapMemoryCard[4].m_checksumIn},
            SIOMCD2CChecksumOut{g_emulator->m_sio->m_tapMemoryCard[4].m_checksumOut},
            SIOMCD2CCommandTicks{g_emulator->m_sio->m_tapMemoryCard[4].m_commandTicks},
            SIOMCD2CCurrentCommand{g_emulator->m_sio->m_tapMemoryCard[4].m_currentCommand},
            SIOMCD2CSector{g_emulator->m_sio->m_tapMemoryCard[4].m_sector},
            SIOMCD2CDataOffset{g_emulator->m_sio->m_tapMemoryCard[4].m_dataOffset},
            SIOMCD2DTempBuffer{g_emulator->m_sio->m_tapMemoryCard[5].m_tempBuffer},
            SIOMCD2DDirectoryFlag{g_emulator->m_sio->m_tapMemoryCard[5].m_directoryFlag},
            SIOMCD2DChecksumIn{g_emulator->m_sio->m_tapMemoryCard[5].m_checksumIn},
            SIOMCD2DChecksumOut{g_emulator->m_sio->m_tapMemoryCard[5].m_checksumOut},
            SIOMCD2DCommandTicks{g_emulator->m_sio->m_tapMemoryCard[5].m_commandTicks},
            SIOMCD2DCurrentCommand{g_emulator->m_sio->m_tapMemoryCard[5].m_currentCommand},
            SIOMCD2DSector{g_emulator->m_sio->m_tapMemoryCard[5].m_sector},
            SIOMCD2DDataOffset{g_emulator->m_sio->m_tapMemoryCard[5].m_dataOffset},
            SIOPad1{},
            SIOPad2{},
        },
        Hardware{},
        Counters{},
        MDEC{},
        PCdrvFilesField{},
        CallStacks{},
        CDRom{
            CDDataFIFO{cd->m_dataFIFO},
            CDDataFIFOIndex{cd->m_dataFIFOIndex},
            CDDataFIFOSize{cd->m_dataFIFOSize},
            CDDataFIFOPending{cd->m_dataFIFOPending},
            CDRegisterAddress{cd->m_registerAddress},
            CDMotorOn{cd->m_motorOn},
            CDSpeedChanged{cd->m_speedChanged},
            CDInvalidLocL{cd->m_invalidLocL},
            CDDataRequested{cd->m_dataRequested},
            CDSubheaderFilter{cd->m_subheaderFilter},
            CDRealtime{cd->m_realtime},
            CDReadingState{reinterpret_cast<uint8_t&>(cd->m_readingState)},
            CDStartPlaying{cd->m_startPlaying},
            CDReadingType{reinterpret_cast<uint8_t&>(cd->m_readingType)},
            CDAutoPause{cd->m_autoPause},
            CDReport{cd->m_report},
            CDSetLocPending{cd->m_setLocPending},
            CDMuted{cd->m_muted},
            CDPeakFlag{cd->m_peakFlag},
            CDPlayTrack{cd->m_playTrack},
            CDPlayStartCycle{cd->m_playStartCycle},
            CDStatus{reinterpret_cast<uint8_t&>(cd->m_status)},
            CDSpeed{reinterpret_cast<uint8_t&>(cd->m_speed)},
            CDReadSpan{reinterpret_cast<uint8_t&>(cd->m_readSpan)},
            CDInterruptCauseMask{cd->m_interruptCauseMask},
            CDATV{cd->m_atv},
            CDATVPending{cd->m_atvPending},
            CDADPCMMuted{cd->m_adpcmMuted},
            CDMode{cd->m_mode},
            CDFilterFile{cd->m_filterFile},
            CDFilterChannel{cd->m_filterChannel},
            CDXAEnded{cd->m_xaEnded},
            CDXAFirstSector{cd->m_xaFirstSector},
            CDSoundMapEnabled{cd->m_soundMapEnabled},
            CDCurrentPosition{CDMSF{CDMSFMinute{cd->m_currentPosition.m}, CDMSFSecond{cd->m_currentPosition.s},
                                    CDMSFFrame{cd->m_currentPosition.f}}},
            CDSeekPosition{CDMSF{CDMSFMinute{cd->m_seekPosition.m}, CDMSFSecond{cd->m_seekPosition.s},
                                 CDMSFFrame{cd->m_seekPosition.f}}},
            CDLastLocP{cd->m_lastLocP},
            CDLastLocL{cd->m_lastLocL},
            CDSeed{cd->m_seed},
            CDLidOpen{cd->m_lidOpen},
            CDWasLidOpened{cd->m_wasLidOpened},
            CDLidCloseScheduled{cd->m_lidCloseScheduled},
            CDLidCloseAtCycles{cd->m_lidCloseAtCycles},
            CDCommandFifo{CDQueueElement{
                CDQueuePayload{cd->m_commandFifo.payload},
                CDQueueValue{cd->m_commandFifo.value},
                CDQueueValueRead{cd->m_commandFifo.valueRead},
                CDQueueHasValue{cd->m_commandFifo.hasValue},
                CDQueueHitMax{cd->m_commandFifo.hitMax},
                CDQueuePayloadSize{cd->m_commandFifo.payloadSize},
                CDQueuePayloadIndex{cd->m_commandFifo.payloadIndex},
            }},
            CDCommandExecuting{CDQueueElement{
                CDQueuePayload{cd->m_commandExecuting.payload},
                CDQueueValue{cd->m_commandExecuting.value},
                CDQueueValueRead{cd->m_commandExecuting.valueRead},
                CDQueueHasValue{cd->m_commandExecuting.hasValue},
                CDQueueHitMax{cd->m_commandExecuting.hitMax},
                CDQueuePayloadSize{cd->m_commandExecuting.payloadSize},
                CDQueuePayloadIndex{cd->m_commandExecuting.payloadIndex},
            }},
            CDResponseFifo0{CDQueueElement{
                CDQueuePayload{cd->m_responseFifo[0].payload},
                CDQueueValue{cd->m_responseFifo[0].value},
                CDQueueValueRead{cd->m_responseFifo[0].valueRead},
                CDQueueHasValue{cd->m_responseFifo[0].hasValue},
                CDQueueHitMax{cd->m_responseFifo[0].hitMax},
                CDQueuePayloadSize{cd->m_responseFifo[0].payloadSize},
                CDQueuePayloadIndex{cd->m_responseFifo[0].payloadIndex},
            }},
            CDResponseFifo1{CDQueueElement{
                CDQueuePayload{cd->m_responseFifo[1].payload},
                CDQueueValue{cd->m_responseFifo[1].value},
                CDQueueValueRead{cd->m_responseFifo[1].valueRead},
                CDQueueHasValue{cd->m_responseFifo[1].hasValue},
                CDQueueHitMax{cd->m_responseFifo[1].hitMax},
                CDQueuePayloadSize{cd->m_responseFifo[1].payloadSize},
                CDQueuePayloadIndex{cd->m_responseFifo[1].payloadIndex},
            }},
            CDXALeft{CDADPCM{CDADPCMY0{cd->m_xa.left.y0}, CDADPCMY1{cd->m_xa.left.y1}}},
            CDXARight{CDADPCM{CDADPCMY0{cd->m_xa.right.y0}, CDADPCMY1{cd->m_xa.right.y1}}},
            CDSectorReadSinceSeek{cd->m_sectorReadSinceSeek},
            CDScexCountersReset{cd->m_scexCountersReset},
        },
    };
}

namespace PCSX {
struct SaveStateWrapper {
    SaveStateWrapper(SaveStates::SaveState& state_) : state(state_) {}
    SaveStates::SaveState& state;
};
}  // namespace PCSX

std::string PCSX::SaveStates::save() {
    SaveState state = constructSaveState();
    SaveStateWrapper wrapper(state);

    state.get<SaveStateInfoField>().get<VersionString>().value = "PCSX-Redux SaveState v5";
    state.get<SaveStateInfoField>().get<Version>().value = 5;

    state.get<MemoryField>().get<BIU>().value = g_emulator->m_mem->getBIU();
    state.get<MemoryField>().get<HasBIU>().value = true;
    g_emulator->m_gpu->serialize(&wrapper);
    g_emulator->m_spu->save(state.get<SPUField>());

    g_emulator->m_counters->serialize(&wrapper);
    g_emulator->m_mdec->serialize(&wrapper);

    g_emulator->m_cpu->listAllPCdrvFiles(
        [&state](uint16_t fd, std::filesystem::path filename, bool create, size_t readPosition, size_t writePosition) {
            state.get<PCdrvFilesField>().value.emplace_back(fd, filename.string(), create, readPosition, writePosition,
                                                            true);
        });

    g_emulator->m_callStacks->serialize(&wrapper);
    serializePads(state);

    Protobuf::OutSlice slice;
    state.serialize(&slice);
    return slice.finalize();
}

void PCSX::SaveStates::capture(SaveState& state) {
    SaveStateWrapper wrapper(state);

    state.get<SaveStateInfoField>().get<VersionString>().value = "PCSX-Redux SaveState v4";
    state.get<SaveStateInfoField>().get<Version>().value = 4;

    /* The manual-hook camp - GPU, SPU, counters, MDEC, call stacks - already pulls live state
       into owned Field storage; that half was always snapshot-shaped and the byte encode was a
       separate layer on top. */
    state.get<MemoryField>().get<BIU>().value = g_emulator->m_mem->getBIU();
    state.get<MemoryField>().get<HasBIU>().value = true;
    g_emulator->m_gpu->serialize(&wrapper);
    g_emulator->m_spu->save(state.get<SPUField>());

    g_emulator->m_counters->serialize(&wrapper);
    g_emulator->m_mdec->serialize(&wrapper);

    state.get<PCdrvFilesField>().value.clear();
    g_emulator->m_cpu->listAllPCdrvFiles(
        [&state](uint16_t fd, std::filesystem::path filename, bool create, size_t readPosition, size_t writePosition) {
            state.get<PCdrvFilesField>().value.emplace_back(fd, filename.string(), create, readPosition, writePosition,
                                                            true);
        });

    g_emulator->m_callStacks->serialize(&wrapper);

    serializePads(state);

    /* And this is the half that never existed: the flat live -> copy for the ref/ptr fields. */
    state.capture();
}

void PCSX::SaveStates::restore(SaveState& state) {
    SaveStateWrapper wrapper(state);
    g_emulator->m_allocTracker->reset();

    /* load() gets code-cache correctness for free by resetting the whole cpu first. We don't
       reset, so we owe it explicitly: without this a recompiler happily runs blocks it
       translated from memory we just rewound out from under it. It also wipes the emulated
       instruction cache, which is part of the snapshot, so it has to run before commit()
       writes the saved one back. */
    g_emulator->m_cpu->invalidateCache();
    state.commit();
    g_emulator->m_cpu->m_regs.lowestTarget = g_emulator->m_cpu->m_regs.cycle;
    g_emulator->m_cpu->m_regs.previousCycles = g_emulator->m_cpu->m_regs.cycle;
    g_emulator->m_cpu->m_regs.pc.raw() &= ~3;
    if (state.get<MemoryField>().get<HasBIU>().value)
        g_emulator->m_mem->setBIU(state.get<MemoryField>().get<BIU>().value);
    g_emulator->m_gpu->deserialize(&wrapper);
    g_emulator->m_spu->load(state.get<SPUField>());
#if 0
    g_emulator->m_cdrom->load();
#endif

    g_emulator->m_counters->deserialize(&wrapper);
    g_emulator->m_mdec->deserialize(&wrapper);
    deserializePads(state);

    auto& xa = state.get<SPUField>().get<SaveStates::XAField>();

#if 0
    g_emulator->m_cdrom->m_xa.freq = xa.get<SaveStates::XAFrequency>().value;
    g_emulator->m_cdrom->m_xa.nbits = xa.get<SaveStates::XANBits>().value;
    g_emulator->m_cdrom->m_xa.nsamples = xa.get<SaveStates::XANSamples>().value;
    g_emulator->m_cdrom->m_xa.stereo = xa.get<SaveStates::XAStereo>().value;
    auto& left = xa.get<SaveStates::XAADPCMLeft>();
    g_emulator->m_cdrom->m_xa.left.y0 = left.get<SaveStates::ADPCMDecodeY0>().value;
    g_emulator->m_cdrom->m_xa.left.y1 = left.get<SaveStates::ADPCMDecodeY1>().value;
    auto& right = xa.get<SaveStates::XAADPCMRight>();
    g_emulator->m_cdrom->m_xa.right.y0 = right.get<SaveStates::ADPCMDecodeY0>().value;
    g_emulator->m_cdrom->m_xa.right.y1 = right.get<SaveStates::ADPCMDecodeY1>().value;
    xa.get<SaveStates::XAPCM>().copyTo(reinterpret_cast<uint8_t*>(g_emulator->m_cdrom->m_xa.pcm));
    g_emulator->m_spu->playADPCMchannel(&g_emulator->m_cdrom->m_xa);
#endif

    g_emulator->m_cpu->closeAllPCdrvFiles();
    for (auto& file : state.get<PCdrvFilesField>().value) {
        uint16_t fd = file.get<PCdrvFD>().value;
        std::string filename = file.get<PCdrvFilename>().value;
        bool create = file.get<PCdrvCreate>().value;
        ssize_t readPosition = 0;
        ssize_t writePosition = -1;
        if (file.get<PCdrvHasPositions>().value) {
            readPosition = file.get<PCdrvReadPosition>().value;
            writePosition = file.get<PCdrvWritePosition>().value;
        }
        g_emulator->m_cpu->restorePCdrvFile(filename, fd, create, readPosition, writePosition);
    }
    g_emulator->m_callStacks->deserialize(&wrapper);

    g_system->m_eventBus->signal(Events::ExecutionFlow::SaveStateLoaded{});
}

void PCSX::CallStacks::serialize(SaveStateWrapper* w) {
    using namespace SaveStates;
    auto& callstacks = w->state.get<SaveStates::CallStacksField>().get<CallStacksMessageField>().value;
    // capture() reuses messages, so whatever an earlier capture left here must go.
    callstacks.clear();
    for (auto& callstack : getCallstacks()) {
        SaveStates::CallStack sscallstack{};
        sscallstack.get<LowSP>().value = callstack.getLow();
        sscallstack.get<HighSP>().value = callstack.getHigh();
        sscallstack.get<PresumedRA>().value = callstack.ra;
        sscallstack.get<PresumedFP>().value = callstack.fp;
        sscallstack.get<CallstackIsCurrent>().value = &callstack == m_current;
        for (auto& call : callstack.calls) {
            sscallstack.get<Calls>().value.emplace_back(call.ra, call.sp, call.fp, call.shadow);
        }
        callstacks.emplace_back(sscallstack);
    }
    w->state.get<SaveStates::CallStacksField>().get<CallStacksCurrentSP>().value = m_currentSP;
}

static void setU32(uint8_t* ptr, uint32_t value) {
    ptr[0] = value & 0xff;
    ptr[1] = (value >> 8) & 0xff;
    ptr[2] = (value >> 16) & 0xff;
    ptr[3] = (value >> 24) & 0xff;
}

void PCSX::GPU::serialize(SaveStateWrapper* w) {
    using namespace SaveStates;
    auto& gpu = w->state.get<GPUField>();
    gpu.get<GPUStatus>() = readStatus();
    gpu.get<GPUVRam>().copyFrom(getVRAM().data<uint8_t>());
    gpu.get<GPUControl>().allocate();
    const auto control = gpu.get<GPUControl>().value;

    for (unsigned i = 0; i < 256; i++) {
        setU32(control + i * 4, m_statusControl[i]);
    }

    gpu.get<GPUEnvironment>().allocate();
    const auto env = gpu.get<GPUEnvironment>().value;
    for (unsigned i = 0; i < 6; i++) {
        setU32(env + i * 4, m_envRaw[i + 1]);
    }
}

void PCSX::MDEC::serialize(SaveStateWrapper* w) {
    using namespace SaveStates;
    uint8_t* base = (uint8_t*)&PCSX::g_emulator->m_mem->m_wram[0x100000];
    auto& mdecSave = w->state.get<MDECField>();

    mdecSave.get<MDECReg0>().value = mdec.reg0;
    mdecSave.get<MDECReg1>().value = mdec.reg1;
    mdecSave.get<MDECRl>().value = reinterpret_cast<uint8_t*>(mdec.rl) - base;
    mdecSave.get<MDECRlEnd>().value = reinterpret_cast<uint8_t*>(mdec.rl_end) - base;
    mdecSave.get<MDECBlockBufferPos>().value = mdec.block_buffer_pos ? mdec.block_buffer_pos - base : 0;
    mdecSave.get<MDECBlockBuffer>().copyFrom(mdec.block_buffer);
    mdecSave.get<MDECDMAADR>().value = mdec.pending_dma1.adr;
    mdecSave.get<MDECDMABCR>().value = mdec.pending_dma1.bcr;
    mdecSave.get<MDECDMACHCR>().value = mdec.pending_dma1.chcr;
    for (unsigned i = 0; i < 64; i++) {
        mdecSave.get<MDECIQY>().value[i].value = m_tables.iqY[i];
        mdecSave.get<MDECIQUV>().value[i].value = m_tables.iqUV[i];
        mdecSave.get<MDECQTY>().value[i].value = m_tables.qtY[i];
        mdecSave.get<MDECQTUV>().value[i].value = m_tables.qtUV[i];
        mdecSave.get<MDECScaleTable>().value[i].value = m_tables.scale[i];
    }
    // Only deserialisation counts elements, and deserialize() reads a zero count as a state from
    // before these fields existed. Say they are full, or an in-memory capture restores as one.
    mdecSave.get<MDECIQY>().count = 64;
    mdecSave.get<MDECIQUV>().count = 64;
    mdecSave.get<MDECQTY>().count = 64;
    mdecSave.get<MDECQTUV>().count = 64;
    mdecSave.get<MDECScaleTable>().count = 64;
    mdecSave.get<MDECCustomScale>().value = m_tables.customScale ? 1 : 0;
}

void PCSX::Counters::serialize(SaveStateWrapper* w) {
    using namespace SaveStates;
    auto& counters = w->state.get<CountersField>();
    for (unsigned i = 0; i < CounterQuantity; i++) {
        counters.get<Rcnts>().value[i].get<RcntMode>().value = m_rcnts[i].mode;
        counters.get<Rcnts>().value[i].get<RcntTarget>().value = m_rcnts[i].target;
        counters.get<Rcnts>().value[i].get<RcntRate>().value = m_rcnts[i].rate;
        counters.get<Rcnts>().value[i].get<RcntIRQ>().value = m_rcnts[i].irq;
        counters.get<Rcnts>().value[i].get<RcntCounterState>().value = m_rcnts[i].counterState;
        counters.get<Rcnts>().value[i].get<RcntIRQState>().value = m_rcnts[i].irqState;
        counters.get<Rcnts>().value[i].get<RcntCycle>().value = m_rcnts[i].cycle;
        counters.get<Rcnts>().value[i].get<RcntCycleStart>().value = m_rcnts[i].cycleStart;
        counters.get<Rcnts>().value[i].get<RcntGateStarted>().value = m_rcnts[i].gateStarted;
    }
    counters.get<HSyncCount>().value = m_hSyncCount;
    counters.get<PSXNextCounter>().value = m_psxNextCounter;
    counters.get<FieldCount>().value = m_fieldCount;
}

static bool parseSaveState(std::string_view data, PCSX::SaveStates::SaveState& state) {
    using namespace PCSX::SaveStates;
    PCSX::Protobuf::InSlice slice(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    try {
        state.deserialize(&slice, 0);
    } catch (...) {
        return false;
    }
    // v4 is v5 with 1MB of VRAM; GPU::deserialize restores it into the first 512 rows.
    const auto version = state.get<SaveStateInfoField>().get<Version>().value;
    if ((version != 4) && (version != 5)) {
        PCSX::g_system->printf("Unsupported save state version %u\n", version);
        return false;
    }
    return true;
}

bool PCSX::SaveStates::load(std::string_view data) {
    SaveState state = constructSaveState();
    if (!parseSaveState(data, state)) return false;

    SaveStateWrapper wrapper(state);
    PCSX::g_emulator->m_cpu->Reset();
    g_emulator->m_allocTracker->reset();
    state.commit();
    g_emulator->m_cdrom->defaultSeedIfMissing();
    g_emulator->m_cpu->m_regs.lowestTarget = g_emulator->m_cpu->m_regs.cycle;
    g_emulator->m_cpu->m_regs.previousCycles = g_emulator->m_cpu->m_regs.cycle;
    // x86-64 recompiler might make save states with an unaligned PC, since it ignores the bottom 2 bits
    // So we just force-align it here, since it's never meant to be misaligned
    g_emulator->m_cpu->m_regs.pc.raw() &= ~3;
    if (g_emulator->m_cpu->m_gtePendingCount > 3) g_emulator->m_cpu->m_gtePendingCount = 0;
    if (state.get<MemoryField>().get<HasBIU>().value)
        g_emulator->m_mem->setBIU(state.get<MemoryField>().get<BIU>().value);
    g_emulator->m_gpu->deserialize(&wrapper);
    g_emulator->m_spu->load(state.get<SPUField>());
#if 0
    g_emulator->m_cdrom->load();
#endif

    g_emulator->m_counters->deserialize(&wrapper);
    g_emulator->m_mdec->deserialize(&wrapper);
    deserializePads(state);

    auto& xa = state.get<SPUField>().get<SaveStates::XAField>();

#if 0
    g_emulator->m_cdrom->m_xa.freq = xa.get<SaveStates::XAFrequency>().value;
    g_emulator->m_cdrom->m_xa.nbits = xa.get<SaveStates::XANBits>().value;
    g_emulator->m_cdrom->m_xa.nsamples = xa.get<SaveStates::XANSamples>().value;
    g_emulator->m_cdrom->m_xa.stereo = xa.get<SaveStates::XAStereo>().value;
    auto& left = xa.get<SaveStates::XAADPCMLeft>();
    g_emulator->m_cdrom->m_xa.left.y0 = left.get<SaveStates::ADPCMDecodeY0>().value;
    g_emulator->m_cdrom->m_xa.left.y1 = left.get<SaveStates::ADPCMDecodeY1>().value;
    auto& right = xa.get<SaveStates::XAADPCMRight>();
    g_emulator->m_cdrom->m_xa.right.y0 = right.get<SaveStates::ADPCMDecodeY0>().value;
    g_emulator->m_cdrom->m_xa.right.y1 = right.get<SaveStates::ADPCMDecodeY1>().value;
    xa.get<SaveStates::XAPCM>().copyTo(reinterpret_cast<uint8_t*>(g_emulator->m_cdrom->m_xa.pcm));
    g_emulator->m_spu->playADPCMchannel(&g_emulator->m_cdrom->m_xa);
#endif

    g_emulator->m_cpu->closeAllPCdrvFiles();
    for (auto& file : state.get<PCdrvFilesField>().value) {
        uint16_t fd = file.get<PCdrvFD>().value;
        std::string filename = file.get<PCdrvFilename>().value;
        bool create = file.get<PCdrvCreate>().value;
        ssize_t readPosition = 0;
        ssize_t writePosition = -1;
        if (file.get<PCdrvHasPositions>().value) {
            readPosition = file.get<PCdrvReadPosition>().value;
            writePosition = file.get<PCdrvWritePosition>().value;
        }
        g_emulator->m_cpu->restorePCdrvFile(filename, fd, create, readPosition, writePosition);
    }
    g_emulator->m_callStacks->deserialize(&wrapper);

    g_system->m_eventBus->signal(Events::ExecutionFlow::SaveStateLoaded{});

    return true;
}

bool PCSX::SaveStates::loadSafe(std::string&& data) {
    // Inside Execute(), we're being called from something the CPU reached: a
    // Lua event listener (including a Pause listener fired by a breakpoint), an
    // ImGui menu drawn during vsync, the web server polled on hsync.
    // Counters::update() and branchTest() both cached
    // values before that call and go on using them afterwards, so swapping the
    // state here corrupts whatever those frames touch next. Let the main loop
    // do it once they're gone.
    if (g_system->inExecute()) {
        // Parse it now so a corrupt or foreign state is refused here, where the
        // caller can still report it, and not later from the main loop.
        SaveState state = constructSaveState();
        if (!parseSaveState(data, state)) return false;
        g_system->scheduleSaveStateLoad(std::move(data));
        return true;
    }
    return load(data);
}

void PCSX::CallStacks::deserialize(const SaveStateWrapper* w) {
    using namespace SaveStates;
    m_callstacks.destroyAll();

    auto& callstacks = w->state.get<CallStacksField>().get<CallStacksMessageField>().value;
    m_current = nullptr;

    for (auto& sscallstack : callstacks) {
        auto& calls = sscallstack.get<Calls>().value;
        uint32_t lowSP = sscallstack.get<LowSP>().value;
        uint32_t highSP = sscallstack.get<HighSP>().value;
        uint32_t ra = sscallstack.get<PresumedRA>().value;
        uint32_t fp = sscallstack.get<PresumedFP>().value;
        bool isCurrent = sscallstack.get<CallstackIsCurrent>().value;
        CallStack* callstack = new CallStack();
        callstack->ra = ra;
        callstack->fp = fp;
        for (auto& call : calls) {
            uint32_t ra = call.get<CallRA>().value;
            uint32_t sp = call.get<CallSP>().value;
            uint32_t fp = call.get<CallFP>().value;
            bool shadow = call.get<Shadow>().value;
            callstack->calls.push_back(new CallStack::Call(sp, fp, ra, shadow));
        }
        if (isCurrent) m_current = callstack;
        m_callstacks.insert(lowSP, highSP, callstack);
    }

    m_currentSP = w->state.get<SaveStates::CallStacksField>().get<CallStacksCurrentSP>().value;
}

static uint32_t getU32(const uint8_t* ptr) { return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24); }

// The replayed GP1(00h) resets the drawing environment; replay GP0(E1h)..GP0(E6h).
// A state saved without them still carries the E1h and E6h bits in GPUSTAT.
void PCSX::GPU::restoreEnvironment(const uint8_t* env, uint32_t status) {
    if (env) {
        for (unsigned i = 0; i < 6; i++) writeData(getU32(env + i * 4));
        return;
    }
    // Savestates from older builds can hold GPUSTAT.10 = 0 after a textured polygon even with
    // GP0(E1h).10 set, so it does not say whether drawing to the display area was allowed; restore it
    // as allowed.
    writeData(0xe1000400 | (status & 0x3ff) | ((status >> 4) & 0x800));
    writeData(0xe6000000 | ((status >> 11) & 3));
}

void PCSX::GPU::deserialize(const SaveStateWrapper* w) {
    using namespace SaveStates;
    reset();
    auto& gpu = w->state.get<GPUField>();
    restoreStatus(gpu.get<GPUStatus>().value);
    const auto control = gpu.get<GPUControl>().value;

    for (unsigned i = 0; i < 256; i++) {
        m_statusControl[i] = getU32(control + i * 4);
    }

    // GP1(09h) is not replayed through writeStatus below: an unwritten slot
    // reads 0, which would decode as GP1(00h). Restore the bank gate directly.
    drainRenderer();
    m_vram2MBGateOpen = m_statusControl[9] & 1;
    m_dmaDirection = (gpu.get<GPUStatus>().value >> 29) & 3;
    vramConfigChanged();

    // A v4 state only carries 1MB of VRAM. Restore it into the lower bank and leave
    // the upper one as clearVRAM() sets it on a fresh console of this fitment.
    const bool vram1MB = w->state.get<SaveStateInfoField>().get<Version>().value == 4;
    if (gpu.get<GPUVRam>().value) {
        if (vram1MB) clearVRAM();
        partialUpdateVRAM(0, 0, 1024, vram1MB ? 512 : 1024,
                          reinterpret_cast<const uint16_t*>(gpu.get<GPUVRam>().value));
    } else {
        clearVRAM();
    }

    // Replay from a copy: GP1(00h) rewrites the GP1(08h) slot with the reset display mode.
    uint32_t saved[256];
    std::copy(std::begin(m_statusControl), std::end(m_statusControl), saved);
    writeStatus(saved[0]);
    writeStatus(saved[1]);
    writeStatus(saved[2]);
    writeStatus(saved[3]);
    writeStatus(saved[8]);  // try to repair things
    writeStatus(saved[6]);
    writeStatus(saved[7]);
    writeStatus(saved[5]);
    writeStatus(saved[4]);

    restoreEnvironment(gpu.get<GPUEnvironment>().value, gpu.get<GPUStatus>().value);
    // The GP1 and environment replays above rebuild the status word from commands, which loses
    // whatever was set since by other paths (the texpage of the last textured primitive, the
    // interlace field). The saved word is what the cpu read; that is what it has to read again,
    // except for the bits readStatus() sets live (IRQ1, DMA request, ready for a command,
    // ready-for-VRAM): those stay as the replay derived them.
    constexpr uint32_t derived = 0x1f000000;
    restoreStatus((gpu.get<GPUStatus>().value & ~derived) | (readStatusInternal() & derived));
    // The replayed GP1(00h)/GP1(02h) above clear the IRQ1 flag; put back the saved one.
    m_irq1Flag = gpu.get<GPUStatus>().value & 0x01000000;
}

void PCSX::MDEC::deserialize(const SaveStateWrapper* w) {
    using namespace SaveStates;
    uint8_t* base = (uint8_t*)&g_emulator->m_mem->m_wram[0x100000];
    auto& mdecSave = w->state.get<MDECField>();

    mdec.reg0 = mdecSave.get<MDECReg0>().value;
    mdec.reg1 = mdecSave.get<MDECReg1>().value;
    mdec.rl = reinterpret_cast<uint16_t*>(mdecSave.get<MDECRl>().value + base);
    mdec.rl_end = reinterpret_cast<uint16_t*>(mdecSave.get<MDECRlEnd>().value + base);
    const auto& pos = mdecSave.get<MDECBlockBufferPos>().value;
    mdec.block_buffer_pos = pos ? pos + base : nullptr;
    mdecSave.get<MDECBlockBuffer>().copyTo(mdec.block_buffer);
    mdec.pending_dma1.adr = mdecSave.get<MDECDMAADR>().value;
    mdec.pending_dma1.bcr = mdecSave.get<MDECDMABCR>().value;
    mdec.pending_dma1.chcr = mdecSave.get<MDECDMACHCR>().value;
    for (unsigned i = 0; i < 64; i++) {
        m_tables.iqY[i] = mdecSave.get<MDECIQY>().value[i].value;
        m_tables.iqUV[i] = mdecSave.get<MDECIQUV>().value[i].value;
    }
    // Fields 12-15 were added without a version bump. A state from before them
    // deserialises zero elements into the repeated fields, which would otherwise
    // leave all-zero quant tables and a zero scale matrix. Those emulators always
    // decoded with the standard matrix; the raw quant tables are recovered from
    // the cached ones, which do carry whatever the game uploaded.
    if (mdecSave.get<MDECQTY>().count == 0 || mdecSave.get<MDECQTUV>().count == 0 ||
        mdecSave.get<MDECScaleTable>().count == 0) {
        standardTables_init();
        qtab_fromIqtab(m_tables.qtY, m_tables.iqY);
        qtab_fromIqtab(m_tables.qtUV, m_tables.iqUV);
        return;
    }
    for (unsigned i = 0; i < 64; i++) {
        m_tables.qtY[i] = static_cast<uint8_t>(mdecSave.get<MDECQTY>().value[i].value);
        m_tables.qtUV[i] = static_cast<uint8_t>(mdecSave.get<MDECQTUV>().value[i].value);
        m_tables.scale[i] = static_cast<int16_t>(mdecSave.get<MDECScaleTable>().value[i].value);
    }
    m_tables.customScale = mdecSave.get<MDECCustomScale>().value != 0;
    // Not serialised: an all-zero matrix is what a console holds before MDEC(3).
    scaleTableUploaded =
        std::any_of(std::begin(m_tables.scale), std::end(m_tables.scale), [](int16_t v) { return v != 0; });
    warnedNoScaleTable = false;
}

void PCSX::Counters::deserialize(const SaveStateWrapper* w) {
    using namespace SaveStates;
    auto& counters = w->state.get<CountersField>();
    for (unsigned i = 0; i < CounterQuantity; i++) {
        m_rcnts[i].mode = counters.get<Rcnts>().value[i].get<RcntMode>().value;
        m_rcnts[i].target = counters.get<Rcnts>().value[i].get<RcntTarget>().value;
        m_rcnts[i].rate = counters.get<Rcnts>().value[i].get<RcntRate>().value;
        m_rcnts[i].irq = counters.get<Rcnts>().value[i].get<RcntIRQ>().value;
        m_rcnts[i].counterState = counters.get<Rcnts>().value[i].get<RcntCounterState>().value;
        m_rcnts[i].irqState = counters.get<Rcnts>().value[i].get<RcntIRQState>().value;
        m_rcnts[i].cycle = counters.get<Rcnts>().value[i].get<RcntCycle>().value;
        m_rcnts[i].cycleStart = counters.get<Rcnts>().value[i].get<RcntCycleStart>().value;
        m_rcnts[i].gateStarted = counters.get<Rcnts>().value[i].get<RcntGateStarted>().value;
    }
    m_hSyncCount = counters.get<HSyncCount>().value;
    m_psxNextCounter = counters.get<PSXNextCounter>().value;
    m_fieldCount = counters.get<FieldCount>().value;

    calculateHsync();
    // Recalculate rates from mode registers (handles overclock changes and dotclock)
    m_rcnts[3].target =
        (g_emulator->m_psxClockSpeed / (FrameRate[g_emulator->settings.get<Emulator::SettingVideo>()] *
                                        m_HSyncTotal[g_emulator->settings.get<Emulator::SettingVideo>()]));
    for (unsigned i = 0; i < 3; i++) {
        recalculateRate(i);
    }

    m_audioFrames = g_emulator->m_spu->getCurrentFrames();
}
