#include "io/mft-runs.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Thousandths in a whole, the unit of WideGap::min_share_per_mille.
constexpr ULONGLONG per_mille = 1000;

}  // namespace

std::vector<MftRun> PlanMftRuns(std::span<const ULONGLONG> slots,
                                ULONGLONG max_gap_slots,
                                ULONGLONG max_run_slots, const WideGap& wide) {
  max_run_slots = (std::max<ULONGLONG>)(1, max_run_slots);
  std::vector<MftRun> runs;
  for (size_t index = 0; index < slots.size(); index++) {
    const ULONGLONG slot = slots[index];
    if (!runs.empty()) {
      MftRun& run = runs.back();
      const ULONGLONG hole = slot - run.last_slot - 1;
      const ULONGLONG span = slot - run.first_slot + 1;
      const ULONGLONG wanted = index + 1 - run.begin;
      // A narrow hole, or a wide one that the run is dense enough to cross,
      // and the run stays short.
      if ((hole <= max_gap_slots ||
           (hole <= wide.gap_slots &&
            wanted * per_mille >= span * wide.min_share_per_mille)) &&
          span <= max_run_slots) {
        run.last_slot = slot;
        run.end = index + 1;
        continue;
      }
    }
    runs.push_back({.begin = index,
                    .end = index + 1,
                    .first_slot = slot,
                    .last_slot = slot});
  }
  return runs;
}

WideGap WideGapOf(const Tuning::ProbeTuning& probe, Tuning::MediaKind kind,
                  ULONGLONG record_size, bool cached) noexcept {
  if (kind != Tuning::MediaKind::Rotational || !cached || record_size == 0 ||
      probe.wide_gap_bytes <= probe.max_gap_bytes) {
    return {};
  }
  return {.gap_slots = probe.wide_gap_bytes / record_size,
          .min_share_per_mille = probe.wide_gap_min_share_per_mille};
}

std::vector<MftRun> WidenToClusters(std::vector<MftRun> runs,
                                    ULONGLONG slots_per_cluster,
                                    ULONGLONG first_readable_slot) {
  if (slots_per_cluster < 2) {
    return runs;
  }
  std::vector<MftRun> widened;
  for (MftRun run : runs) {
    const ULONGLONG aligned =
        run.first_slot - (run.first_slot % slots_per_cluster);
    run.first_slot =
        (std::max)(aligned, (std::min)(run.first_slot, first_readable_slot));
    run.last_slot +=
        slots_per_cluster - 1 - (run.last_slot % slots_per_cluster);
    if (!widened.empty() && run.first_slot <= widened.back().last_slot + 1) {
      MftRun& previous = widened.back();
      previous.last_slot = (std::max)(previous.last_slot, run.last_slot);
      previous.end = run.end;
      continue;
    }
    widened.push_back(run);
  }
  return widened;
}

template <Cache::Strategy S>
ProbeRunRules ProbeRunRulesOf(const NtfsVolume<S>& volume, bool keeps) {
  const DWORD record_size = volume.GetFileRecordSize();
  const Tuning::ProbeTuning& probe = volume.GetOptions().tuning.probe;
  ProbeRunRules rules{.mapped = record_size != 0 && volume.HasMftBase(),
                      .record_size = record_size,
                      .max_gap_slots = 0,
                      .max_run_slots = 0,
                      .wide = {},
                      .slots_per_cluster = 0,
                      .first_mapped_slot =
                          static_cast<ULONGLONG>(Mft::Idx::User)};
  if (record_size == 0) {
    return rules;
  }
  rules.max_gap_slots = probe.max_gap_bytes / record_size;
  rules.max_run_slots = probe.max_run_bytes / record_size;
  rules.wide = WideGapOf(probe, volume.GetMediaKind(), record_size,
                         S == Cache::Strategy::FullCache);
  if (keeps) {
    rules.slots_per_cluster = volume.GetClusterSize() / record_size;
  }
  return rules;
}

ProbeRunPlan PlanProbeRuns(std::span<const ULONGLONG> chunk,
                           const ProbeRunRules& rules,
                           const HeldRecordSource& find,
                           const HeldRecordSink& held) {
  ProbeRunPlan plan;
  // The records below first_mapped_slot sort first.
  while (plan.alone < chunk.size() &&
         (!rules.mapped || chunk[plan.alone] < rules.first_mapped_slot)) {
    plan.alone++;
  }
  const std::span<const ULONGLONG> rest = chunk.subspan(plan.alone);
  if (!rules.mapped) {
    return plan;
  }

  if (find) {
    for (const ULONGLONG record : rest) {
      const std::span<const BYTE> bytes = find(record);
      if (bytes.size() == rules.record_size) {
        if (held) {
          held(record, bytes);
        }
      } else {
        plan.to_read.push_back(record);
      }
    }
  } else {
    plan.to_read.assign(rest.begin(), rest.end());
  }

  plan.runs = PlanMftRuns(plan.to_read, rules.max_gap_slots,
                          rules.max_run_slots, rules.wide);
  // A run that is kept is kept whole: the disk reads its clusters anyway.
  if (rules.slots_per_cluster >= 2) {
    plan.runs = WidenToClusters(std::move(plan.runs), rules.slots_per_cluster,
                                rules.first_mapped_slot);
  }
  return plan;
}

template NTFS_BROWSER_EXPORT_TESTS_ONLY ProbeRunRules
    ProbeRunRulesOf<Cache::Strategy::NoCache>(
        const NtfsVolume<Cache::Strategy::NoCache>&, bool);
template NTFS_BROWSER_EXPORT_TESTS_ONLY ProbeRunRules
    ProbeRunRulesOf<Cache::Strategy::FullCache>(
        const NtfsVolume<Cache::Strategy::FullCache>&, bool);

}  // namespace NTFS_BROWSER_NAMESPACE::Io
