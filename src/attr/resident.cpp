#include "attr/resident.h"

#include <cstring>
#include <span>
#include <stdexcept>

#include <gsl/narrow>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/cache/strategy.h>

#include "data/header-resident.h"
#include "ntfs-browser/win-types.h"

namespace NtfsBrowser::Attr {

namespace {

// Returns the attribute body. ValidateResidentBounds() MUST have accepted
// the header first.
std::span<const BYTE> ResidentBody(const Data::HeaderResident& header) {
  return std::span<const BYTE>(reinterpret_cast<const BYTE*>(&header),
                               header.header.total_size)
      .subspan(header.attr_offset, header.attr_size);
}

// Rejects an attr_offset/attr_size pair reaching past the attribute's own
// total_size, which is already bounds-checked against the record buffer.
void ValidateResidentBounds(const Data::HeaderResident& header) {
  if (static_cast<ULONGLONG>(header.attr_offset) + header.attr_size >
      header.header.total_size) {
    throw std::runtime_error(
        "Resident attribute body exceeds attribute bounds.\n");
  }
}

}  // namespace

template <Cache::Strategy S>
AttrResident<S>::AttrResident(const HeaderCommon& ahc,
                              const Io::FileRecord<S>& file_record)
    : Attr::AttrBase<S>(ahc, file_record) {}

// A resident attribute's bytes live inline in the MFT record: NTFS still
// reserves space for them within the attribute record, padded to the
// record's own 8-byte alignment, which can exceed the real data size by a
// few bytes. This is what Windows itself reports as a resident attribute's
// allocation size, so match it instead of returning GetDataSize().
template <Cache::Strategy S>
ULONGLONG AttrResident<S>::GetAllocatedSize() const noexcept {
  const auto& header =
      reinterpret_cast<const Data::HeaderResident&>(this->GetAttrHeader());
  return header.header.total_size - header.attr_offset;
}

// Read "bufLen" bytes from "offset" into "bufv"
// Number of bytes acturally read is returned in "*actural"
template <Cache::Strategy S>
std::optional<ULONGLONG>
    AttrResident<S>::ReadData(ULONGLONG offset,
                              const std::span<BYTE>& buffer) const {
  ULONGLONG buf_len = buffer.size();
  ULONGLONG actural = 0;
  if (buf_len == 0) {
    return buf_len;
  }

  // offset parameter error
  if (offset >= this->GetDataSize()) {
    return {};
  }

  if ((offset + buf_len) > this->GetDataSize()) {
    actural = gsl::narrow<DWORD>(this->GetDataSize() - offset);  // Beyond scope
  } else {
    actural = buf_len;
  }

  const std::span<const BYTE> body(this->GetData(), this->GetDataSize());
  // offset < GetDataSize() was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  memcpy(buffer.data(), &body[offset], actural);

  return actural;
}

AttrResidentNoCache::AttrResidentNoCache(
    const HeaderCommon& ahc,
    const Io::FileRecord<Cache::Strategy::NoCache>& file_record)
    : AttrResident(ahc, file_record) {
  const auto& header = reinterpret_cast<const Data::HeaderResident&>(ahc);
  ValidateResidentBounds(header);

  body_ = ResidentBody(header);
}

const BYTE* AttrResidentNoCache::GetData() const noexcept {
  return body_.data();
}

ULONGLONG AttrResidentNoCache::GetDataSize() const noexcept {
  return body_.size();
}

AttrResidentFullCache::AttrResidentFullCache(
    const HeaderCommon& ahc,
    const Io::FileRecord<Cache::Strategy::FullCache>& file_record)
    : AttrResident(ahc, file_record) {
  const auto& header = reinterpret_cast<const Data::HeaderResident&>(ahc);
  ValidateResidentBounds(header);

  body_.resize(header.attr_size);
  // An empty body has a null data(), which memcpy must not receive.
  if (header.attr_size != 0) {
    memcpy(body_.data(), ResidentBody(header).data(), header.attr_size);
  }
}

const BYTE* AttrResidentFullCache::GetData() const noexcept {
  return body_.data();
}

ULONGLONG AttrResidentFullCache::GetDataSize() const noexcept {
  return body_.size();
}

template class AttrResident<Cache::Strategy::NoCache>;
template class AttrResident<Cache::Strategy::FullCache>;

}  // namespace NtfsBrowser::Attr
