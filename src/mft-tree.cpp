#include <algorithm>
#include <exception>
#include <unordered_set>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/filename.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/mft-tree.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr-file-name.h"
#include "attr-resident.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{
namespace
{

// The root directory's record number, fixed by NTFS.
constexpr ULONGLONG kRootRecord = static_cast<ULONGLONG>(Enum::MftIdx::ROOT);

// Records between two progress() calls: often enough for a live progress
// line, rare enough to cost nothing next to a record read.
constexpr ULONGLONG kProgressInterval = 4096;

// The sequence number NTFS gives a record when it frees it: one more,
// skipping 0, since a reference carrying 0 means "do not check".
WORD NextSequence(WORD sequence) noexcept
{
  return sequence == 0xFFFF ? 1 : static_cast<WORD>(sequence + 1);
}

// The Filename side of a $FILE_NAME attribute. FileRecord<S> always wraps a
// $FILE_NAME in the AttrFileName over the resident type that matches S.
template <Strategy S>
const Filename& AsFilename(const AttrBase<S>& attr)
{
  if constexpr (S == Strategy::NO_CACHE)
  {
    return static_cast<
        const AttrFileName<AttrResidentNoCache, Strategy::NO_CACHE>&>(attr);
  }
  else
  {
    return static_cast<
        const AttrFileName<AttrResidentFullCache, Strategy::FULL_CACHE>&>(attr);
  }
}

// Copies what the record fr just parsed says about its file into entry.
// Returns false when an attribute failed to parse: entry then holds what
// parsed before the failure.
template <Strategy S>
bool ReadEntry(FileRecord<S>& fr, ULONGLONG record, MftEntry& entry)
{
  bool parsed = false;
  try
  {
    parsed = fr.ParseAttrs();
  }
  catch (const std::exception& e)
  {
    LogException(e);
  }

  entry.record = record;
  entry.sequence = fr.GetSequenceNumber();
  entry.in_use = !fr.IsDeleted();
  entry.directory = fr.IsDirectory();

  for (const std::unique_ptr<AttrBase<S>>& attr :
       fr.getAttr(AttrType::FILE_NAME))
  {
    const Filename& fn = AsFilename<S>(*attr);
    const std::wstring_view name = fn.GetFilename();
    if (name.empty())
    {
      continue;
    }

    MftName& added = entry.names.emplace_back();
    added.name = std::wstring(name);
    added.parent_record = fn.GetParentReference();
    added.parent_sequence = fn.GetParentSequenceNumber();
    added.dos_only = !fn.IsWin32Name();
  }

  const AttrBase<S>* data = fr.FindStream({});
  entry.size = data != nullptr ? data->GetDataSize() : fr.GetFileSize();

  fr.GetFileTime(&entry.write_time, &entry.create_time, &entry.access_time);
  entry.read_only = fr.IsReadOnly();
  entry.hidden = fr.IsHidden();
  entry.system = fr.IsSystem();
  entry.compressed = fr.IsCompressed();
  entry.encrypted = fr.IsEncrypted();
  entry.sparse = fr.IsSparse();

  return parsed;
}

// The name a record's path goes through by default: the first non-DOS name
// with a valid parent, else the first non-DOS name, else the first name.
const MftName* PrimaryName(const MftEntry& entry) noexcept
{
  const MftName* fallback = nullptr;
  for (const MftName& name : entry.names)
  {
    if (name.dos_only)
    {
      continue;
    }
    if (name.parent_valid)
    {
      return &name;
    }
    if (fallback == nullptr)
    {
      fallback = &name;
    }
  }
  if (fallback != nullptr)
  {
    return fallback;
  }
  return entry.names.empty() ? nullptr : &entry.names.front();
}

}  // namespace

MftTree::MftTree(const NtfsVolume<Strategy::NO_CACHE>& volume,
                 const MftScanOptions& options)
{
  Scan(volume, options);
}

MftTree::MftTree(const NtfsVolume<Strategy::FULL_CACHE>& volume,
                 const MftScanOptions& options)
{
  Scan(volume, options);
}

// Reads every record slot of volume's $MFT through one reused FileRecord,
// keeps each base record's entry, then links the entries into a tree.
template <Strategy S>
void MftTree::Scan(const NtfsVolume<S>& volume, const MftScanOptions& options)
{
  const ULONGLONG total = volume.GetRecordsCount();
  stats_.slots = total;

  FileRecord<S> fr(volume);
  fr.SetAttrMask(Mask::FILE_NAME | Mask::DATA);

  for (ULONGLONG record = 0; record < total; record++)
  {
    // Every kProgressInterval records, the caller follows along or stops.
    if (options.progress && record % kProgressInterval == 0 &&
        !options.progress(record, total))
    {
      stats_.complete = false;
      break;
    }

    if (!fr.ParseFileRecord(record))
    {
      stats_.unreadable++;
      continue;
    }
    if (fr.GetBaseRecordReference() != 0)
    {
      stats_.extensions++;
      continue;
    }
    if (fr.IsDeleted())
    {
      stats_.deleted++;
      if (!options.include_deleted)
      {
        continue;
      }
    }
    else
    {
      stats_.in_use++;
    }

    MftEntry entry;
    if (!ReadEntry(fr, record, entry))
    {
      stats_.damaged++;
    }
    by_record_.emplace(record, entries_.size());
    entries_.push_back(std::move(entry));
  }

  if (options.progress && stats_.complete)
  {
    options.progress(total, total);
  }

  LogInfo("MFT scan: {} slots, {} in use, {} deleted, {} unreadable",
          stats_.slots, stats_.in_use, stats_.deleted, stats_.unreadable);
  Link();
}

// Validates every name's parent reference, files each record under the
// parents its valid names give, then marks what the root reaches.
void MftTree::Link()
{
  for (MftEntry& entry : entries_)
  {
    // Parents this entry is already filed under: two hard links in one
    // directory still list the record once.
    std::vector<ULONGLONG> filedUnder;
    for (MftName& name : entry.names)
    {
      name.parent_valid = IsValidParent(entry, name);
      // Only a valid, non-DOS name files a record, and the root is filed
      // under nothing.
      if (!name.parent_valid || name.dos_only || entry.record == kRootRecord)
      {
        continue;
      }
      if (std::find(filedUnder.begin(), filedUnder.end(), name.parent_record) !=
          filedUnder.end())
      {
        continue;
      }
      filedUnder.push_back(name.parent_record);
      children_[name.parent_record].push_back(entry.record);
    }
  }

  reachable_.assign(entries_.size(), false);
  if (const auto root = by_record_.find(kRootRecord); root != by_record_.end())
  {
    reachable_[root->second] = true;
  }

  std::vector<ULONGLONG> pending{kRootRecord};
  while (!pending.empty())
  {
    const ULONGLONG dir = pending.back();
    pending.pop_back();
    for (const ULONGLONG child : Children(dir))
    {
      const size_t index = by_record_.at(child);
      if (!reachable_[index])
      {
        reachable_[index] = true;
        pending.push_back(child);
      }
    }
  }

  stats_.unreachable = static_cast<ULONGLONG>(
      std::count(reachable_.begin(), reachable_.end(), false));
}

// Whether name's parent reference, read from child's record, names a
// directory the scan found. See the rules in mft-tree.h.
bool MftTree::IsValidParent(const MftEntry& child, const MftName& name) const
{
  // Only the root directory is filed under itself.
  if (name.parent_record == child.record)
  {
    return child.record == kRootRecord;
  }

  const MftEntry* parent = Find(name.parent_record);
  if (parent == nullptr)
  {
    return name.parent_record == kRootRecord;
  }
  if (!parent->directory)
  {
    return false;
  }

  // The parent is the same one the name was filed under: unchecked, still
  // the same generation, or the generation NTFS freed right after.
  return name.parent_sequence == 0 ||
         name.parent_sequence == parent->sequence ||
         (!parent->in_use &&
          parent->sequence == NextSequence(name.parent_sequence));
}

// Joins name and its ancestors' primary names up to the root, or up to the
// record where the chain of valid parent references breaks.
std::wstring MftTree::PathThrough(const MftEntry& entry, const MftName& name,
                                  std::optional<ULONGLONG>* lostAncestor) const
{
  if (lostAncestor != nullptr)
  {
    lostAncestor->reset();
  }
  if (entry.record == kRootRecord)
  {
    return L"\\";
  }

  std::vector<const std::wstring*> parts;
  // Guards against a forged chain of parents that loops back on itself.
  std::unordered_set<ULONGLONG> visited{entry.record};
  const MftName* current = &name;
  std::optional<ULONGLONG> lost;

  while (true)
  {
    parts.push_back(&current->name);
    const ULONGLONG parent = current->parent_record;
    if (!current->parent_valid || !visited.insert(parent).second)
    {
      lost = parent;
      break;
    }
    if (parent == kRootRecord)
    {
      break;
    }

    const MftEntry* parentEntry = Find(parent);
    current = parentEntry != nullptr ? PrimaryName(*parentEntry) : nullptr;
    if (current == nullptr)
    {
      lost = parent;
      break;
    }
  }

  std::wstring path;
  for (auto part = parts.rbegin(); part != parts.rend(); ++part)
  {
    if (!path.empty() || !lost)
    {
      path += L'\\';
    }
    path += **part;
  }

  if (lostAncestor != nullptr)
  {
    *lostAncestor = lost;
  }
  return path;
}

const std::vector<MftEntry>& MftTree::Entries() const noexcept
{
  return entries_;
}

const MftEntry* MftTree::Find(ULONGLONG record) const
{
  const auto it = by_record_.find(record);
  return it == by_record_.end() ? nullptr : &entries_[it->second];
}

std::span<const ULONGLONG> MftTree::Children(ULONGLONG dirRecord) const
{
  const auto it = children_.find(dirRecord);
  if (it == children_.end())
  {
    return {};
  }
  return it->second;
}

bool MftTree::IsReachable(ULONGLONG record) const
{
  const auto it = by_record_.find(record);
  return it != by_record_.end() && reachable_[it->second];
}

std::wstring MftTree::GetPath(ULONGLONG record,
                              std::optional<ULONGLONG>* lostAncestor) const
{
  const MftEntry* entry = Find(record);
  const MftName* name = entry != nullptr ? PrimaryName(*entry) : nullptr;
  if (name == nullptr)
  {
    if (lostAncestor != nullptr)
    {
      lostAncestor->reset();
    }
    return {};
  }
  return PathThrough(*entry, *name, lostAncestor);
}

std::wstring MftTree::GetPath(ULONGLONG record, size_t nameIndex,
                              std::optional<ULONGLONG>* lostAncestor) const
{
  const MftEntry* entry = Find(record);
  if (entry == nullptr || nameIndex >= entry->names.size())
  {
    if (lostAncestor != nullptr)
    {
      lostAncestor->reset();
    }
    return {};
  }
  return PathThrough(*entry, entry->names[nameIndex], lostAncestor);
}

const MftScanStats& MftTree::Stats() const noexcept { return stats_; }

}  // namespace NtfsBrowser
