#include <cassert>
#include <stdexcept>

#include <gsl/narrow>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/data/file-record-header.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/flag/file-record.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr-bitmap.h"
#include "attr-data.h"
#include "attr-file-name.h"
#include "attr-index-alloc.h"
#include "attr-index-root.h"
#include "attr-list.h"
#include "attr-non-resident.h"
#include "attr-resident.h"
#include "attr-std-info.h"
#include "attr-vol-info.h"
#include "attr-vol-name.h"
#include "attr/header-non-resident.h"
#include "attr/header-resident.h"
#include "data/run-entry.h"
#include "efs/efs-context.h"
#include "efs/efs-stream.h"
#include "index-block.h"
#include "ntfs-common.h"
#include "utf.h"

namespace NtfsBrowser
{

template <Strategy S>
FileRecord<S>::FileRecord(const NtfsVolume<S>& volume) : volume_(volume)
{
  ClearAttrRawCB();

  // Default to parse all attributes
}

template <Strategy S>
FileRecord<S>::~FileRecord()
{
}

template <Strategy S>
const NtfsVolume<S>& FileRecord<S>::GetVolume() const noexcept
{
  return volume_;
}

template <Strategy S>
void FileRecord<S>::ClearAttrs() noexcept
{
  for (std::vector<std::unique_ptr<AttrBase<S>>>& arr : attr_list_)
  {
    arr.clear();
  }
}

// Call user defined Callback routines for an attribute
template <Strategy S>
void FileRecord<S>::UserCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                                 bool& bDiscard)
{
  bDiscard = false;

  if (attr_raw_call_back_[attType] != nullptr)
  {
    attr_raw_call_back_[attType](ahc, bDiscard);
  }
  else
  {
    volume_.AttrRawCallBack(attType, ahc, bDiscard);
  }
}

template <Strategy S>
template <typename RESIDENT>
std::unique_ptr<AttrBase<S>>
    FileRecord<S>::AllocAttr(const AttrHeaderCommon& ahc, bool& bUnhandled,
                             std::unordered_set<ULONGLONG>& attrListChain)
{
  switch (ahc.type)
  {
    // These attribute types are always resident on disk; reject any
    // record claiming otherwise before its bytes get reinterpreted as one.
    case AttrType::STANDARD_INFORMATION:
      if (ahc.non_resident != 0)
      {
        throw std::runtime_error(
            "Standard Information attribute must be resident.\n");
      }
      return std::make_unique<AttrStdInfo<RESIDENT, S>>(ahc, *this);

    case AttrType::ATTRIBUTE_LIST:
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrList<AttrNonResident<S>, S>>(ahc, *this,
                                                                 attrListChain);
      }
      return std::make_unique<AttrList<RESIDENT, S>>(ahc, *this, attrListChain);

    case AttrType::FILE_NAME:
      if (ahc.non_resident != 0)
      {
        throw std::runtime_error("File Name attribute must be resident.\n");
      }
      return std::make_unique<AttrFileName<RESIDENT, S>>(ahc, *this);

    case AttrType::VOLUME_NAME:
      if (ahc.non_resident != 0)
      {
        throw std::runtime_error("Volume Name attribute must be resident.\n");
      }
      return std::make_unique<AttrVolName<RESIDENT, S>>(ahc, *this);

    case AttrType::VOLUME_INFORMATION:
      if (ahc.non_resident != 0)
      {
        throw std::runtime_error(
            "Volume Information attribute must be resident.\n");
      }
      return std::make_unique<AttrVolInfo<RESIDENT, S>>(ahc, *this);

    case AttrType::DATA:
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrData<AttrNonResident<S>, S>>(ahc, *this);
      }
      return std::make_unique<AttrData<RESIDENT, S>>(ahc, *this);

    case AttrType::INDEX_ROOT:
      if (ahc.non_resident != 0)
      {
        throw std::runtime_error("Index Root attribute must be resident.\n");
      }
      return std::make_unique<AttrIndexRoot<RESIDENT, S>>(ahc, *this);

    // INDEX_ALLOCATION is always non-resident on disk; reject a record
    // claiming otherwise before its bytes get reinterpreted as one.
    case AttrType::INDEX_ALLOCATION:
      if (ahc.non_resident == 0)
      {
        throw std::runtime_error(
            "Index Allocation attribute must be non-resident.\n");
      }
      return std::make_unique<AttrIndexAlloc<S>>(ahc, *this);

    case AttrType::BITMAP:
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrBitmap<AttrNonResident<S>, S>>(ahc, *this);
      }
      // Resident Bitmap may exist in a directory's FileRecord
      // or in $MFT for a very small volume in theory
      return std::make_unique<AttrBitmap<RESIDENT, S>>(ahc, *this);

    // $EFS, the only one this library reads, is read through the generic
    // wrappers. Any other logged utility stream is not needed, but not
    // worth a warning either.
    case AttrType::LOGGED_UTILITY_STREAM:
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrNonResident<S>>(ahc, *this);
      }
      return std::make_unique<RESIDENT>(ahc, *this);

    // Unhandled Attributes
    default:
      bUnhandled = true;
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrNonResident<S>>(ahc, *this);
      }
      return std::make_unique<RESIDENT>(ahc, *this);
  }
}

// Parse a single Attribute
// Return False on error
template <Strategy S>
bool FileRecord<S>::ParseAttr(const AttrHeaderCommon& ahc,
                              std::unordered_set<ULONGLONG>& attrListChain)
{
  const DWORD attrIndex = ATTR_INDEX(ahc.type);
  if (attrIndex >= kAttrNums)
  {
    LogWarn("Invalid Attribute Type: 0x{:04X}", static_cast<DWORD>(ahc.type));
    return false;
  }

  bool bDiscard = false;
  UserCallBack(attrIndex, ahc, bDiscard);

  if (bDiscard)
  {
    LogDebug("User Callback has processed this Attribute: 0x{:04X}",
             static_cast<DWORD>(ahc.type));
    return true;
  }

  bool bUnhandled = false;

  std::unique_ptr<AttrBase<S>> attr;
  try
  {
    if constexpr (S == Strategy::NO_CACHE)
      attr = AllocAttr<AttrResidentNoCache>(ahc, bUnhandled, attrListChain);
    else
      attr = AllocAttr<AttrResidentFullCache>(ahc, bUnhandled, attrListChain);
  }
  catch (const std::exception& e)
  {
    // gsl::narrow(), reachable through AllocAttr(), can throw a
    // gsl::narrowing_error, which is not a std::runtime_error.
    LogError("Attribute Parse error: 0x{:04X}", static_cast<DWORD>(ahc.type));
    LogException(e);
    return false;
  }

  if (bUnhandled)
  {
    LogWarn("Unhandled attribute: 0x{:04X}", static_cast<DWORD>(ahc.type));
  }
  attr_list_[attrIndex].push_back(std::move(attr));
  return true;
}

// Reads file record fileRef into record_buffer_ and returns its parsed
// header. Early records (and any record read before $MFT's own DATA
// attribute is known) come straight from disk at a fixed offset; later
// records go through $MFT's DATA attribute, since $MFT itself may be
// fragmented across the disk.
template <Strategy S>
std::optional<FileRecordHeaderImpl<S>>
    FileRecord<S>::ReadFileRecord(ULONGLONG fileRef)
{
  if (record_buffer_.size() != volume_.GetFileRecordSize())
  {
    record_buffer_.resize(volume_.GetFileRecordSize());
  }

  if (fileRef < static_cast<ULONGLONG>(Enum::MftIdx::USER) ||
      volume_.mft_data_ == nullptr)
  {
    // Take as continuous disk allocation
    LARGE_INTEGER frAddr;
    try
    {
      frAddr.QuadPart = gsl::narrow<LONGLONG>(
          volume_.GetMFTAddr() + (volume_.GetFileRecordSize()) * fileRef);
    }
    catch (const std::exception& e)
    {
      // fileRef is attacker-controlled and unbounded, so this sum can
      // still overflow a LONGLONG even with mft_addr_ validated.
      LogException(e);
      return {};
    }

    if (!volume_.ReadInto(frAddr, record_buffer_))
    {
      return {};
    }

    try
    {
      return FileRecordHeader::Factory<S>(record_buffer_,
                                          volume_.GetSectorSize());
    }
    catch (const std::exception& e)
    {
      LogException(e);
      return {};
    }
  }

  // May be fragmented $MFT, and its DATA attribute itself may be split
  // across extension records - ReadMftData() picks whichever instance
  // covers this offset.
  const ULONGLONG frAddr = (volume_.GetFileRecordSize()) * fileRef;

  if (std::optional<ULONGLONG> len =
          volume_.ReadMftData(frAddr, record_buffer_);
      !len || *len != volume_.GetFileRecordSize())
  {
    return {};
  }

  try
  {
    return FileRecordHeader::Factory<S>(record_buffer_,
                                        volume_.GetSectorSize());
  }
  catch (const std::exception& e)
  {
    // Reachable through the same FileRecordHeader::Factory<S>() call as
    // the direct-allocation path above.
    LogException(e);
    return {};
  }
}

// Read File Record, verify and patch the US (update sequence)
template <Strategy S>
bool FileRecord<S>::ParseFileRecord(ULONGLONG fileRef)
{
  // Clear previous data
  ClearAttrs();
  if (file_record_)
  {
    file_record_.reset();
  }

  std::optional<FileRecordHeaderImpl<S>> fr = ReadFileRecord(fileRef);
  if (!fr)
  {
    LogError("Cannot read file record {}", fileRef);

    file_reference_ = {};

    return false;
  }

  file_reference_ = fileRef;

  // Debug, not warning: a slot NTFS never used has no magic, so an MFT scan
  // meets this on every such slot. A caller gets false either way.
  if (fr->GetData()->magic != kFileRecordMagic)
  {
    LogDebug("Invalid file record");
    return false;
  }

  if (!fr->PatchUS())
  {
    LogWarn("Update Sequence Number error");
    return false;
  }

  LogDebug("File Record {} Found", fileRef);
  file_record_ = std::move(fr);

  return true;
}

// Visit IndexBlocks recursivly to find a specific Filename
template <Strategy S>
std::optional<IndexEntry>
    FileRecord<S>::VisitIndexBlock(ULONGLONG vcn, std::wstring_view fileName,
                                   std::unordered_set<ULONGLONG>& visitedVcns,
                                   size_t depth) const
{
  if (depth >= kMaxIndexBlockDepth)
  {
    LogWarn("VisitIndexBlock() aborting: recursion depth limit exceeded");
    return {};
  }

  // A subnode VCN already on this walk means the on-disk B+ tree is
  // malformed (self-loop or cycle) - stop instead of recursing forever.
  if (!visitedVcns.insert(vcn).second)
  {
    return {};
  }

  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      getAttr(AttrType::INDEX_ALLOCATION);
  if (vec.empty())
  {
    return {};
  }

  IndexBlock ib;
  if (!static_cast<AttrIndexAlloc<S>*>(vec.front().get())
           ->ParseIndexBlock(vcn, ib))
  {
    return {};
  }

  for (const IndexEntry& ie : ib)
  {
    if (ie.HasName())
    {
      // Compare name
      const int i = ie.Compare(fileName);
      if (i == 0)
      {
        // Must be a copy: ie's shared_ptr<BYTE[]> keeps its backing bytes
        // alive after ib is destroyed.
        LogDebug("VisitIndexBlock() found entry in sub-node");
        return ie;
      }
      if (i < 0)  // fileName is smaller than IndexEntry
      {
        // Visit SubNode
        if (!ie.IsSubNodePtr())
        {
          return {};  // not found
        }
        // Search in SubNode (IndexBlock), recursive call
        std::optional<IndexEntry> retval = VisitIndexBlock(
            ie.GetSubNodeVCN(), fileName, visitedVcns, depth + 1);
        if (retval)
        {
          return retval;
        }
      }
      // Just step forward if fileName is bigger than IndexEntry
    }
    else if (ie.IsSubNodePtr())
    {
      // Search in SubNode (IndexBlock), recursive call
      std::optional<IndexEntry> retval =
          VisitIndexBlock(ie.GetSubNodeVCN(), fileName, visitedVcns, depth + 1);
      if (retval)
      {
        return retval;
      }
    }
  }

  return {};
}

// Traverse SubNode recursivly in ascending order
// Call user defined callback routine once found an subentry
// visitedVcns guards against a malformed/malicious B+ tree where a
// subnode VCN is revisited, which would otherwise recurse without bound.
template <Strategy S>
void FileRecord<S>::TraverseSubNode(ULONGLONG vcn, SUBENTRY_CALLBACK seCallBack,
                                    void* context,
                                    std::unordered_set<ULONGLONG>& visitedVcns,
                                    size_t depth) const
{
  if (depth >= kMaxIndexBlockDepth)
  {
    LogWarn("TraverseSubNode() aborting: recursion depth limit exceeded");
    return;
  }

  // A subnode VCN already on this walk means the on-disk B+ tree is
  // malformed (self-loop or cycle) - stop instead of recursing forever.
  if (!visitedVcns.insert(vcn).second)
  {
    return;
  }

  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      getAttr(AttrType::INDEX_ALLOCATION);
  if (vec.empty())
  {
    return;
  }

  IndexBlock ib;
  if (!static_cast<AttrIndexAlloc<S>*>(vec.front().get())
           ->ParseIndexBlock(vcn, ib))
  {
    return;
  }

  for (const IndexEntry& ie : ib)
  {
    if (ie.IsSubNodePtr())
    {
      // recursive call
      TraverseSubNode(ie.GetSubNodeVCN(), seCallBack, context, visitedVcns,
                      depth + 1);
    }

    if (ie.HasName())
    {
      seCallBack(ie, context);
    }
  }
}

// Parse all the attributes in a File Record
// And insert them into a link list
template <Strategy S>
bool FileRecord<S>::ParseAttrs()
{
  // A fresh chain, unrelated to any previous ParseFileRecord() on this object.
  std::unordered_set<ULONGLONG> attrListChain;
  return ParseAttrs(attrListChain);
}

template <Strategy S>
bool FileRecord<S>::ParseAttrs(std::unordered_set<ULONGLONG>& attrListChain)
{
  assert(file_record_);

  // Clear previous data
  ClearAttrs();

  // Visit all attributes

  DWORD dataPtr = 0;  // guard if data exceeds file_record_size_ bounds
  const AttrHeaderCommon* ahc = file_record_->HeaderCommon();

  if (ahc == nullptr)
  {
    return false;
  }

  dataPtr += file_record_->GetData()->offset_of_attr;

  // True only while the header fits, it isn't the terminator, and the
  // whole attribute fits within the record.
  while ((static_cast<ULONGLONG>(dataPtr) + sizeof(AttrHeaderCommon) <=
          volume_.GetFileRecordSize()) &&
         ahc->type != AttrType::ALL &&
         (static_cast<ULONGLONG>(dataPtr) + ahc->total_size <=
          volume_.GetFileRecordSize()))
  {
    const DWORD minTotalSize =
        ahc->non_resident != 0
            ? Attr::kHeaderNonResidentBaseSize
            : static_cast<DWORD>(sizeof(Attr::HeaderResident));
    if (ahc->total_size < minTotalSize)
    {
      LogWarn("Attribute total_size too small for its header.");
      return false;
    }

    if (ahc->non_resident != 0)
    {
      const auto& nonResident =
          reinterpret_cast<const Attr::HeaderNonResident&>(*ahc);
      if (Attr::HasCompressedSizeField(nonResident) &&
          ahc->total_size < minTotalSize + Attr::kCompressedSizeFieldSize)
      {
        LogWarn(
            "Compressed attribute total_size too small for its compressed "
            "size field.");
        return false;
      }
    }

    // True only when the type is a real attribute slot and the caller's
    // mask requests that slot.
    if (IsValidAttrType(ahc->type) &&
        static_cast<bool>(ATTR_MASK(ahc->type) & attr_mask_))
    {
      if (!ParseAttr(*ahc, attrListChain))
      {
        return false;
      }
    }

    dataPtr += ahc->total_size;
    ahc = reinterpret_cast<const AttrHeaderCommon*>(
        reinterpret_cast<const BYTE*>(ahc) +
        ahc->total_size);  // next attribute
  }

  AttachEfsContext();
  return true;
}

// The largest $EFS stream read. It holds a few key entries, a few KiB at
// most. A forged size must not decide how much memory a parse allocates.
constexpr ULONGLONG kMaxEfsStreamSize = 64ULL * 1024;

// Name of the $LOGGED_UTILITY_STREAM that holds the EFS keys.
constexpr std::wstring_view kEfsStreamName = L"$EFS";

// Copies the key entries out of this record's $EFS stream. Returns none if the
// stream is absent or malformed: the parse goes on, and the read that needs
// the key fails, with the cause logged.
template <Strategy S>
std::vector<Efs::WrappedFek> FileRecord<S>::ReadEfsEntries() const
{
#if !(defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
      (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)))
  // Neither backend is compiled in: there is no decryptor to feed keys to.
  return {};
#else
  for (const std::unique_ptr<AttrBase<S>>& attr :
       attr_list_[ATTR_INDEX(AttrType::LOGGED_UTILITY_STREAM)])
  {
    if (attr->GetAttrName() != kEfsStreamName)
    {
      continue;
    }

    const ULONGLONG size = attr->GetDataSize();
    if (size > kMaxEfsStreamSize)
    {
      LogWarn("$EFS stream is too large: {} bytes.", size);
      return {};
    }

    // Owned copy: under NO_CACHE the attribute's bytes are short-lived.
    std::vector<BYTE> bytes(static_cast<size_t>(size));
    const std::optional<ULONGLONG> read = attr->ReadData(0, bytes);
    if (!read || *read != size)
    {
      LogWarn("Cannot read the $EFS stream.");
      return {};
    }

    return Efs::ParseEfsStream(bytes).value_or(std::vector<Efs::WrappedFek>{});
  }

  return {};
#endif
}

// Gives every encrypted $DATA stream of this record the context that
// decrypts it. Only a non-resident stream is encrypted: EFS never leaves file
// data inside the record.
template <Strategy S>
void FileRecord<S>::AttachEfsContext()
{
#if !(defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
      (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)))
  // Neither backend is compiled in: no stream ever gets a decryption context,
  // so ReadData() returns raw ciphertext for an encrypted $DATA stream.
  return;
#else
  // AttrHeaderCommon::flags bit 0: the on-disk "compressed" flag. Real NTFS
  // never sets it alongside 0x4000 (compression and encryption are mutually
  // exclusive), but a forged record could. Decrypting a compressed stream's
  // bytes before LZNT1 decoding sees them would corrupt them for no gain, so
  // that combination is left undecrypted rather than misprocessed.
  constexpr WORD kAttrFlagCompressed = 0x0001;

  std::vector<AttrNonResident<S>*> encrypted;
  for (const std::unique_ptr<AttrBase<S>>& attr :
       attr_list_[ATTR_INDEX(AttrType::DATA)])
  {
    const WORD flags = attr->GetAttrFlags();
    if ((flags & Efs::kAttrFlagEncrypted) == 0)
    {
      continue;
    }
    if ((flags & kAttrFlagCompressed) != 0)
    {
      LogWarn(
          "A $DATA stream is flagged both compressed and encrypted; NTFS "
          "never combines them. Reading it undecrypted.");
      continue;
    }

    auto* nonResident = dynamic_cast<AttrNonResident<S>*>(attr.get());
    if (nonResident == nullptr)
    {
      LogWarn("A resident $DATA is flagged encrypted. Read as is.");
      continue;
    }
    encrypted.push_back(nonResident);
  }

  if (encrypted.empty())
  {
    return;
  }

  // One context for the record: its streams share one FEK, resolved once.
  auto context = std::make_shared<const Efs::Context>(
      ReadEfsEntries(),
      [&volume = volume_] { return volume.GetEfsKeyProvider(); });
  for (AttrNonResident<S>* stream : encrypted)
  {
    stream->SetEfsContext(context);
  }
#endif
}

template <Strategy S>
std::optional<ULONGLONG> FileRecord<S>::GetFileReference() const noexcept
{
  return file_reference_;
}

template <Strategy S>
WORD FileRecord<S>::GetSequenceNumber() const noexcept
{
  return file_record_ ? file_record_->GetData()->seq_no : 0;
}

template <Strategy S>
ULONGLONG FileRecord<S>::GetBaseRecordReference() const noexcept
{
  return file_record_
             ? file_record_->GetData()->ref_to_base & kMftRecordNumberMask
             : 0;
}

// Install Attribute raw data CallBack routines for a single File Record
template <Strategy S>
bool FileRecord<S>::InstallAttrRawCB(AttrType attrType,
                                     AttrRawCallback cb) noexcept
{
  const DWORD atIdx = ATTR_INDEX(attrType);
  if (atIdx >= kAttrNums)
  {
    return false;
  }

  attr_raw_call_back_[atIdx] = cb;
  return true;
}

// Clear all Attribute CallBack routines
template <Strategy S>
void FileRecord<S>::ClearAttrRawCB() noexcept
{
  for (AttrRawCallback& cb : attr_raw_call_back_)
  {
    cb = nullptr;
  }
}

// Choose attributes to handle, unwanted attributes will be discarded silently
template <Strategy S>
void FileRecord<S>::SetAttrMask(Mask mask) noexcept
{
  // Standard Information and Attribute List is needed always
  attr_mask_ = mask | Mask::STANDARD_INFORMATION | Mask::ATTRIBUTE_LIST;

  // The $EFS stream holds the key of every encrypted $DATA.
  if ((mask & Mask::DATA) == Mask::DATA)
  {
    attr_mask_ |= Mask::LOGGED_UTILITY_STREAM;
  }
}

// Traverse all Attribute and return CAttr_xxx classes to User Callback routine
template <Strategy S>
void FileRecord<S>::TraverseAttrs(ATTRS_CALLBACK<S> attrCallBack, void* context)
{
  if (!attrCallBack)
  {
    LogWarn("TraverseAttrs() called with an empty callback");
    return;
  }

  for (size_t i = 0; i < kAttrNums; i++)
  {
    // skip masked attributes
    if (static_cast<bool>(attr_mask_ & (static_cast<Mask>(1U << i))))
    {
      for (const std::unique_ptr<AttrBase<S>>& ab : attr_list_[i])
      {
        bool bStop = false;
        attrCallBack(*ab.get(), context, &bStop);
        if (bStop)
        {
          return;
        }
      }
    }
  }
}

// Find Attributes
template <Strategy S>
const std::vector<std::unique_ptr<AttrBase<S>>>&
    FileRecord<S>::getAttr(AttrType attrType) const noexcept
{
  static std::vector<std::unique_ptr<AttrBase<S>>> dummy{};
  const DWORD attrIdx = ATTR_INDEX(attrType);

  if (attrIdx >= kAttrNums)
  {
    return dummy;
  }

  return attr_list_[attrIdx];
}

template <Strategy S>
std::vector<std::unique_ptr<AttrBase<S>>>&
    FileRecord<S>::getAttr(AttrType attrType) noexcept
{
  static std::vector<std::unique_ptr<AttrBase<S>>> dummy{};
  const DWORD attrIdx = ATTR_INDEX(attrType);

  if (attrIdx >= kAttrNums)
  {
    return dummy;
  }

  return attr_list_[attrIdx];
}

// Get File Name (First Win32 name)
template <Strategy S>
std::wstring_view FileRecord<S>::GetFileName() const
{
  // A file may have several filenames
  // Return the first Win32 filename
  for (const std::unique_ptr<AttrBase<S>>& fn_ :
       attr_list_[ATTR_INDEX(AttrType::FILE_NAME)])
  {
    const Filename* fn;
    switch (S)
    {
      case Strategy::NO_CACHE:
      {
        fn = reinterpret_cast<
            const AttrFileName<AttrResidentNoCache, Strategy::NO_CACHE>*>(
            fn_.get());
        break;
      }
      case Strategy::FULL_CACHE:
      {
        fn = reinterpret_cast<
            const AttrFileName<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
            fn_.get());
        break;
      }
      default:
        assert(false);
        return {};
    }

    if (fn->IsWin32Name() && !fn->GetFilename().empty())
    {
      return fn->GetFilename();
    }
  }

  return {};
}

// Get File Size
template <Strategy S>
ULONGLONG FileRecord<S>::GetFileSize() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::FILE_NAME)];
  if (vec.empty())
  {
    return 0;
  }
  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrFileName<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->GetFileSize();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<const AttrFileName<AttrResidentFullCache,
                                               Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->GetFileSize();
  }
  return 0;
}

// Get File Times
template <Strategy S>
void FileRecord<S>::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                                FILETIME* accessTm) const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  // Standard Information attribute hold the most updated file time
  if (!vec.empty())
  {
    if (S == Strategy::NO_CACHE)
    {
      return reinterpret_cast<
                 const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
                 vec.front().get())
          ->GetFileTime(writeTm, createTm, accessTm);
    }
    else if (S == Strategy::FULL_CACHE)
    {
      return reinterpret_cast<const AttrStdInfo<AttrResidentFullCache,
                                                Strategy::FULL_CACHE>*>(
                 vec.front().get())
          ->GetFileTime(writeTm, createTm, accessTm);
    }
    return;
  }

  if (writeTm != nullptr)
  {
    writeTm->dwHighDateTime = 0;
    writeTm->dwLowDateTime = 0;
  }
  if (createTm != nullptr)
  {
    createTm->dwHighDateTime = 0;
    createTm->dwLowDateTime = 0;
  }
  if (accessTm != nullptr)
  {
    accessTm->dwHighDateTime = 0;
    accessTm->dwLowDateTime = 0;
  }
}

// Traverse all sub directories and files contained
// Call user defined callback routine once found an entry
template <Strategy S>
void FileRecord<S>::TraverseSubEntries(SUBENTRY_CALLBACK seCallBack,
                                       void* context,
                                       bool recoverOrphanedBlocks) const
{
  assert(seCallBack);

  // Start traversing from IndexRoot (B+ tree root node)

  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      getAttr(AttrType::INDEX_ROOT);
  if (vec.empty())
  {
    // No IndexRoot at all to start the normal walk from, but $INDEX_ALLOCATION
    // blocks may still exist and hold every entry.
    if (recoverOrphanedBlocks)
    {
      std::unordered_set<ULONGLONG> visitedVcns;
      ScanOrphanedIndexBlocks(seCallBack, context, visitedVcns);
    }
    return;
  }

  const std::vector<IndexEntry>* all_ie;

  switch (S)
  {
    case Strategy::NO_CACHE:
    {
      const auto* ir = reinterpret_cast<
          const AttrIndexRoot<AttrResidentNoCache, Strategy::NO_CACHE>*>(
          vec.front().get());

      if (!ir->IsFileName())
      {
        return;
      }
      all_ie = ir;
      break;
    }
    case Strategy::FULL_CACHE:
    {
      const auto* ir = reinterpret_cast<
          const AttrIndexRoot<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
          vec.front().get());

      if (!ir->IsFileName())
      {
        return;
      }
      all_ie = ir;
      break;
    }
    default:
    {
      assert(false);
      return;
    }
  }

  std::unordered_set<ULONGLONG> visitedVcns;

  for (const IndexEntry& ie : *all_ie)
  {
    // Visit subnode first
    if (ie.IsSubNodePtr())
    {
      TraverseSubNode(ie.GetSubNodeVCN(), seCallBack, context, visitedVcns, 0);
    }

    if (ie.HasName())
    {
      seCallBack(ie, context);
    }
  }

  if (recoverOrphanedBlocks)
  {
    ScanOrphanedIndexBlocks(seCallBack, context, visitedVcns);
  }
}

// Recovery pass for TraverseSubEntries(): a corrupt $INDEX_ROOT or internal
// node can leave real $INDEX_ALLOCATION blocks with no surviving pointer to
// them. Since every name appears exactly once in the B+ tree, scanning every
// block the normal walk missed finds them without relying on any pointer at
// all - unlike the normal walk, in VCN order rather than collation order.
template <Strategy S>
void FileRecord<S>::ScanOrphanedIndexBlocks(
    SUBENTRY_CALLBACK seCallBack, void* context,
    std::unordered_set<ULONGLONG>& visitedVcns) const
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      getAttr(AttrType::INDEX_ALLOCATION);
  if (vec.empty())
  {
    return;
  }

  auto* alloc = static_cast<AttrIndexAlloc<S>*>(vec.front().get());
  const ULONGLONG blockCount = alloc->GetIndexBlockCount();
  const std::optional<ULONGLONG> selfRef = GetFileReference();

  for (ULONGLONG vcn = 0; vcn < blockCount; vcn++)
  {
    if (!visitedVcns.insert(vcn).second)
    {
      continue;
    }

    IndexBlock ib;
    if (!alloc->ParseIndexBlock(vcn, ib))
    {
      continue;
    }

    LogWarn("TraverseSubEntries() recovery: reporting orphaned index block {}",
            vcn);

    for (const IndexEntry& ie : ib)
    {
      if (!ie.HasName())
      {
        continue;
      }
      // An orphaned block may hold a stale entry left over from a file
      // already deleted from this directory - only report one still filed
      // under it.
      if (selfRef && ie.GetParentReference() != *selfRef)
      {
        continue;
      }
      seCallBack(ie, context);
    }
  }
}

// Find a specific Filename from InexRoot described B+ tree
template <Strategy S>
std::optional<IndexEntry>
    FileRecord<S>::FindSubEntry(std::wstring_view fileName) const
{
  // Start searching from IndexRoot (B+ tree root node)
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      getAttr(AttrType::INDEX_ROOT);
  if (vec.empty())
  {
    return {};
  }

  const std::vector<IndexEntry>* all_ie = nullptr;

  if (S == Strategy::NO_CACHE)
  {
    const auto* ir = reinterpret_cast<
        const AttrIndexRoot<AttrResidentNoCache, Strategy::NO_CACHE>*>(
        vec.front().get());

    if (!ir->IsFileName())
    {
      return {};
    }
    all_ie = ir;
  }
  else if (S == Strategy::FULL_CACHE)
  {
    const auto* ir = reinterpret_cast<
        const AttrIndexRoot<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
        vec.front().get());

    if (!ir->IsFileName())
    {
      return {};
    }
    all_ie = ir;
  }
  else
  {
    return {};
  }

  std::unordered_set<ULONGLONG> visitedVcns;

  for (const IndexEntry& ie : *all_ie)
  {
    if (ie.HasName())
    {
      // Compare name
      const int i = ie.Compare(fileName);
      if (i == 0)
      {
        // Must be a copy: ie's shared_ptr<BYTE[]> keeps its backing bytes
        // alive independently of this FileRecord.
        LogDebug("FindSubEntry() found entry in Index Root");
        return ie;
      }
      if (i < 0)  // fileName is smaller than IndexEntry
      {
        // Visit SubNode
        if (ie.IsSubNodePtr())
        {
          // Search in SubNode (IndexBlock)
          std::optional<IndexEntry> retval =
              VisitIndexBlock(ie.GetSubNodeVCN(), fileName, visitedVcns, 0);
          if (retval)
          {
            return retval;
          }
        }
        // not found
        else
        {
          return {};
        }
      }
      // Just step forward if fileName is bigger than IndexEntry
    }
    else if (ie.IsSubNodePtr())
    {
      // Search in SubNode (IndexBlock)
      std::optional<IndexEntry> retval =
          VisitIndexBlock(ie.GetSubNodeVCN(), fileName, visitedVcns, 0);
      if (retval)
      {
        return retval;
      }
    }
  }

  return {};
}

// Find Data attribute class of
template <Strategy S>
const AttrBase<S>* FileRecord<S>::FindStream(std::wstring_view name)
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      getAttr(AttrType::DATA);
  for (const std::unique_ptr<AttrBase<S>>& data : vec)
  {
    // Unnamed stream
    if (data->IsUnNamed() && name.empty())
    {
      LogDebug("FindStream() found the unnamed stream");
      return data.get();
    }
    // Named stream
    if ((!data->IsUnNamed()) && data->GetAttrName() == name)
    {
      LogDebug("FindStream() found stream named \"{}\"", WideToUtf8(name));
      return data.get();
    }
  }

  LogDebug("FindStream() found no stream named \"{}\"", WideToUtf8(name));
  return nullptr;
}

// Check if it's deleted or in use
template <Strategy S>
bool FileRecord<S>::IsDeleted() const noexcept
{
  if (!file_record_)
  {
    LogWarn("IsDeleted() called on a FileRecord with no parsed record");
    return false;
  }

  return !static_cast<bool>(file_record_->GetData()->flags &
                            Flag::FileRecord::INUSE);
}

// Check if it's a directory
template <Strategy S>
bool FileRecord<S>::IsDirectory() const noexcept
{
  if (!file_record_)
  {
    LogWarn("IsDirectory() called on a FileRecord with no parsed record");
    return false;
  }

  return static_cast<bool>(file_record_->GetData()->flags &
                           Flag::FileRecord::DIR);
}

template <Strategy S>
bool FileRecord<S>::IsReadOnly() const noexcept
{
  // Standard Information attribute holds the most updated file time
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsReadOnly();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsReadOnly();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsHidden() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsHidden();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsHidden();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsSystem() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsSystem();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsSystem();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsCompressed() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsCompressed();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsCompressed();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsEncrypted() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsEncrypted();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsEncrypted();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsSparse() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsSparse();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsSparse();
  }
  return false;
}

template class FileRecord<Strategy::NO_CACHE>;
template class FileRecord<Strategy::FULL_CACHE>;

/*
template <Strategy S>
template <typename RESIDENT>
std::unique_ptr<AttrBase<Strategy::FULL_CACHE>>
    FileRecord<S>::AllocAttr<AttrResidentFullCache>(
        const AttrHeaderCommon& ahc, bool& bUnhandled);
template <Strategy S>
template <typename RESIDENT>
std::unique_ptr<AttrBase<Strategy::NO_CACHE>>
    FileRecord<S>::AllocAttr<AttrResidentNoCache>(
        const AttrHeaderCommon& ahc, bool& bUnhandled);
        */
}  // namespace NtfsBrowser
