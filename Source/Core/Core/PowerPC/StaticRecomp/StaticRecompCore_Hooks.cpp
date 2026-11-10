// RecompCore: StaticRecomp CPU core - Memory and instruction fallback HLE hooks.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/HW/GPFifo.h"
#include "VideoCommon/CommandProcessor.h"
#include "Core/HW/SystemTimers.h"
#include "Common/Logging/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

namespace
{
constexpr u32 LOCKED_CACHE_BASE = 0xE0000000u;
}

bool StaticRecompCore::HookHostCall(CPUState* cpu, u32 address)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  return core->m_module_source.host_call &&
         core->m_module_source.host_call(cpu, address, core->m_module_source.host_call_user);
}

u64 StaticRecompCore::HookExternalRead(CPUState* cpu, u32 ea, u8 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  ea = core->TranslateRelAddress(ea);
  if (ea == 0)
    std::fprintf(stderr, "[zero-access] read size=%u guest_pc=%08x ppc_pc=%08x lr=%08x\n", size,
                 cpu->pc, core->m_ppc->pc, cpu->lr);
  // Publish the gather-pipe blocks the CPU has staged but not accounted for
  // before touching hardware: MMIO is the guest's only way to observe (or
  // depend on) FIFO state, and it runs a few thousand times a second, so the
  // drain is free. This is the same trigger Dolphin's JITs use.
  core->m_cp->FlushPendingGatherPipe();
  core->PropagateGuestMSR();
  auto& mmu = *core->m_mmu;
  u64 value;
  switch (size)
  {
  case 1:
    value = mmu.Read<u8>(ea);
    break;
  case 2:
    value = mmu.Read<u16>(ea);
    break;
  case 4:
    value = mmu.Read<u32>(ea);
    break;
  case 8:
    value = mmu.Read<u64>(ea);
    break;
  default:
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: external read of bad size {} at 0x{:08X}", size, ea);
    return 0;
  }
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_reads.push_back({ea, static_cast<u32>(value), size});
  }
  return value;
}

void StaticRecompCore::HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);

  ea = core->TranslateRelAddress(ea);

  // Gather-pipe fast path: stores to the write-gather pipe page at effective
  // 0xCC008000 go straight to GPFifo, mirroring the MMU's masked-write
  // special case without an MMU round trip. Keying on the effective page is
  // the same shortcut Dolphin's JITs take (optimizeGatherPipe). GPFifo
  // maintains ppc_state.gather_pipe_ptr internally.
  if ((ea & 0xFFFFF000) == 0xCC008000u)
  {
    ++core->m_hook_mmio_gather;
    if (core->m_lockstep_verifier->m_ls_journaling)
      core->m_lockstep_verifier->m_journal.native_mmio.push_back({ea, static_cast<u32>(value), size});
    auto& gpfifo = *core->m_gpfifo;
    switch (size)
    {
    case 1:
      gpfifo.Write8(static_cast<u8>(value));
      return;
    case 2:
      gpfifo.Write16(static_cast<u16>(value));
      return;
    case 4:
      gpfifo.Write32(static_cast<u32>(value));
      return;
    default:
      for (u32 i = size * 8u; i > 0;)
      {
        i -= 8;
        gpfifo.Write8(static_cast<u8>(value >> i));
      }
      return;
    }
  }

  ++core->m_hook_mmio_other;
  // Non-gather MMIO write: publish pending pipe blocks first (see HookExternalRead).
  core->m_cp->FlushPendingGatherPipe();
  core->PropagateGuestMSR();
  auto& mmu = *core->m_mmu;
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_mmio.push_back({ea, static_cast<u32>(value), size});
  }
  switch (size)
  {
  case 1:
    mmu.Write<u8>(static_cast<u8>(value), ea);
    break;
  case 2:
    mmu.Write<u16>(static_cast<u16>(value), ea);
    break;
  case 4:
    mmu.Write<u32>(static_cast<u32>(value), ea);
    break;
  case 8:
    mmu.Write<u64>(value, ea);
    break;
  default:
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: external write of bad size {} at 0x{:08X}", size, ea);
    break;
  }
}

void StaticRecompCore::HookGatherFlush(CPUState* cpu)
{
  // ABI v5: the module has staged a full 32-byte gather-pipe block, writing
  // through PPCState.gather_pipe_ptr (its cursor points straight at that
  // field), so all the host has to run is GPFifo's normal drain -- the same
  // tail GPFifoManager::Write32 performs after every store, minus the store.
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  core->m_gpfifo->CheckGatherPipe();
}

u32 StaticRecompCore::HookExternalRead32(CPUState* cpu, u32 ea, u8 rid)
{
  // eciwx external-control read. EAR-enable and alignment were checked by the
  // generated helper; Dolphin's interpreter services the access as a plain
  // MMU read (the rid is carried in EAR only).
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  ea = core->TranslateRelAddress(ea);
  core->m_cp->FlushPendingGatherPipe();
  core->PropagateGuestMSR();
  auto& mmu = *core->m_mmu;
  const u32 value = mmu.Read<u32>(ea);
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_reads.push_back({ea, value, 4});
  }
  return value;
}

void StaticRecompCore::HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid)
{
  // ecowx external-control write; see HookExternalRead32.
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  ea = core->TranslateRelAddress(ea);
  core->m_cp->FlushPendingGatherPipe();
  core->PropagateGuestMSR();
  auto& mmu = *core->m_mmu;
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_mmio.push_back({ea, value, 4});
  }
  mmu.Write<u32>(value, ea);
}

// Prove that one page of the segment-0xE window is identity translated, then
// let the module serve it inline. Called from the host fallback, i.e. only after
// the page already translated successfully once, so the endpoint lookups cost
// nothing measurable (one page, once). Exactness is the point: a page that BATs
// or the TLB map anywhere else simply never gets a window, and the hook path
// keeps handling it.
void StaticRecompCore::PublishL1Window(u32 effective_address)
{
  ++m_pub_calls;
  m_guest.l1_win_host = nullptr;

  auto& memory = *m_memory;
  if (!m_guest.l1_cache || m_guest.l1_cache_size == 0 || memory.GetL1Cache() != m_guest.l1_cache)
    return;
  if (m_lockstep_verifier->m_ls_journaling)
    return;

  const u32 page_start = effective_address & ~0xFFFu;
  const u32 page_last = page_start + 0xFFFu;
  const u32 l1_offset = page_start & 0x0FFFFFFFu;
  // The whole page must live inside the locked-cache buffer: an access near the
  // page end has to keep taking the hook path, which is what rejects it when
  // the hardware path would fall through to RAM/MMIO.
  if (static_cast<u64>(l1_offset) + 0x1000ull > m_guest.l1_cache_size)
    return;

  if (m_ppc->msr.DR.Value())
  {
    auto& mmu = *m_mmu;
    const auto first = mmu.GetTranslatedAddress(page_start);
    const auto last = mmu.GetTranslatedAddress(page_last);
    if (!first || !last || *first != page_start || *last != page_last)
      return;
  }

  m_guest.l1_win_page = effective_address >> 12;
  // PROBE: the module compares this against the live MSR for equality. MSR[FP]
  // is set lazily by the guest mid-burst, so a snapshot taken before the first
  // FP instruction can never match again. Publishing with FP forced set tests
  // how much of the hook traffic that one bit is responsible for.
  m_guest.l1_win_msr = m_guest.msr | 0x00002000u;
  m_guest.l1_win_host = m_guest.l1_cache + l1_offset;
  ++m_pub_ok;
}

void* StaticRecompCore::HookExternalPointer(CPUState* cpu, u32 ea, u32 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  ++core->m_hook_ptr_calls;

  // Lockstep must observe every hardware access through the journaling MMU
  // hooks, and odd sizes have no direct-mapping contract: fall back.
  if (core->m_lockstep_verifier->m_ls_journaling)
    return nullptr;
  if (size == 0 || size > 8)
    return nullptr;

  ea = core->TranslateRelAddress(ea);

  // ReadFromHardware splits cross-page accesses byte-by-byte before
  // translating; hand those back so the hook path reproduces the split.
  if ((ea & ~0xFFFu) != ((ea + size - 1u) & ~0xFFFu))
    return nullptr;

  // Translate exactly like the hardware path does: live MSR.DR + BAT/TLB,
  // refreshed on every access. No identity assumption about where the
  // window lands -- the SDK map can resolve to RAM, and a stale or wrong
  // placement splits the module's view from the host's.
  core->PropagateGuestMSR();
  u32 addr = ea;
  if (core->m_ppc->msr.DR.Value())
  {
    auto translated = core->m_mmu->GetTranslatedAddress(ea);
    if (!translated)
      return nullptr;  // no mapping / page fault: let the MMU hooks raise it
    addr = *translated;
  }

  auto& memory = *core->m_memory;
  // Locked L1: same guard and offset math as ReadFromHardware's branch.
  if (memory.GetL1Cache() && (addr >> 28) == 0xE &&
      addr < (LOCKED_CACHE_BASE + memory.GetL1CacheSize()))
  {
    const u32 off = addr - LOCKED_CACHE_BASE;
    if (off + size <= memory.GetL1CacheSize())
    {
      ++core->m_hook_ptr_l1;
      if (!core->m_guest.l1_win_host)
        ++core->m_win_null;
      else if (core->m_guest.l1_win_page != (ea >> 12))
        ++core->m_win_page;
      else
      {
        ++core->m_win_msr;
        core->m_msr_xor |= (u64)(core->m_guest.msr ^ core->m_guest.l1_win_msr);
        core->m_msr_pub = core->m_guest.l1_win_msr;
        core->m_msr_live = core->m_guest.msr;
      }
      // Teach the module to serve this page inline from now on. After that this
      // hook only runs again if the window is invalidated or the guest reaches
      // a different page of the window.
      if (!core->m_guest.l1_win_host || core->m_guest.l1_win_page != (ea >> 12))
        core->PublishL1Window(ea);
      return memory.GetL1Cache() + off;
    }
  }
  // Plain RAM: mirror the MMU's masked-branch semantics. Games with dcache
  // enabled must go through the MMU's dCache path, and only in-bounds
  // mirrors get a pointer; everything else falls back to the hooks.
  if (!core->m_ppc->m_enable_dcache && memory.GetRAM() &&
      (addr & 0xF8000000) == 0x00000000)
  {
    const u32 off = addr & memory.GetRamMask();
    if (static_cast<u64>(off) + size <= memory.GetRamSize())
    {
      ++core->m_hook_ptr_ram;
      return memory.GetRAM() + off;
    }
  }
  ++core->m_hook_ptr_null;
  return nullptr;
}

u32 StaticRecompCore::HookSPRRead(CPUState* cpu, u16 spr, u32 cia)
{
  // Note: no L1-window invalidation here. Reading an SPR cannot change the
  // address mapping, and mfspr DEC/TB run in the OS scheduler tick, so clearing
  // the window on every read would just make the guest republish it.
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  ++core->m_spr_reads;
  core->InvalidateL1Window();
  auto& ppc = *core->m_ppc;
  if (spr >= 1024)
  {
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
    return 0;
  }

  switch (spr)
  {
  case SPR_DEC:
    if ((ppc.spr[SPR_DEC] & 0x80000000u) == 0)
      ppc.spr[SPR_DEC] = core->m_system.GetSystemTimers().GetFakeDecrementer();
    break;
  case SPR_WPAR:
    if (core->m_gpfifo->IsBNE())
      ppc.spr[SPR_WPAR] |= 1;
    else
      ppc.spr[SPR_WPAR] &= ~1u;
    break;
  case SPR_UPMC1:
    return ppc.spr[SPR_PMC1];
  case SPR_UPMC2:
    return ppc.spr[SPR_PMC2];
  case SPR_UPMC3:
    return ppc.spr[SPR_PMC3];
  case SPR_UPMC4:
    return ppc.spr[SPR_PMC4];
  case SPR_IABR:
    return ppc.spr[SPR_IABR] & ~1u;
  default:
    break;
  }
  return ppc.spr[spr];
}

void StaticRecompCore::HookSPRWrite(CPUState* cpu, u16 spr, u32 value, u32 cia)
{
  // Translation state may be about to change: every BAT/TLB/HID0/MSR/SPR
  // write lands here (or in a sibling hook). Drop the inline locked-L1 window
  // so the next locked-cache access re-validates through the host path.
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  core->InvalidateL1Window();
  auto& system = core->m_system;
  auto& ppc = system.GetPPCState();
  if (spr >= 1024)
  {
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
    return;
  }

  const u32 old_value = ppc.spr[spr];
  ppc.spr[spr] = value;

  switch (spr)
  {
  case SPR_TL_W:
    TL(ppc) = value;
    system.GetSystemTimers().TimeBaseSet();
    return;
  case SPR_TU_W:
    TU(ppc) = value;
    system.GetSystemTimers().TimeBaseSet();
    return;
  case SPR_PVR:
    ppc.spr[SPR_PVR] = old_value;
    return;
  case SPR_HID0:
  {
    if (HID0(ppc).ICFI)
    {
      HID0(ppc).ICFI = 0;
      ppc.iCache.Reset(system.GetJitInterface());
    }
    return;
  }
  case SPR_HID1:
    ppc.spr[SPR_HID1] &= 0xF8000000u;
    return;
  case SPR_HID4:
    if (old_value != value)
    {
      system.GetMMU().IBATUpdated();
      system.GetMMU().DBATUpdated();
    }
    return;
  case SPR_WPAR:
    system.GetGPFifo().ResetGatherPipe();
    return;
  case SPR_DMAL:
    if (DMAL(ppc).DMA_T)
    {
      const u32 mem_address = DMAU(ppc).MEM_ADDR << 5;
      const u32 cache_address = DMAL(ppc).LC_ADDR << 5;
      u32 length = (DMAU(ppc).DMA_LEN_U << 2) | DMAL(ppc).DMA_LEN_L;
      if (length == 0)
        length = 128;
      if (DMAL(ppc).DMA_LD)
        system.GetMMU().DMA_MemoryToLC(cache_address, mem_address, length);
      else
        system.GetMMU().DMA_LCToMemory(mem_address, cache_address, length);
    }
    DMAL(ppc).DMA_T = 0;
    return;
  case SPR_DEC:
    if ((old_value >> 31) == 0 && (value >> 31) != 0)
      ppc.Exceptions |= EXCEPTION_DECREMENTER;
    system.GetSystemTimers().DecrementerSet();
    return;
  case SPR_SDR:
    system.GetMMU().SDRUpdated();
    return;
  case SPR_MMCR0:
  case SPR_MMCR1:
    PowerPC::MMCRUpdated(ppc);
    return;
  case SPR_THRM1:
  case SPR_THRM2:
  case SPR_THRM3:
  {
    const auto update = [&ppc](UReg_THRM12* reg) {
      if (!THRM3(ppc).E || !reg->V)
      {
        reg->TIV = 0;
      }
      else
      {
        reg->TIV = 1;
        reg->TIN = reg->TID ? 42 < reg->THRESHOLD : 42 > reg->THRESHOLD;
      }
    };
    update(&THRM1(ppc));
    update(&THRM2(ppc));
    return;
  }
  default:
    break;
  }

  const bool ibat = (spr >= SPR_IBAT0U && spr <= SPR_IBAT3L) ||
                    (spr >= SPR_IBAT4U && spr <= SPR_IBAT7L);
  const bool dbat = (spr >= SPR_DBAT0U && spr <= SPR_DBAT3L) ||
                    (spr >= SPR_DBAT4U && spr <= SPR_DBAT7L);
  if (old_value != value && ibat)
    system.GetMMU().IBATUpdated();
  else if (old_value != value && dbat)
    system.GetMMU().DBATUpdated();
}

void StaticRecompCore::HookCacheControl(CPUState* cpu, u8 operation, u32 ea, u32 cia)
{
  // Translation state may be about to change: every BAT/TLB/HID0/MSR/SPR
  // write lands here (or in a sibling hook). Drop the inline locked-L1 window
  // so the next locked-cache access re-validates through the host path.
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  core->InvalidateL1Window();
  ea = core->TranslateRelAddress(ea);
  core->PropagateGuestMSR();
  auto& ppc = *core->m_ppc;
  auto& mmu = *core->m_mmu;

  if (operation == PPC_CACHE_ICBI)
  {
    ++core->m_cacheop_icbi;
    ppc.iCache.Invalidate(*core->m_memory, core->m_system.GetJitInterface(), ea);
    return;
  }

  if (!ppc.m_enable_dcache)
  {
    // The only instruction-cache effect a dcb* has under static recompilation
    // is demoting the chunks overlapping the line back to Unverified (see
    // EmptyBlockCache). A line with no guest code on it demotes nothing, and
    // this loop runs millions of times a second on data.
    ++core->m_cacheop_probes;
    if (!core->CacheOpTouchesCode(ea))
    {
      ++core->m_cacheop_skipped;
      return;
    }
    core->m_system.GetJitInterface().InvalidateICacheLine(ea);
    return;
  }

  switch (operation)
  {
  case PPC_CACHE_DCBST:
    mmu.StoreDCacheLine(ea);
    break;
  case PPC_CACHE_DCBF:
    mmu.FlushDCacheLine(ea);
    break;
  case PPC_CACHE_DCBI:
    mmu.InvalidateDCacheLine(ea);
    break;
  default:
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
    break;
  }
}

void StaticRecompCore::HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia)
{
  // Translation state may be about to change: every BAT/TLB/HID0/MSR/SPR
  // write lands here (or in a sibling hook). Drop the inline locked-L1 window
  // so the next locked-cache access re-validates through the host path.
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  core->InvalidateL1Window();
  ++core->m_fb_calls;
  cia = core->TranslateRelAddress(cia);
  ++core->m_hook_fallback_instructions;

  // Lockstep: a block that fell back to the interpreter for an unmodeled
  // instruction (DMA mtspr, cache op, ...) performed side effects not captured
  // by the RAM journal / MMIO hooks, so re-running it on the shadow would
  // double-issue them. Mark it unsafe to differentially check.
  if (core->m_lockstep_verifier->m_ls_journaling)
    core->m_lockstep_verifier->m_ls_fallback_seen = true;

  auto& system = core->m_system;
  auto& ppc = system.GetPPCState();

  // Fast path for dcbf/dcbst/dcbi/icbi: streaming code flushes caches in
  // 32-byte loops (thousands per frame), and these ops read two GPRs and
  // change no CPU state, so they run straight off ctx without the full
  // SyncOut/interpreter/SyncIn round trip. This mirrors Dolphin's
  // interpreter with dcache emulation off: every one funnels into
  // InvalidateICacheLine (keeping the SMC guard exact). dcbi's PR!=0
  // privilege trap and dcache-on configs take the slow path.
  if ((raw >> 26) == 31u && !ppc.m_enable_dcache)
  {
    const u32 xo = (raw >> 1) & 0x3FFu;
    if (xo == 86u || xo == 54u || xo == 982u || (xo == 470u && (cpu->msr & 0x4000u) == 0))
    {
      const u32 ra = (raw >> 16) & 31u;
      const u32 rb = (raw >> 11) & 31u;
      const u32 ea = (ra ? cpu->gpr[ra] : 0u) + cpu->gpr[rb];
      if (xo == 982u)
      {
        ++core->m_cacheop_icbi;
        ppc.iCache.Invalidate(system.GetMemory(), system.GetJitInterface(), ea);
      }
      else if (core->m_skip_dataline_cacheops && !core->CacheOpTouchesCode(ea))
      {
        // Streaming dcb* over data: the line holds no guest code, so there is
        // no chunk for an instruction-cache invalidation to demote. Skip the
        // address translation and range walk entirely.
        ++core->m_cacheop_probes;
        ++core->m_cacheop_skipped;
      }
      else
      {
        ++core->m_cacheop_probes;
        system.GetJitInterface().InvalidateICacheLine(ea);
      }
      // These bypass SingleStepInner, so charge Dolphin's PPCTables cost
      // here (icbi 4, dcbf/dcbst/dcbi 5); their emitted block cost is zero.
      ppc.downcount -= (xo == 982u) ? 4 : 5;
      cpu->pc = cia + 4u;
      ++core->m_hook_fallback_fast;
      return;
    }
  }

  // The recompiled segment resumes via the dispatcher at the PC this leaves
  // behind, so this must execute exactly the instruction at cia via
  // Dolphin's interpreter and hand the register state back.
  ++core->m_hook_fallback_slow;
  if (core->m_collect_dispatch_samples)
    ++core->m_fallback_words[raw];
  core->SyncOut();
  ppc.pc = cia;
  ppc.npc = cia + 4;
  ppc.downcount -= system.GetInterpreter().SingleStepInner();
  core->SyncIn();
}
