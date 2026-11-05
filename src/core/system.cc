/***************************************************************************
 *   Copyright (C) 2018 PCSX-Redux authors                                 *
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

#include <stddef.h>
#include <uv.h>

#include <iomanip>
#include <sstream>

#include "support/file.h"

PCSX::System* PCSX::g_system = NULL;


// locale names have to be written in basic latin or extended latin, in order
// to be properly displayed in the UI with the default range
PCSX::System::System() : m_loop(new uv_loop_t) { uv_loop_init(m_loop); }

PCSX::System::~System() {
    if (!m_emergencyExit) uv_loop_close(m_loop);
    delete m_loop;
}

uv_loop_t* PCSX::System::getLoop() { return m_loop; }

const std::map<std::string, PCSX::System::LocaleInfo> PCSX::System::LOCALES = {
    {
        "Deutsch",
        {"de.po", {}},
    },
    {
        "Ellinika",
        {"el.po", {}},
    },
    {
        "Español (España)",
        {"es_ES.po", {}},
    },
    {
        "Français",
        {"fr.po", {}},
    },
    {
        "Hindi",
        {"hi.po", {MAKEU8("NotoSansDevanagari-Regular.ttf")}},
    },
    {
        "Italiano",
        {"it.po", {}},
    },
    {
        "Malti",
        {"mt.po", {}},
    },
    {
        "Nihongo",
        {"ja.po", {}},
    },
    {
        "Polski",
        {"pl.po", {}},
    },
    {
        "Português (Brasil)",
        {"pt_BR.po", {}},
    },
    {
        "Svenska",
        {"sv.po", {}},
    },
    {
        "Ukrainska",
        {"uk.po", {}},
    },
    {
        "Zhongwen",
        {"zh_CN.po", {MAKEU8("NotoSansSC-Regular.otf")}},
    },
};

bool PCSX::System::loadLocale(const std::string& name, const std::filesystem::path& path) {
    IO<File> in(new PosixFile(path));
    int c;
    std::string currentString = "";
    std::string token = "";
    std::string singleChar = ".";
    bool newLine = true;
    bool inComment = false;
    bool inString = false;
    bool gotBackquote = false;
    unsigned inlineIndex = 0;
    unsigned shift = 0;
    uint8_t inlined = 0;
    enum {
        WAITING_MSGIDTOKEN,
        WAITING_MSGCTXT,
        WAITING_MSGIDTOKEN_AFTER_CTXT,
        WAITING_MSGID,
        WAITING_MSGSTRTOKEN,
        WAITING_MSGSTR,
    } state = WAITING_MSGIDTOKEN;
    uint64_t hashValue;
    std::string context;
    bool hasContext = false;
    std::map<uint64_t, std::string> locale;

    if (in->failed()) return false;

    std::string comment;
    // The flags comment comes before the entry it applies to, which is only stored
    // once the next entry starts, so it is kept per entry.
    bool fuzzy = false;
    bool entryFuzzy = false;

    while ((c = in->getc()) >= 0) {
        if (c == '\n' || c == '\r') {
            if (inString) return false;
            if (inComment && comment.find(", fuzzy") != std::string::npos) fuzzy = true;
            inComment = false;
            newLine = true;
            comment.clear();
            continue;
        }
        if (inComment) {
            comment += c;
            continue;
        }
        if (newLine) {
            newLine = false;
            if (c == '#') {
                inComment = true;
                continue;
            }
            if (c == '\"') {
                if (state != WAITING_MSGIDTOKEN && state != WAITING_MSGIDTOKEN_AFTER_CTXT &&
                    state != WAITING_MSGSTRTOKEN) {
                    return false;
                }
                inString = true;
                continue;
            }
        }
        if (inString) {
            singleChar[0] = c;
            if (c == '\"' && !gotBackquote) {
                inString = false;
            } else if (gotBackquote) {
                if (inlineIndex) {
                    inlineIndex++;
                    inlined <<= shift;
                    if (c >= '0' && c <= '9') {
                        inlined += c - '0';
                    } else if (c >= 'a' && c <= 'f') {
                        inlined += c - 'a' + 10;
                    } else if (c >= 'A' && c <= 'F') {
                        inlined += c - 'A' + 10;
                    }
                    if (inlineIndex == 3) {
                        gotBackquote = false;
                        singleChar[0] = inlined;
                        currentString += singleChar;
                        inlineIndex = 0;
                    }
                } else {
                    if (c == 'x') {
                        inlineIndex = 1;
                        shift = 4;
                        inlined = 0;
                    } else if (c >= '0' && c <= '7') {
                        inlineIndex = 1;
                        shift = 3;
                        inlined = c - '0';
                    } else {
                        switch (c) {
                            case 'b':
                                singleChar[0] = '\b';
                                break;
                            case 't':
                                singleChar[0] = '\t';
                                break;
                            case 'n':
                                singleChar[0] = '\n';
                                break;
                            case 'f':
                                singleChar[0] = '\f';
                                break;
                            case 'r':
                                singleChar[0] = '\r';
                                break;
                            case '\\':
                                singleChar[0] = '\\';
                                break;
                            case '"':
                            case '\'':
                                singleChar[0] = c;
                                break;
                            default:
                                currentString += '\\';
                                singleChar[0] = c;
                                break;
                        }
                        currentString += singleChar;
                        gotBackquote = false;
                    }
                }
            } else {
                gotBackquote = c == '\\';
                if (!gotBackquote) currentString += singleChar;
            }
        } else {
            if (c == ' ' || c == '\t') continue;
            switch (state) {
                case WAITING_MSGCTXT:
                case WAITING_MSGID:
                case WAITING_MSGSTR:
                    if (c != '\"') return false;
                    inString = true;
                    switch (state) {
                        case WAITING_MSGCTXT:
                            state = WAITING_MSGIDTOKEN_AFTER_CTXT;
                            break;
                        case WAITING_MSGID:
                            state = WAITING_MSGSTRTOKEN;
                            break;
                        default:
                            state = WAITING_MSGIDTOKEN;
                            break;
                    }
                    break;
                case WAITING_MSGIDTOKEN:
                case WAITING_MSGIDTOKEN_AFTER_CTXT:
                case WAITING_MSGSTRTOKEN:
                    if (token.empty()) {
                        switch (state) {
                            case WAITING_MSGIDTOKEN:
                                if (!currentString.empty() && !entryFuzzy) locale[hashValue] = currentString;
                                entryFuzzy = fuzzy;
                                fuzzy = false;
                                break;
                            case WAITING_MSGIDTOKEN_AFTER_CTXT:
                                context = currentString;
                                hasContext = true;
                                break;
                            default:
                                // A string with a context is looked up as "context\004string",
                                // the same key gettext uses for msgctxt entries.
                                hashValue = djb::hash(hasContext ? context + '\004' + currentString : currentString);
                                hasContext = false;
                                break;
                        }
                        currentString = "";
                    }
                    token += c;
                    switch (state) {
                        case WAITING_MSGIDTOKEN:
                            if (token == "msgid") {
                                token = "";
                                state = WAITING_MSGID;
                            } else if (token == "msgctxt") {
                                token = "";
                                state = WAITING_MSGCTXT;
                            } else if (token.length() >= 7) {
                                return false;
                            }
                            break;
                        case WAITING_MSGIDTOKEN_AFTER_CTXT:
                            if (token.length() == 5) {
                                if (token != "msgid") return false;
                                token = "";
                                state = WAITING_MSGID;
                            }
                            break;
                        default:
                            if (token.length() == 6) {
                                if (token != "msgstr") return false;
                                token = "";
                                state = WAITING_MSGSTR;
                            }
                            break;
                    }
                    break;
            }
        }
    }

    if (inString || (state != WAITING_MSGIDTOKEN)) return false;

    if (!currentString.empty() && !entryFuzzy) locale[hashValue] = currentString;
    m_locales[name] = locale;
    return true;
}

bool PCSX::System::findResource(std::function<bool(const std::filesystem::path& path)> walker,
                                const std::filesystem::path& name, const std::filesystem::path& releasePath,
                                const std::filesystem::path& sourcePath) {
    // First, let's try the base filename from the same directory as our main binary.
    if (walker(m_binDir / name)) return true;

    // Then, let's search for our release folders...
    // Maybe in a subfolder next to our main binary? That's the Windows way.
    if (walker(m_binDir / releasePath / name)) return true;
    // Next up, the Unix way: binary is in /bin, and resources are in /share/pcsx-redux.
    if (walker(m_binDir / ".." / "share" / "pcsx-redux" / releasePath / name)) return true;
    // And finally, MacOS had to do differently, of course. The MacOS app has this layout:
    // binary is in Contents/MacOS/
    // rest is in Contents/Resources/share/pcsx-redux
    if (walker(m_binDir / ".." / "Resources" / "share" / "pcsx-redux" / releasePath / name)) return true;

    // And finally, let's try if we're running from sources.
    // If our main binary is at the root - that's the Unix way.
    if (walker(m_binDir / sourcePath / name)) return true;
    // And if it's in a subfolder - that's the Window / Visual Studio way.
    if (walker(m_binDir / ".." / ".." / sourcePath / name)) return true;
    if (walker(m_binDir / ".." / ".." / ".." / sourcePath / name)) return true;

    // No luck here...
    return false;
}

std::filesystem::path PCSX::System::getPersistentDir() const {
    if (getArgs().isPortable()) return getArgs().getPortablePath();
#ifdef _WIN32
    char* homeDir;
    auto ret = _dupenv_s(&homeDir, nullptr, "APPDATA");
    if ((ret != 0) || (!homeDir)) {
        return "";
    }
    std::filesystem::path persistentDir = std::filesystem::path(homeDir) / "pcsx-redux";
    free(homeDir);
#else
    char* homeDir = getenv("HOME");
    if (!homeDir) {
        return "";
    }
    std::filesystem::path persistentDir = std::filesystem::path(homeDir) / ".config" / "pcsx-redux";
#endif
    if (!std::filesystem::exists(persistentDir)) {
        if (!std::filesystem::create_directories(persistentDir)) {
            return "";
        }
    }
    return persistentDir;
}
