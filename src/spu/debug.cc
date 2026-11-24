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

#include "core/system.h"
#include "imgui.h"
#include "spu/interface.h"

struct Grid {
    static constexpr auto FlagsColumn = ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_WidthFixed;

    static constexpr auto FlagsRow = ImGuiTableRowFlags_None;

    static constexpr auto FlagsTable =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;

    static constexpr auto FlagsTableOuter = FlagsTable | ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_ScrollX;
    static constexpr auto FlagsTableInner = ImGuiTableFlags_Borders;

    static constexpr auto WidthGeneralIndex = 22.0f;
    static constexpr auto WidthGeneralOn = 30.0f;
    static constexpr auto WidthGeneralTag = 120.0f;
    static constexpr auto WidthGeneralOff = 30.0f;
    static constexpr auto WidthGeneralMute = 45.0f;
    static constexpr auto WidthGeneralSolo = 45.0f;
    static constexpr auto WidthGeneralNoise = 70.0f;
    static constexpr auto WidthGeneralFMod = 70.0f;
    static constexpr auto WidthGeneralPlot = 150.0f;
    static constexpr auto WidthGeneral = WidthGeneralIndex + WidthGeneralTag + WidthGeneralOn + WidthGeneralOff +
                                         WidthGeneralMute + WidthGeneralSolo + WidthGeneralNoise + WidthGeneralFMod +
                                         WidthGeneralPlot;

    static constexpr auto WidthFrequencyActive = 70.0f;
    static constexpr auto WidthFrequencyUsed = 70.0f;
    static constexpr auto WidthFrequency = WidthFrequencyActive + WidthFrequencyUsed;

    static constexpr auto WidthPositionStart = 120.0f;
    static constexpr auto WidthPositionCurrent = 120.0f;
    static constexpr auto WidthPositionLoop = 120.0f;
    static constexpr auto WidthPosition = WidthPositionStart + WidthPositionCurrent + WidthPositionLoop;

    static constexpr auto WidthVolumeL = 70.0f;
    static constexpr auto WidthVolumeR = 70.0f;
    static constexpr auto WidthVolume = WidthVolumeL + WidthVolumeR;

    static constexpr auto WidthAdsrA = 70.0f;
    static constexpr auto WidthAdsrD = 70.0f;
    static constexpr auto WidthAdsrS = 70.0f;
    static constexpr auto WidthAdsrR = 70.0f;
    static constexpr auto WidthAdsr = WidthAdsrA + WidthAdsrD + WidthAdsrS + WidthAdsrR;

    static constexpr auto WidthAdsrSustainLevel = 70.0f;
    static constexpr auto WidthAdsrSustainIncrease = 70.0f;
    static constexpr auto WidthAdsrSustain = WidthAdsrSustainLevel + WidthAdsrSustainIncrease;

    static constexpr auto WidthAdsrVolumeCurrent = 70.0f;
    static constexpr auto WidthAdsrVolumeEnvelope = 80.0f;
    static constexpr auto WidthAdsrVolume = WidthAdsrVolumeCurrent + WidthAdsrVolumeEnvelope;

    static constexpr auto WidthReverbAllowed = 70.0f;
    static constexpr auto WidthReverbActive = 70.0f;
    static constexpr auto WidthReverbNumber = 70.0f;
    static constexpr auto WidthReverbOffset = 70.0f;
    static constexpr auto WidthReverbRepeat = 70.0f;
    static constexpr auto WidthReverb =
        WidthReverbAllowed + WidthReverbActive + WidthReverbNumber + WidthReverbOffset + WidthReverbRepeat;
};

namespace {

using namespace PCSX::SPU;
constexpr auto SPU_CHANNELS_SIZE = impl::MAXCHAN;
using SPU_CHANNELS_INFO = SPUCHAN (&)[SPU_CHANNELS_SIZE + 1];
using SPU_CHANNELS_TAGS = char (&)[SPU_CHANNELS_SIZE][impl::CHANNEL_TAG];
using SPU_CHANNELS_PLOT = float (&)[SPU_CHANNELS_SIZE][impl::DEBUG_SAMPLES];

constexpr auto TableColumnFix = 2;  // fixes ImGui possibly screwing last column width
constexpr auto TablePadding = 18;   // inner padding to make it look neat

constexpr auto BasicTableFlags = ImGuiTableFlags_SizingFixedSame | ImGuiTableFlags_NoHostExtendX;
constexpr auto BasicTableColumnWidth = 150;

template <typename T>
T& GetChannelData(SPU_CHANNELS_INFO channels, const size_t channel) {
    return channels[channel].data.get<T>();
}

bool& GetChannelMute(SPU_CHANNELS_INFO channels, const size_t channel) {
    return GetChannelData<Chan::Mute>(channels, channel).value;
}

bool& GetChannelSolo(SPU_CHANNELS_INFO channels, const size_t channel) {
    return GetChannelData<Chan::Solo>(channels, channel).value;
}

void HandleChannelMute(SPU_CHANNELS_INFO channels, bool& muteThis, bool& soloThis) {
    muteThis = !muteThis;
    if (muteThis) {
        soloThis = false;
    }
    if (ImGui::GetIO().KeyShift) {
        std::for_each(channels, channels + SPU_CHANNELS_SIZE, [muteThis](SPUCHAN& c) {
            c.data.get<Chan::Mute>().value = muteThis;
            if (muteThis) {
                c.data.get<Chan::Solo>().value = false;
            }
        });
    }
}

void HandleChannelSoloMute(SPU_CHANNELS_INFO channels, const size_t channel1, const size_t channel2) {
    auto& muteOther = GetChannelMute(channels, channel2);
    auto& soloOther = GetChannelSolo(channels, channel2);

    if (GetChannelSolo(channels, channel1)) {
        // multi/single solo
        if (ImGui::GetIO().KeyShift) {
            if (soloOther == false) {
                muteOther = true;
            }
        } else {
            muteOther = true;
            soloOther = false;
        }
    } else {
        // mute this to keep solo ones correct
        if (std::any_of(channels, channels + SPU_CHANNELS_SIZE,
                        [](const SPUCHAN& c) { return c.data.get<Chan::Solo>().value; })) {
            GetChannelMute(channels, channel1) = true;
        }
    }
}

void HandleChannelSolo(SPU_CHANNELS_INFO channels, const size_t channel, bool& muteThis, bool& soloThis) {
    soloThis = !soloThis;
    if (soloThis) {
        muteThis = false;
    }
    for (auto j = 0u; j < SPU_CHANNELS_SIZE; j++) {
        if (j != channel) {
            HandleChannelSoloMute(channels, muteThis, soloThis);
        }
    }

    // no more solo channels -> ensure none are muted
    if (std::all_of(channels, channels + SPU_CHANNELS_SIZE,
                    [](const SPUCHAN& c) { return c.data.get<Chan::Solo>().value == false; })) {
        std::for_each(channels, channels + SPU_CHANNELS_SIZE,
                      [](SPUCHAN& c) { c.data.get<Chan::Mute>().value = false; });
    }
}

void DrawTableGeneralIndex(const size_t channel) {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%02i", static_cast<int>(channel));
}

void DrawTableGeneralTag(const size_t channel, SPU_CHANNELS_TAGS& tags) {
    ImGui::PushItemWidth(Grid::WidthGeneralTag);
    const auto tagLabel = "##SpuChannelTag" + std::to_string(channel);
    const auto tagHint = "Channel " + std::to_string(channel);
    ImGui::InputTextWithHint(tagLabel.c_str(), tagHint.c_str(), tags[channel], impl::CHANNEL_TAG);
    ImGui::PopItemWidth();
}

void DrawTableGeneralOn(const Chan::Data& data) {
    ImGui::BeginDisabled();
    auto bit1 = data.get<Chan::On>().value;
    ImGui::Checkbox("##On", &bit1);
    ImGui::EndDisabled();
}

void DrawTableGeneralOff(const Chan::Data& data) {
    auto bit2 = data.get<Chan::Stop>().value;
    ImGui::BeginDisabled();
    ImGui::Checkbox("##Off", &bit2);
    ImGui::EndDisabled();
}

ImVec4 GetMuteSoloButtonTint(const bool down, const ImVec4& downTint) {
    return down ? downTint : ImGui::GetStyleColorVec4(ImGuiCol_Button);
}

struct MSButton {
    const char* Text;
    ImVec4 Tint;
    float Size;
};

bool DrawMuteSoloButton(const size_t channel, const MSButton& button, const bool& active) {
    const auto size = ImVec2(ImGui::GetFrameHeightWithSpacing(), 0);
    const auto temp = ImVec2((button.Size - size.x) * 0.5f - ImGui::GetStyle().FramePadding.x * 2.0f, 0);
    ImGui::Dummy(temp);
    ImGui::SameLine();
    const auto tint = GetMuteSoloButtonTint(active, button.Tint);
    ImGui::PushStyleColor(ImGuiCol_Button, tint);
    const auto text = button.Text + std::to_string(channel);
    const auto pressed = ImGui::Button(text.c_str(), size);
    ImGui::PopStyleColor();
    return pressed;
}

void DrawTableGeneralMute(SPU_CHANNELS_INFO channels, const size_t channel, const MSButton& button) {
    auto& mute = GetChannelMute(channels, channel);
    auto& solo = GetChannelSolo(channels, channel);
    if (DrawMuteSoloButton(channel, button, mute)) {
        HandleChannelMute(channels, mute, solo);
    }
}

void DrawTableGeneralSolo(SPU_CHANNELS_INFO channels, const size_t channel, const MSButton& button) {
    auto& mute = GetChannelMute(channels, channel);
    auto& solo = GetChannelSolo(channels, channel);
    if (DrawMuteSoloButton(channel, button, solo)) {
        HandleChannelSolo(channels, channel, mute, solo);
    }
}

void DrawTableGeneralNoise(const Chan::Data& data) { ImGui::Text("%i", data.get<Chan::Noise>().value); }

void DrawTableGeneralFMod(const Chan::Data& data) { ImGui::Text("%i", data.get<Chan::FMod>().value); }

void DrawTableGeneralPlot(SPU_CHANNELS_PLOT plot, size_t channel) {
    constexpr auto plotSize = ImVec2(Grid::WidthGeneralPlot - TablePadding, 0);
    ImGui::PlotHistogram("##Plot", plot[channel], impl::DEBUG_SAMPLES, 0, nullptr, 0.0f, 1.0f, plotSize);
}

void DrawTableGeneral(SPU_CHANNELS_INFO channels, const float rowHeight, SPU_CHANNELS_TAGS tags,
                      SPU_CHANNELS_PLOT plot) {
    if (ImGui::BeginTable("TableGeneral", 9, Grid::FlagsTableInner)) {
        ImGui::TableSetupColumn("#", Grid::FlagsColumn, Grid::WidthGeneralIndex);
        ImGui::TableSetupColumn("Tag", Grid::FlagsColumn, Grid::WidthGeneralTag);
        ImGui::TableSetupColumn("On", Grid::FlagsColumn, Grid::WidthGeneralOn);
        ImGui::TableSetupColumn("Off", Grid::FlagsColumn, Grid::WidthGeneralOff);
        ImGui::TableSetupColumn("Mute", Grid::FlagsColumn, Grid::WidthGeneralMute);
        ImGui::TableSetupColumn("Solo", Grid::FlagsColumn, Grid::WidthGeneralSolo);
        ImGui::TableSetupColumn("Noise", Grid::FlagsColumn, Grid::WidthGeneralNoise);
        ImGui::TableSetupColumn("FMod", Grid::FlagsColumn, Grid::WidthGeneralFMod);
        ImGui::TableSetupColumn("Plot", Grid::FlagsColumn, Grid::WidthGeneralPlot);

        ImGui::TableHeadersRow();
        for (auto i = 0u; i < SPU_CHANNELS_SIZE; ++i) {
            ImGui::PushID(i);
            const auto& data = channels[i].data;

            ImGui::TableNextRow(Grid::FlagsRow, rowHeight);

            constexpr auto mute = MSButton{"M##SpuMute", ImVec4(0.6f, 0.0f, 0.0f, 1.0f), Grid::WidthGeneralMute};
            constexpr auto solo = MSButton{"S##SpuSolo", ImVec4(0.0f, 0.6f, 0.0f, 1.0f), Grid::WidthGeneralSolo};

            // @formatter:off
            ImGui::TableNextColumn();
            DrawTableGeneralIndex(i);
            ImGui::TableNextColumn();
            DrawTableGeneralTag(i, tags);
            ImGui::TableNextColumn();
            DrawTableGeneralOn(data);
            ImGui::TableNextColumn();
            DrawTableGeneralOff(data);
            ImGui::TableNextColumn();
            DrawTableGeneralMute(channels, i, mute);
            ImGui::TableNextColumn();
            DrawTableGeneralSolo(channels, i, solo);
            ImGui::TableNextColumn();
            DrawTableGeneralNoise(data);
            ImGui::TableNextColumn();
            DrawTableGeneralFMod(data);
            ImGui::TableNextColumn();
            DrawTableGeneralPlot(plot, i);
            // @formatter:on
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DrawTableFrequency(SPU_CHANNELS_INFO channels, const float rowHeight) {
    if (ImGui::BeginTable("TableFrequency", 2, Grid::FlagsTableInner)) {
        ImGui::TableSetupColumn("Active", Grid::FlagsColumn, Grid::WidthFrequencyActive);
        ImGui::TableSetupColumn("Used", Grid::FlagsColumn, Grid::WidthFrequencyUsed);
        ImGui::TableHeadersRow();
        for (auto i = 0u; i < SPU_CHANNELS_SIZE; ++i) {
            const auto& data = channels[i].data;
            ImGui::TableNextRow(Grid::FlagsRow, rowHeight);
            ImGui::AlignTextToFramePadding();
            // @formatter:off
            ImGui::TableNextColumn();
            ImGui::Text("%i", data.get<Chan::ActFreq>().value);
            ImGui::TableNextColumn();
            ImGui::Text("%i", data.get<Chan::UsedFreq>().value);
            // @formatter:on
        }
        ImGui::EndTable();
    }
}

// Show a sound-RAM pointer as a byte offset, or a dash when it points outside
// the 512 KiB SPU RAM - i.e. it is null or the AdpcmDecoder stop sentinel.
// Subtracting an out-of-range pointer would print a meaningless huge number.
static void drawRamOffset(const uint8_t* ptr, const uint8_t* spuRamBase) {
    if (ptr >= spuRamBase && ptr < spuRamBase + 512 * 1024) {
        ImGui::Text("%i", static_cast<int>(ptr - spuRamBase));
    } else {
        ImGui::TextUnformatted("-");
    }
}

void DrawTablePosition(SPU_CHANNELS_INFO channels, const float rowHeight, const uint8_t* spuRamBase) {
    if (ImGui::BeginTable("TablePosition", 3, Grid::FlagsTableInner)) {
        ImGui::TableSetupColumn("Start", Grid::FlagsColumn, Grid::WidthPositionStart);
        ImGui::TableSetupColumn("Current", Grid::FlagsColumn, Grid::WidthPositionCurrent);
        ImGui::TableSetupColumn("Loop", Grid::FlagsColumn, Grid::WidthPositionLoop);
        ImGui::TableHeadersRow();
        for (auto i = 0u; i < SPU_CHANNELS_SIZE; ++i) {
            const auto& chan = channels[i];
            ImGui::TableNextRow(Grid::FlagsRow, rowHeight);
            ImGui::AlignTextToFramePadding();
            // @formatter:off
            ImGui::TableNextColumn();
            drawRamOffset(chan.adpcm.start(), spuRamBase);
            ImGui::TableNextColumn();
            drawRamOffset(chan.adpcm.curr(), spuRamBase);
            ImGui::TableNextColumn();
            drawRamOffset(chan.adpcm.loop(), spuRamBase);
            // @formatter:on
        }
        ImGui::EndTable();
    }
}

void DrawTableVolume(SPU_CHANNELS_INFO channels, const float rowHeight) {
    if (ImGui::BeginTable("TableVolume", 2, Grid::FlagsTableInner)) {
        ImGui::TableSetupColumn("L", Grid::FlagsColumn, Grid::WidthVolumeL);
        ImGui::TableSetupColumn("R", Grid::FlagsColumn, Grid::WidthVolumeR);
        ImGui::TableHeadersRow();
        for (auto i = 0u; i < SPU_CHANNELS_SIZE; ++i) {
            ImGui::TableNextRow(Grid::FlagsRow, rowHeight);
            ImGui::AlignTextToFramePadding();
            // @formatter:off
            ImGui::TableNextColumn();
            ImGui::Text("%i", channels[i].volume.left());
            ImGui::TableNextColumn();
            ImGui::Text("%i", channels[i].volume.right());
            // @formatter:on
        }
        ImGui::EndTable();
    }
}

void DrawTableAdsr(SPU_CHANNELS_INFO channels, const float rowHeight) {
    if (ImGui::BeginTable("TableAdsr", 4, Grid::FlagsTableInner)) {
        ImGui::TableSetupColumn("A", Grid::FlagsColumn, Grid::WidthAdsrA);
        ImGui::TableSetupColumn("D", Grid::FlagsColumn, Grid::WidthAdsrD);
        ImGui::TableSetupColumn("S", Grid::FlagsColumn, Grid::WidthAdsrS);
        ImGui::TableSetupColumn("R", Grid::FlagsColumn, Grid::WidthAdsrR);
        ImGui::TableHeadersRow();
        for (auto i = 0u; i < SPU_CHANNELS_SIZE; ++i) {
            const auto& data = channels[i].adsr.ex();
            ImGui::TableNextRow(Grid::FlagsRow, rowHeight);
            ImGui::AlignTextToFramePadding();
            // @formatter:off
            ImGui::TableNextColumn();
            ImGui::Text("%i", data.get<exAttackRate>().value ^ 0x7F);
            ImGui::TableNextColumn();
            ImGui::Text("%i", (data.get<exDecayRate>().value ^ 0x1F) / 4);
            ImGui::TableNextColumn();
            ImGui::Text("%i", data.get<exSustainRate>().value ^ 0x7F);
            ImGui::TableNextColumn();
            ImGui::Text("%i", (data.get<exReleaseRate>().value ^ 0x1F) / 4);
            // @formatter:on
        }
        ImGui::EndTable();
    }
}

void DrawTableAdsrSustain(SPU_CHANNELS_INFO channels, const float rowHeight) {
    if (ImGui::BeginTable("TableAdsrSustain", 2, Grid::FlagsTableInner)) {
        ImGui::TableSetupColumn("Level", Grid::FlagsColumn, Grid::WidthAdsrSustainLevel);
        ImGui::TableSetupColumn("Increase", Grid::FlagsColumn, Grid::WidthAdsrSustainIncrease);
        ImGui::TableHeadersRow();
        for (auto i = 0u; i < SPU_CHANNELS_SIZE; ++i) {
            const auto& data = channels[i].adsr.ex();
            ImGui::TableNextRow(Grid::FlagsRow, rowHeight);
            ImGui::AlignTextToFramePadding();
            // @formatter:off
            ImGui::TableNextColumn();
            ImGui::Text("%i", data.get<exSustainLevel>().value);
            ImGui::TableNextColumn();
            ImGui::Text("%i", data.get<exSustainIncrease>().value);
            // @formatter:on
        }
        ImGui::EndTable();
    }
}

void DrawTableAdsrVolume(SPU_CHANNELS_INFO channels, const float rowHeight) {
    if (ImGui::BeginTable("TableAdsrVolume", 2, Grid::FlagsTableInner)) {
        ImGui::TableSetupColumn("Current", Grid::FlagsColumn, Grid::WidthAdsrVolumeCurrent);
        ImGui::TableSetupColumn("Envelope", Grid::FlagsColumn, Grid::WidthAdsrVolumeEnvelope);
        ImGui::TableHeadersRow();
        for (auto i = 0u; i < SPU_CHANNELS_SIZE; ++i) {
            const auto& data = channels[i].adsr.ex();
            ImGui::TableNextRow(Grid::FlagsRow, rowHeight);
            ImGui::AlignTextToFramePadding();
            // @formatter:off
            ImGui::TableNextColumn();
            ImGui::Text("%i", data.get<exVolume>().value);
            ImGui::TableNextColumn();
            ImGui::Text("%08X", data.get<exEnvelopeVol>().value);
            // @formatter:on
        }
        ImGui::EndTable();
    }
}

void DrawTableReverb(SPU_CHANNELS_INFO channels, const float rowHeight) {
    if (ImGui::BeginTable("TableReverb", 5, Grid::FlagsTableInner)) {
        ImGui::TableSetupColumn("Allowed", Grid::FlagsColumn, Grid::WidthReverbAllowed);
        ImGui::TableSetupColumn("Active", Grid::FlagsColumn, Grid::WidthReverbActive);
        ImGui::TableSetupColumn("Number", Grid::FlagsColumn, Grid::WidthReverbNumber);
        ImGui::TableSetupColumn("Offset", Grid::FlagsColumn, Grid::WidthReverbOffset);
        ImGui::TableSetupColumn("Repeat", Grid::FlagsColumn, Grid::WidthReverbRepeat);
        ImGui::TableHeadersRow();
        for (auto i = 0u; i < SPU_CHANNELS_SIZE; ++i) {
            const auto& data = channels[i].data;
            ImGui::TableNextRow(Grid::FlagsRow, rowHeight);
            ImGui::AlignTextToFramePadding();
            // @formatter:off
            ImGui::TableNextColumn();
            ImGui::Text("%i", data.get<Chan::Reverb>().value);
            ImGui::TableNextColumn();
            ImGui::Text("%i", data.get<Chan::RVBActive>().value);
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            // @formatter:on
        }
        ImGui::EndTable();
    }
}

void DrawSectionChannels(SPU_CHANNELS_INFO channels, SPU_CHANNELS_TAGS tags, SPU_CHANNELS_PLOT plot,
                         const uint8_t* spuRamBase) {
    if (ImGui::CollapsingHeader("Channels", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto style = ImGui::GetStyle();
        const auto rowHeight = ImGui::GetFrameHeightWithSpacing();
        const auto headerHeight = ImGui::GetTextLineHeightWithSpacing();
        const auto tableHeight = rowHeight * SPU_CHANNELS_SIZE + headerHeight * 2 + 4 + style.ScrollbarSize;

        // BUG ImGui hides last column border when scrolling (off by 1px)
        if (ImGui::BeginTable("SpuChannels", 8, Grid::FlagsTableOuter, ImVec2(0, tableHeight))) {
            ImGui::TableSetupColumn("General", Grid::FlagsColumn, Grid::WidthGeneral + TablePadding * TableColumnFix);
            ImGui::TableSetupColumn("Frequency", Grid::FlagsColumn, Grid::WidthFrequency + TablePadding);
            ImGui::TableSetupColumn("Position", Grid::FlagsColumn, Grid::WidthPosition + TablePadding);
            ImGui::TableSetupColumn("Volume", Grid::FlagsColumn, Grid::WidthVolume + TablePadding);
            ImGui::TableSetupColumn("ADSR", Grid::FlagsColumn, Grid::WidthAdsr + TablePadding * TableColumnFix);
            ImGui::TableSetupColumn("ADSR Sustain", Grid::FlagsColumn, Grid::WidthAdsrSustain + TablePadding);
            ImGui::TableSetupColumn("ADSR Volume", Grid::FlagsColumn, Grid::WidthAdsrVolume + TablePadding);
            ImGui::TableSetupColumn("Reverb", Grid::FlagsColumn, Grid::WidthReverb + TablePadding * TableColumnFix);
            ImGui::TableHeadersRow();

            // @formatter:off
            ImGui::TableNextColumn();
            DrawTableGeneral(channels, rowHeight, tags, plot);
            ImGui::TableNextColumn();
            DrawTableFrequency(channels, rowHeight);
            ImGui::TableNextColumn();
            DrawTablePosition(channels, rowHeight, spuRamBase);
            ImGui::TableNextColumn();
            DrawTableVolume(channels, rowHeight);
            ImGui::TableNextColumn();
            DrawTableAdsr(channels, rowHeight);
            ImGui::TableNextColumn();
            DrawTableAdsrSustain(channels, rowHeight);
            ImGui::TableNextColumn();
            DrawTableAdsrVolume(channels, rowHeight);
            ImGui::TableNextColumn();
            DrawTableReverb(channels, rowHeight);
            // @formatter:on
            ImGui::EndTable();
        }
    }
}

void DrawSectionSpu(const uint16_t spuCtrl, const uint16_t spuStat, const uint32_t spuAddr, const uint8_t* spuRamBase,
                    const uint8_t* irqAddress) {
    if (ImGui::CollapsingHeader("SPU", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("SpuBase", 4, BasicTableFlags)) {
            ImGui::TableSetupColumn("IRQ", 0, BasicTableColumnWidth);
            ImGui::TableSetupColumn("CTRL", 0, BasicTableColumnWidth);
            ImGui::TableSetupColumn("STAT", 0, BasicTableColumnWidth);
            ImGui::TableSetupColumn("MEM", 0, BasicTableColumnWidth);
            ImGui::TableHeadersRow();
            // @formatter:off
            ImGui::TableNextColumn();
            if (irqAddress) {
                ImGui::Text("%08X", static_cast<uint32_t>(irqAddress - spuRamBase));
            } else {
                ImGui::TextUnformatted("-");
            }
            ImGui::TableNextColumn();
            ImGui::Text("%04X", spuCtrl);
            ImGui::TableNextColumn();
            ImGui::Text("%04X", spuStat);
            ImGui::TableNextColumn();
            ImGui::Text("%i", spuAddr);
            // @formatter:on
            ImGui::EndTable();
        }
    }
}

void DrawSectionXa(const xa_decode_t* xapGlobal, const int iLeftXAVol, const int iRightXAVol) {
    if (ImGui::CollapsingHeader("XA", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("SpuXa", 5, BasicTableFlags)) {
            ImGui::TableSetupColumn("Frequency", 0, BasicTableColumnWidth);
            ImGui::TableSetupColumn("Stereo", 0, BasicTableColumnWidth);
            ImGui::TableSetupColumn("Samples", 0, BasicTableColumnWidth);
            ImGui::TableSetupColumn("Volume L", 0, BasicTableColumnWidth);
            ImGui::TableSetupColumn("Volume R", 0, BasicTableColumnWidth);
            ImGui::TableHeadersRow();
            // @formatter:off
            ImGui::TableNextColumn();
            ImGui::Text("%i", xapGlobal ? xapGlobal->freq : 0);
            ImGui::TableNextColumn();
            ImGui::Text("%i", xapGlobal ? xapGlobal->stereo : 0);
            ImGui::TableNextColumn();
            ImGui::Text("%i", xapGlobal ? xapGlobal->nsamples : 0);
            ImGui::TableNextColumn();
            ImGui::Text("%i", iLeftXAVol);
            ImGui::TableNextColumn();
            ImGui::Text("%i", iRightXAVol);
            // @formatter:on
            ImGui::EndTable();
        }
    }
}

}  // namespace

void impl::debug() {
    auto delta = std::chrono::steady_clock::now() - m_lastUpdated;
    using namespace std::chrono_literals;
    while (delta >= 50ms) {
        m_lastUpdated += 50ms;
        delta -= 50ms;
        for (unsigned ch = 0; ch < MAXCHAN; ch++) {
            if (!s_chan[ch].data.get<Chan::On>().value) {
                m_channelDebugTypes[ch][m_currentDebugSample] = EMPTY;
                m_channelDebugData[ch][m_currentDebugSample] = 0.0f;
            }
            if (s_chan[ch].data.get<Chan::IrqDone>().value) {
                m_channelDebugTypes[ch][m_currentDebugSample] = IRQ;
                m_channelDebugData[ch][m_currentDebugSample] = 0.0f;
                s_chan[ch].data.get<Chan::IrqDone>().value = 0;
                continue;
            }

            if (s_chan[ch].data.get<Chan::Mute>().value) {
                m_channelDebugTypes[ch][m_currentDebugSample] = MUTED;
            } else if (s_chan[ch].data.get<Chan::Noise>().value) {
                m_channelDebugTypes[ch][m_currentDebugSample] = NOISE;
            } else if (s_chan[ch].data.get<Chan::FMod>().value == 1) {
                m_channelDebugTypes[ch][m_currentDebugSample] = FMOD1;
            } else if (s_chan[ch].data.get<Chan::FMod>().value == 2) {
                m_channelDebugTypes[ch][m_currentDebugSample] = FMOD2;
            } else {
                m_channelDebugTypes[ch][m_currentDebugSample] = DATA;
            }

            m_channelDebugData[ch][m_currentDebugSample] =
                fabsf(static_cast<float>(s_chan[ch].data.get<Chan::sval>().value) / 32768.0f);
        }
        if (++m_currentDebugSample == DEBUG_SAMPLES) m_currentDebugSample = 0;
    }
    drawScopes();
    if (!m_showDebug) return;
    ImGui::SetNextWindowPos(ImVec2(20, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1200, 430), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(_("SPU Debug"), &m_showDebug)) {
        ImGui::End();
        return;
    }

    DrawSectionSpu(spuCtrl, spuStat, spuAddr, spuRamBase, irqAddress);
    DrawSectionXa(xapGlobal, iLeftXAVol, iRightXAVol);
    DrawSectionChannels(s_chan, m_channelTag, m_channelDebugData, spuRamBase);

    ImGui::End();
}

namespace {

// The newest start position in [first, last] that sits on a rising zero crossing, or -1.
int FindRisingEdge(const int16_t* samples, int first, int last) {
    for (int i = last; i >= first; i--) {
        if (i > 0 && samples[i - 1] < 0 && samples[i] >= 0) return i;
    }
    return -1;
}

void DrawScope(const int16_t* samples, int count, float gain, bool on, int channel) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const float width = size.x;
    const float height = ImGui::GetTextLineHeight() * 4.0f;
    ImGui::Dummy(ImVec2(width, height));
    if (width < 2.0f) return;

    auto* drawList = ImGui::GetWindowDrawList();
    const ImVec2 end = ImVec2(origin.x + width, origin.y + height);
    const float middle = origin.y + height * 0.5f;
    const float scale = height * 0.5f * gain / 32768.0f;
    const auto y = [&](int sample) { return std::clamp(middle - sample * scale, origin.y, end.y); };

    bool clipped = false;
    for (int i = 0; i < count; i++) {
        if (samples[i] == 32767 || samples[i] == -32768) clipped = true;
    }

    drawList->PushClipRect(origin, end, true);
    drawList->AddRectFilled(origin, end, ImGui::GetColorU32(ImGuiCol_FrameBg));
    drawList->AddLine(ImVec2(origin.x, middle), ImVec2(end.x, middle), ImGui::GetColorU32(ImGuiCol_Separator));
    const ImU32 color = ImGui::GetColorU32(on ? ImGuiCol_PlotLines : ImGuiCol_TextDisabled);
    if (count <= width) {
        // Fewer samples than pixels: join the samples.
        const float step = count > 1 ? width / (count - 1) : 0.0f;
        for (int i = 1; i < count; i++) {
            drawList->AddLine(ImVec2(origin.x + (i - 1) * step, y(samples[i - 1])),
                              ImVec2(origin.x + i * step, y(samples[i])), color);
        }
    } else {
        // More samples than pixels: one vertical span per column, from its lowest sample to its
        // highest, starting at the last sample of the column before so the trace stays joined.
        const int columns = static_cast<int>(width);
        int previous = samples[0];
        for (int x = 0; x < columns; x++) {
            const int from = static_cast<int>(static_cast<int64_t>(x) * count / columns);
            const int to = std::max(from + 1, static_cast<int>(static_cast<int64_t>(x + 1) * count / columns));
            int low = previous, high = previous;
            for (int i = from; i < to; i++) {
                low = std::min<int>(low, samples[i]);
                high = std::max<int>(high, samples[i]);
            }
            previous = samples[to - 1];
            drawList->AddLine(ImVec2(origin.x + x + 0.5f, y(high)), ImVec2(origin.x + x + 0.5f, y(low) + 1.0f), color);
        }
    }
    char label[8];
    snprintf(label, sizeof(label), "%02i", channel);
    drawList->AddText(ImVec2(origin.x + 3.0f, origin.y + 1.0f),
                      ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
    drawList->PopClipRect();
    // A sample at full scale is usually a clipping encoder or a filter blowing up.
    drawList->AddRect(origin, end, clipped ? IM_COL32(230, 60, 60, 255) : ImGui::GetColorU32(ImGuiCol_Border));
}

}  // namespace

void impl::clearScopeChunk(int n) {
    for (int ns = 0; ns < n; ns++) {
        const unsigned index = (m_mixPos + ns) & (kScopeSamples - 1);
        for (unsigned ch = 0; ch < MAXCHAN; ch++) {
            m_scopeRaw[ch][index].store(0, std::memory_order_relaxed);
            m_scopeEnveloped[ch][index].store(0, std::memory_order_relaxed);
        }
    }
}

void impl::drawScopes() {
    if (!m_showScope) {
        m_scopeRecording.store(false, std::memory_order_relaxed);
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(900, 520), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin(_("SPU Voice Scopes"), &m_showScope);
    // Only an open window pays for the recording.
    m_scopeRecording.store(visible && m_showScope, std::memory_order_relaxed);
    if (!visible) {
        ImGui::End();
        return;
    }

    // The trigger looks for an edge in the window's length of history before the shown
    // window, and the whole span stays in the older half of the ring, away from the samples
    // the mixer is writing.
    constexpr int kMaxWindow = kScopeSamples / 4;
    int tap = m_scopeAfterEnvelope ? 1 : 0;
    ImGui::RadioButton(_("Decoded, before envelope"), &tap, 0);
    ImGui::SameLine();
    ImGui::RadioButton(_("After envelope"), &tap, 1);
    m_scopeAfterEnvelope = tap == 1;
    ImGui::SameLine();
    ImGui::Checkbox(_("Trigger on rising edge"), &m_scopeTrigger);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderInt(_("Window"), &m_scopeWindow, 32, kMaxWindow, _("%d samples"), ImGuiSliderFlags_Logarithmic);
    m_scopeWindow = std::clamp(m_scopeWindow, 32, kMaxWindow);
    ImGui::SameLine();
    ImGui::Text("%.2f ms", m_scopeWindow * 1000.0f / 44100.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat(_("Gain"), &m_scopeGain, 1.0f, 16.0f, "%.1fx", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderInt(_("Columns"), &m_scopeColumns, 1, 8);
    m_scopeColumns = std::clamp(m_scopeColumns, 1, 8);

    const int window = m_scopeWindow;
    const int span = window * 2;
    const uint64_t head = m_scopeHead.load(std::memory_order_acquire);
    const uint64_t first = m_scopeStart.load(std::memory_order_relaxed);
    int16_t samples[kMaxWindow * 2];
    if (ImGui::BeginTable("##SpuScopes", m_scopeColumns, ImGuiTableFlags_SizingStretchSame)) {
        for (unsigned ch = 0; ch < MAXCHAN; ch++) {
            ImGui::TableNextColumn();
            const auto& ring = m_scopeAfterEnvelope ? m_scopeEnveloped[ch] : m_scopeRaw[ch];
            // Samples from before this recording started read as silence.
            for (int i = 0; i < span; i++) {
                const uint64_t back = span - i;
                const bool recorded = back <= head && head - back >= first;
                samples[i] = recorded ? ring[(head - back) & (kScopeSamples - 1)].load(std::memory_order_relaxed) : 0;
            }
            int start = span - window;
            if (m_scopeTrigger) {
                const int edge = FindRisingEdge(samples, 1, span - window);
                if (edge >= 0) start = edge;
            }
            ImGui::PushID(ch);
            DrawScope(samples + start, window, m_scopeGain, s_chan[ch].data.get<Chan::On>().value, ch);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
