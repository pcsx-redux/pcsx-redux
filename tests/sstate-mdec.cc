/*
 * Save states written before the MDEC raw quant tables and scale matrix were
 * serialised (fields 12-15) must be distinguishable from new ones, because
 * MDEC::deserialize relies on it to fall back to the standard tables.
 */

#include <stdint.h>

#include <string>

#include "core/mdec.h"
#include "core/sstate.h"
#include "gtest/gtest.h"

namespace {

using namespace PCSX::SaveStates;

// The MDEC message as it was before fields 12-15 existed.
typedef PCSX::Protobuf::Message<TYPESTRING("MDEC"), MDECReg0, MDECReg1, MDECRl, MDECRlEnd, MDECBlockBufferPos,
                                MDECBlockBuffer, MDECDMAADR, MDECDMABCR, MDECDMACHCR, MDECIQY, MDECIQUV>
    OldMDEC;

template <typename T>
std::string serializeMessage(const T& msg) {
    PCSX::Protobuf::OutSlice slice;
    msg.serialize(&slice);
    return slice.finalize();
}

MDEC deserializeMDEC(const std::string& data) {
    MDEC mdec;
    PCSX::Protobuf::InSlice slice(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    mdec.deserialize(&slice, 0);
    return mdec;
}

}  // namespace

TEST(SaveStateMDEC, OldStateHasNoQuantOrScaleTables) {
    OldMDEC old;
    for (unsigned i = 0; i < 64; i++) old.get<MDECIQY>().value[i].value = 1000 + i;
    MDEC loaded = deserializeMDEC(serializeMessage(old));
    // The fields that were present did load, so the absence below is not a
    // decode that silently read nothing.
    EXPECT_EQ(loaded.get<MDECIQY>().count, 64u);
    EXPECT_EQ(loaded.get<MDECIQY>().value[5].value, 1005);
    EXPECT_EQ(loaded.get<MDECQTY>().count, 0u);
    EXPECT_EQ(loaded.get<MDECQTUV>().count, 0u);
    EXPECT_EQ(loaded.get<MDECScaleTable>().count, 0u);
}

TEST(SaveStateMDEC, NewStateHasQuantAndScaleTables) {
    MDEC current;
    // All zero on purpose: presence must not be inferred from the values.
    MDEC loaded = deserializeMDEC(serializeMessage(current));
    EXPECT_EQ(loaded.get<MDECQTY>().count, 64u);
    EXPECT_EQ(loaded.get<MDECQTUV>().count, 64u);
    EXPECT_EQ(loaded.get<MDECScaleTable>().count, 64u);
}

// A state from before fields 12-15 only has the cached tables, so the raw quant
// tables have to come back out of them. A non-standard table catches a loader
// that substitutes the default one.
TEST(SaveStateMDEC, QuantTableRecoversFromCachedTable) {
    unsigned char qt[64], back[64];
    for (unsigned i = 0; i < 64; i++) qt[i] = static_cast<unsigned char>((i * 37 + 11) & 0xff);
    qt[0] = 0;
    qt[1] = 255;
    qt[2] = 16;
    int iq[64];
    PCSX::MDEC::iqtab_init(iq, qt);
    PCSX::MDEC::qtab_fromIqtab(back, iq);
    for (unsigned i = 0; i < 64; i++) EXPECT_EQ(back[i], qt[i]) << "entry " << i;
}
