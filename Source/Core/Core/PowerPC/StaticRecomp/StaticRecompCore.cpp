// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "Common/Config/Config.h"
#include "Common/DynamicLibrary.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"

#ifdef _M_X86_64
#include "Core/PowerPC/Jit64/Jit.h"
#endif
#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/Jit.h"
#endif


StaticRecompCore* g_static_recomp_core = nullptr;

u32 StaticRecompShouldYieldAt(u32 address)
{
  return g_static_recomp_core && g_static_recomp_core->ShouldYieldAt(address);
}

namespace
{
bool RangesAreSorted(const StaticRecompRange* ranges, u32 count)
{
  if (!ranges || count == 0)
    return false;
  for (u32 i = 0; i < count; ++i)
  {
    if (ranges[i].start >= ranges[i].end ||
        (i != 0 && ranges[i - 1].end > ranges[i].start))
      return false;
  }
  return true;
}

bool AddressIsCovered(const StaticRecompRange* ranges, u32 count, u32 address)
{
  for (u32 i = 0; i < count; ++i)
  {
    if (address >= ranges[i].start && address < ranges[i].end)
      return true;
  }
  return false;
}

bool ChunksTileCode(const StaticRecompModuleDesc& desc)
{
  if (!RangesAreSorted(desc.chunk_ranges, desc.num_chunk_ranges) || !desc.chunk_hashes)
    return false;
  u32 chunk = 0;
  for (u32 code = 0; code < desc.num_code_ranges; ++code)
  {
    u32 cursor = desc.code_ranges[code].start;
    while (chunk < desc.num_chunk_ranges && desc.chunk_ranges[chunk].start < desc.code_ranges[code].end)
    {
      if (desc.chunk_ranges[chunk].start != cursor ||
          desc.chunk_ranges[chunk].end > desc.code_ranges[code].end)
        return false;
      cursor = desc.chunk_ranges[chunk++].end;
    }
    if (cursor != desc.code_ranges[code].end)
      return false;
  }
  return chunk == desc.num_chunk_ranges;
}

bool RelModulesValid(const StaticRecompModuleDesc& desc)
{
  if (desc.num_rel_modules == 0)
    return desc.rel_modules == nullptr;
  if (!desc.rel_modules)
    return false;
  for (u32 i = 0; i < desc.num_rel_modules; ++i)
  {
    const StaticRecompRelModule& module = desc.rel_modules[i];
    if (module.module_id == 0 || module.section_count == 0 ||
        module.section_info_offset < 0x40 || module.file_size < 0x40 ||
        !module.sections || module.num_sections == 0)
      return false;
    for (u32 j = 0; j < module.num_sections; ++j)
    {
      const StaticRecompRelSection& section = module.sections[j];
      const u64 end = static_cast<u64>(section.linked_start) + section.size;
      if (section.module_id != module.module_id || section.section_index >= module.section_count ||
          section.size == 0 || end > 0x100000000ull ||
          !AddressIsCovered(desc.code_ranges, desc.num_code_ranges, section.linked_start) ||
          !AddressIsCovered(desc.code_ranges, desc.num_code_ranges, static_cast<u32>(end - 1)))
        return false;
    }
  }
  return true;
}
}  // namespace

bool StaticRecompCore::IsModuleActive() const
{
  return m_module_active;
}

bool StaticRecompCore::IsHostCallAddress(u32 address) const
{
  if (!m_module_source.host_call_contains)
    return false;
  if (m_module_source.host_call_contains(address, m_module_source.host_call_user))
    return true;
  return address < m_guest.ram_size &&
         m_module_source.host_call_contains(address | 0x80000000u,
                                            m_module_source.host_call_user);
}

bool StaticRecompCore::ShouldYieldAt(u32 address)
{
  if (m_host_call_passthrough && m_host_call_passthrough_pc == address)
  {
    m_host_call_passthrough = false;
    return false;
  }
  if (m_module_active && DispatchableAt(address))
    return true;
  return IsHostCallAddress(address);
}

StaticRecompCore::StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source)
    : JitBase(system), m_module_source(std::move(module_source))
{
}

StaticRecompCore::~StaticRecompCore() = default;

void StaticRecompCore::Init()
{
  g_static_recomp_core = this;
  RefreshConfig();
  m_ppc = &m_system.GetPPCState();
  m_mmu = &m_system.GetMMU();
  m_memory = &m_system.GetMemory();
  m_gpfifo = &m_system.GetGPFifo();
  m_cp = &m_system.GetCommandProcessor();
  m_collect_dispatch_samples = std::getenv("STATICRECOMP_DISPATCH_SAMPLES") != nullptr;
  if (const char* const gate = std::getenv("STATICRECOMP_CACHEOP_GATE"))
    m_skip_dataline_cacheops = std::strcmp(gate, "0") != 0;
  const char* fallback_override = std::getenv("STATICRECOMP_FALLBACK_RANGES");
  std::istringstream fallback_ranges(fallback_override ? fallback_override :
                                                         Config::Get(Config::MAIN_STATICRECOMP_FALLBACK_RANGES));
  std::string fallback_range;
  while (std::getline(fallback_ranges, fallback_range, ','))
  {
    StaticRecompRange range{};
    if (std::sscanf(fallback_range.c_str(), "%x-%x", &range.start, &range.end) == 2 &&
        range.start < range.end)
      m_forced_fallback_ranges.push_back(range);
  }
  jo.enableBlocklink = false;
  jo.fastmem = false;
  jo.fastmem_arena = false;

  m_block_cache.Init();

  m_guest = CPUState{};
  m_guest.external_read = HookExternalRead;
  m_guest.external_write = HookExternalWrite;
  m_guest.external_read32 = HookExternalRead32;
  m_guest.external_write32 = HookExternalWrite32;
  m_guest.external_pointer = HookExternalPointer;
  m_guest.spr_read = HookSPRRead;
  m_guest.spr_write = HookSPRWrite;
  m_guest.cache_control = HookCacheControl;
  m_guest.instruction_fallback = HookInstructionFallback;
  m_guest.host_call = m_module_source.host_call ? HookHostCall : nullptr;
  m_guest.external_user_data = this;

  std::fprintf(stderr, "[staticrecomp] core init\n");

  LoadModule();
  {
    const u32 single = Config::Get(Config::MAIN_STATICRECOMP_IDLE_PC);
    if (single != 0)
      m_idle_pcs.push_back(single);
    const std::string list = Config::Get(Config::MAIN_STATICRECOMP_IDLE_PCS);
    std::size_t pos = 0;
    while (pos < list.size())
    {
      std::size_t comma = list.find(',', pos);
      if (comma == std::string::npos) comma = list.size();
      std::string tok = list.substr(pos, comma - pos);
      // trim
      while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
      while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
      if (!tok.empty())
      {
        errno = 0;
        char* endp = nullptr;
        unsigned long v = std::strtoul(tok.c_str(), &endp, 0);
        if (endp != tok.c_str() && *endp == '\0' && errno == 0)
          m_idle_pcs.push_back(static_cast<u32>(v));
      }
      pos = comma + 1;
    }
  }
  // Collapse duplicates, then build the fast-path bloom. Do this after parsing
  // both the single-pc and the list setting so the hot loop never sees a
  // partially populated set.
  std::sort(m_idle_pcs.begin(), m_idle_pcs.end());
  m_idle_pcs.erase(std::unique(m_idle_pcs.begin(), m_idle_pcs.end()), m_idle_pcs.end());
  for (const u32 idle : m_idle_pcs)
    m_idle_pc_bloom |= 1ull << ((idle >> 2) & 63);
  m_idle_pc_cached = m_idle_pcs.empty() ? 0 : m_idle_pcs.front();

  m_lockstep_verifier = std::make_unique<StaticRecompLockstep::StaticRecompLockstepVerifier>(*this);
  m_lockstep_verifier->Init();

#ifdef _M_ARM_64
  m_fallback_jit = std::make_unique<JitArm64>(m_system);
#elif defined(_M_X86_64)
  m_fallback_jit = std::make_unique<Jit64>(m_system);
#endif
  if (m_fallback_jit)
  {
    m_fallback_jit->SetStaticRecompFallback(true);
    m_fallback_jit->Init();
    m_fallback_jit->SetStaticRecompFallback(true);
  }
}

void StaticRecompCore::Shutdown()
{
  g_static_recomp_core = nullptr;
  std::fprintf(stderr,
               "[staticrecomp] shutdown: native=%llu fallback=%llu native_exc=%llu hook_fb=%llu "
               "fb_fast=%llu fb_slow=%llu smc_failed=%u verifications=%llu reverify_events=%llu "
               "bursts=%llu cycles=%llu\n",
               (unsigned long long)m_native_dispatches, (unsigned long long)m_fallback_steps,
               (unsigned long long)m_native_exceptions,
               (unsigned long long)m_hook_fallback_instructions,
               (unsigned long long)m_hook_fallback_fast,
               (unsigned long long)m_hook_fallback_slow, m_failed_chunks,
               (unsigned long long)m_verifications, (unsigned long long)m_reverify_events,
               (unsigned long long)m_bursts, (unsigned long long)m_charged_cycles);
  std::fprintf(stderr, "[staticrecomp] stalls: dispatches=%llu distinct_pcs=%zu\n",
               (unsigned long long)m_stall_dispatches, m_stall_pcs.size());
  {
    std::vector<std::pair<u32, u64>> stalls(m_stall_pcs.begin(), m_stall_pcs.end());
    std::sort(stalls.begin(), stalls.end(),
              [](const auto& l, const auto& r) { return l.second > r.second; });
    for (std::size_t i = 0; i < std::min<std::size_t>(stalls.size(), 16); ++i)
      std::fprintf(stderr, "[staticrecomp] stall pc=%08x count=%llu\n", stalls[i].first,
                   static_cast<unsigned long long>(stalls[i].second));
  }
  std::fprintf(stderr,
               "[staticrecomp] hooks: ptr_calls=%llu l1=%llu ram=%llu null=%llu "
               "mmio_gather=%llu mmio_other=%llu\n",               (unsigned long long)m_hook_ptr_calls, (unsigned long long)m_hook_ptr_l1,
               (unsigned long long)m_hook_ptr_ram, (unsigned long long)m_hook_ptr_null,
               (unsigned long long)m_hook_mmio_gather, (unsigned long long)m_hook_mmio_other);
  std::fprintf(stderr,
               "[staticrecomp] cacheops: probes=%llu skipped=%llu icbi=%llu "
               "lines=%u gather_bursts=%llu\n",
               (unsigned long long)m_cacheop_probes, (unsigned long long)m_cacheop_skipped,
               (unsigned long long)m_cacheop_icbi, m_code_line_count,
               static_cast<unsigned long long>(
                   m_system.GetCommandProcessor().GetGatherPipeBurstCount()));
  std::fprintf(stderr, "[staticrecomp] cpctrl: hex=%04x fifo_int_possible=%d\n",
               m_system.GetCommandProcessor().GetCtrlHexSeen(),
               static_cast<int>(m_system.GetCommandProcessor().FifoInterruptPossible()));
  {
    u32 hi_wm = 0, lo_wm = 0, hi_int = 0, lo_int = 0, size = 0, max_dist = 0, bp_int = 0;
    m_system.GetCommandProcessor().GetFifoDiag(&hi_wm, &lo_wm, &hi_int, &lo_int, &size,
                                               &max_dist, &bp_int);
    std::fprintf(stderr,
                 "[staticrecomp] fifo: size=%u hi_wm=%u lo_wm=%u hi_int=%u lo_int=%u "
                 "bp_int=%u max_dist=%u\n",
                 size, hi_wm, lo_wm, hi_int, lo_int, bp_int, max_dist);
  }
  std::vector<std::pair<u32, u64>> dispatch_samples(m_dispatch_samples.begin(),
                                                    m_dispatch_samples.end());
  std::sort(dispatch_samples.begin(), dispatch_samples.end(),
            [](const auto& left, const auto& right) { return left.second > right.second; });
  for (std::size_t i = 0; i < std::min<std::size_t>(dispatch_samples.size(), 16); ++i)
  {
    std::fprintf(stderr, "[staticrecomp] dispatch-site pc=%08x samples=%llu\n",
                 dispatch_samples[i].first,
                 static_cast<unsigned long long>(dispatch_samples[i].second));
  }
  std::vector<std::pair<u32, u64>> fallback_words(m_fallback_words.begin(),
                                                  m_fallback_words.end());
  std::sort(fallback_words.begin(), fallback_words.end(),
            [](const auto& left, const auto& right) { return left.second > right.second; });
  for (std::size_t i = 0; i < std::min<std::size_t>(fallback_words.size(), 16); ++i)
  {
    std::fprintf(stderr, "[staticrecomp] fallback-word raw=%08x count=%llu\n",
                 fallback_words[i].first,
                 static_cast<unsigned long long>(fallback_words[i].second));
  }
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: shutdown. native_dispatches={} fallback_steps={} "
                 "native_exceptions={} hook_fallback_instructions={} smc_failed_chunks={} "
                 "verifications={} reverify_events={}",
                 m_native_dispatches, m_fallback_steps, m_native_exceptions,
                 m_hook_fallback_instructions, m_failed_chunks, m_verifications,
                 m_reverify_events);
  m_lockstep_verifier.reset();
  m_block_cache.Shutdown();
  m_module = nullptr;
  if (m_library.IsOpen())
    m_library.Close();

  if (m_fallback_jit)
  {
    m_fallback_jit->Shutdown();
    m_fallback_jit.reset();
  }
}

void StaticRecompCore::LoadModule()
{
  if (m_module_source.kind == StaticRecompModuleSource::Kind::None)
  {
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: no explicit module source; interpreter-only.");
    return;
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  std::string path = m_module_source.path;
  const StaticRecompModuleDesc* desc = nullptr;
  if (m_module_source.kind == StaticRecompModuleSource::Kind::AttachedDescriptor)
  {
    desc = m_module_source.descriptor;
  }
  else
  {
    if (path.empty() || !File::Exists(path) || !m_library.Open(path.c_str()))
    {
      ERROR_LOG_FMT(POWERPC, "StaticRecomp: failed to open explicit module '{}'.", path);
      return;
    }
    const auto get_module = reinterpret_cast<StaticRecompGetModuleFn>(
        m_library.GetSymbolAddress(STATICRECOMP_GET_MODULE_SYMBOL));
    desc = get_module ? get_module() : nullptr;
  }

  const auto reject = [&](const std::string& why) {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: rejecting module '{}': {}. Interpreter-only.", path, why);
    m_module = nullptr;
    if (m_library.IsOpen())
      m_library.Close();
  };

  if (!desc)
    return reject("missing or null " STATICRECOMP_GET_MODULE_SYMBOL);
  if (desc->abi_version != STATICRECOMP_ABI_VERSION)
    return reject(fmt::format("abi_version {} != {}", desc->abi_version, STATICRECOMP_ABI_VERSION));
  if (desc->cpu_abi_version != GXRUNTIME_CPU_ABI_VERSION)
    return reject(fmt::format("cpu_abi_version {} != {}", desc->cpu_abi_version,
                              GXRUNTIME_CPU_ABI_VERSION));
  if (desc->cpu_state_size != sizeof(CPUState))
    return reject(fmt::format("cpu_state_size {} != sizeof(CPUState) {}", desc->cpu_state_size,
                              sizeof(CPUState)));
  if (!desc->dispatch || !desc->code_ranges || desc->num_code_ranges == 0)
    return reject("no dispatch entry or empty code ranges");
  if (!std::memchr(desc->game_id, '\0', sizeof(desc->game_id)) || desc->game_id[0] == '\0')
    return reject("invalid game_id");
  if (!RangesAreSorted(desc->code_ranges, desc->num_code_ranges))
    return reject("malformed or overlapping code ranges");
  if (desc->num_smc_ranges != 0 && !RangesAreSorted(desc->smc_ranges, desc->num_smc_ranges))
    return reject("malformed or overlapping SMC ranges");
  if (!desc->chunk_ranges || desc->num_chunk_ranges == 0 || !desc->chunk_hashes)
    return reject("no chunk ranges/hashes (required for the SMC guard)");
  if (!ChunksTileCode(*desc))
    return reject("chunk ranges do not exactly tile code ranges");
  if (!RelModulesValid(*desc))
    return reject("malformed REL module metadata");
  if (!AddressIsCovered(desc->code_ranges, desc->num_code_ranges, desc->entry_point))
    return reject("entry point is not covered by the module");
  if (!game_id.empty() && game_id != desc->game_id)
    return reject(fmt::format("module game_id '{}' != running game '{}'", desc->game_id, game_id));

  m_module = desc;
  m_module_active = (desc != nullptr);
  m_has_rel_modules = desc->num_rel_modules != 0;
  m_chunk_state.assign(desc->num_chunk_ranges, CHUNK_UNVERIFIED);
  m_chunk_host_call_state.assign(desc->num_chunk_ranges, 0);
  m_effective_chunk_hashes.assign(desc->chunk_hashes,
                                  desc->chunk_hashes + desc->num_chunk_ranges);
  m_chunk_rel_sections.assign(desc->num_chunk_ranges, -1);
  for (u32 chunk_index = 0; chunk_index < desc->num_chunk_ranges; ++chunk_index)
  {
    const StaticRecompRange& chunk = desc->chunk_ranges[chunk_index];
    u32 flat_section = 0;
    for (u32 module_index = 0; module_index < desc->num_rel_modules; ++module_index)
    {
      const StaticRecompRelModule& module = desc->rel_modules[module_index];
      for (u32 section_index = 0; section_index < module.num_sections; ++section_index, ++flat_section)
      {
        const StaticRecompRelSection& section = module.sections[section_index];
        const u64 section_end = static_cast<u64>(section.linked_start) + section.size;
        if (chunk.start >= section.linked_start && chunk.end <= section_end)
          m_chunk_rel_sections[chunk_index] = static_cast<int>(flat_section);
      }
    }
  }
  m_active_rel_sections.clear();
  m_rel_mapping_generation = 0;
  m_failed_chunks = 0;
  m_lookup_ram_size = 0;
  m_lookup_exram_size = 0;
  m_chunk_lookup_table.clear();

  if (m_module_source.host_call_range_contains)
  {
    for (u32 i = 0; i < desc->num_chunk_ranges; ++i)
      ChunkContainsHostCall(i);
  }

  std::fprintf(stderr, "[staticrecomp] module loaded: %s entry=0x%08X\n", path.c_str(),
               desc->entry_point);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: loaded module '{}' (game_id={} entry=0x{:08X} "
                 "code_ranges={} smc_ranges={})",
                 path, desc->game_id, desc->entry_point, desc->num_code_ranges,
                 desc->num_smc_ranges);
}

void StaticRecompCore::ClearCache()
{
  if (m_fallback_jit)
    m_fallback_jit->ClearCache();

  if (!m_module)
    return;
  std::fill(m_chunk_state.begin(), m_chunk_state.end(), u8{CHUNK_UNVERIFIED});
  m_failed_chunks = 0;
  ++m_reverify_events;
}
