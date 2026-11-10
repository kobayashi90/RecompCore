// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"

namespace Core
{
class System;
}

namespace Memory
{
class MemoryManager;
}
namespace PowerPC
{
struct PowerPCState;
class MMU;
}
namespace GPFifo
{
class GPFifoManager;
}
namespace CommandProcessor
{
class CommandProcessorManager;
}

namespace StaticRecompLockstep
{
class StaticRecompLockstepVerifier;
}

// Executes statically recompiled per-game native code when the PC is covered by
// a loaded module; falls back to Dolphin's interpreter for everything else.
// With no module loaded this core is exactly an interpreter loop.
class StaticRecompCore : public JitBase
{
public:
  friend class StaticRecompLockstep::StaticRecompLockstepVerifier;

  explicit StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source);
  StaticRecompCore(const StaticRecompCore&) = delete;
  StaticRecompCore(StaticRecompCore&&) = delete;
  StaticRecompCore& operator=(const StaticRecompCore&) = delete;
  StaticRecompCore& operator=(StaticRecompCore&&) = delete;
  ~StaticRecompCore() override;

  void Init() override;
  void Shutdown() override;

  void Run() override;
  void SingleStep() override;
  bool IsModuleActive() const;
  bool DispatchableAt(u32 address);
  bool FastDispatchableAt(u32 address);
  bool IsHostCallAddress(u32 address) const;
  bool ShouldYieldAt(u32 address);

  void ClearCache() override;
  void Jit(u32 em_address) override {}
  bool HandleFault(uintptr_t access_address, SContext* ctx) override { return false; }

  JitBaseBlockCache* GetBlockCache() override { return &m_block_cache; }

  // Gather-pipe and quantize hooks do not only fire from module code: anything
  // the module does not cover runs on m_fallback_jit, a real Jit64/JitArm64.
  // That core reads fifoWriteAddresses at compile time and owns the block that
  // has to be invalidated, so registering against this core instead left the
  // check uncompiled and the hook firing on every gather-pipe store forever.
  JitBase* GetExceptionCheckTarget() override
  {
    return m_fallback_jit ? m_fallback_jit.get() : this;
  }

  void EraseSingleBlock(const JitBlock& block) override {}
  std::vector<MemoryStats> GetMemoryStats() const override { return {}; }
  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  const CommonAsmRoutinesBase* GetAsmRoutines() override { return nullptr; }
  const char* GetName() const override { return "StaticRecomp"; }

private:
  // JitBaseBlockCache with no generated blocks; exists so generic
  // icache-invalidation plumbing in JitInterface has a real object to talk
  // to, and to feed every invalidation into the SMC demotion guard (D4).
  class EmptyBlockCache : public JitBaseBlockCache
  {
  public:
    explicit EmptyBlockCache(StaticRecompCore& core) : JitBaseBlockCache(core), m_core(core) {}
    void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override {}

  protected:
    void InvalidateICacheInternal(u32 physical_address, u32 address, u32 length,
                                  bool forced) override
    {
      m_core.OnICacheInvalidate(address, length);
    }

  private:
    StaticRecompCore& m_core;
  };

  void LoadModule();

  // D4 SMC guard, verify-on-entry model. Every chunk starts Unverified; the
  // first native dispatch into it hashes its guest RAM against the module's
  // recorded hash of the original text. An icache invalidation touching a
  // chunk resets it to Unverified (Dolphin invalidates while *loading* code,
  // so invalidation alone must not retire coverage); a hash mismatch (real
  // SMC) marks it Failed, interpreter-only until the next invalidation.
  enum ChunkState : u8
  {
    CHUNK_UNVERIFIED = 0,
    CHUNK_VERIFIED = 1,
    CHUNK_FAILED = 2,
  };

  void OnICacheInvalidate(u32 address, u32 length);
  int ChunkIndexOf(u32 address);
  bool IsForcedFallbackAddress(u32 address) const;
  bool ChunkContainsHostCall(u32 index) const;
  void VerifyChunk(u32 index);
  bool ResolveNativeAddress(u32 runtime_address, u32* linked_address, u32* rel_section_index);
  bool ResolveRuntimeAddress(u32 linked_address, u32* runtime_address) const;

  // Locked-L1 identity window: publish the page that just fell through to the
  // host hook (after proving TranslateAddress is the identity across it), and
  // drop it whenever translation state may have changed.
  void PublishL1Window(u32 effective_address);
  void InvalidateL1Window() { m_guest.l1_win_host = nullptr; }
  u32 TranslateRelAddress(u32 linked_address);
  void RefreshRelSections();

  static void SetPPCStateFromGuestState(const CPUState& s, PowerPC::PowerPCState& ppc);

  // D1 state residency: registers live in m_guest while native code runs;
  // full sync at every native-burst boundary.
  void SyncIn();   // Dolphin PowerPCState -> m_guest
  void SyncOut();  // m_guest -> Dolphin PowerPCState
  void AdvanceGuestTimebase(u64 cpu_cycles);

  // CPUState hooks (module -> chassis environment). `cpu->external_user_data`
  // is the StaticRecompCore*.
  static u64 HookExternalRead(CPUState* cpu, u32 ea, u8 size);
  static void HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size);
  static u32 HookExternalRead32(CPUState* cpu, u32 ea, u8 rid);
  static void HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid);
  static void* HookExternalPointer(CPUState* cpu, u32 ea, u32 size);
  static u32 HookSPRRead(CPUState* cpu, u16 spr, u32 cia);
  static void HookSPRWrite(CPUState* cpu, u16 spr, u32 value, u32 cia);
  static void HookCacheControl(CPUState* cpu, u8 operation, u32 ea, u32 cia);
  static void HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia);
  static bool HookHostCall(CPUState* cpu, u32 address);
  // ABI v5 gather-pipe staging: the module calls this once a staged block
  // reaches 32 bytes (see gather_pipe_store in GXRuntime's core/cpu.h).
  static void HookGatherFlush(CPUState* cpu);

  // Keep Dolphin's MSR-derived state (translation mode, feature flags) in step
  // with the guest MSR before any MMU access or exception delivery.
  void PropagateGuestMSR();

  std::unique_ptr<StaticRecompLockstep::StaticRecompLockstepVerifier> m_lockstep_verifier;

  EmptyBlockCache m_block_cache{*this};

  CPUState m_guest{};
  // Hot subsystem pointers, cached once instead of going through
  // System::GetX() (a non-inlined pimpl dereference) on every hardware access,
  // burst and hook. The subsystems outlive this object.
  PowerPC::PowerPCState* m_ppc = nullptr;
  PowerPC::MMU* m_mmu = nullptr;
  Memory::MemoryManager* m_memory = nullptr;
  GPFifo::GPFifoManager* m_gpfifo = nullptr;
  // Cached CommandProcessor pointer: every guest MMIO access drains the pending
  // gather-pipe publishes (the same trigger Dolphin's JITs use), and that path
  // must not pay a System::GetCommandProcessor() pimpl hop.
  CommandProcessor::CommandProcessorManager* m_cp = nullptr;
  Common::DynamicLibrary m_library;
  StaticRecompModuleSource m_module_source;
  const StaticRecompModuleDesc* m_module = nullptr;
  bool m_module_active = false;
  u32 m_host_call_passthrough_pc = 0;
  bool m_host_call_passthrough = false;
  std::unique_ptr<JitBase> m_fallback_jit;

  u64 m_native_dispatches = 0;
  u64 m_fallback_steps = 0;
  u64 m_native_exceptions = 0;
  u64 m_hook_fallback_instructions = 0;
  u64 m_hook_fallback_fast = 0;  // cache-op fast path (no interpreter round trip)
  u64 m_hook_fallback_slow = 0;  // full SyncOut/interpreter/SyncIn round trip
  // Effective-address traffic that missed the module's inline RAM fast path and
  // therefore reached the host hooks. Splitting by outcome is what tells apart
  // "the guest really does use the locked-cache window / MMIO this much" from
  // "the module is missing an address alias and paying a host round trip for
  // plain RAM".
  u64 m_hook_ptr_calls = 0;
  u64 m_hook_ptr_l1 = 0;
  u64 m_hook_ptr_ram = 0;
  u64 m_hook_ptr_null = 0;
  u64 m_hook_mmio_gather = 0;  // 0xCC008xxx write-gather page
  u64 m_hook_mmio_other = 0;
  // Why the module's inline locked-L1 window did not serve an access. The
  // module only calls external_pointer when its own (page, msr) test failed, so
  // each of these is a distinct cause and they demand different fixes: a null
  // window means something invalidated it, a page mismatch means the guest
  // walks pages, and an msr mismatch means the guest toggles MSR between
  // locked-cache accesses.
  u64 m_spr_reads = 0;
  u64 m_pub_calls = 0;
  u64 m_pub_ok = 0;
  u64 m_win_null = 0;
  u64 m_win_page = 0;
  u64 m_win_msr = 0;
  // OR of every (live MSR ^ published MSR) seen on an msr-mismatch miss. This
  // names the exact bits that make the window unusable instead of guessing.
  u64 m_msr_xor = 0;
  u64 m_fb_calls = 0;
  u64 m_msr_pub = 0;
  u64 m_msr_live = 0;
  // Raw guest instruction words that took the slow fallback path. Only
  // populated with STATICRECOMP_DISPATCH_SAMPLES set.
  std::unordered_map<u32, u64> m_fallback_words;
  u64 m_timebase_cycle_remainder = 0;
  std::unordered_map<u32, u64> m_dispatch_samples;
  u64 m_bursts = 0;          // SyncIn..SyncOut native runs (diagnostic)
  u64 m_charged_cycles = 0;  // cycles flushed from module charges (diagnostic)

  // "Dispatched into a chunk but the chunk charged nothing" counter. The
  // generated chunk functions charge cycles per basic block and return without
  // charging when the pc lands outside the emitted instruction set (the switch
  // default). Today every guest instruction has its own case, so this must stay
  // at zero; it is the tripwire for pruning that case list to real entry points
  // only, which is what lets the compiler keep guest GPRs in host registers.
  u64 m_stall_dispatches = 0;
  std::unordered_map<u32, u64> m_stall_pcs;

  // Per-second performance accounting, enabled with STATICRECOMP_PERF=1.
  // Shutdown totals cannot answer "which second was slow": boot dominates them
  // and a wall-clock window does not contain a fixed amount of guest work. One
  // line per second with deltas gives bursts/s, guest cycles/s and the split of
  // external-pointer outcomes, which is what identifies a burst as module work
  // versus hardware-access glue.
  static constexpr std::size_t PERF_FIELDS = 19;
  bool m_perf_log = false;
  std::chrono::steady_clock::time_point m_perf_last{};
  u64 m_perf_prev[PERF_FIELDS]{};
  u64 m_perf_secs = 0;

  // D4 guard state: parallel to m_module->chunk_ranges.
  std::vector<u8> m_chunk_state;
  mutable std::vector<u8> m_chunk_host_call_state;
  std::vector<StaticRecompRange> m_forced_fallback_ranges;
  struct ActiveRelSection
  {
    u32 module_id;
    u32 section_index;
    u32 linked_start;
    u32 runtime_start;
    u32 size;
  };
  std::vector<ActiveRelSection> m_active_rel_sections;
  std::vector<int> m_chunk_rel_sections;
  std::vector<u64> m_effective_chunk_hashes;
  u64 m_rel_mapping_generation = 0;
  u32 m_failed_chunks = 0;    // chunks currently failing verification (real SMC)
  u64 m_verifications = 0;    // chunk hash checks performed
  u64 m_reverify_events = 0;  // invalidations that reset a chunk to Unverified

  // Lookup table optimization for O(1) chunk searches
  std::vector<int> m_chunk_lookup_table;
  u32 m_lookup_ram_size = 0;
  u32 m_lookup_exram_size = 0;
  int GetAddressLookupIndex(u32 address) const;
  void InitLookupTable(u32 ram_size, u32 exram_size);

  // 32-byte-granular "this line holds guest code" bitmap over RAM+EXRAM, in the
  // same runtime address space as m_chunk_lookup_table. Guest code issues dcb*
  // and icbi in tight streaming loops (millions per second) purely to keep the
  // real data cache coherent; under static recompilation the only thing an
  // instruction-cache invalidation can do is demote the chunks overlapping the
  // line back to Unverified. A line with no chunk on it therefore has nothing
  // to demote, and this bitmap answers that in a handful of instructions
  // instead of the address resolution + binary search + range walk that
  // OnICacheInvalidate otherwise needs per call.
  std::vector<u64> m_code_line_bits;
  u32 m_code_line_count = 0;
  u64 m_cacheop_probes = 0;
  u64 m_cacheop_skipped = 0;
  u64 m_cacheop_icbi = 0;
  // A/B switch for the data-line cache-op skip (STATICRECOMP_CACHEOP_GATE=0
  // restores the unconditional invalidation) so the win can be measured
  // against a moved ambient load on the same binary.
  bool m_skip_dataline_cacheops = true;
  bool CacheOpTouchesCode(u32 ea) const;

  // Dispatch locality: most control transfers stay inside one chunk, so the
  // last hit short-circuits the chunk binary search on the hot path.
  mutable u32 m_last_chunk_index = 0;

  bool m_collect_dispatch_samples = false;
  bool m_has_rel_modules = false;

  // Idle-loop skip targets. This is consulted once per dispatch, so it must not
  // cost a hash lookup: a spin loop returns to the dispatcher with the same pc
  // over and over, so the one-entry cache answers almost every hit, and the
  // 64-bit bloom rejects the "not an idle pc" case in two instructions instead
  // of an unordered_set probe. The vector is sorted so a false positive over a
  // handful of entries is a couple of compares.
  std::vector<u32> m_idle_pcs;
  u64 m_idle_pc_bloom = 0;
  mutable u32 m_idle_pc_cached = 0;

  bool IsIdlePc(u32 pc) const
  {
    for (const u32 idle : m_idle_pcs)
    {
      if (idle == pc)
        return true;
    }
    return false;
  }
};

extern StaticRecompCore* g_static_recomp_core;
u32 StaticRecompShouldYieldAt(u32 address);
