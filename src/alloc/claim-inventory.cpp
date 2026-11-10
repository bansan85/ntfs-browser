#include "alloc/claim-inventory.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "attr/base.h"
#include "attr/slot.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "io/file-record.h"
#include "mft/idx.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

template <Cache::Strategy S>
class AttrBase;

}  // namespace Attr
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Alloc {

namespace {

// The name of the $Bad stream of $BadClus. It lists the bad clusters, and owns
// none of them.
constexpr std::wstring_view bad_stream_name = L"$Bad";

// One part of a run: [vcn, vcn + clusters) at lcn.
struct RunPart {
  ULONGLONG lcn;
  ULONGLONG vcn;
  ULONGLONG clusters;
  // The part lies past the initialized size: allocated, never written.
  bool unwritten;
};

// Appends the claims of one run part to out. A part longer than a claim holds
// is cut: what is left is noted in account, when the stream has one.
void AppendPart(std::vector<Claim>& out, std::uint32_t stream,
                const RunPart& part, AccountStream* account) {
  if (part.clusters == 0) {
    return;
  }
  const ULONGLONG held = (std::min)(part.clusters, max_claim_clusters);
  out.push_back(
      {.lcn = part.lcn,
       .vcn = part.vcn,
       .clusters_and_flags = static_cast<std::uint32_t>(held) |
                             (part.unwritten ? claim_unwritten_bit : 0U),
       .stream = stream});
  if (account != nullptr) {
    account->real_clusters += held;
    if (part.clusters > held) {
      account->invalid.push_back(
          {.vcn = part.vcn + held, .clusters = part.clusters - held});
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
    AppendPart(out, stream,
               {.lcn = *run.lcn,
                .vcn = run.vcn,
                .clusters = written_end - run.vcn,
                .unwritten = false},
               account);
    AppendPart(out, stream,
               {.lcn = *run.lcn + (written_end - run.vcn),
                .vcn = written_end,
                .clusters = run_end - written_end,
                .unwritten = true},
               account);
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

// Whether the stream is the $Bad stream of $BadClus.
bool IsBadStream(ULONGLONG record, Attr::Type type, std::wstring_view name) {
  return record == static_cast<ULONGLONG>(Mft::Idx::BadCluster) &&
         type == Attr::Type::Data && name == bad_stream_name;
}

// Builds the streams, accounts and claims of one record.
class ClaimBuilder {
 public:
  explicit ClaimBuilder(RecordClaims& out) : out_(&out) {}

  // Adds a stream that has an account: a data stream of a concerned record.
  void AddAccounted(std::wstring_view name, const StreamLayout& layout) {
    const std::uint32_t stream = StreamOf(name);
    AccountStream& account = out_->accounts.at(out_->stream_account.at(stream));
    TakeSizes(layout, account);
    AppendClaims(layout, stream, out_->claims, &account);
  }

  // Adds a stream that has no account.
  void AddPlain(const StreamLayout& layout) {
    const auto stream = static_cast<std::uint32_t>(out_->stream_account.size());
    out_->stream_account.push_back(no_account);
    AppendClaims(layout, stream, out_->claims, nullptr);
  }

 private:
  // The local stream of a concerned stream by name, made when first met.
  std::uint32_t StreamOf(std::wstring_view name) {
    for (std::size_t i = 0; i < out_->accounts.size(); i++) {
      if (out_->accounts.at(i).name == name) {
        return account_stream_.at(i);
      }
    }
    const auto stream = static_cast<std::uint32_t>(out_->stream_account.size());
    out_->stream_account.push_back(
        static_cast<std::uint32_t>(out_->accounts.size()));
    out_->accounts.push_back({.name = std::wstring(name)});
    account_stream_.push_back(stream);
    return stream;
  }

  RecordClaims* out_;
  // The local stream of each account, by account.
  std::vector<std::uint32_t> account_stream_;
};

}  // namespace

template <Cache::Strategy S>
RecordClaims CollectClaims(const Io::FileRecord<S>& file_record,
                           ULONGLONG record, bool freed, bool concerned) {
  RecordClaims out{.record = record,
                   .lsn = file_record.GetLogSequenceNumber(),
                   .freed = freed,
                   .concerned = concerned};
  ClaimBuilder builder(out);

  for (std::size_t index = 0; index < Attr::attr_nums; index++) {
    const auto type = static_cast<Attr::Type>((index + 1) << 4U);
    for (const std::unique_ptr<Attr::AttrBase<S>>& attr :
         file_record.GetAttr(type)) {
      if (!attr->IsNonResident()) {
        continue;
      }
      const std::wstring_view name = attr->GetAttrName();
      if (IsBadStream(record, type, name)) {
        continue;
      }
      const StreamLayout layout = attr->GetLayout();
      if (concerned && type == Attr::Type::Data) {
        builder.AddAccounted(name, layout);
      } else {
        builder.AddPlain(layout);
      }
    }
  }
  for (const StreamLayout& layout : file_record.GetClaimedLayouts()) {
    builder.AddPlain(layout);
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
