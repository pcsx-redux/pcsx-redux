/*
 * Every field of the CD-ROM save state has to come back out of a save state
 * exactly as it went in.
 */

#include <stdint.h>

#include <string>

#include "core/sstate.h"
#include "gtest/gtest.h"

namespace {

using namespace PCSX::SaveStates;

struct Queue {
    uint8_t payload[16] = {};
    uint8_t value = 0;
    bool valueRead = false;
    bool hasValue = false;
    bool hitMax = false;
    uint8_t payloadSize = 0;
    uint8_t payloadIndex = 0;
};

struct State {
    uint8_t datafifo[2352] = {};
    uint32_t datafifoindex = 0;
    uint32_t datafifosize = 0;
    uint32_t datafifopending = 0;
    uint8_t registeraddress = 0;
    bool motoron = false;
    bool speedchanged = false;
    bool invalidlocl = false;
    bool datarequested = false;
    bool subheaderfilter = false;
    bool realtime = false;
    uint8_t readingstate = 0;
    bool startplaying = false;
    uint8_t readingtype = 0;
    bool autopause = false;
    bool report = false;
    bool setlocpending = false;
    bool muted = false;
    bool peakflag = false;
    uint8_t playtrack = 0;
    uint32_t playstartcycle = 0;
    uint8_t status = 0;
    uint8_t speed = 0;
    uint8_t readspan = 0;
    uint8_t interruptcausemask = 0;
    uint8_t atv[4] = {};
    uint8_t atvpending[4] = {};
    bool adpcmmuted = false;
    uint8_t mode = 0;
    uint8_t filterfile = 0;
    uint8_t filterchannel = 0;
    bool xaended = false;
    bool xafirstsector = false;
    bool soundmapenabled = false;
    uint8_t currentposition[3] = {};
    uint8_t seekposition[3] = {};
    uint8_t lastlocp[8] = {};
    uint8_t lastlocl[8] = {};
    uint64_t seed = 0;
    bool lidopen = false;
    bool waslidopened = false;
    bool lidclosescheduled = false;
    uint32_t lidcloseatcycles = 0;
    Queue commandfifo;
    Queue commandexecuting;
    Queue responsefifo0;
    Queue responsefifo1;
    int32_t xaleft[2] = {};
    int32_t xaright[2] = {};
    bool sectorreadsinceseek = false;
    bool scexcountersreset = false;
};

CDRom bind(State& s) {
    return CDRom{
        CDDataFIFO{s.datafifo},
        CDDataFIFOIndex{s.datafifoindex},
        CDDataFIFOSize{s.datafifosize},
        CDDataFIFOPending{s.datafifopending},
        CDRegisterAddress{s.registeraddress},
        CDMotorOn{s.motoron},
        CDSpeedChanged{s.speedchanged},
        CDInvalidLocL{s.invalidlocl},
        CDDataRequested{s.datarequested},
        CDSubheaderFilter{s.subheaderfilter},
        CDRealtime{s.realtime},
        CDReadingState{s.readingstate},
        CDStartPlaying{s.startplaying},
        CDReadingType{s.readingtype},
        CDAutoPause{s.autopause},
        CDReport{s.report},
        CDSetLocPending{s.setlocpending},
        CDMuted{s.muted},
        CDPeakFlag{s.peakflag},
        CDPlayTrack{s.playtrack},
        CDPlayStartCycle{s.playstartcycle},
        CDStatus{s.status},
        CDSpeed{s.speed},
        CDReadSpan{s.readspan},
        CDInterruptCauseMask{s.interruptcausemask},
        CDATV{s.atv},
        CDATVPending{s.atvpending},
        CDADPCMMuted{s.adpcmmuted},
        CDMode{s.mode},
        CDFilterFile{s.filterfile},
        CDFilterChannel{s.filterchannel},
        CDXAEnded{s.xaended},
        CDXAFirstSector{s.xafirstsector},
        CDSoundMapEnabled{s.soundmapenabled},
        CDCurrentPosition{CDMSF{CDMSFMinute{s.currentposition[0]}, CDMSFSecond{s.currentposition[1]},
                                CDMSFFrame{s.currentposition[2]}}},
        CDSeekPosition{
            CDMSF{CDMSFMinute{s.seekposition[0]}, CDMSFSecond{s.seekposition[1]}, CDMSFFrame{s.seekposition[2]}}},
        CDLastLocP{s.lastlocp},
        CDLastLocL{s.lastlocl},
        CDSeed{s.seed},
        CDLidOpen{s.lidopen},
        CDWasLidOpened{s.waslidopened},
        CDLidCloseScheduled{s.lidclosescheduled},
        CDLidCloseAtCycles{s.lidcloseatcycles},
        CDCommandFifo{CDQueueElement{CDQueuePayload{s.commandfifo.payload}, CDQueueValue{s.commandfifo.value},
                                     CDQueueValueRead{s.commandfifo.valueRead}, CDQueueHasValue{s.commandfifo.hasValue},
                                     CDQueueHitMax{s.commandfifo.hitMax}, CDQueuePayloadSize{s.commandfifo.payloadSize},
                                     CDQueuePayloadIndex{s.commandfifo.payloadIndex}}},
        CDCommandExecuting{
            CDQueueElement{CDQueuePayload{s.commandexecuting.payload}, CDQueueValue{s.commandexecuting.value},
                           CDQueueValueRead{s.commandexecuting.valueRead}, CDQueueHasValue{s.commandexecuting.hasValue},
                           CDQueueHitMax{s.commandexecuting.hitMax}, CDQueuePayloadSize{s.commandexecuting.payloadSize},
                           CDQueuePayloadIndex{s.commandexecuting.payloadIndex}}},
        CDResponseFifo0{CDQueueElement{CDQueuePayload{s.responsefifo0.payload}, CDQueueValue{s.responsefifo0.value},
                                       CDQueueValueRead{s.responsefifo0.valueRead},
                                       CDQueueHasValue{s.responsefifo0.hasValue}, CDQueueHitMax{s.responsefifo0.hitMax},
                                       CDQueuePayloadSize{s.responsefifo0.payloadSize},
                                       CDQueuePayloadIndex{s.responsefifo0.payloadIndex}}},
        CDResponseFifo1{CDQueueElement{CDQueuePayload{s.responsefifo1.payload}, CDQueueValue{s.responsefifo1.value},
                                       CDQueueValueRead{s.responsefifo1.valueRead},
                                       CDQueueHasValue{s.responsefifo1.hasValue}, CDQueueHitMax{s.responsefifo1.hitMax},
                                       CDQueuePayloadSize{s.responsefifo1.payloadSize},
                                       CDQueuePayloadIndex{s.responsefifo1.payloadIndex}}},
        CDXALeft{CDADPCM{CDADPCMY0{s.xaleft[0]}, CDADPCMY1{s.xaleft[1]}}},
        CDXARight{CDADPCM{CDADPCMY0{s.xaright[0]}, CDADPCMY1{s.xaright[1]}}},
        CDSectorReadSinceSeek{s.sectorreadsinceseek},
        CDScexCountersReset{s.scexcountersreset},
    };
}

// No field may hold its default, so a field that is lost or swapped shows up.
void fill(State& s) {
    for (unsigned i = 0; i < 2352; i++) s.datafifo[i] = static_cast<uint8_t>(i * 7 + 2);
    s.datafifoindex = 0x80000000u + 314187;
    s.datafifosize = 0x80000000u + 418916;
    s.datafifopending = 0x80000000u + 523645;
    s.registeraddress = 79;
    s.motoron = true;
    s.speedchanged = true;
    s.invalidlocl = true;
    s.datarequested = true;
    s.subheaderfilter = true;
    s.realtime = true;
    s.readingstate = 92;
    s.startplaying = true;
    s.readingtype = 105;
    s.autopause = true;
    s.report = true;
    s.setlocpending = true;
    s.muted = true;
    s.peakflag = true;
    s.playtrack = 118;
    s.playstartcycle = 0x80000000u + 1047290;
    s.status = 144;
    s.speed = 157;
    s.readspan = 170;
    s.interruptcausemask = 183;
    for (unsigned i = 0; i < 4; i++) s.atv[i] = static_cast<uint8_t>(i * 7 + 15);
    for (unsigned i = 0; i < 4; i++) s.atvpending[i] = static_cast<uint8_t>(i * 7 + 16);
    s.adpcmmuted = true;
    s.mode = 222;
    s.filterfile = 235;
    s.filterchannel = 248;
    s.xaended = true;
    s.xafirstsector = true;
    s.soundmapenabled = true;
    for (unsigned i = 0; i < 3; i++) s.currentposition[i] = static_cast<uint8_t>(i * 7 + 20);
    for (unsigned i = 0; i < 3; i++) s.seekposition[i] = static_cast<uint8_t>(i * 7 + 21);
    for (unsigned i = 0; i < 8; i++) s.lastlocp[i] = static_cast<uint8_t>(i * 7 + 22);
    for (unsigned i = 0; i < 8; i++) s.lastlocl[i] = static_cast<uint8_t>(i * 7 + 23);
    s.seed = 0x8000000000000000ull + 24000072;
    s.lidopen = true;
    s.waslidopened = true;
    s.lidclosescheduled = true;
    s.lidcloseatcycles = 0x80000000u + 2618225;
    for (unsigned i = 0; i < 16; i++) s.commandfifo.payload[i] = static_cast<uint8_t>(i * 7 + 26);
    s.commandfifo.value = 101;
    s.commandfifo.valueRead = true;
    s.commandfifo.hasValue = true;
    s.commandfifo.hitMax = true;
    s.commandfifo.payloadSize = 114;
    s.commandfifo.payloadIndex = 127;
    for (unsigned i = 0; i < 16; i++) s.commandexecuting.payload[i] = static_cast<uint8_t>(i * 7 + 30);
    s.commandexecuting.value = 153;
    s.commandexecuting.valueRead = true;
    s.commandexecuting.hasValue = true;
    s.commandexecuting.hitMax = true;
    s.commandexecuting.payloadSize = 166;
    s.commandexecuting.payloadIndex = 179;
    for (unsigned i = 0; i < 16; i++) s.responsefifo0.payload[i] = static_cast<uint8_t>(i * 7 + 34);
    s.responsefifo0.value = 205;
    s.responsefifo0.valueRead = true;
    s.responsefifo0.hasValue = true;
    s.responsefifo0.hitMax = true;
    s.responsefifo0.payloadSize = 218;
    s.responsefifo0.payloadIndex = 231;
    for (unsigned i = 0; i < 16; i++) s.responsefifo1.payload[i] = static_cast<uint8_t>(i * 7 + 38);
    s.responsefifo1.value = 6;
    s.responsefifo1.valueRead = true;
    s.responsefifo1.hasValue = true;
    s.responsefifo1.hitMax = true;
    s.responsefifo1.payloadSize = 19;
    s.responsefifo1.payloadIndex = 32;
    s.xaleft[0] = -332598;
    s.xaleft[1] = -340517;
    s.xaright[0] = -348436;
    s.xaright[1] = -356355;
    s.sectorreadsinceseek = true;
    s.scexcountersreset = true;
}

}  // namespace

TEST(SaveStateCDRom, EveryFieldRoundTrips) {
    State in, out;
    fill(in);
    PCSX::Protobuf::OutSlice outSlice;
    bind(in).serialize(&outSlice);
    std::string data = outSlice.finalize();

    CDRom loaded = bind(out);
    PCSX::Protobuf::InSlice inSlice(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    loaded.deserialize(&inSlice, 0);
    loaded.commit();

    for (unsigned i = 0; i < 2352; i++) EXPECT_EQ(out.datafifo[i], in.datafifo[i]) << "datafifo[" << i << "]";
    EXPECT_EQ(out.datafifoindex, in.datafifoindex) << "datafifoindex";
    EXPECT_EQ(out.datafifosize, in.datafifosize) << "datafifosize";
    EXPECT_EQ(out.datafifopending, in.datafifopending) << "datafifopending";
    EXPECT_EQ(out.registeraddress, in.registeraddress) << "registeraddress";
    EXPECT_EQ(out.motoron, in.motoron) << "motoron";
    EXPECT_EQ(out.speedchanged, in.speedchanged) << "speedchanged";
    EXPECT_EQ(out.invalidlocl, in.invalidlocl) << "invalidlocl";
    EXPECT_EQ(out.datarequested, in.datarequested) << "datarequested";
    EXPECT_EQ(out.subheaderfilter, in.subheaderfilter) << "subheaderfilter";
    EXPECT_EQ(out.realtime, in.realtime) << "realtime";
    EXPECT_EQ(out.readingstate, in.readingstate) << "readingstate";
    EXPECT_EQ(out.startplaying, in.startplaying) << "startplaying";
    EXPECT_EQ(out.readingtype, in.readingtype) << "readingtype";
    EXPECT_EQ(out.autopause, in.autopause) << "autopause";
    EXPECT_EQ(out.report, in.report) << "report";
    EXPECT_EQ(out.setlocpending, in.setlocpending) << "setlocpending";
    EXPECT_EQ(out.muted, in.muted) << "muted";
    EXPECT_EQ(out.peakflag, in.peakflag) << "peakflag";
    EXPECT_EQ(out.playtrack, in.playtrack) << "playtrack";
    EXPECT_EQ(out.playstartcycle, in.playstartcycle) << "playstartcycle";
    EXPECT_EQ(out.status, in.status) << "status";
    EXPECT_EQ(out.speed, in.speed) << "speed";
    EXPECT_EQ(out.readspan, in.readspan) << "readspan";
    EXPECT_EQ(out.interruptcausemask, in.interruptcausemask) << "interruptcausemask";
    for (unsigned i = 0; i < 4; i++) EXPECT_EQ(out.atv[i], in.atv[i]) << "atv[" << i << "]";
    for (unsigned i = 0; i < 4; i++) EXPECT_EQ(out.atvpending[i], in.atvpending[i]) << "atvpending[" << i << "]";
    EXPECT_EQ(out.adpcmmuted, in.adpcmmuted) << "adpcmmuted";
    EXPECT_EQ(out.mode, in.mode) << "mode";
    EXPECT_EQ(out.filterfile, in.filterfile) << "filterfile";
    EXPECT_EQ(out.filterchannel, in.filterchannel) << "filterchannel";
    EXPECT_EQ(out.xaended, in.xaended) << "xaended";
    EXPECT_EQ(out.xafirstsector, in.xafirstsector) << "xafirstsector";
    EXPECT_EQ(out.soundmapenabled, in.soundmapenabled) << "soundmapenabled";
    for (unsigned i = 0; i < 3; i++)
        EXPECT_EQ(out.currentposition[i], in.currentposition[i]) << "currentposition[" << i << "]";
    for (unsigned i = 0; i < 3; i++) EXPECT_EQ(out.seekposition[i], in.seekposition[i]) << "seekposition[" << i << "]";
    for (unsigned i = 0; i < 8; i++) EXPECT_EQ(out.lastlocp[i], in.lastlocp[i]) << "lastlocp[" << i << "]";
    for (unsigned i = 0; i < 8; i++) EXPECT_EQ(out.lastlocl[i], in.lastlocl[i]) << "lastlocl[" << i << "]";
    EXPECT_EQ(out.seed, in.seed) << "seed";
    EXPECT_EQ(out.lidopen, in.lidopen) << "lidopen";
    EXPECT_EQ(out.waslidopened, in.waslidopened) << "waslidopened";
    EXPECT_EQ(out.lidclosescheduled, in.lidclosescheduled) << "lidclosescheduled";
    EXPECT_EQ(out.lidcloseatcycles, in.lidcloseatcycles) << "lidcloseatcycles";
    for (unsigned i = 0; i < 16; i++)
        EXPECT_EQ(out.commandfifo.payload[i], in.commandfifo.payload[i]) << "commandfifo.payload[" << i << "]";
    EXPECT_EQ(out.commandfifo.value, in.commandfifo.value) << "commandfifo.value";
    EXPECT_EQ(out.commandfifo.valueRead, in.commandfifo.valueRead) << "commandfifo.valueRead";
    EXPECT_EQ(out.commandfifo.hasValue, in.commandfifo.hasValue) << "commandfifo.hasValue";
    EXPECT_EQ(out.commandfifo.hitMax, in.commandfifo.hitMax) << "commandfifo.hitMax";
    EXPECT_EQ(out.commandfifo.payloadSize, in.commandfifo.payloadSize) << "commandfifo.payloadSize";
    EXPECT_EQ(out.commandfifo.payloadIndex, in.commandfifo.payloadIndex) << "commandfifo.payloadIndex";
    for (unsigned i = 0; i < 16; i++)
        EXPECT_EQ(out.commandexecuting.payload[i], in.commandexecuting.payload[i])
            << "commandexecuting.payload[" << i << "]";
    EXPECT_EQ(out.commandexecuting.value, in.commandexecuting.value) << "commandexecuting.value";
    EXPECT_EQ(out.commandexecuting.valueRead, in.commandexecuting.valueRead) << "commandexecuting.valueRead";
    EXPECT_EQ(out.commandexecuting.hasValue, in.commandexecuting.hasValue) << "commandexecuting.hasValue";
    EXPECT_EQ(out.commandexecuting.hitMax, in.commandexecuting.hitMax) << "commandexecuting.hitMax";
    EXPECT_EQ(out.commandexecuting.payloadSize, in.commandexecuting.payloadSize) << "commandexecuting.payloadSize";
    EXPECT_EQ(out.commandexecuting.payloadIndex, in.commandexecuting.payloadIndex) << "commandexecuting.payloadIndex";
    for (unsigned i = 0; i < 16; i++)
        EXPECT_EQ(out.responsefifo0.payload[i], in.responsefifo0.payload[i]) << "responsefifo0.payload[" << i << "]";
    EXPECT_EQ(out.responsefifo0.value, in.responsefifo0.value) << "responsefifo0.value";
    EXPECT_EQ(out.responsefifo0.valueRead, in.responsefifo0.valueRead) << "responsefifo0.valueRead";
    EXPECT_EQ(out.responsefifo0.hasValue, in.responsefifo0.hasValue) << "responsefifo0.hasValue";
    EXPECT_EQ(out.responsefifo0.hitMax, in.responsefifo0.hitMax) << "responsefifo0.hitMax";
    EXPECT_EQ(out.responsefifo0.payloadSize, in.responsefifo0.payloadSize) << "responsefifo0.payloadSize";
    EXPECT_EQ(out.responsefifo0.payloadIndex, in.responsefifo0.payloadIndex) << "responsefifo0.payloadIndex";
    for (unsigned i = 0; i < 16; i++)
        EXPECT_EQ(out.responsefifo1.payload[i], in.responsefifo1.payload[i]) << "responsefifo1.payload[" << i << "]";
    EXPECT_EQ(out.responsefifo1.value, in.responsefifo1.value) << "responsefifo1.value";
    EXPECT_EQ(out.responsefifo1.valueRead, in.responsefifo1.valueRead) << "responsefifo1.valueRead";
    EXPECT_EQ(out.responsefifo1.hasValue, in.responsefifo1.hasValue) << "responsefifo1.hasValue";
    EXPECT_EQ(out.responsefifo1.hitMax, in.responsefifo1.hitMax) << "responsefifo1.hitMax";
    EXPECT_EQ(out.responsefifo1.payloadSize, in.responsefifo1.payloadSize) << "responsefifo1.payloadSize";
    EXPECT_EQ(out.responsefifo1.payloadIndex, in.responsefifo1.payloadIndex) << "responsefifo1.payloadIndex";
    EXPECT_EQ(out.xaleft[0], in.xaleft[0]) << "xaleft[0]";
    EXPECT_EQ(out.xaleft[1], in.xaleft[1]) << "xaleft[1]";
    EXPECT_EQ(out.xaright[0], in.xaright[0]) << "xaright[0]";
    EXPECT_EQ(out.xaright[1], in.xaright[1]) << "xaright[1]";
    EXPECT_EQ(out.sectorreadsinceseek, in.sectorreadsinceseek) << "sectorreadsinceseek";
    EXPECT_EQ(out.scexcountersreset, in.scexcountersreset) << "scexcountersreset";
}
