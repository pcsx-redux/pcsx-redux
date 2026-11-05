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

#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "core/sstate.h"
#include "gtest/gtest.h"
#include "main/main.h"

TEST(Protobuf, DumpProto) {
    MainInvoker invoker("-dumpproto");
    int ret = invoker.invoke();
    EXPECT_EQ(ret, 0);
}

// Every message type a field refers to has to be defined in the dumped schema, or protoc refuses it.
TEST(Protobuf, DumpProtoDefinesEveryMessageType) {
    std::ostringstream schema;
    PCSX::SaveStates::ProtoFile::dumpSchema(schema);
    std::istringstream lines(schema.str());
    const std::set<std::string> scalars = {"double",   "float",    "int32",  "int64",   "uint32",
                                           "uint64",   "sint32",   "sint64", "fixed32", "fixed64",
                                           "sfixed32", "sfixed64", "bool",   "string",  "bytes"};
    const std::regex messageRe("message ([A-Za-z_][A-Za-z0-9_]*) \\{.*");
    const std::regex fieldRe(" +(?:repeated )?([A-Za-z_][A-Za-z0-9_]*) [A-Za-z_][A-Za-z0-9_]* = [0-9]+;");
    std::set<std::string> defined;
    std::set<std::string> referenced;
    std::string line;
    std::smatch m;
    while (std::getline(lines, line)) {
        if (std::regex_match(line, m, messageRe)) defined.insert(m[1]);
        if (std::regex_match(line, m, fieldRe) && !scalars.contains(m[1])) referenced.insert(m[1]);
    }
    EXPECT_FALSE(referenced.empty());
    for (auto& type : referenced) EXPECT_TRUE(defined.contains(type)) << "no message definition for " << type;
}
