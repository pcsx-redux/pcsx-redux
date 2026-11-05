/***************************************************************************
 *   Copyright (C) 2020 PCSX-Redux authors                                 *
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

#include <stdlib.h>
#include <string.h>

#include <filesystem>

int pcsxMain(int argc, char** argv);

class MainInvoker {
  public:
    template <typename... Args>
    MainInvoker(Args... args) {
        m_count = sizeof...(Args) + 1;
        m_args = new char*[m_count + 1];
        m_args[0] = strdup("pcsx-redux");
        argGenerateOne(m_args, 1, args...);
    }
    ~MainInvoker() {
        for (char** ptr = m_args; *ptr; ptr++) {
            free(*ptr);
        }
        delete[] m_args;
    }
    int invoke() {
        fprintf(stderr, "Starting PCSX-Redux test with arguments:\n");
        for (int i = 0; i < m_count; i++) {
            fprintf(stderr, "  %s\n", m_args[i]);
        }
        int r = pcsxMain(m_count, m_args);
        fprintf(stderr, "PCSX-Redux test finished with exit code %d\n", r);
        return r;
    }

  private:
    int m_count;
    char** m_args;

    void argGenerateOne(char** array, int index) { array[index] = nullptr; }

    template <typename Head, typename... Args>
    void argGenerateOne(char** array, int index, Head head, Args... args) {
        std::filesystem::path cwd = std::filesystem::current_path();
        while (true) {
            std::filesystem::path maybe = cwd / head;
            // An argument can be a whole Lua script, too long to be a file name.
            std::error_code ec;
            if (std::filesystem::exists(maybe, ec)) {
                array[index] = strdup(reinterpret_cast<const char*>(maybe.u8string().c_str()));
                argGenerateOne(array, index + 1, args...);
                return;
            }
            if (!cwd.has_parent_path()) break;
            auto newcwd = cwd.parent_path();
            if (cwd == newcwd) break;
            cwd = newcwd;
        }
        array[index] = strdup(head);
        argGenerateOne(array, index + 1, args...);
    }
};
