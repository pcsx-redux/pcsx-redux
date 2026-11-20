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

#pragma once

#include "support/binstruct.h"
#include "support/typestring-wrapper.h"

namespace PCSX {

typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt32, TYPESTRING("Signature")> Signature;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt16, TYPESTRING("MadeByVersion")> MadeByVersion;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt16, TYPESTRING("MinVersion")> MinVersion;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt16, TYPESTRING("GPFlag")> GPFlag;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt16, TYPESTRING("CompressionMethod")> CompressionMethod;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt16, TYPESTRING("FileTime")> FileTime;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt16, TYPESTRING("FileDate")> FileDate;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt32, TYPESTRING("CRC32")> CRC32;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt32, TYPESTRING("CompressedSize")> CompressedSize;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt32, TYPESTRING("UncompressedSize")> UncompressedSize;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt16, TYPESTRING("FilenameLength")> FilenameLength;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt16, TYPESTRING("ExtraFieldLength")> ExtraFieldLength;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt16, TYPESTRING("CommentLength")> CommentLength;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt16, TYPESTRING("DiskNumberStart")> DiskNumberStart;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt16, TYPESTRING("InternalFileAttributes")> InternalFileAttributes;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt32, TYPESTRING("ExternalFileAttributes")> ExternalFileAttributes;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt32, TYPESTRING("RelativeOffsetOfLocalHeader")>
    RelativeOffsetOfLocalHeader;
typedef PCSX::BinStruct::Struct<TYPESTRING("LocalFileHeader"), Signature, MinVersion, GPFlag, CompressionMethod,
                                FileTime, FileDate, CRC32, CompressedSize, UncompressedSize, FilenameLength,
                                ExtraFieldLength>
    LocalFileHeader;
typedef PCSX::BinStruct::Struct<TYPESTRING("CentralDirectoryFileHeader"), Signature, MadeByVersion, MinVersion, GPFlag,
                                CompressionMethod, FileTime, FileDate, CRC32, CompressedSize, UncompressedSize,
                                FilenameLength, ExtraFieldLength, CommentLength, DiskNumberStart,
                                InternalFileAttributes, ExternalFileAttributes, RelativeOffsetOfLocalHeader>
    CentralDirectoryFileHeader;

}  // namespace PCSX
