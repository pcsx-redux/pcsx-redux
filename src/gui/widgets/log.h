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

#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "imgui.h"
#include "json.hpp"
#include "support/ansi-sgr.h"
#include "support/hashtable.h"
#include "support/list.h"
#include "support/strings-helpers.h"
#include "support/tree.h"

namespace PCSX {
class GUI;
namespace Widgets {

class Log {
  public:
    using json = nlohmann::json;
    json serialize() const;
    void deserialize(const json& j);
    Log(bool& show);
    ~Log() {
        clear();
        m_classes.destroyAll();
    }
    void clear() { m_allLogs.destroyAll(); }
    template <size_t L>
    bool addLog(unsigned logClass, const char (&log)[L]) {
        std::string str(log);
        return addLog(logClass, str);
    }
    bool addLog(unsigned logClass, const char* log) {
        std::string str(log);
        return addLog(logClass, str);
    }
    bool addLog(unsigned logClass, const std::string& log) {
        auto c = m_classes.find(logClass);
        if (c == m_classes.end()) throw std::runtime_error("Unknown log class");
        if (!c->enabled) return false;
        c->buffer += log;
        auto lines = StringsHelpers::split(c->buffer, "\n", true);
        c->buffer = lines.back();
        lines.pop_back();
        for (auto& line : lines) {
            Element* element = new Element(c->sgr, line);
            c->list.push_back(element);
            m_allLogs.push_back(element);
            if (c->displayed) m_activeLogs.insert(m_activeLogs.size(), element);
        }
        return true;
    }
    bool draw(GUI* gui, const char* title);

    bool& m_show;

  private:
    void addClass(unsigned logClass, const std::string& s) { m_classes.insert(logClass, new ClassElement(s)); }
    void rebuildActive();
    struct Element;
    static void drawEntry(const Element& element, bool plain);
    struct Class {};
    typedef Intrusive::List<Element> All;
    typedef Intrusive::Tree<unsigned, Element> ActiveTree;
    typedef Intrusive::List<Element, Class> ClassList;
    struct Element : public All::Node, public ActiveTree::Node, public ClassList::Node {
        Element(AnsiSGR& sgr, std::string_view e) : entry(sgr.parse(e, spans)) {}
        // Colour spans come first: the entry initializer fills them in while stripping escapes.
        std::vector<AnsiSGR::Span> spans;
        const std::string entry;
    };

    All m_allLogs;
    ActiveTree m_activeLogs;
    struct ClassElement;
    typedef Intrusive::HashTable<unsigned, ClassElement> ClassMap;
    struct ClassElement : public ClassMap::Node {
        ClassElement(const std::string& n) : name(n) {}
        const std::string name;
        std::string buffer;
        AnsiSGR sgr;
        ClassList list;
        bool enabled = true;
        bool displayed = true;
    };
    ClassMap m_classes;

    bool m_scrollToBottom = false;
    bool m_follow = true;
    bool m_mono = true;
};

}  // namespace Widgets
}  // namespace PCSX
