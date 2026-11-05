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

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace PCSX {

// Strips ANSI escape sequences from text and records the foreground colour changes requested
// by SGR sequences (ESC [ ... m). Colours are 0xRRGGBB, or AnsiSGR::c_default for the default
// colour. Every other escape sequence is removed and otherwise ignored.
class AnsiSGR {
  public:
    static constexpr int32_t c_default = -1;
    struct Span {
        uint32_t offset;  // offset in the stripped text where this colour starts
        int32_t color;
    };

    // Parses one chunk of text. The SGR state carries over from the previous call, so a colour
    // set on one line still applies to the next. Spans are only recorded when the text is
    // coloured at some point: plain text with the default colour leaves spans empty.
    std::string parse(std::string_view in, std::vector<Span>& spans) {
        std::string out;
        out.reserve(in.size());
        spans.clear();
        int32_t current = color();
        if (current != c_default) spans.push_back({0, current});
        size_t i = 0;
        while (i < in.size()) {
            char c = in[i];
            if (c != '\x1b') {
                out += c;
                i++;
                continue;
            }
            i++;
            if (i >= in.size()) break;
            if (in[i] != '[') {
                char intro = in[i++];
                if (intro == ']' || intro == 'P' || intro == 'X' || intro == '^' || intro == '_') {
                    // OSC, DCS, SOS, PM and APC strings run until BEL or ST (ESC \). Lines are split
                    // before parsing, so an unterminated one only swallows the rest of its line.
                    while (i < in.size()) {
                        if (in[i] == '\x07') {
                            i++;
                            break;
                        }
                        if (in[i] == '\x1b' && i + 1 < in.size() && in[i + 1] == '\\') {
                            i += 2;
                            break;
                        }
                        i++;
                    }
                } else if (intro >= 0x20 && intro <= 0x2f) {
                    // nF escape such as ESC ( B: intermediate bytes, then one final byte.
                    while (i < in.size() && in[i] >= 0x20 && in[i] <= 0x2f) i++;
                    if (i < in.size()) i++;
                }
                // Anything else is a two-character escape such as ESC c, already consumed.
                continue;
            }
            i++;
            size_t paramsStart = i;
            while (i < in.size() && in[i] >= 0x20 && in[i] <= 0x3f) i++;
            if (i >= in.size()) break;  // truncated sequence
            char final = in[i++];
            if (final != 'm') continue;
            applySGR(in.substr(paramsStart, i - 1 - paramsStart));
            int32_t next = color();
            if (next == current) continue;
            current = next;
            uint32_t offset = out.size();
            if (!spans.empty() && spans.back().offset == offset) {
                spans.back().color = current;
            } else {
                spans.push_back({offset, current});
            }
        }
        // A lone default-colour span at the start carries no information.
        if (spans.size() == 1 && spans[0].offset == 0 && spans[0].color == c_default) spans.clear();
        return out;
    }

    void reset() {
        m_fg = c_default;
        m_bold = false;
    }

    // Current foreground colour, with bold brightening the eight basic colours.
    int32_t color() const {
        if (m_fg >= 0 && m_fg < 8 && m_bold) return s_palette[m_fg + 8];
        if (m_fg >= 0 && m_fg < 16) return s_palette[m_fg];
        if (m_fg >= 16) return m_fg - 16;
        return c_default;
    }

  private:
    // m_fg: c_default, a palette index 0-15, or 16 + 0xRRGGBB for 256-colour and truecolor.
    int32_t m_fg = c_default;
    bool m_bold = false;

    static constexpr int32_t s_palette[16] = {
        0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
        0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
    };

    static int32_t xterm256(unsigned n) {
        if (n < 16) return s_palette[n];
        if (n < 232) {
            n -= 16;
            static constexpr unsigned levels[6] = {0, 95, 135, 175, 215, 255};
            return (levels[n / 36] << 16) | (levels[(n / 6) % 6] << 8) | levels[n % 6];
        }
        unsigned g = 8 + (n - 232) * 10;
        return (g << 16) | (g << 8) | g;
    }

    // Applies one SGR parameter string. Groups are separated by ';'. A group may carry ':'
    // sub-parameters (the ISO 8613-6 form), where an empty field is an omitted value.
    void applySGR(std::string_view params) {
        std::vector<std::vector<unsigned>> groups(1);
        groups.back().push_back(0);
        for (char c : params) {
            if (c >= '0' && c <= '9') {
                unsigned& value = groups.back().back();
                value = value * 10 + (c - '0');
                if (value > 0xffff) value = 0xffff;
            } else if (c == ';') {
                groups.emplace_back(1, 0);
            } else if (c == ':') {
                groups.back().push_back(0);
            } else {
                // Private markers make the sequence meaningless to us.
                return;
            }
        }
        for (size_t i = 0; i < groups.size(); i++) {
            auto& group = groups[i];
            unsigned code = group[0];
            if (group.size() > 1) {
                // Colon form: 38:5:n, or 38:2:[colour space]:r:g:b. Other codes ignore sub-parameters.
                if (code == 38 && group[1] == 5 && group.size() >= 3) {
                    m_fg = 16 + xterm256(group[2] & 0xff);
                } else if (code == 38 && group[1] == 2 && group.size() >= 5) {
                    size_t r = group.size() >= 6 ? 3 : 2;
                    m_fg = 16 + rgb(group[r], group[r + 1], group[r + 2]);
                } else if (code != 38 && code != 48) {
                    applyCode(code);
                }
                continue;
            }
            if (code == 38 || code == 48) {
                // Semicolon form: 38;5;n or 38;2;r;g;b. Background colours are parsed and dropped.
                auto arg = [&](size_t n) { return groups[i + n][0]; };
                if (i + 2 < groups.size() && arg(1) == 5) {
                    if (code == 38) m_fg = 16 + xterm256(arg(2) & 0xff);
                    i += 2;
                } else if (i + 4 < groups.size() && arg(1) == 2) {
                    if (code == 38) m_fg = 16 + rgb(arg(2), arg(3), arg(4));
                    i += 4;
                } else {
                    return;
                }
                continue;
            }
            applyCode(code);
        }
    }

    static int32_t rgb(unsigned r, unsigned g, unsigned b) {
        return (int32_t)(((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff));
    }

    void applyCode(unsigned code) {
        if (code == 0) {
            reset();
        } else if (code == 1) {
            m_bold = true;
        } else if (code == 22) {
            m_bold = false;
        } else if (code >= 30 && code <= 37) {
            m_fg = code - 30;
        } else if (code == 39) {
            m_fg = c_default;
        } else if (code >= 90 && code <= 97) {
            m_fg = code - 90 + 8;
        }
    }
};

}  // namespace PCSX
