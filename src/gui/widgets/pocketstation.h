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

#include <stdint.h>

#include "GL/gl3w.h"

namespace PCSX {

class MemoryCard;

namespace PocketStation {
class PocketStation;
}

namespace Widgets {

// Displays a docked PocketStation's 32x32 1bpp LCD as a scaled-up GL texture, and carries the
// dock/undock control for that slot. Takes the owning card rather than the bare device, because
// dock state belongs to the card; if it is null, the window shows a "no device" note.
class PocketStationLCD {
  public:
    PocketStationLCD(bool& show) : m_show(show) {}
    void draw(MemoryCard* card, unsigned slot, const char* title);

    bool& m_show;

  private:
    GLuint m_texture = 0;            // lazily created on first draw (needs a GL context).
    uint32_t m_pixels[32 * 32] = {};  // RGBA8 staging buffer expanded from the 1bpp vram.
};

}  // namespace Widgets
}  // namespace PCSX
