#include "alloc/claim-inventory.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "attr/slot.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

namespace {

// The name of the $Bad stream of $BadClus. It lists the bad clusters, and owns
// none of them.
constexpr std::wstring_view bad_stream_name = L"$Bad";

// Appends the claims of one run part, [vcn, vcn + clusters) at lcn, to out. A
// part longer than a claim holds is cut: what is left is noted in account,
// when the stream has one.
void AppendPart(std::vector<Claim>& out, std::uint32_t stream, ULONGLONG lcn,
                ULONGLONG vcn, ULONGLONG clusters, bool unwritten,
                AccountStream* account) {
  if (clusters == 0) {
    return;
  }
  const ULONGLONG held = (std::min)(clusters, max_claim_clusters);
  out.push_back({.lcn = lcn,
                 .vcn = vcn,
                 .clusters_and_flags = static_cast<std::uint32_t>(held) |
                                       (unwritten ? claim_unwritten_bit : 0U),
                 .stream = stream});
  if (account != nullptr) {
    account->real_clusters += held;
    if (clusters > held) {
      account->invalid.push_back(
          {.vcn = vcn + held, .clusters = clusters - held});
    }
  }
}

// Appends the claims of every real run of layout to out, each cut where the
// stream's initialized size falls. Sparse runs and runs that overflow 64 bits
// go to account, when the stream has one.
void AppendClaims(const StreamLayout& layout, std::uint32_t stream,
                  std::vector<Claim>& out, AccountStream* account) {
  for (const LayoutRun& run : layout.runs) {
    if (!run.lcn) {
      if (account != nullptr) {
        account->sparse.push_back({.vcn = run.vcn, .clusters = run.clusters});
      }
      continue;
    }
    if (run.clusters > (std::numeric_limits<ULONGLONG>::max)() - *run.lcn) {
      if (account != nullptr) {
        account->invalid.push_back({.vcn = run.vcn, .clusters = run.clusters});
      }
      continue;
    }
    // The run's VCNs before initialized_clusters were written, the others
    // never.
    const ULONGLONG run_end = run.vcn + run.clusters;
    const ULONGLONG written_end =
        (std::min)((std::max)(layout.initialized_clusters, run.vcn), run_end);
    AppendPart(out, stream, *run.lcn, run.vcn, written_end - run.vcn, false,
               account);
    AppendPart(out, stream, *run.lcn + (written_end - run.vcn), written_end,
               run_end - written_end, true, account);
  }
}

// What the first part of a stream says of it. A continuation has no sizes.
void TakeSizes(const StreamLayout& layout, AccountStream& account) {
  const bool first_part =
      layout.initialized_clusters != (std::numeric_limits<ULONGLONG>::max)();
  if (first_part || account.span == 0) {
    account.data_size = layout.data_size;
    account.initialized_size = layout.initialized_size;
    account.unit_clusters = layout.unit_clusters;
    account.encrypted = layout.encrypted;
  }
  account.span =
      (std::max)({account.span, layout.declared_clusters, layout.covered_end});
}

}  // namespace

template <Cache::Strategy S>
RecordClaims CollectClaims(const Io::FileRecord<S>& file_record,
                           ULONGLONG record, bool freed, bool concerned) {
  RecordClaims out{.record = record,
                   .lsn = file_record.GetLogSequenceNumber(),
                   .freed = freed,
                   .concerned = concerned};

  // The local stream of a concerned stream by name, made when first met.
  // account_stream holds it for each account.
  std::vector<std::uint32_t> account_stream;
  const auto stream_of = [&out, &account_stream](std::wstring_view name) {
    for (std::size_t i = 0; i < out.accounts.size(); i++) {
      if (out.accounts.at(i).name == name) {
        return account_stream.at(i);
      }
    }
    const auto stream = static_cast<std::uint32_t>(out.stream_account.size());
    out.stream_account.push_back(
        static_cast<std::uint32_t>(out.accounts.size()));
    out.accounts.push_back({.name = std::wstring(name)});
    account_stream.push_back(stream);
    return stream;
  };

  for (std::size_t index = 0; index < Attr::attr_nums; index++) {
    const auto type = static_cast<Attr::Type>((index + 1) << 4U);
    for (const std::unique_ptr<Attr::AttrBase<S>>& attr :
         file_record.GetAttr(type)) {
      if (!attr->IsNonResident()) {
        continue;
      }
      const std::wstring_view name = attr->GetAttrName();
      if (record == static_cast<ULONGLONG>(Mft::Idx::BadCluster) &&
          type == Attr::Type::Data && name == bad_stream_name) {
        continue;
      }
      const StreamLayout layout = attr->GetLayout();
      if (concerned && type == Attr::Type::Data) {
        const std::uint32_t stream = stream_of(name);
        AccountStream& account = out.accounts.at(out.stream_account.at(stream));
        TakeSizes(layout, account);
        AppendClaims(layout, stream, out.claims, &account);
        continue;
      }
      const auto stream = static_cast<std::uint32_t>(out.stream_account.size());
      out.stream_account.push_back(no_account);
      AppendClaims(layout, stream, out.claims, nullptr);
    }
  }
  for (const StreamLayout& layout : file_record.GetClaimedLayouts()) {
    const auto stream = static_cast<std::uint32_t>(out.stream_account.size());
    out.stream_account.push_back(no_account);
    AppendClaims(layout, stream, out.claims, nullptr);
  }
  return out;
}

void ClaimInventory::Add(RecordClaims&& record) {
  if (record.record > (std::numeric_limits<std::uint32_t>::max)()) {
    return;
  }
  if (record.claims.empty() && record.accounts.empty()) {
    return;
  }

  const auto owner = static_cast<std::uint32_t>(owners.size());
  owners.push_back({.lsn = record.lsn,
                    .record = static_cast<std::uint32_t>(record.record),
                    .freed = record.freed});
  std::uint32_t dense = 0;
  if (!record.accounts.empty()) {
    dense = static_cast<std::uint32_t>(concerned_records.size());
    concerned_records.push_back(record.record);
  }

  const auto stream_base = static_cast<std::uint32_t>(streams.size());
  const auto account_base = static_cast<std::uint32_t>(accounts.size());
  for (const std::uint32_t local : record.stream_account) {
    streams.push_back(
        {.owner = owner,
         .account = local == no_account ? no_account : account_base + local});
  }
  for (AccountStream& account : record.accounts) {
    account.owner_dense = dense;
    accounts.push_back(std::move(account));
  }
  for (Claim claim : record.claims) {
    claim.stream += stream_base;
    claims.push_back(claim);
  }
}

template RecordClaims CollectClaims<Cache::Strategy::NoCache>(
    const Io::FileRecord<Cache::Strategy::NoCache>&, ULONGLONG, bool, bool);
template RecordClaims CollectClaims<Cache::Strategy::FullCache>(
    const Io::FileRecord<Cache::Strategy::FullCache>&, ULONGLONG, bool, bool);

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
