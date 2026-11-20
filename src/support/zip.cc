/*

MIT License

Copyright (c) 2022 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "support/zip.h"

#include "support/zfile.h"
#include "support/zip-internal.h"

PCSX::ZipArchive::ZipArchive(IO<File> file) : m_file(file) {
    file->rSeek(0);
    while (!file->eof()) {
        uint32_t signature = file->peek<uint32_t>();
        switch (signature) {
            case 0x04034b50: {
                CompressedFile fileInfo;
                LocalFileHeader header;
                header.deserialize(file);
                auto flags = header.get<GPFlag>();
                if (flags & 8) {
                    m_failed = true;
                    return;
                }
                fileInfo.name = file->readString(header.get<FilenameLength>());
                file->skip(header.get<ExtraFieldLength>());
                fileInfo.offset = file->rTell();
                file->skip(header.get<CompressedSize>());
                fileInfo.size = header.get<UncompressedSize>();
                fileInfo.compressedSize = header.get<CompressedSize>();
                if ((fileInfo.size == 0xffffffff) && (fileInfo.compressedSize == 0xffffffff)) {
                    m_failed = true;
                    return;
                }
                auto method = header.get<CompressionMethod>();
                if ((method != 0) && (method != 8)) {
                    m_failed = true;
                    return;
                }
                fileInfo.compressed = method == 8;
                m_files.push_back(fileInfo);
                break;
            }
            case 0x02014b50: {
                CentralDirectoryFileHeader header;
                header.deserialize(file);
                file->skip(header.get<FilenameLength>());
                file->skip(header.get<ExtraFieldLength>());
                file->skip(header.get<CommentLength>());
                break;
            }
            case 0x06054b50: {
                return;
            }
            default: {
                m_failed = true;
                return;
            }
        }
    }
}

void PCSX::ZipArchive::listFiles(std::function<bool(std::string_view)> walker) {
    for (auto& file : m_files) {
        if (!file.isDirectory()) {
            if (!walker(file.name)) return;
        }
    }
}

void PCSX::ZipArchive::listDirectories(std::function<bool(std::string_view)> walker) {
    for (auto& file : m_files) {
        if (file.isDirectory()) {
            if (!walker(std::string_view(file.name.c_str(), file.name.length() - 1))) return;
        }
    }
}

PCSX::File* PCSX::ZipArchive::openFile(std::string path) {
    for (auto& c : path) {
        if (c == '\\') c = '/';
    }
    File* ret = nullptr;
    for (auto& file : m_files) {
        if (file.name == path) {
            SubFile* sub = new SubFile(m_file, file.offset, file.compressedSize);
            if (file.compressed) {
                ret = new ZReader(sub, file.size, ZReader::RAW);
            } else {
                ret = sub;
            }
            break;
        }
    }

    if (!ret) ret = new FailedFile();
    return ret;
}
