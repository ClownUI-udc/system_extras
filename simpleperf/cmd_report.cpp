/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include "command.h"
#include "event_attr.h"
#include "event_type.h"
#include "perf_regs.h"
#include "record.h"
#include "record_file.h"
#include "sample_tree.h"
#include "thread_tree.h"
#include "tracing.h"
#include "utils.h"

using android::base::Split;

namespace {

static std::set<std::string> branch_sort_keys = {
    "dso_from",
    "dso_to",
    "symbol_from",
    "symbol_to",
};
struct BranchFromEntry {
  const MapEntry* map;
  const Symbol* symbol;
  uint64_t vaddr_in_file;
  uint64_t flags;

  BranchFromEntry() : map(nullptr), symbol(nullptr), vaddr_in_file(0), flags(0) {}
};

struct SampleEntry {
  uint64_t time;
  uint64_t period;
  // accumuated when appearing in other sample's callchain
  uint64_t accumulated_period;
  uint64_t sample_count;
  int cpu;
  pid_t pid;
  pid_t tid;
  const char* thread_comm;
  const MapEntry* map;
  const Symbol* symbol;
  uint64_t vaddr_in_file;
  BranchFromEntry branch_from;
  // a callchain tree representing all callchains in the sample
  CallChainRoot<SampleEntry> callchain;

  SampleEntry(uint64_t time, uint64_t period, uint64_t accumulated_period, uint64_t sample_count,
              int cpu, const ThreadEntry* thread, const MapEntry* map, const Symbol* symbol,
              uint64_t vaddr_in_file)
      : time(time),
        period(period),
        accumulated_period(accumulated_period),
        sample_count(sample_count),
        cpu(cpu),
        pid(thread->pid),
        tid(thread->tid),
        thread_comm(thread->comm),
        map(map),
        symbol(symbol),
        vaddr_in_file(vaddr_in_file) {}

  // The data member 'callchain' can only move, not copy.
  SampleEntry(SampleEntry&&) = default;
  SampleEntry(SampleEntry&) = delete;

  uint64_t GetPeriod() const { return period; }
};

struct SampleTree {
  std::vector<SampleEntry*> samples;
  uint64_t total_samples;
  uint64_t total_period;
  uint64_t total_error_callchains;
  std::string event_name;
};

BUILD_COMPARE_VALUE_FUNCTION(CompareVaddrInFile, vaddr_in_file);
BUILD_DISPLAY_HEX64_FUNCTION(DisplayVaddrInFile, vaddr_in_file);

static std::string DisplayEventName(const SampleEntry*, const SampleTree* info) {
  return info->event_name;
}

class ReportCmdSampleTreeBuilder : public SampleTreeBuilder<SampleEntry, uint64_t> {
 public:
  ReportCmdSampleTreeBuilder(const SampleComparator<SampleEntry>& sample_comparator,
                             ThreadTree* thread_tree)
      : SampleTreeBuilder(sample_comparator),
        thread_tree_(thread_tree),
        total_samples_(0),
        total_period_(0),
        total_error_callchains_(0) {}

  void SetFilters(const std::unordered_set<int>& cpu_filter,
                  const std::unordered_set<int>& pid_filter,
                  const std::unordered_set<int>& tid_filter,
                  const std::unordered_set<std::string>& comm_filter,
                  const std::unordered_set<std::string>& dso_filter,
                  const std::unordered_set<std::string>& symbol_filter) {
    cpu_filter_ = cpu_filter;
    pid_filter_ = pid_filter;
    tid_filter_ = tid_filter;
    comm_filter_ = comm_filter;
    dso_filter_ = dso_filter;
    symbol_filter_ = symbol_filter;
  }

  void SetEventName(const std::string& event_name) { event_name_ = event_name; }

  SampleTree GetSampleTree() {
    AddCallChainDuplicateInfo();
    SampleTree sample_tree;
    sample_tree.samples = GetSamples();
    sample_tree.total_samples = total_samples_;
    sample_tree.total_period = total_period_;
    sample_tree.total_error_callchains = total_error_callchains_;
    sample_tree.event_name = event_name_;
    return sample_tree;
  }

  virtual void ReportCmdProcessSampleRecord(std::shared_ptr<SampleRecord>& r) {
    return ProcessSampleRecord(*r);
  }

  virtual void ReportCmdProcessSampleRecord(const SampleRecord& r) {
    return ProcessSampleRecord(r);
  }

 protected:
  virtual uint64_t GetPeriod(const SampleRecord& r) = 0;

  SampleEntry* CreateSample(const SampleRecord& r, bool in_kernel, uint64_t* acc_info) override {
    const ThreadEntry* thread = thread_tree_->FindThreadOrNew(r.tid_data.pid, r.tid_data.tid);
    const MapEntry* map = thread_tree_->FindMap(thread, r.ip_data.ip, in_kernel);
    uint64_t vaddr_in_file;
    const Symbol* symbol = thread_tree_->FindSymbol(map, r.ip_data.ip, &vaddr_in_file);
    uint64_t period = GetPeriod(r);
    *acc_info = period;
    return InsertSample(std::make_unique<SampleEntry>(r.time_data.time, period, 0, 1, r.Cpu(),
                                                      thread, map, symbol, vaddr_in_file));
  }

  SampleEntry* CreateBranchSample(const SampleRecord& r, const BranchStackItemType& item) override {
    const ThreadEntry* thread = thread_tree_->FindThreadOrNew(r.tid_data.pid, r.tid_data.tid);
    const MapEntry* from_map = thread_tree_->FindMap(thread, item.from);
    uint64_t from_vaddr_in_file;
    const Symbol* from_symbol = thread_tree_->FindSymbol(from_map, item.from, &from_vaddr_in_file);
    const MapEntry* to_map = thread_tree_->FindMap(thread, item.to);
    uint64_t to_vaddr_in_file;
    const Symbol* to_symbol = thread_tree_->FindSymbol(to_map, item.to, &to_vaddr_in_file);
    auto sample =
        std::make_unique<SampleEntry>(r.time_data.time, r.period_data.period, 0, 1, r.Cpu(), thread,
                                      to_map, to_symbol, to_vaddr_in_file);
    sample->branch_from.map = from_map;
    sample->branch_from.symbol = from_symbol;
    sample->branch_from.vaddr_in_file = from_vaddr_in_file;
    sample->branch_from.flags = item.flags;
    return InsertSample(std::move(sample));
  }

  SampleEntry* CreateCallChainSample(const ThreadEntry* thread, const SampleEntry* sample,
                                     uint64_t ip, bool in_kernel,
                                     const std::vector<SampleEntry*>& callchain,
                                     const uint64_t& acc_info) override {
    const MapEntry* map = thread_tree_->FindMap(thread, ip, in_kernel);
    if (thread_tree_->IsUnknownDso(map->dso)) {
      // The unwinders can give wrong ip addresses, which can't map to a valid dso. Skip them.
      total_error_callchains_++;
      return nullptr;
    }
    uint64_t vaddr_in_file;
    const Symbol* symbol = thread_tree_->FindSymbol(map, ip, &vaddr_in_file);
    auto callchain_sample = std::make_unique<SampleEntry>(sample->time, 0, acc_info, 0, sample->cpu,
                                                          thread, map, symbol, vaddr_in_file);
    callchain_sample->thread_comm = sample->thread_comm;
    return InsertCallChainSample(std::move(callchain_sample), callchain);
  }

  const ThreadEntry* GetThreadOfSample(SampleEntry* sample) override {
    return thread_tree_->FindThreadOrNew(sample->pid, sample->tid);
  }

  uint64_t GetPeriodForCallChain(const uint64_t& acc_info) override { return acc_info; }

  bool FilterSample(const SampleEntry* sample) override {
    if (!cpu_filter_.empty() && cpu_filter_.count(sample->cpu) == 0) {
      return false;
    }
    if (!pid_filter_.empty() && pid_filter_.count(sample->pid) == 0) {
      return false;
    }
    if (!tid_filter_.empty() && tid_filter_.count(sample->tid) == 0) {
      return false;
    }
    if (!comm_filter_.empty() && comm_filter_.count(sample->thread_comm) == 0) {
      return false;
    }
    if (!dso_filter_.empty() && dso_filter_.count(sample->map->dso->GetReportPath().data()) == 0) {
      return false;
    }
    if (!symbol_filter_.empty() && symbol_filter_.count(sample->symbol->DemangledName()) == 0) {
      return false;
    }
    return true;
  }

  void UpdateSummary(const SampleEntry* sample) override {
    total_samples_ += sample->sample_count;
    total_period_ += sample->period;
  }

  void MergeSample(SampleEntry* sample1, SampleEntry* sample2) override {
    sample1->period += sample2->period;
    sample1->accumulated_period += sample2->accumulated_period;
    sample1->sample_count += sample2->sample_count;
  }

 private:
  ThreadTree* thread_tree_;

  std::unordered_set<int> cpu_filter_;
  std::unordered_set<int> pid_filter_;
  std::unordered_set<int> tid_filter_;
  std::unordered_set<std::string> comm_filter_;
  std::unordered_set<std::string> dso_filter_;
  std::unordered_set<std::string> symbol_filter_;

  uint64_t total_samples_;
  uint64_t total_period_;
  uint64_t total_error_callchains_;

  std::string event_name_;
};

// Build sample tree based on event count in each sample.
class EventCountSampleTreeBuilder : public ReportCmdSampleTreeBuilder {
 public:
  EventCountSampleTreeBuilder(const SampleComparator<SampleEntry>& sample_comparator,
                              ThreadTree* thread_tree)
      : ReportCmdSampleTreeBuilder(sample_comparator, thread_tree) {}

 protected:
  uint64_t GetPeriod(const SampleRecord& r) override { return r.period_data.period; }
};

// Build sample tree based on the time difference between current sample and next sample.
class TimestampSampleTreeBuilder : public ReportCmdSampleTreeBuilder {
 public:
  TimestampSampleTreeBuilder(const SampleComparator<SampleEntry>& sample_comparator,
                             ThreadTree* thread_tree)
      : ReportCmdSampleTreeBuilder(sample_comparator, thread_tree) {}

  void ReportCmdProcessSampleRecord(std::shared_ptr<SampleRecord>& r) override {
    pid_t tid = static_cast<pid_t>(r->tid_data.tid);
    auto it = next_sample_cache_.find(tid);
    if (it == next_sample_cache_.end()) {
      next_sample_cache_[tid] = r;
    } else {
      std::shared_ptr<SampleRecord> cur = it->second;
      it->second = r;
      ProcessSampleRecord(*cur);
    }
  }

 protected:
  uint64_t GetPeriod(const SampleRecord& r) override {
    auto it = next_sample_cache_.find(r.tid_data.tid);
    CHECK(it != next_sample_cache_.end());
    // Normally the samples are sorted by time, but check here for safety.
    if (it->second->time_data.time > r.time_data.time) {
      return it->second->time_data.time - r.time_data.time;
    }
    return 1u;
  }

 private:
  std::unordered_map<pid_t, std::shared_ptr<SampleRecord>> next_sample_cache_;
};

struct SampleTreeBuilderOptions {
  SampleComparator<SampleEntry> comparator;
  ThreadTree* thread_tree;
  std::unordered_set<std::string> comm_filter;
  std::unordered_set<std::string> dso_filter;
  std::unordered_set<std::string> symbol_filter;
  std::unordered_set<int> cpu_filter;
  std::unordered_set<int> pid_filter;
  std::unordered_set<int> tid_filter;
  bool use_branch_address;
  bool accumulate_callchain;
  bool build_callchain;
  bool use_caller_as_callchain_root;
  bool trace_offcpu;

  std::unique_ptr<ReportCmdSampleTreeBuilder> CreateSampleTreeBuilder() {
    std::unique_ptr<ReportCmdSampleTreeBuilder> builder;
    if (trace_offcpu) {
      builder.reset(new TimestampSampleTreeBuilder(comparator, thread_tree));
    } else {
      builder.reset(new EventCountSampleTreeBuilder(comparator, thread_tree));
    }
    builder->SetFilters(cpu_filter, pid_filter, tid_filter, comm_filter, dso_filter, symbol_filter);
    builder->SetBranchSampleOption(use_branch_address);
    builder->SetCallChainSampleOptions(accumulate_callchain, build_callchain,
                                       use_caller_as_callchain_root);
    return builder;
  }
};

using ReportCmdSampleTreeSorter = SampleTreeSorter<SampleEntry>;
using ReportCmdSampleTreeDisplayer = SampleTreeDisplayer<SampleEntry, SampleTree>;

using ReportCmdCallgraphDisplayer = CallgraphDisplayer<SampleEntry, CallChainNode<SampleEntry>>;

class ReportCmdCallgraphDisplayerWithVaddrInFile : public ReportCmdCallgraphDisplayer {
 protected:
  std::string PrintSampleName(const SampleEntry* sample) override {
    return android::base::StringPrintf("%s [+0x%" PRIx64 "]", sample->symbol->DemangledName(),
                                       sample->vaddr_in_file);
  }
};

struct EventAttrWithName {
  perf_event_attr attr;
  std::string name;
};

class ReportCommand : public Command {
 public:
  ReportCommand()
      : Command("report", "report sampling information in perf.data",
                // clang-format off
"Usage: simpleperf report [options]\n"
"The default options are: -i perf.data --sort comm,pid,tid,dso,symbol.\n"
"-b    Use the branch-to addresses in sampled take branches instead of the\n"
"      instruction addresses. Only valid for perf.data recorded with -b/-j\n"
"      option.\n"
"--children    Print the overhead accumulated by appearing in the callchain.\n"
"--comms comm1,comm2,...   Report only for selected comms.\n"
"--cpu   cpu_item1,cpu_item2,...\n"
"                  Report samples on the selected cpus. cpu_item can be cpu\n"
"                  number like 1, or cpu range like 0-3.\n"
"--csv                     Report in csv format.\n"
"--dsos dso1,dso2,...      Report only for selected dsos.\n"
"--full-callgraph  Print full call graph. Used with -g option. By default,\n"
"                  brief call graph is printed.\n"
"-g [callee|caller]    Print call graph. If callee mode is used, the graph\n"
"                      shows how functions are called from others. Otherwise,\n"
"                      the graph shows how functions call others.\n"
"                      Default is caller mode.\n"
"-i <file>  Specify path of record file, default is perf.data.\n"
"--kallsyms <file>     Set the file to read kernel symbols.\n"
"--max-stack <frames>  Set max stack frames shown when printing call graph.\n"
"-n         Print the sample count for each item.\n"
"--no-demangle         Don't demangle symbol names.\n"
"--no-show-ip          Don't show vaddr in file for unknown symbols.\n"
"-o report_file_name   Set report file name, default is stdout.\n"
"--percent-limit <percent>  Set min percentage shown when printing call graph.\n"
"--pids pid1,pid2,...  Report only for selected pids.\n"
"--raw-period          Report period count instead of period percentage.\n"
"--sort key1,key2,...  Select keys used to sort and print the report. The\n"
"                      appearance order of keys decides the order of keys used\n"
"                      to sort and print the report.\n"
"                      Possible keys include:\n"
"                        pid             -- process id\n"
"                        tid             -- thread id\n"
"                        comm            -- thread name (can be changed during\n"
"                                           the lifetime of a thread)\n"
"                        dso             -- shared library\n"
"                        symbol          -- function name in the shared library\n"
"                        vaddr_in_file   -- virtual address in the shared\n"
"                                           library\n"
"                      Keys can only be used with -b option:\n"
"                        dso_from        -- shared library branched from\n"
"                        dso_to          -- shared library branched to\n"
"                        symbol_from     -- name of function branched from\n"
"                        symbol_to       -- name of function branched to\n"
"                      The default sort keys are:\n"
"                        comm,pid,tid,dso,symbol\n"
"--symbols symbol1;symbol2;...    Report only for selected symbols.\n"
"--symfs <dir>         Look for files with symbols relative to this directory.\n"
"--tids tid1,tid2,...  Report only for selected tids.\n"
"--vmlinux <file>      Parse kernel symbols from <file>.\n"
                // clang-format on
                ),
        record_filename_("perf.data"),
        record_file_arch_(GetBuildArch()),
        use_branch_address_(false),
        system_wide_collection_(false),
        accumulate_callchain_(false),
        print_callgraph_(false),
        callgraph_show_callee_(false),
        callgraph_max_stack_(UINT32_MAX),
        callgraph_percent_limit_(0),
        raw_period_(false),
        brief_callgraph_(true),
        trace_offcpu_(false),
        sched_switch_attr_id_(0u) {}

  bool Run(const std::vector<std::string>& args);

 private:
  bool ParseOptions(const std::vector<std::string>& args);
  bool BuildSampleComparatorAndDisplayer(bool print_sample_count,
                                         const std::vector<std::string>& sort_keys);
  void ReadMetaInfoFromRecordFile();
  bool ReadEventAttrFromRecordFile();
  bool ReadFeaturesFromRecordFile();
  bool ReadSampleTreeFromRecordFile();
  bool ProcessRecord(std::unique_ptr<Record> record);
  void ProcessSampleRecordInTraceOffCpuMode(std::unique_ptr<Record> record, size_t attr_id);
  bool ProcessTracingData(const std::vector<char>& data);
  bool PrintReport();
  void PrintReportContext(FILE* fp);

  std::string record_filename_;
  ArchType record_file_arch_;
  std::unique_ptr<RecordFileReader> record_file_reader_;
  std::vector<EventAttrWithName> event_attrs_;
  ThreadTree thread_tree_;
  // Create a SampleTreeBuilder and SampleTree for each event_attr.
  std::vector<SampleTree> sample_tree_;
  SampleTreeBuilderOptions sample_tree_builder_options_;
  std::vector<std::unique_ptr<ReportCmdSampleTreeBuilder>> sample_tree_builder_;

  std::unique_ptr<ReportCmdSampleTreeSorter> sample_tree_sorter_;
  std::unique_ptr<ReportCmdSampleTreeDisplayer> sample_tree_displayer_;
  bool use_branch_address_;
  std::string record_cmdline_;
  bool system_wide_collection_;
  bool accumulate_callchain_;
  bool print_callgraph_;
  bool callgraph_show_callee_;
  uint32_t callgraph_max_stack_;
  double callgraph_percent_limit_;
  bool raw_period_;
  bool brief_callgraph_;
  bool trace_offcpu_;
  size_t sched_switch_attr_id_;
  bool report_csv_ = false;

  std::string report_filename_;
};

bool ReportCommand::Run(const std::vector<std::string>& args) {
  // 1. Parse options.
  if (!ParseOptions(args)) {
    return false;
  }

  // 2. Read record file and build SampleTree.
  record_file_reader_ = RecordFileReader::CreateInstance(record_filename_);
  if (record_file_reader_ == nullptr) {
    return false;
  }
  ReadMetaInfoFromRecordFile();
  if (!ReadEventAttrFromRecordFile()) {
    return false;
  }
  // Read features first to prepare build ids used when building SampleTree.
  if (!ReadFeaturesFromRecordFile()) {
    return false;
  }
  ScopedCurrentArch scoped_arch(record_file_arch_);
  if (!ReadSampleTreeFromRecordFile()) {
    return false;
  }

  // 3. Show collected information.
  if (!PrintReport()) {
    return false;
  }

  return true;
}

bool ReportCommand::ParseOptions(const std::vector<std::string>& args) {
  static OptionFormatMap option_formats = {
      {"-b", {OptionValueType::NONE, OptionType::SINGLE}},
      {"--children", {OptionValueType::NONE, OptionType::SINGLE}},
      {"--comms", {OptionValueType::STRING, OptionType::MULTIPLE}},
      {"--cpu", {OptionValueType::STRING, OptionType::MULTIPLE}},
      {"--csv", {OptionValueType::NONE, OptionType::SINGLE}},
      {"--dsos", {OptionValueType::STRING, OptionType::MULTIPLE}},
      {"--full-callgraph", {OptionValueType::NONE, OptionType::SINGLE}},
      {"-g", {OptionValueType::OPT_STRING, OptionType::SINGLE}},
      {"-i", {OptionValueType::STRING, OptionType::SINGLE}},
      {"--kallsyms", {OptionValueType::STRING, OptionType::SINGLE}},
      {"--max-stack", {OptionValueType::UINT, OptionType::SINGLE}},
      {"-n", {OptionValueType::NONE, OptionType::SINGLE}},
      {"--no-demangle", {OptionValueType::NONE, OptionType::SINGLE}},
      {"--no-show-ip", {OptionValueType::NONE, OptionType::SINGLE}},
      {"-o", {OptionValueType::STRING, OptionType::SINGLE}},
      {"--percent-limit", {OptionValueType::DOUBLE, OptionType::SINGLE}},
      {"--pids", {OptionValueType::STRING, OptionType::MULTIPLE}},
      {"--tids", {OptionValueType::STRING, OptionType::MULTIPLE}},
      {"--raw-period", {OptionValueType::NONE, OptionType::SINGLE}},
      {"--sort", {OptionValueType::STRING, OptionType::SINGLE}},
      {"--symbols", {OptionValueType::STRING, OptionType::MULTIPLE}},
      {"--symfs", {OptionValueType::STRING, OptionType::SINGLE}},
      {"--vmlinux", {OptionValueType::STRING, OptionType::SINGLE}},
  };

  OptionValueMap options;
  std::vector<std::pair<OptionName, OptionValue>> ordered_options;
  if (!PreprocessOptions(args, option_formats, &options, &ordered_options, nullptr)) {
    return false;
  }

  // Process options.
  use_branch_address_ = options.PullBoolValue("-b");
  accumulate_callchain_ = options.PullBoolValue("--children");
  for (const OptionValue& value : options.PullValues("--comms")) {
    std::vector<std::string> strs = Split(*value.str_value, ",");
    sample_tree_builder_options_.comm_filter.insert(strs.begin(), strs.end());
  }
  for (const OptionValue& value : options.PullValues("--cpu")) {
    if (auto cpus = GetCpusFromString(*value.str_value); cpus) {
      sample_tree_builder_options_.cpu_filter.insert(cpus->begin(), cpus->end());
    } else {
      return false;
    }
  }
  report_csv_ = options.PullBoolValue("--csv");
  for (const OptionValue& value : options.PullValues("--dsos")) {
    std::vector<std::string> strs = Split(*value.str_value, ",");
    sample_tree_builder_options_.dso_filter.insert(strs.begin(), strs.end());
  }
  brief_callgraph_ = !options.PullBoolValue("--full-callgraph");

  if (auto value = options.PullValue("-g"); value) {
    print_callgraph_ = true;
    accumulate_callchain_ = true;
    if (value->str_value != nullptr) {
      if (*value->str_value == "callee") {
        callgraph_show_callee_ = true;
      } else if (*value->str_value == "caller") {
        callgraph_show_callee_ = false;
      } else {
        LOG(ERROR) << "Unknown argument with -g option: " << *value->str_value;
        return false;
      }
    }
  }
  options.PullStringValue("-i", &record_filename_);
  if (auto value = options.PullValue("--kallsyms"); value) {
    std::string kallsyms;
    if (!android::base::ReadFileToString(*value->str_value, &kallsyms)) {
      LOG(ERROR) << "Can't read kernel symbols from " << *value->str_value;
      return false;
    }
    Dso::SetKallsyms(kallsyms);
  }
  if (!options.PullUintValue("--max-stack", &callgraph_max_stack_)) {
    return false;
  }
  bool print_sample_count = options.PullBoolValue("-n");

  Dso::SetDemangle(!options.PullBoolValue("--no-demangle"));

  if (!options.PullBoolValue("--no-show-ip")) {
    thread_tree_.ShowIpForUnknownSymbol();
  }

  options.PullStringValue("-o", &report_filename_);
  if (!options.PullDoubleValue("--percent-limit", &callgraph_percent_limit_, 0)) {
    return false;
  }

  for (const OptionValue& value : options.PullValues("--pids")) {
    if (auto pids = GetTidsFromString(*value.str_value, false); pids) {
      sample_tree_builder_options_.pid_filter.insert(pids->begin(), pids->end());
    } else {
      return false;
    }
  }
  for (const OptionValue& value : options.PullValues("--tids")) {
    if (auto tids = GetTidsFromString(*value.str_value, false); tids) {
      sample_tree_builder_options_.tid_filter.insert(tids->begin(), tids->end());
    } else {
      return false;
    }
  }
  raw_period_ = options.PullBoolValue("--raw-period");

  std::vector<std::string> sort_keys = {"comm", "pid", "tid", "dso", "symbol"};
  if (auto value = options.PullValue("--sort"); value) {
    sort_keys = Split(*value->str_value, ",");
  }

  for (const OptionValue& value : options.PullValues("--symbols")) {
    std::vector<std::string> symbols = Split(*value.str_value, ";");
    sample_tree_builder_options_.symbol_filter.insert(symbols.begin(), symbols.end());
  }

  if (auto value = options.PullValue("--symfs"); value) {
    if (!Dso::SetSymFsDir(*value->str_value)) {
      return false;
    }
  }
  if (auto value = options.PullValue("--vmlinux"); value) {
    Dso::SetVmlinux(*value->str_value);
  }
  CHECK(options.values.empty());
  return BuildSampleComparatorAndDisplayer(print_sample_count, sort_keys);
}

bool ReportCommand::BuildSampleComparatorAndDisplayer(bool print_sample_count,
                                                      const std::vector<std::string>& sort_keys) {
  SampleDisplayer<SampleEntry, SampleTree> displayer;
  displayer.SetReportFormat(report_csv_);
  SampleComparator<SampleEntry> comparator;

  if (accumulate_callchain_) {
    if (raw_period_) {
      displayer.AddDisplayFunction("Children", DisplayAccumulatedPeriod);
      displayer.AddDisplayFunction("Self", DisplaySelfPeriod);
    } else {
      displayer.AddDisplayFunction("Children", DisplayAccumulatedOverhead);
      displayer.AddDisplayFunction("Self", DisplaySelfOverhead);
    }
  } else {
    if (raw_period_) {
      displayer.AddDisplayFunction("Overhead", DisplaySelfPeriod);
    } else {
      displayer.AddDisplayFunction("Overhead", DisplaySelfOverhead);
    }
  }
  if (print_sample_count) {
    displayer.AddDisplayFunction("Sample", DisplaySampleCount);
  }

  for (auto& key : sort_keys) {
    if (!use_branch_address_ && branch_sort_keys.find(key) != branch_sort_keys.end()) {
      LOG(ERROR) << "sort key '" << key << "' can only be used with -b option.";
      return false;
    }
    if (key == "pid") {
      comparator.AddCompareFunction(ComparePid);
      displayer.AddDisplayFunction("Pid", DisplayPid);
    } else if (key == "tid") {
      comparator.AddCompareFunction(CompareTid);
      displayer.AddDisplayFunction("Tid", DisplayTid);
    } else if (key == "comm") {
      comparator.AddCompareFunction(CompareComm);
      displayer.AddDisplayFunction("Command", DisplayComm);
    } else if (key == "dso") {
      comparator.AddCompareFunction(CompareDso);
      displayer.AddDisplayFunction("Shared Object", DisplayDso);
    } else if (key == "symbol") {
      comparator.AddCompareFunction(CompareSymbol);
      displayer.AddDisplayFunction("Symbol", DisplaySymbol);
    } else if (key == "vaddr_in_file") {
      comparator.AddCompareFunction(CompareVaddrInFile);
      displayer.AddDisplayFunction("VaddrInFile", DisplayVaddrInFile);
    } else if (key == "dso_from") {
      comparator.AddCompareFunction(CompareDsoFrom);
      displayer.AddDisplayFunction("Source Shared Object", DisplayDsoFrom);
    } else if (key == "dso_to") {
      comparator.AddCompareFunction(CompareDso);
      displayer.AddDisplayFunction("Target Shared Object", DisplayDso);
    } else if (key == "symbol_from") {
      comparator.AddCompareFunction(CompareSymbolFrom);
      displayer.AddDisplayFunction("Source Symbol", DisplaySymbolFrom);
    } else if (key == "symbol_to") {
      comparator.AddCompareFunction(CompareSymbol);
      displayer.AddDisplayFunction("Target Symbol", DisplaySymbol);
    } else {
      LOG(ERROR) << "Unknown sort key: " << key;
      return false;
    }
  }

  if (report_csv_) {
    if (accumulate_callchain_) {
      displayer.AddDisplayFunction("AccEventCount", DisplayAccumulatedPeriod);
      displayer.AddDisplayFunction("SelfEventCount", DisplaySelfPeriod);
    } else {
      displayer.AddDisplayFunction("EventCount", DisplaySelfPeriod);
    }
    displayer.AddDisplayFunction("EventName", DisplayEventName);
  }

  if (print_callgraph_) {
    bool has_symbol_key = false;
    bool has_vaddr_in_file_key = false;
    for (const auto& key : sort_keys) {
      if (key == "symbol") {
        has_symbol_key = true;
      } else if (key == "vaddr_in_file") {
        has_vaddr_in_file_key = true;
      }
    }
    if (has_symbol_key) {
      if (has_vaddr_in_file_key) {
        displayer.AddExclusiveDisplayFunction(ReportCmdCallgraphDisplayerWithVaddrInFile());
      } else {
        displayer.AddExclusiveDisplayFunction(ReportCmdCallgraphDisplayer(
            callgraph_max_stack_, callgraph_percent_limit_, brief_callgraph_));
      }
    }
  }

  sample_tree_builder_options_.comparator = comparator;
  sample_tree_builder_options_.thread_tree = &thread_tree_;

  SampleComparator<SampleEntry> sort_comparator;
  sort_comparator.AddCompareFunction(CompareTotalPeriod);
  if (print_callgraph_) {
    sort_comparator.AddCompareFunction(CompareCallGraphDuplicated);
  }
  sort_comparator.AddCompareFunction(ComparePeriod);
  sort_comparator.AddComparator(comparator);
  sample_tree_sorter_.reset(new ReportCmdSampleTreeSorter(sort_comparator));
  sample_tree_displayer_.reset(new ReportCmdSampleTreeDisplayer(displayer));
  return true;
}

void ReportCommand::ReadMetaInfoFromRecordFile() {
  auto& meta_info = record_file_reader_->GetMetaInfoFeature();
  if (auto it = meta_info.find("system_wide_collection"); it != meta_info.end()) {
    system_wide_collection_ = it->second == "true";
  }
  if (auto it = meta_info.find("trace_offcpu"); it != meta_info.end()) {
    trace_offcpu_ = it->second == "true";
  }
}

bool ReportCommand::ReadEventAttrFromRecordFile() {
  std::vector<EventAttrWithId> attrs = record_file_reader_->AttrSection();
  for (const auto& attr_with_id : attrs) {
    EventAttrWithName attr;
    attr.attr = *attr_with_id.attr;
    attr.name = GetEventNameByAttr(attr.attr);
    event_attrs_.push_back(attr);
  }
  if (use_branch_address_) {
    bool has_branch_stack = true;
    for (const auto& attr : event_attrs_) {
      if ((attr.attr.sample_type & PERF_SAMPLE_BRANCH_STACK) == 0) {
        has_branch_stack = false;
        break;
      }
    }
    if (!has_branch_stack) {
      LOG(ERROR) << record_filename_ << " is not recorded with branch stack sampling option.";
      return false;
    }
  }
  if (trace_offcpu_) {
    size_t i;
    for (i = 0; i < event_attrs_.size(); ++i) {
      if (event_attrs_[i].name == "sched:sched_switch") {
        break;
      }
    }
    CHECK_NE(i, event_attrs_.size());
    sched_switch_attr_id_ = i;
  }
  return true;
}

bool ReportCommand::ReadFeaturesFromRecordFile() {
  record_file_reader_->LoadBuildIdAndFileFeatures(thread_tree_);

  std::string arch = record_file_reader_->ReadFeatureString(PerfFileFormat::FEAT_ARCH);
  if (!arch.empty()) {
    record_file_arch_ = GetArchType(arch);
    if (record_file_arch_ == ARCH_UNSUPPORTED) {
      return false;
    }
  }

  std::vector<std::string> cmdline = record_file_reader_->ReadCmdlineFeature();
  if (!cmdline.empty()) {
    record_cmdline_ = android::base::Join(cmdline, ' ');
    if (record_file_reader_->GetMetaInfoFeature().count("system_wide_collection")) {
      // TODO: the code to detect system wide collection option is fragile, remove
      // it once we can do cross unwinding.
      for (size_t i = 0; i < cmdline.size(); i++) {
        std::string& s = cmdline[i];
        if (s == "-a") {
          system_wide_collection_ = true;
          break;
        } else if (s == "--call-graph" || s == "--cpu" || s == "-e" || s == "-f" || s == "-F" ||
                   s == "-j" || s == "-m" || s == "-o" || s == "-p" || s == "-t") {
          i++;
        } else if (!s.empty() && s[0] != '-') {
          break;
        }
      }
    }
  }
  if (record_file_reader_->HasFeature(PerfFileFormat::FEAT_TRACING_DATA)) {
    std::vector<char> tracing_data;
    if (!record_file_reader_->ReadFeatureSection(PerfFileFormat::FEAT_TRACING_DATA,
                                                 &tracing_data)) {
      return false;
    }
    if (!ProcessTracingData(tracing_data)) {
      return false;
    }
  }
  return true;
}

bool ReportCommand::ReadSampleTreeFromRecordFile() {
  sample_tree_builder_options_.use_branch_address = use_branch_address_;
  sample_tree_builder_options_.accumulate_callchain = accumulate_callchain_;
  sample_tree_builder_options_.build_callchain = print_callgraph_;
  sample_tree_builder_options_.use_caller_as_callchain_root = !callgraph_show_callee_;
  sample_tree_builder_options_.trace_offcpu = trace_offcpu_;

  for (size_t i = 0; i < event_attrs_.size(); ++i) {
    sample_tree_builder_.push_back(sample_tree_builder_options_.CreateSampleTreeBuilder());
    sample_tree_builder_.back()->SetEventName(event_attrs_[i].name);
    OfflineUnwinder* unwinder = sample_tree_builder_.back()->GetUnwinder();
    if (unwinder != nullptr) {
      unwinder->LoadMetaInfo(record_file_reader_->GetMetaInfoFeature());
    }
  }

  if (!record_file_reader_->ReadDataSection(
          [this](std::unique_ptr<Record> record) { return ProcessRecord(std::move(record)); })) {
    return false;
  }
  for (size_t i = 0; i < sample_tree_builder_.size(); ++i) {
    sample_tree_.push_back(sample_tree_builder_[i]->GetSampleTree());
    sample_tree_sorter_->Sort(sample_tree_.back().samples, print_callgraph_);
  }
  return true;
}

bool ReportCommand::ProcessRecord(std::unique_ptr<Record> record) {
  thread_tree_.Update(*record);
  if (record->type() == PERF_RECORD_SAMPLE) {
    size_t attr_id = record_file_reader_->GetAttrIndexOfRecord(record.get());
    if (!trace_offcpu_) {
      sample_tree_builder_[attr_id]->ReportCmdProcessSampleRecord(
          *static_cast<SampleRecord*>(record.get()));
    } else {
      ProcessSampleRecordInTraceOffCpuMode(std::move(record), attr_id);
    }
  } else if (record->type() == PERF_RECORD_TRACING_DATA ||
             record->type() == SIMPLE_PERF_RECORD_TRACING_DATA) {
    const auto& r = *static_cast<TracingDataRecord*>(record.get());
    if (!ProcessTracingData(std::vector<char>(r.data, r.data + r.data_size))) {
      return false;
    }
  }
  return true;
}

void ReportCommand::ProcessSampleRecordInTraceOffCpuMode(std::unique_ptr<Record> record,
                                                         size_t attr_id) {
  std::shared_ptr<SampleRecord> r(static_cast<SampleRecord*>(record.release()));
  if (attr_id == sched_switch_attr_id_) {
    // If this sample belongs to sched_switch event, we should broadcast the offcpu info
    // to other event types.
    for (size_t i = 0; i < event_attrs_.size(); ++i) {
      if (i == sched_switch_attr_id_) {
        continue;
      }
      sample_tree_builder_[i]->ReportCmdProcessSampleRecord(r);
    }
  } else {
    sample_tree_builder_[attr_id]->ReportCmdProcessSampleRecord(r);
  }
}

bool ReportCommand::ProcessTracingData(const std::vector<char>& data) {
  Tracing tracing(data);
  for (auto& attr : event_attrs_) {
    if (attr.attr.type == PERF_TYPE_TRACEPOINT) {
      uint64_t trace_event_id = attr.attr.config;
      attr.name = tracing.GetTracingEventNameHavingId(trace_event_id);
    }
  }
  return true;
}

bool ReportCommand::PrintReport() {
  std::unique_ptr<FILE, decltype(&fclose)> file_handler(nullptr, fclose);
  FILE* report_fp = stdout;
  if (!report_filename_.empty()) {
    report_fp = fopen(report_filename_.c_str(), "w");
    if (report_fp == nullptr) {
      PLOG(ERROR) << "failed to open file " << report_filename_;
      return false;
    }
    file_handler.reset(report_fp);
  }
  PrintReportContext(report_fp);
  for (size_t i = 0; i < event_attrs_.size(); ++i) {
    if (trace_offcpu_ && i == sched_switch_attr_id_) {
      continue;
    }
    if (i != 0) {
      fprintf(report_fp, "\n");
    }
    EventAttrWithName& attr = event_attrs_[i];
    SampleTree& sample_tree = sample_tree_[i];
    fprintf(report_fp, "Event: %s (type %u, config %llu)\n", attr.name.c_str(), attr.attr.type,
            attr.attr.config);
    fprintf(report_fp, "Samples: %" PRIu64 "\n", sample_tree.total_samples);
    if (sample_tree.total_error_callchains != 0) {
      fprintf(report_fp, "Error Callchains: %" PRIu64 ", %f%%\n",
              sample_tree.total_error_callchains,
              sample_tree.total_error_callchains * 100.0 / sample_tree.total_samples);
    }
    const char* period_prefix = trace_offcpu_ ? "Time in ns" : "Event count";
    fprintf(report_fp, "%s: %" PRIu64 "\n\n", period_prefix, sample_tree.total_period);
    sample_tree_displayer_->DisplaySamples(report_fp, sample_tree.samples, &sample_tree);
  }
  fflush(report_fp);
  if (ferror(report_fp) != 0) {
    PLOG(ERROR) << "print report failed";
    return false;
  }
  return true;
}

void ReportCommand::PrintReportContext(FILE* report_fp) {
  if (!record_cmdline_.empty()) {
    fprintf(report_fp, "Cmdline: %s\n", record_cmdline_.c_str());
  }
  fprintf(report_fp, "Arch: %s\n", GetArchString(record_file_arch_).c_str());
}

}  // namespace

namespace simpleperf {

void RegisterReportCommand() {
  RegisterCommand("report", [] { return std::unique_ptr<Command>(new ReportCommand()); });
}

}  // namespace simpleperf
