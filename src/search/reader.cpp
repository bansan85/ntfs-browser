#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/reader-types.h>
#include <ntfs-browser/search/reader.h>
#include <ntfs-browser/search/types.h>

#include "alloc/types.h"
#include "attr/base.h"
#include "attr/flags.h"
#include "attr/header-common.h"
#include "attr/mask.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "io/file-record.h"
#include "log/ntfs-common.h"
#include "ntfs-volume.h"
#include "search/attribute-parse.h"
#include "search/reader-probe.h"
#include "search/target.h"
#include "utf/utf.h"
#include "volume-options.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

namespace {

// The volume and the record a Reader works on. No block cache: see reader.h.
using NoCacheVolume = NtfsVolume<Cache::Strategy::NoCache>;
using NoCacheRecord = Io::FileRecord<Cache::Strategy::NoCache>;
using NoCacheAttr = Attr::AttrBase<Cache::Strategy::NoCache>;

// The bit of the flags word of an attribute header that marks a sparse stream.
constexpr std::uint16_t flag_sparse = 0x8000;

// The most bytes of a $STANDARD_INFORMATION or a $FILE_NAME that the Reader
// decodes. Both are a few hundred bytes at most: a larger one is damaged, and a
// forged size must not decide how much memory a call takes.
constexpr std::uint64_t max_decoded_attribute_bytes = 1024;

// The sequence number that follows sequence. NTFS counts up to 0xFFFF and then
// skips 0, the value that means no sequence.
std::uint16_t NextSequence(std::uint16_t sequence) {
  return sequence == 0xFFFF ? std::uint16_t{1}
                            : static_cast<std::uint16_t>(sequence + 1);
}

// The value of a call that failed with error.
template <typename T>
ReadResult<T> Failure(ReadError error) {
  return {.error = error, .value = T{}};
}

// Whether the header flags of attr say it is encrypted.
bool IsEncrypted(const NoCacheAttr& attr) {
  return (attr.GetAttrFlags() & Attr::flag_encrypted) != 0;
}

// The data state of Alloc::DataState, as the Reader reports it.
DataState DataStateOf(Alloc::DataState state) {
  switch (state) {
    case Alloc::DataState::Intact:
      return DataState::Intact;
    case Alloc::DataState::Reallocated:
      return DataState::Reallocated;
    case Alloc::DataState::Bad:
      return DataState::Bad;
    case Alloc::DataState::Unknown:
      break;
  }
  return DataState::Unknown;
}

// The whole content of attr, when it is not larger than the bound of the
// decoders. Nothing when it is larger or does not read.
std::optional<std::vector<std::uint8_t>> ReadSmall(const NoCacheAttr& attr) {
  const std::uint64_t size = attr.GetDataSize();
  if (size > max_decoded_attribute_bytes) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size == 0) {
    return bytes;
  }
  const std::optional<ULONGLONG> got = attr.ReadData(0, bytes);
  if (!got || *got != size) {
    return std::nullopt;
  }
  return bytes;
}

// Reads the bytes of attr from offset into buffer. Gives the count read, which
// is less than the buffer only at the end of the content. A read that comes
// short is a failure: Encrypted for an encrypted stream, else Unreadable. The
// bytes of the buffer that the read touched are then zero.
ReadResult<std::uint64_t> ReadBytes(const NoCacheAttr& attr,
                                    std::uint64_t offset,
                                    std::span<std::uint8_t> buffer) {
  const std::uint64_t size = attr.GetDataSize();
  if (offset >= size || buffer.empty()) {
    return {.error = ReadError::None, .value = 0};
  }
  const std::uint64_t want =
      std::min<std::uint64_t>(buffer.size(), size - offset);
  const std::optional<ULONGLONG> got =
      attr.ReadData(offset, buffer.first(static_cast<std::size_t>(want)));
  if (!got || *got != want) {
    // What the failed read left in the buffer is partial, or still encrypted.
    std::ranges::fill(buffer.first(static_cast<std::size_t>(want)), 0);
    return Failure<std::uint64_t>(IsEncrypted(attr) ? ReadError::Encrypted
                                                    : ReadError::Unreadable);
  }
  return {.error = ReadError::None, .value = want};
}

// The name of the handle among list, which is not empty: see
// Reader::GetFileName(). A long name is better than a DOS alias, and a name
// filed under the parent of the handle is better than another.
const FileName* ChooseName(const std::vector<FileName>& list,
                           const Handle& handle) {
  const auto is_long = [](const FileName& name) {
    return name.name_space != NameSpace::Dos;
  };
  const auto in_parent = [&handle](const FileName& name) {
    return name.parent_record == handle.parent_record;
  };
  const auto both = [&](const FileName& name) {
    return in_parent(name) && is_long(name);
  };
  for (const auto& found :
       {std::ranges::find_if(list, both), std::ranges::find_if(list, in_parent),
        std::ranges::find_if(list, is_long)}) {
    if (found != list.end()) {
      return &*found;
    }
  }
  return &list.front();
}

}  // namespace

// Every member and private method of Reader. It owns the volume and the last
// record it parsed.
class Reader::Impl {
 public:
  explicit Impl(const ReaderOptions& options) {
    const std::optional<TargetParts> parts =
        SplitTarget(Utf::Utf16ToWide(options.target));
    if (!parts) {
      Log::Error("Cannot find a device or image file in {}",
                 Utf::WideToUtf8(Utf::Utf16ToWide(options.target)));
      return;
    }
    VolumeOptions volume_options;
    // A freed record is only read when the volume lists deleted items.
    volume_options.include_deleted = true;
    volume_options.recover_errors = options.recover_errors;
    OpenedVolume<Cache::Strategy::NoCache> opened =
        OpenVolume<Cache::Strategy::NoCache>(*parts, volume_options);
    if (!opened.volume) {
      return;
    }
    volume_ = std::move(opened.volume);
    if (const auto& provider = ReaderKeyProviderOverride(); provider) {
      volume_->SetEfsKeyProvider(*provider);
    }
    record_ = std::make_unique<NoCacheRecord>(*volume_);
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() = default;

  // Whether the volume opened.
  [[nodiscard]] bool IsOpen() const noexcept { return volume_ != nullptr; }

  // Parses the record that handle names, with the attributes of mask, and
  // checks it against the handle. The parsed record is Record(). Gives the
  // standing of the record: Live or Deleted. A parse that was done already for
  // the same record and a mask that covers this one is not repeated.
  [[nodiscard]] ReadResult<Standing> Open(const Handle& handle,
                                          Attr::Mask mask) {
    if (!IsOpen()) {
      return Failure<Standing>(ReadError::NotOpen);
    }
    // 0 is the value of a handle that was not filled: any volume takes it.
    if (handle.volume != 0 && handle.volume != volume_->GetSerialNumber()) {
      Log::Error("The handle of record {} comes from another volume",
                 handle.record);
      return Failure<Standing>(ReadError::WrongVolume);
    }
    if (const ReadError error = ParseHeader(handle.record);
        error != ReadError::None) {
      return Failure<Standing>(error);
    }
    const bool deleted = record_->IsDeleted();
    if (!SameFile(handle, deleted)) {
      Log::Warn("Record {} is not the file of the handle any more",
                handle.record);
      return Failure<Standing>(ReadError::SequenceChanged);
    }
    if (const ReadError error = ParseAttributes(mask);
        error != ReadError::None) {
      return Failure<Standing>(error);
    }
    return {.error = ReadError::None,
            .value = deleted ? Standing::Deleted : Standing::Live};
  }

  // The record that the last Open() parsed.
  [[nodiscard]] const NoCacheRecord& Record() const noexcept {
    return *record_;
  }

  // Calls callback for each attribute of the parsed record, until it sets stop.
  void Traverse(const NoCacheRecord::AttrsCallback& callback) {
    record_->TraverseAttrs(callback, nullptr);
  }

  // The $FILE_NAME attributes of the parsed record that have a name, decoded,
  // in the order of the record. FileName::index is the position in the list.
  [[nodiscard]] std::vector<FileName> Names() const {
    std::vector<FileName> names;
    for (const std::unique_ptr<NoCacheAttr>& attr :
         record_->GetAttr(Attr::Type::FileName)) {
      const std::optional<std::vector<std::uint8_t>> content = ReadSmall(*attr);
      if (!content) {
        continue;
      }
      if (std::optional<FileName> name = ParseFileName(
              *content, static_cast<std::uint16_t>(names.size()))) {
        names.push_back(*std::move(name));
      }
    }
    return names;
  }

  // The $DATA stream of the parsed record called name, or null.
  [[nodiscard]] const NoCacheAttr* Stream(std::u16string_view name) const {
    return record_->FindStream(Utf::Utf16ToWide(name));
  }

 private:
  // Parses the header of record, unless the last call did it already.
  ReadError ParseHeader(std::uint64_t record) {
    if (parsed_record_ == record) {
      return ReadError::None;
    }
    parsed_record_.reset();
    parsed_mask_.reset();
    if (!record_->ParseFileRecord(record)) {
      Log::Error("Record {} does not parse", record);
      return ReadError::NoSuchRecord;
    }
    if (record_->IsExtensionRecord()) {
      Log::Error("Record {} is an extension record, not a file", record);
      return ReadError::NoSuchRecord;
    }
    parsed_record_ = record;
    return ReadError::None;
  }

  // Whether the parsed record is still the file of handle. A record that is in
  // use must carry the sequence of the handle, and not be the record of a file
  // the handle saw deleted. A freed record carries the sequence of the handle,
  // or the one after it, which NTFS gives it on deletion. A handle with
  // sequence 0 takes any sequence.
  [[nodiscard]] bool SameFile(const Handle& handle, bool deleted) const {
    const std::uint16_t sequence = record_->GetSequenceNumber();
    if (!deleted && handle.deleted) {
      return false;
    }
    if (handle.sequence == 0) {
      return true;
    }
    if (deleted) {
      return sequence == handle.sequence ||
             sequence == NextSequence(handle.sequence);
    }
    return sequence == handle.sequence;
  }

  // Parses the attributes of mask of the parsed record, unless an earlier parse
  // covers them.
  ReadError ParseAttributes(Attr::Mask mask) {
    if (parsed_mask_ &&
        (*parsed_mask_ == Attr::Mask::All || *parsed_mask_ == mask)) {
      return ReadError::None;
    }
    record_->ClearAttrs();
    record_->SetAttrMask(mask);
    if (!record_->ParseAttrs()) {
      Log::Error("The attributes of record {} do not parse",
                 parsed_record_.value_or(0));
      parsed_mask_.reset();
      return ReadError::RecordDamaged;
    }
    parsed_mask_ = mask;
    return ReadError::None;
  }

  std::unique_ptr<NoCacheVolume> volume_;
  std::unique_ptr<NoCacheRecord> record_;
  // The record number, and the attributes, that record_ holds now.
  std::optional<std::uint64_t> parsed_record_;
  std::optional<Attr::Mask> parsed_mask_;
};

namespace {

// Runs call and turns an exception into a failed result: nothing leaves the
// Reader.
template <typename T, typename Call>
ReadResult<T> Guarded(Call&& call) noexcept {
  try {
    return call();
  } catch (const std::exception& error) {
    Log::Error("A read failed: {}", error.what());
  } catch (...) {
    Log::Error("A read failed for an unknown reason");
  }
  return Failure<T>(ReadError::Unreadable);
}

}  // namespace

Reader::Reader(ReaderOptions options) : impl_(nullptr) {
  try {
    impl_ = std::make_unique<Impl>(options);
  } catch (const std::exception& error) {
    Log::Error("Cannot open {}: {}",
               Utf::WideToUtf8(Utf::Utf16ToWide(options.target)), error.what());
  } catch (...) {
    Log::Error("Cannot open {}",
               Utf::WideToUtf8(Utf::Utf16ToWide(options.target)));
  }
}

Reader::~Reader() = default;

bool Reader::IsOpen() const noexcept {
  return impl_ != nullptr && impl_->IsOpen();
}

ReadResult<Standing> Reader::Check(const Handle& handle) {
  return Guarded<Standing>([&]() -> ReadResult<Standing> {
    if (!IsOpen()) {
      return Failure<Standing>(ReadError::NotOpen);
    }
    return impl_->Open(handle, Attr::Mask::StandardInformation);
  });
}

ReadResult<StandardInformation>
    Reader::GetStandardInformation(const Handle& handle) {
  return Guarded<StandardInformation>([&]() -> ReadResult<StandardInformation> {
    if (!IsOpen()) {
      return Failure<StandardInformation>(ReadError::NotOpen);
    }
    if (const auto opened =
            impl_->Open(handle, Attr::Mask::StandardInformation);
        !opened) {
      return Failure<StandardInformation>(opened.error);
    }
    const auto& attrs =
        impl_->Record().GetAttr(Attr::Type::StandardInformation);
    if (attrs.empty()) {
      return Failure<StandardInformation>(ReadError::NoSuchAttribute);
    }
    const std::optional<std::vector<std::uint8_t>> content =
        ReadSmall(*attrs.front());
    if (!content) {
      return Failure<StandardInformation>(ReadError::RecordDamaged);
    }
    std::optional<StandardInformation> info =
        ParseStandardInformation(*content);
    if (!info) {
      return Failure<StandardInformation>(ReadError::RecordDamaged);
    }
    return {.error = ReadError::None, .value = *std::move(info)};
  });
}

ReadResult<std::vector<FileName>> Reader::GetFileNames(const Handle& handle) {
  return Guarded<std::vector<FileName>>(
      [&]() -> ReadResult<std::vector<FileName>> {
        if (!IsOpen()) {
          return Failure<std::vector<FileName>>(ReadError::NotOpen);
        }
        if (const auto opened = impl_->Open(handle, Attr::Mask::FileName);
            !opened) {
          return Failure<std::vector<FileName>>(opened.error);
        }
        return {.error = ReadError::None, .value = impl_->Names()};
      });
}

ReadResult<FileName> Reader::GetFileName(const Handle& handle) {
  return Guarded<FileName>([&]() -> ReadResult<FileName> {
    ReadResult<std::vector<FileName>> names = GetFileNames(handle);
    if (!names) {
      return Failure<FileName>(names.error);
    }
    const std::vector<FileName>& list = names.value;
    if (list.empty()) {
      return Failure<FileName>(ReadError::NoSuchAttribute);
    }
    if (handle.name_index == Handle::unknown_name) {
      return {.error = ReadError::None, .value = *ChooseName(list, handle)};
    }
    if (handle.name_index >= list.size()) {
      return Failure<FileName>(ReadError::NoSuchAttribute);
    }
    return {.error = ReadError::None, .value = list.at(handle.name_index)};
  });
}

ReadResult<std::vector<StreamInfo>> Reader::GetStreams(const Handle& handle) {
  return Guarded<std::vector<StreamInfo>>(
      [&]() -> ReadResult<std::vector<StreamInfo>> {
        if (!IsOpen()) {
          return Failure<std::vector<StreamInfo>>(ReadError::NotOpen);
        }
        if (const auto opened = impl_->Open(handle, Attr::Mask::Data);
            !opened) {
          return Failure<std::vector<StreamInfo>>(opened.error);
        }
        std::vector<StreamInfo> streams;
        for (const std::unique_ptr<NoCacheAttr>& listed :
             impl_->Record().GetAttr(Attr::Type::Data)) {
          const std::u16string name = Utf::WideToUtf16(listed->GetAttrName());
          const bool known =
              std::ranges::any_of(streams, [&name](const StreamInfo& stream) {
                return stream.name == name;
              });
          if (known) {
            continue;
          }
          const NoCacheAttr* attr = impl_->Stream(name);
          if (attr == nullptr) {
            attr = listed.get();
          }
          const std::uint16_t flags = attr->GetAttrFlags();
          streams.push_back({.name = name,
                             .size = attr->GetDataSize(),
                             .allocated_size = attr->GetAllocatedSize(),
                             .resident = !attr->IsNonResident(),
                             .compressed = (flags & Attr::flag_compressed) != 0,
                             .sparse = (flags & flag_sparse) != 0,
                             .encrypted = IsEncrypted(*attr)});
        }
        return {.error = ReadError::None, .value = std::move(streams)};
      });
}

ReadResult<std::uint64_t> Reader::Read(const Handle& handle,
                                       std::u16string_view stream,
                                       std::uint64_t offset,
                                       std::span<std::uint8_t> buffer) {
  return Guarded<std::uint64_t>([&]() -> ReadResult<std::uint64_t> {
    if (!IsOpen()) {
      return Failure<std::uint64_t>(ReadError::NotOpen);
    }
    if (const auto opened = impl_->Open(handle, Attr::Mask::Data); !opened) {
      return Failure<std::uint64_t>(opened.error);
    }
    const NoCacheAttr* attr = impl_->Stream(stream);
    if (attr == nullptr) {
      return Failure<std::uint64_t>(ReadError::NoSuchStream);
    }
    return ReadBytes(*attr, offset, buffer);
  });
}

ReadResult<std::uint64_t> Reader::CopyTo(const Handle& handle,
                                         std::u16string_view stream,
                                         const ByteSink& sink,
                                         std::uint64_t offset,
                                         std::optional<std::uint64_t> length) {
  return Guarded<std::uint64_t>([&]() -> ReadResult<std::uint64_t> {
    if (!IsOpen()) {
      return Failure<std::uint64_t>(ReadError::NotOpen);
    }
    if (const auto opened = impl_->Open(handle, Attr::Mask::Data); !opened) {
      return Failure<std::uint64_t>(opened.error);
    }
    const NoCacheAttr* attr = impl_->Stream(stream);
    if (attr == nullptr) {
      return Failure<std::uint64_t>(ReadError::NoSuchStream);
    }
    const std::uint64_t size = attr->GetDataSize();
    const std::uint64_t first = std::min(offset, size);
    const std::uint64_t last =
        length ? first + std::min(*length, size - first) : size;
    std::vector<std::uint8_t> chunk(copy_chunk_bytes);
    std::uint64_t given = 0;
    for (std::uint64_t position = first; position < last;) {
      const auto count = static_cast<std::size_t>(
          std::min<std::uint64_t>(chunk.size(), last - position));
      const ReadResult<std::uint64_t> read =
          ReadBytes(*attr, position, std::span(chunk).first(count));
      if (!read) {
        return {.error = read.error, .value = given};
      }
      if (!sink(std::span<const std::uint8_t>(chunk).first(count))) {
        return {.error = ReadError::Aborted, .value = given};
      }
      given += count;
      position += count;
    }
    return {.error = ReadError::None, .value = given};
  });
}

ReadResult<DataState> Reader::GetDataState(const Handle& handle,
                                           std::u16string_view stream) {
  return Guarded<DataState>([&]() -> ReadResult<DataState> {
    if (!IsOpen()) {
      return Failure<DataState>(ReadError::NotOpen);
    }
    if (const auto opened = impl_->Open(handle, Attr::Mask::Data); !opened) {
      return Failure<DataState>(opened.error);
    }
    const std::optional<Alloc::DataState> state =
        impl_->Record().GetDataState(Utf::Utf16ToWide(stream));
    if (!state) {
      return Failure<DataState>(ReadError::NoSuchStream);
    }
    return {.error = ReadError::None, .value = DataStateOf(*state)};
  });
}

ReadResult<std::vector<AttributeInfo>>
    Reader::GetAttributes(const Handle& handle) {
  return Guarded<std::vector<AttributeInfo>>(
      [&]() -> ReadResult<std::vector<AttributeInfo>> {
        if (!IsOpen()) {
          return Failure<std::vector<AttributeInfo>>(ReadError::NotOpen);
        }
        if (const auto opened = impl_->Open(handle, Attr::Mask::All); !opened) {
          return Failure<std::vector<AttributeInfo>>(opened.error);
        }
        std::vector<AttributeInfo> attributes;
        impl_->Traverse([&attributes](const NoCacheAttr& attr,
                                      void* /*context*/, bool* /*stop*/) {
          attributes.push_back(
              {.type = static_cast<AttributeType>(attr.GetAttrType()),
               .name = Utf::WideToUtf16(attr.GetAttrName()),
               .instance = attr.GetAttrHeader().id,
               .resident = !attr.IsNonResident(),
               .size = attr.GetDataSize(),
               .allocated_size = attr.GetAllocatedSize(),
               .flags = attr.GetAttrFlags()});
        });
        return {.error = ReadError::None, .value = std::move(attributes)};
      });
}

ReadResult<std::uint64_t>
    Reader::ReadAttribute(const Handle& handle, const AttributeKey& key,
                          std::uint64_t offset,
                          std::span<std::uint8_t> buffer) {
  return Guarded<std::uint64_t>([&]() -> ReadResult<std::uint64_t> {
    if (!IsOpen()) {
      return Failure<std::uint64_t>(ReadError::NotOpen);
    }
    if (const auto opened = impl_->Open(handle, Attr::Mask::All); !opened) {
      return Failure<std::uint64_t>(opened.error);
    }
    const NoCacheAttr* found = nullptr;
    const std::wstring name = Utf::Utf16ToWide(key.name);
    impl_->Traverse(
        [&](const NoCacheAttr& attr, void* /*context*/, bool* stop) {
          const bool same_instance = key.instance == any_instance ||
                                     attr.GetAttrHeader().id == key.instance;
          if (static_cast<AttributeType>(attr.GetAttrType()) == key.type &&
              attr.GetAttrName() == name && same_instance) {
            found = &attr;
            *stop = true;
          }
        });
    if (found == nullptr) {
      return Failure<std::uint64_t>(ReadError::NoSuchAttribute);
    }
    return ReadBytes(*found, offset, buffer);
  });
}

}  // namespace NTFS_BROWSER_NAMESPACE::Search
