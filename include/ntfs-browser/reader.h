#pragma once

// Reader: reads what a Scan found. A Scan lists files and gives each entry a
// Handle. A Reader takes that handle and reads the file behind it: its times,
// its names, its attributes, and its content, whether the file is live or
// deleted. It reads raw disk structures, like Scan, so it works on an image
// file and on a deleted file whose record is still there.
//
// Use:
//
//   Reader reader({.target = u"c:"});
//   // handle comes from Entry::GetHandle(), kept past the callback
//   std::vector<std::uint8_t> bytes(4096);
//   const auto read = reader.Read(handle, u"", 0, bytes);
//   if (read) { /* read.value bytes of the unnamed stream are in bytes */ }
//
// Handle. A handle names a record of the $MFT, the times it was reused, and
// which name of the record the entry came from. The Reader opens the record on
// each call and checks it against the handle. It fails with an error, and
// returns no data, when the record is another file's now, is gone, does not
// parse, or belongs to another volume. It never returns the bytes of another
// file.
//
// Deleted files. A freed record keeps its attributes until another file takes
// the slot. The Reader reads it with its sequence number and its clusters as
// the volume holds them. The clusters of a deleted file MAY belong to another
// file by now: GetDataState() says what is known, and a read does not check it.
//
// Content. Read() and CopyTo() give the bytes of any $DATA stream the way a
// program would read the file: a sparse hole reads as zeros, a compressed
// stream is decompressed (LZNT1, when the library is built with it), and an
// encrypted (EFS) stream is decrypted when a key is found: the key of the
// current user, on Windows. Without a key the call fails with
// ReadError::Encrypted, and the part of the buffer that the call touched is
// zero. A read past the end of the stream gives no byte. Every other attribute
// is readable as raw bytes with ReadAttribute().
//
// Threads. A Reader is synchronous: every call runs on the caller's thread and
// returns when done. A Reader MUST NOT be used from two threads at once. It
// MUST NOT be used while a Scan runs on the same volume: the library and its
// log are not thread safe. Run the Scan to its end, keep the handles, then
// read.
//
// Memory. The Reader opens its volume without a block cache: a read goes to the
// disk, and the memory stays small for a file of any size. It keeps the last
// record it parsed, so a loop of reads of one file parses it once.
//
// Errors. Nothing is thrown. A call returns a ReadResult with a ReadError. The
// detail goes to the library log (see log.h).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/reader-types.h>
#include <ntfs-browser/types.h>

namespace NTFS_BROWSER_NAMESPACE {

// MSVC warns (C4251, at /W4) that the exported class holds a std::unique_ptr
// of a type it does not export. The member is private and Impl is never seen
// by a client, so the warning has nothing to tell.
#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 4251)
#endif

// A reader of the files of one NTFS volume. See the top of this file for the
// use and the rules.
class NTFS_BROWSER_EXPORT Reader {
 public:
  // Opens the volume that options.target names, once. The volume stays open
  // until the Reader goes. A target that does not open leaves the Reader
  // closed, with the reason in the log: IsOpen() tells, and every call fails.
  explicit Reader(ReaderOptions options);
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader(Reader&&) = delete;
  Reader& operator=(Reader&&) = delete;
  ~Reader();

 private:
  // Every member and private method, kept out of this header.
  class Impl;
  std::unique_ptr<Impl> impl_;

 public:
  // Whether the target opened and holds an NTFS volume.
  [[nodiscard]] bool IsOpen() const noexcept;

  // Checks that handle still names its file, and tells whether the file is live
  // or deleted: Standing::Live or Standing::Deleted. Reads the record only.
  [[nodiscard]] ReadResult<Standing> Check(const Handle& handle);

  // The $STANDARD_INFORMATION of the file: its four times, its attribute bits
  // and the NTFS 3 fields.
  [[nodiscard]] ReadResult<StandardInformation>
      GetStandardInformation(const Handle& handle);

  // Every $FILE_NAME of the file that has a name, in the order of the record,
  // DOS aliases included. The position in the list is FileName::index, and the
  // value of Handle::name_index.
  [[nodiscard]] ReadResult<std::vector<FileName>>
      GetFileNames(const Handle& handle);

  // The name of the handle: the one at Handle::name_index. With
  // Handle::unknown_name, the name filed under Handle::parent_record, a long
  // name before a DOS alias, or else the first. Fails with NoSuchAttribute when
  // the record has no such name.
  [[nodiscard]] ReadResult<FileName> GetFileName(const Handle& handle);

  // Every $DATA stream of the file: the unnamed one, which holds the content,
  // and the named ones, in the order of the record. A directory usually has
  // none.
  [[nodiscard]] ReadResult<std::vector<StreamInfo>>
      GetStreams(const Handle& handle);

  // Reads the bytes of a $DATA stream, from offset, into buffer. stream is the
  // name of the stream, empty for the unnamed one. Returns the count of bytes
  // read: less than buffer.size() only at the end of the stream, and 0 at or
  // past it. A sparse hole reads as zeros.
  [[nodiscard]] ReadResult<std::uint64_t> Read(const Handle& handle,
                                               std::u16string_view stream,
                                               std::uint64_t offset,
                                               std::span<std::uint8_t> buffer);

  // Gives the bytes of a $DATA stream to sink, in order, copy_chunk_bytes at a
  // time, from offset on. length is the count of bytes to give, or nothing for
  // all that is left. Returns the count of bytes given. A read that fails ends
  // the copy and keeps the bytes already given: the sink MAY have received a
  // part of the content. The sink returns false to stop, which fails with
  // ReadError::Aborted.
  [[nodiscard]] ReadResult<std::uint64_t>
      CopyTo(const Handle& handle, std::u16string_view stream,
             const ByteSink& sink, std::uint64_t offset = 0,
             std::optional<std::uint64_t> length = std::nullopt);

  // What is known about the data of a $DATA stream of a deleted file: whether
  // its clusters are probably still its own, in the sense of DataState. A live
  // file is Intact. Fails with NoSuchStream when the stream does not exist. A
  // hint, not a proof, and it reads no content.
  [[nodiscard]] ReadResult<DataState>
      GetDataState(const Handle& handle, std::u16string_view stream = {});

  // The header facts of every attribute of the file the library parsed, in the
  // order of the record: the type, the name, the instance, the sizes and the
  // flags.
  [[nodiscard]] ReadResult<std::vector<AttributeInfo>>
      GetAttributes(const Handle& handle);

  // Reads the raw bytes of the content of the attribute that key names, from
  // offset, into buffer, with the rules of Read(). A resident attribute reads
  // from the record, a non-resident one from its clusters. The attribute is not
  // decoded, except that a $DATA stream is decompressed and decrypted as
  // Read() does. Fails with NoSuchAttribute when no attribute matches.
  [[nodiscard]] ReadResult<std::uint64_t>
      ReadAttribute(const Handle& handle, const AttributeKey& key,
                    std::uint64_t offset, std::span<std::uint8_t> buffer);
};  // Reader

#ifdef _MSC_VER
  #pragma warning(pop)
#endif

}  // namespace NTFS_BROWSER_NAMESPACE
