// RecompCore: StaticRecomp CPU core - Main execution loop.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/HW/SystemTimers.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace
{
constexpr u32 SYNC_EXCEPTION_MASK = ~static_cast<u32>(
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);

struct FileCloser
{
  void operator()(std::FILE* file) const
  {
    if (file)
      std::fclose(file);
  }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr OpenDispatchTrace()
{
  const char* path = std::getenv("STATICRECOMP_TRACE_FILE");
  if (!path || !*path)
    return {};

  FilePtr file(std::fopen(path, "w"));
  if (file)
  {
    std::fprintf(file.get(), "dispatch,pc,lr,ctr,cr,timebase,ppc_downcount\n");
    std::fflush(file.get());
  }
  return file;
}
}

void StaticRecompCore::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interpreter = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();
  FilePtr dispatch_trace = OpenDispatchTrace();
  m_perf_log = std::getenv("STATICRECOMP_PERF") != nullptr;

  // Every address handed to the module's chunk dispatcher is, by definition, an
  // entry point into a chunk function's PC switch. DolRecomp cannot prove that
  // set, so it emits a case for every single guest instruction, which makes the
  // whole guest register file memory-resident. Recording the addresses the
  // dispatcher actually sees gives the exact set to prune down to.
  // STATICRECOMP_ENTRY_LOG=<file> writes one hex address per line at shutdown.
  constexpr u32 ENTRY_LOG_BASE = 0x80000000u;
  constexpr u32 ENTRY_LOG_SPAN = 0x00200000u;  // covers the whole DOL text range
  std::unique_ptr<u8[]> entry_seen;
  const char* entry_log_path = std::getenv("STATICRECOMP_ENTRY_LOG");
  const auto dump_entry_log = [&](const char* why) {
    FilePtr entry_file(std::fopen(entry_log_path, "w"));
    if (!entry_file)
    {
      std::fprintf(stderr, "[staticrecomp] entry log: cannot write %s\n", entry_log_path);
      return;
    }
    u32 entry_count = 0;
    for (u32 off = 0; off < ENTRY_LOG_SPAN; off += 4)
    {
      if (entry_seen[off >> 2])
      {
        std::fprintf(entry_file.get(), "%08X\n", ENTRY_LOG_BASE + off);
        ++entry_count;
      }
    }
    std::fprintf(stderr, "[staticrecomp] entry log (%s): %u addresses\n", why, entry_count);
  };
  if (entry_log_path && !*entry_log_path)
    entry_log_path = nullptr;
  if (entry_log_path)
  {
    entry_seen = std::make_unique<u8[]>(ENTRY_LOG_SPAN >> 2);
    std::fprintf(stderr, "[staticrecomp] entry log enabled -> %s\n", entry_log_path);
  }

  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.exram = memory.GetEXRAM();
  m_guest.exram_size = memory.GetExRamSizeReal();
  InitLookupTable(m_guest.ram_size, m_guest.exram_size);
  const bool lockstep_enabled = m_lockstep_verifier->IsEnabled();
  // Locked-L1 location published for consumers. Informational only:
  // get_ram_ptr no longer assumes identity placement of the window -- it
  // asks external_pointer for a per-access host-translated pointer (live
  // MSR.DR + BAT/TLB) instead of dereferencing this cached location.
  // Null while lockstep is on.
  m_guest.l1_cache = lockstep_enabled ? nullptr : memory.GetL1Cache();
  m_guest.l1_cache_size = lockstep_enabled ? 0u : memory.GetL1CacheSize();
  // No window until the host has validated a page against the live mapping.
  m_guest.l1_win_host = nullptr;
  // Write-gather-pipe staging published for the module's ABI v5 inline fast
  // path: base is GPFifo's staging buffer (NULL while lockstep is on -- the
  // hooks own journaling then, and the fast path must not bypass it), cursor
  // points straight at the host's live byte count so neither side ever has
  // to republish, and gather_flush drains a full 32-byte block.
  m_guest.gather_pipe_base = lockstep_enabled ? nullptr : ppc.gather_pipe_base_ptr;
  m_guest.gather_pipe_cursor = &ppc.gather_pipe_ptr;
  m_guest.gather_flush = HookGatherFlush;
  const auto fast_dispatchable_at = [this](u32 address) {
    if (m_has_rel_modules || !m_forced_fallback_ranges.empty())
      return FastDispatchableAt(address);
    if (!m_module_active || m_chunk_lookup_table.empty())
      return false;

    int lookup_index = -1;
    if (address >= 0x80000000u && address < 0x80000000u + m_lookup_ram_size)
    {
      lookup_index = static_cast<int>((address - 0x80000000u) >> 2);
    }
    else if (address >= 0x90000000u && address < 0x90000000u + m_lookup_exram_size)
    {
      lookup_index = static_cast<int>((m_lookup_ram_size >> 2) + ((address - 0x90000000u) >> 2));
    }
    if (lookup_index < 0 || lookup_index >= static_cast<int>(m_chunk_lookup_table.size()))
      return false;
    const int chunk = m_chunk_lookup_table[lookup_index];
    return chunk >= 0 && m_chunk_state[chunk] == CHUNK_VERIFIED;
  };

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  const auto after_mtmsr = [this](u32 pc) {
    if (pc < 4u || (pc & 3u) != 0)
      return false;

    const u32 instruction_pc = pc - 4u;
    const u8* code = nullptr;
    if (instruction_pc >= 0x80000000u &&
        instruction_pc - 0x80000000u + 4u <= m_guest.ram_size)
    {
      code = m_guest.ram + instruction_pc - 0x80000000u;
    }
    else if (instruction_pc >= 0x90000000u &&
             instruction_pc - 0x90000000u + 4u <= m_guest.exram_size)
    {
      code = m_guest.exram + instruction_pc - 0x90000000u;
    }
    if (!code)
      return false;

    const u32 raw = static_cast<u32>(code[0]) << 24 | static_cast<u32>(code[1]) << 16 |
                    static_cast<u32>(code[2]) << 8 | code[3];
    return (raw & 0xFC0007FEu) == 0x7C000124u;
  };
  m_module_active = m_module && (initial_game_id.empty() || initial_game_id == m_module->game_id);

  if (!m_module_active && m_fallback_jit && !m_guest.host_call)
  {
    m_fallback_jit->Run();
    return;
  }

  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();

    if (m_perf_log)
    {
      const auto now = std::chrono::steady_clock::now();
      if (now - m_perf_last >= std::chrono::milliseconds(1000))
      {
        if (m_perf_secs == 0)
          m_perf_last = now;  // first tick only arms the clock
        const u64 cur[PERF_FIELDS] = {m_bursts,        m_charged_cycles, m_native_dispatches,
                                      m_hook_ptr_calls, m_hook_ptr_l1,   m_hook_ptr_ram,
                                      m_hook_ptr_null, m_hook_mmio_gather, m_hook_mmio_other,
                                      m_hook_fallback_instructions, m_spr_reads, m_pub_calls,
                                      m_pub_ok,       m_win_null,       m_win_page,
                                      m_win_msr,      m_msr_xor,        m_fb_calls,
                                      m_system.GetCommandProcessor().GetGatherPipeBurstCount()};
        std::fprintf(stderr,
                     "[perf] t=%llu bursts=%llu cycles=%llu dispatch=%llu ptr=%llu(l1=%llu "
                     "ram=%llu null=%llu) mmio=%llu/%llu fb=%llu spr=%llu pub=%llu/%llu "
                     "winmiss=%llu/%llu/%llu msr=%08x/%08x xor=%016llx fbhook=%llu gather=%llu\n",
                     static_cast<unsigned long long>(++m_perf_secs),
                     static_cast<unsigned long long>(cur[0] - m_perf_prev[0]),
                     static_cast<unsigned long long>(cur[1] - m_perf_prev[1]),
                     static_cast<unsigned long long>(cur[2] - m_perf_prev[2]),
                     static_cast<unsigned long long>(cur[3] - m_perf_prev[3]),
                     static_cast<unsigned long long>(cur[4] - m_perf_prev[4]),
                     static_cast<unsigned long long>(cur[5] - m_perf_prev[5]),
                     static_cast<unsigned long long>(cur[6] - m_perf_prev[6]),
                     static_cast<unsigned long long>(cur[7] - m_perf_prev[7]),
                     static_cast<unsigned long long>(cur[8] - m_perf_prev[8]),
                     static_cast<unsigned long long>(cur[9] - m_perf_prev[9]),
                     static_cast<unsigned long long>(cur[10] - m_perf_prev[10]),
                     static_cast<unsigned long long>(cur[11] - m_perf_prev[11]),
                     static_cast<unsigned long long>(cur[12] - m_perf_prev[12]),
                     static_cast<unsigned long long>(cur[13] - m_perf_prev[13]),
                     static_cast<unsigned long long>(cur[14] - m_perf_prev[14]),
                     static_cast<unsigned long long>(cur[15] - m_perf_prev[15]),
                     (unsigned)m_msr_pub, (unsigned)m_msr_live,
                     (unsigned long long)m_msr_xor,
                     static_cast<unsigned long long>(cur[17] - m_perf_prev[17]),
                     static_cast<unsigned long long>(cur[18] - m_perf_prev[18]));
        std::fflush(stderr);
        for (std::size_t i = 0; i < PERF_FIELDS; ++i)
          m_perf_prev[i] = cur[i];
        m_perf_last = now;
      }
    }

    // Mods can only intercept guest code when they actually installed a hook,
    // patch or pending return, yet host_call_contains is always wired. Probe
    // that once per burst instead of 2-4 unordered_map lookups per dispatch.
    const bool mod_interception =
        m_module_source.host_call_active == nullptr ||
        m_module_source.host_call_active(m_module_source.host_call_user);

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module_active && DispatchableAt(ppc.pc) &&
          !(mod_interception && m_guest.host_call && IsHostCallAddress(ppc.pc)))
      {
        SyncIn();
        ++m_bursts;
        do
        {
          if (dispatch_trace && (m_native_dispatches & 0xFFFFFu) == 0)
          {
            std::fprintf(dispatch_trace.get(), "%llu,%08x,%08x,%08x,%08x,%llu,%d\n",
                         static_cast<unsigned long long>(m_native_dispatches), m_guest.pc,
                         m_guest.lr, m_guest.ctr, m_guest.cr,
                         static_cast<unsigned long long>(m_guest.timebase), ppc.downcount);
            std::fflush(dispatch_trace.get());
          }
          const bool do_ls = lockstep_enabled && m_lockstep_verifier->ShouldCheck(m_guest.pc);
          if (do_ls)
          {
            m_lockstep_verifier->Prepare(m_guest);
          }

          if (m_collect_dispatch_samples && (m_native_dispatches & 4095u) == 0)
            ++m_dispatch_samples[m_guest.pc];
          const u32 runtime_dispatch_address = m_guest.pc;
          u32 linked_dispatch_address = runtime_dispatch_address;
          if (m_has_rel_modules)
            ResolveNativeAddress(runtime_dispatch_address, &linked_dispatch_address, nullptr);
          m_guest.pc = linked_dispatch_address;
          if (entry_seen)
          {
            const u32 entry_off = linked_dispatch_address - ENTRY_LOG_BASE;
            if (entry_off < ENTRY_LOG_SPAN)
              entry_seen[entry_off >> 2] = 1;
            // The in-app panic modal blocks clean shutdown, so flush the set
            // periodically rather than only on the way out.
            if ((m_native_dispatches & 0x3FFFFFu) == 0)
              dump_entry_log("periodic");
          }
          m_module->dispatch(&m_guest, linked_dispatch_address);
          if (m_has_rel_modules)
            m_guest.pc = TranslateRelAddress(m_guest.pc);
          ++m_native_dispatches;

          if (do_ls)
          {
            m_lockstep_verifier->Verify(m_guest);
          }

          // Flush the module's per-block cycle charges into Dolphin's
          // downcount. A dispatch that charged nothing (PC-switch default,
          // pure embedded data) still costs 1 so the burst always makes
          // downcount progress; this per-dispatch flush is also the
          // dispatcher back-edge timing check — CoreTiming regains control
          // with at least CachedInterpreter's per-block frequency, so
          // external-interrupt latency matches stock.
          const s64 charge = -m_guest.downcount;
          m_guest.downcount = 0;
          if (charge <= 0)
          {
            ++m_stall_dispatches;
            if (m_stall_pcs.size() < 4096)
              ++m_stall_pcs[m_guest.pc];
          }
          const u64 effective_charge = static_cast<u64>(charge > 0 ? charge : 1);
          ppc.downcount -= static_cast<int>(effective_charge);
          m_charged_cycles += effective_charge;
          const u64 total_cycles = m_timebase_cycle_remainder + effective_charge;
          m_guest.timebase += total_cycles / SystemTimers::TIMER_RATIO;
          m_timebase_cycle_remainder = total_cycles % SystemTimers::TIMER_RATIO;

          // Idle loop skipping for configured target loops (e.g. Wii Menu OSIdleThread).
          // A spinning loop returns here with the same pc every time, so the
          // one-entry cache answers the common hit; the bloom rejects the rest
          // without touching the list.
          if (m_guest.pc == m_idle_pc_cached ||
              ((m_idle_pc_bloom >> ((m_guest.pc >> 2) & 63)) & 1u) != 0)
          {
            if (IsIdlePc(m_guest.pc))
            {
              m_idle_pc_cached = m_guest.pc;
              m_system.GetCoreTiming().Idle();
            }
          }

          // ctx->timebase is refreshed at burst start (SyncIn), and here we
          // incrementally advance it by the exact block cycle charges to
          // prevent guest busy-wait loops from spinning on a stale timebase.
          if (m_guest.exception)
          {
            // DolRecomp's runtime already redirected pc/msr/srr to the guest
            // exception vector; the flag only signals that it happened.
            m_guest.exception = 0;
            m_guest.program_exception = 0;
            ++m_native_exceptions;
          }
          if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
            break;  // Hook-raised synchronous exception: deliver via Dolphin below.
          if ((ppc.Exceptions & EXCEPTION_EXTERNAL_INT) != 0 &&
              (m_guest.msr & 0x8000u) != 0 && after_mtmsr(m_guest.pc))
            break;
        } while (m_module_active && fast_dispatchable_at(m_guest.pc) &&
                 !(mod_interception && m_guest.host_call && IsHostCallAddress(m_guest.pc)) &&
                 ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
        SyncOut();
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
        else if ((ppc.Exceptions & EXCEPTION_EXTERNAL_INT) != 0 && ppc.msr.EE &&
                 after_mtmsr(ppc.pc))
          power_pc.CheckExternalExceptions();
      }
      else
      {
        if (m_guest.host_call && IsHostCallAddress(ppc.pc))
        {
          SyncIn();
          bool handled = m_guest.host_call(&m_guest, m_guest.pc);
          if (!handled && m_guest.pc < m_guest.ram_size)
            handled = m_guest.host_call(&m_guest, m_guest.pc | 0x80000000u);
          if (m_fallback_jit && IsHostCallAddress(m_guest.lr))
            m_fallback_jit->GetBlockCache()->InvalidateICache(m_guest.lr, 4, true);
          if (handled)
          {
            const s64 charge = -m_guest.downcount;
            m_guest.downcount = 0;
            const u64 effective_charge = static_cast<u64>(charge > 0 ? charge : 1);
            ppc.downcount -= static_cast<int>(effective_charge);
            AdvanceGuestTimebase(effective_charge);
            SyncOut();
            continue;
          }
          SyncOut();
          if (m_fallback_jit)
          {
            m_host_call_passthrough_pc = ppc.pc;
            m_host_call_passthrough = true;
          }
        }
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        if (m_module_active && IsForcedFallbackAddress(ppc.pc))
        {
          ppc.downcount -= interpreter.SingleStepInner();
          ++m_fallback_steps;
        }
        else if (m_fallback_jit)
        {
          m_fallback_jit->Run();
        }
        else
        {
          do
          {
            ppc.downcount -= interpreter.SingleStepInner();
            ++m_fallback_steps;
          } while (!(m_module_active && DispatchableAt(ppc.pc)) &&
                   !IsHostCallAddress(ppc.pc) && ppc.downcount > 0 &&
                   *state_ptr == CPU::State::Running);
        }
      }
    } while (ppc.downcount > 0 && *state_ptr == CPU::State::Running);
  }

  if (entry_seen)
    dump_entry_log("shutdown");
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
