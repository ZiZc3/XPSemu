// fexrun — runs FEX's 32-bit ASM unit tests through our own FEXCore
// embedding (no Linux syscall front end), on the device.
//
//   fexrun [options] <test-dir> <manifest>
//     --base identity       guest addresses are host addresses (baseline)
//     --base <hex>|auto     guest memory behind the memory-base register
//     --diff                run every test both ways and compare the full
//                           CPU state and every mapped guest page
//     --maxinst N           FEX MaxInst (default 500)
//     --multiblock 0|1      FEX Multiblock (default 1)
//     --timeout S           per-test timeout in seconds (default 10)
//     --verbose             print register dumps for failures
//     --sysops              FEX system mode: privileged/system instructions
//                           exit to the test handler (tests/native/sysops)
//
// The manifest lists one test per line: "<relative name> [tags...]".
// Each test is <test-dir>/<name>.bin (assembled at 0x10000) plus
// <name>.cfg (written by tools/native/fexasm_prep.py from the test's JSON
// header). Every run is a forked child, so a crash or a hang only fails that
// test. Output: one "PASS|FAIL|FAULT|TIMEOUT|SKIP|ERROR <name>" line per test
// (with --diff: "SAME|DIFF <name> <identity>/<membase> [differing parts]"),
// then a summary line.

#include "Common/HostFeatures.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/LogManager.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

namespace {

// ---------------------------------------------------------------- config

struct RegExpect {
  unsigned Index; // FEX harness name index: 0 RIP, 1-16 GPRs, 17-32 XMM, 33 GS, 34 FS, 35-42 MM
  std::vector<uint64_t> Values;
};

struct MemData {
  uint64_t Addr;
  std::vector<uint8_t> Bytes;
};

struct TestConfig {
  bool Is64 = false;
  uint32_t Features = 0;
  std::vector<std::pair<uint64_t, uint64_t>> Regions;
  std::vector<MemData> Mem;
  std::vector<RegExpect> Regs;
  std::vector<std::pair<std::string, std::string>> Env;
};

bool ParseConfig(const std::string& Path, TestConfig& Out) {
  std::ifstream In(Path);
  if (!In) {
    return false;
  }
  std::string Line;
  while (std::getline(In, Line)) {
    std::istringstream S(Line);
    std::string Key;
    if (!(S >> Key)) {
      continue;
    }
    if (Key == "mode") {
      int M;
      S >> M;
      Out.Is64 = M == 64;
    } else if (Key == "feat") {
      std::string V;
      S >> V;
      Out.Features = std::stoul(V, nullptr, 0);
    } else if (Key == "region") {
      std::string A, L;
      S >> A >> L;
      Out.Regions.emplace_back(std::stoull(A, nullptr, 0), std::stoull(L, nullptr, 0));
    } else if (Key == "mem") {
      std::string A, Hex;
      S >> A >> Hex;
      MemData D {std::stoull(A, nullptr, 0), {}};
      for (size_t i = 0; i + 1 < Hex.size(); i += 2) {
        D.Bytes.push_back(static_cast<uint8_t>(std::stoul(Hex.substr(i, 2), nullptr, 16)));
      }
      Out.Mem.push_back(std::move(D));
    } else if (Key == "reg") {
      RegExpect R {};
      std::string V;
      S >> R.Index;
      while (S >> V) {
        R.Values.push_back(std::stoull(V, nullptr, 0));
      }
      Out.Regs.push_back(std::move(R));
    } else if (Key == "env") {
      std::string K, V;
      S >> K >> V;
      Out.Env.emplace_back(K, V);
    }
  }
  return true;
}

// Feature bits from FEX's Scripts/json_config_parse.py.
enum : uint32_t {
  FEATURE_AVX = 1u << 2,
  FEATURE_RAND = 1u << 3,
  FEATURE_SHA = 1u << 4,
  FEATURE_CLZERO = 1u << 5,
  FEATURE_LINUX = 1u << 9,
  FEATURE_AES256 = 1u << 10,
  FEATURE_AFP = 1u << 11,
};

// ------------------------------------------------------------- options

struct Options {
  bool Identity = true;
  uint64_t Base = 0; // 0 with !Identity: let the kernel pick
  bool Diff = false;
  int MaxInst = 500;
  int Multiblock = 1;
  int TimeoutSec = 10;
  bool Verbose = false;
  bool SysOps = false; // FEX system mode, with the test handler below
};

// ------------------------------------------------------------- FEX glue

void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  if (Level <= LogMan::DebugLevels::INFO) {
    fprintf(stderr, "[fex %s] %s\n", LogMan::DebugLevelStr(Level), Message);
  }
}

void AssertHandler(const char* Message) {
  fprintf(stderr, "[fex ASSERT] %s\n", Message);
  fflush(stderr);
  _exit(4);
}

bool HandleSystemOp(FEXCore::Core::CpuStateFrame* Frame);

class HarnessSyscalls final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    if (Frame->SystemOpInfo && HandleSystemOp(Frame)) {
      return;
    }
    fprintf(stderr, "unexpected syscall at rip=0x%llx\n", (unsigned long long)Frame->State.rip);
    fflush(stderr);
    _exit(5);
  }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

class HarnessSignals final : public FEXCore::SignalDelegator {};

uint64_t g_MemBase;

void FaultHandler(int Sig, siginfo_t* Info, void* Ctx) {
  const auto& Mc = static_cast<ucontext_t*>(Ctx)->uc_mcontext;
  char Buf[256];
  int N = snprintf(Buf, sizeof(Buf), "fault sig=%d addr=%p pc=0x%llx x19=0x%llx x24=0x%llx base=0x%llx\n", Sig, Info->si_addr,
                   (unsigned long long)Mc.pc, (unsigned long long)Mc.regs[19], (unsigned long long)Mc.regs[24], (unsigned long long)g_MemBase);
  write(2, Buf, N);
  _exit(2);
}

void InstallFaultHandlers() {
  struct sigaction Sa {};
  Sa.sa_sigaction = FaultHandler;
  Sa.sa_flags = SA_SIGINFO;
  sigemptyset(&Sa.sa_mask);
  for (int Sig : {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE}) {
    sigaction(Sig, &Sa, nullptr);
  }
}

constexpr uint64_t PAGE = 4096;
constexpr uint64_t CODE_START = 0x10000;
constexpr uint64_t STACK_OFFSET = 0xC000'0000;
constexpr uint64_t GUARD = 64 * 1024;
constexpr uint64_t GUEST_SPACE = 1ull << 32;

// Guest memory: identity maps host == guest, otherwise Base + guest inside a
// 4 GiB reservation with PROT_NONE guards on both sides.
struct GuestMem {
  bool Identity = true;
  uint8_t* Base = nullptr;
  std::vector<std::pair<uint64_t, uint64_t>> Mapped; // page-aligned guest ranges

  bool Reserve(uint64_t Want) {
    if (Identity) {
      return true;
    }
    void* Hint = Want ? reinterpret_cast<void*>(Want - GUARD) : nullptr;
    int Flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (Want ? MAP_FIXED_NOREPLACE : 0);
    void* R = mmap(Hint, GUEST_SPACE + 2 * GUARD, PROT_NONE, Flags, -1, 0);
    if (R == MAP_FAILED) {
      fprintf(stderr, "reserve failed: %s\n", strerror(errno));
      return false;
    }
    Base = static_cast<uint8_t*>(R) + GUARD;
    return true;
  }

  uint8_t* Host(uint64_t Guest) const {
    return Identity ? reinterpret_cast<uint8_t*>(Guest) : Base + static_cast<uint32_t>(Guest);
  }

  bool Map(uint64_t Guest, uint64_t Size) {
    uint64_t Start = Guest & ~(PAGE - 1);
    uint64_t End = (Guest + Size + PAGE - 1) & ~(PAGE - 1);
    void* Want = Host(Start);
    int Flags = MAP_ANONYMOUS | MAP_PRIVATE | (Identity ? MAP_FIXED_NOREPLACE : MAP_FIXED);
    void* R = mmap(Want, End - Start, PROT_READ | PROT_WRITE | PROT_EXEC, Flags, -1, 0);
    if (R == MAP_FAILED && errno == EEXIST) {
      return true; // overlapping test regions
    }
    if (R != Want) {
      fprintf(stderr, "map 0x%llx+0x%llx failed: %s\n", (unsigned long long)Start, (unsigned long long)(End - Start), strerror(errno));
      return false;
    }
    Mapped.emplace_back(Start, End - Start);
    return true;
  }
};

// ------------------------------------------------------- system ops (--sysops)

const GuestMem* g_Mem;
FEXCore::Core::InternalThreadState* g_Thread;
std::thread g_Irq;
std::map<uint16_t, uint32_t> g_Ports;
std::map<uint32_t, uint64_t> g_Msrs;
uint32_t g_Cr[8] = {0x8001003Bu};

uint64_t HostCounter() {
  uint64_t V;
  asm volatile("mrs %0, cntvct_el0" : "=r"(V));
  return V;
}

// RDTSC at the Xbox's 733.333 MHz: CNTVCT * Ratio / 2^32.
uint64_t TscRatioQ32() {
  uint64_t Freq;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(Freq));
  return static_cast<uint64_t>((static_cast<unsigned __int128>(733'333'333) << 32) / Freq);
}

// Test semantics for tests/native/sysops (see the comments there). Operands
// come from the instruction bytes at State.rip, as the real core will do.
bool HandleSystemOp(FEXCore::Core::CpuStateFrame* Frame) {
  using enum FEXCore::Core::SystemOpKind;
  using namespace FEXCore::X86State;
  const auto Kind = static_cast<FEXCore::Core::SystemOpKind>(Frame->SystemOpInfo & 0xff);
  const unsigned Size = (Frame->SystemOpInfo >> 8) & 0xff;
  Frame->SystemOpInfo = 0;

  auto& S = Frame->State;
  const uint8_t* I = g_Mem->Host(S.rip);
  unsigned P = 0;
  bool Op16 = false;
  while (P < Size && (I[P] == 0x66 || I[P] == 0xF2 || I[P] == 0xF3 || I[P] == 0x2E || I[P] == 0x3E)) {
    Op16 |= I[P] == 0x66;
    ++P;
  }
  const uint8_t Opc = I[P];
  auto Set32 = [&](int Reg, uint32_t V) { S.gregs[Reg] = V; };
  auto Merge = [&](int Reg, uint32_t V, unsigned Bytes) {
    const uint32_t Mask = Bytes == 4 ? 0xFFFF'FFFFu : (1u << (Bytes * 8)) - 1;
    S.gregs[Reg] = (static_cast<uint32_t>(S.gregs[Reg]) & ~Mask) | (V & Mask);
  };

  switch (Kind) {
  case In:
  case Out: {
    const bool Imm = Opc == 0xE4 || Opc == 0xE5 || Opc == 0xE6 || Opc == 0xE7;
    const bool Byte = Opc == 0xE4 || Opc == 0xE6 || Opc == 0xEC || Opc == 0xEE;
    const unsigned Bytes = Byte ? 1 : Op16 ? 2 : 4;
    const uint16_t Port = Imm ? I[P + 1] : static_cast<uint16_t>(S.gregs[REG_RDX]);
    if (Kind == In) {
      Merge(REG_RAX, g_Ports[Port], Bytes);
    } else if (Port == 0xF0 && !g_Irq.joinable()) {
      // Arms one interrupt request, 20 ms later, from another host thread.
      g_Irq = std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        __atomic_store_n(&g_Thread->CurrentFrame->InterruptPending, 1u, __ATOMIC_RELEASE);
      });
    } else {
      const uint32_t Mask = Bytes == 4 ? 0xFFFF'FFFFu : (1u << (Bytes * 8)) - 1;
      g_Ports[Port] = static_cast<uint32_t>(S.gregs[REG_RAX]) & Mask;
    }
    break;
  }
  case Cli: S.flags[RFLAG_IF_LOC] = 0; break;
  case Sti: S.flags[RFLAG_IF_LOC] = 1; break;
  case Int: {
    const uint8_t Vector = I[P + 1];
    if (Vector == 0x42) {
      const uint64_t Guest = (static_cast<uint64_t>(static_cast<uint32_t>(S.gregs[REG_RDI])) << 32) | static_cast<uint32_t>(S.gregs[REG_RSI]);
      const uint64_t Now = static_cast<uint64_t>((static_cast<unsigned __int128>(HostCounter()) * TscRatioQ32()) >> 32);
      Set32(REG_RAX, Guest <= Now && Now - Guest < 733'333 ? 1 : 0);
    } else {
      Set32(REG_RAX, 0x1000 + Vector);
    }
    break;
  }
  case Into:
  case Int1:
  case Int3:
  case Ud2: Set32(REG_RAX, 0x2000 + static_cast<uint32_t>(Kind)); break;
  case Cpuid: {
    const uint32_t Leaf = static_cast<uint32_t>(S.gregs[REG_RAX]);
    uint32_t A = 0, B = 0, C = 0, D = 0;
    if (Leaf == 0) {
      A = 2;
      B = 0x756E6547; // Genu
      D = 0x49656E69; // ineI
      C = 0x6C65746E; // ntel
    } else if (Leaf == 1) {
      A = 0x068A;     // family 6, model 8, stepping 10
      D = 0x0383F9FF; // FPU..CMOV, PAT, PSE36, MMX, FXSR, SSE
    } else if (Leaf == 2) {
      A = 0x03020101;
      D = 0x0C040843;
    }
    Set32(REG_RAX, A);
    Set32(REG_RBX, B);
    Set32(REG_RCX, C);
    Set32(REG_RDX, D);
    break;
  }
  case Rdmsr: {
    const uint64_t V = g_Msrs[static_cast<uint32_t>(S.gregs[REG_RCX])];
    Set32(REG_RAX, static_cast<uint32_t>(V));
    Set32(REG_RDX, static_cast<uint32_t>(V >> 32));
    break;
  }
  case Wrmsr:
    g_Msrs[static_cast<uint32_t>(S.gregs[REG_RCX])] =
      (static_cast<uint64_t>(static_cast<uint32_t>(S.gregs[REG_RDX])) << 32) | static_cast<uint32_t>(S.gregs[REG_RAX]);
    break;
  case Other:
    if (Opc == 0x0F && (I[P + 1] == 0x20 || I[P + 1] == 0x22)) {
      // MOV r32, CRn / MOV CRn, r32: ModRM reg = CR number, rm = GPR (x86 and FEX order agree).
      const uint8_t ModRM = I[P + 2];
      const unsigned Cr = (ModRM >> 3) & 7, Gpr = ModRM & 7;
      if (I[P + 1] == 0x20) {
        Set32(Gpr, g_Cr[Cr]);
      } else {
        g_Cr[Cr] = static_cast<uint32_t>(S.gregs[Gpr]);
      }
    } else {
      fprintf(stderr, "unhandled system op at 0x%llx (opcode %02x %02x)\n", (unsigned long long)S.rip, Opc, I[P + 1]);
      return false;
    }
    break;
  case Interrupt:
    // Requested 20 ms after the test wrote port 0xF0: take it, mark it, and
    // resume the block it stopped (size 0).
    __atomic_store_n(&Frame->InterruptPending, 0u, __ATOMIC_RELAXED);
    *reinterpret_cast<uint32_t*>(g_Mem->Host(0xC0000800u)) = 0xC0DE;
    break;
  default: break; // HLT, WBINVD, INVD, INVLPG, ...: resume after the instruction
  }

  S.rip += Size;
  return true;
}

uint64_t Fnv(const void* Data, size_t Len, uint64_t H = 0xcbf29ce484222325ull) {
  auto* P = static_cast<const uint8_t*>(Data);
  for (size_t i = 0; i < Len; ++i) {
    H = (H ^ P[i]) * 0x100000001b3ull;
  }
  return H;
}

const char* RegName(unsigned Index, size_t J, char* Buf, size_t N) {
  static const char* Gpr[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP",
                              "R8",  "R9",  "R10", "R11", "R12", "R13", "R14", "R15"};
  if (Index == 0) {
    snprintf(Buf, N, "RIP");
  } else if (Index < 17) {
    snprintf(Buf, N, "%s", Gpr[Index - 1]);
  } else if (Index < 33) {
    snprintf(Buf, N, "XMM%u[%zu]", Index - 17, J);
  } else if (Index == 33) {
    snprintf(Buf, N, "GS");
  } else if (Index == 34) {
    snprintf(Buf, N, "FS");
  } else {
    snprintf(Buf, N, "MM%u[%zu]", Index - 35, J);
  }
  return Buf;
}

// Mirrors FEX's HarnessHelpers offset table.
const uint64_t* RegPtr(const FEXCore::Core::CPUState& S, unsigned Index, bool AVX) {
  using namespace FEXCore::X86State;
  static const int GprMap[] = {REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RSI, REG_RDI, REG_RBP, REG_RSP,
                               REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
  if (Index == 0) {
    return &S.rip;
  } else if (Index < 17) {
    return &S.gregs[GprMap[Index - 1]];
  } else if (Index < 33) {
    return AVX ? &S.xmm.avx.data[Index - 17][0] : &S.xmm.sse.data[Index - 17][0];
  } else if (Index == 33) {
    return &S.gs_cached;
  } else if (Index == 34) {
    return &S.fs_cached;
  }
  return &S.mm[Index - 35][0];
}

// With --diff, each run prints "D <part> <hash>" lines describing its final
// guest-visible state, for the parent to compare across the two modes.
void PrintDigest(FEXCore::Context::Context* CTX, FEXCore::Core::InternalThreadState* Thread, const FEXCore::Core::CPUState& S,
                 const GuestMem& M) {
  for (int i = 0; i < 8; ++i) {
    printf("D gpr%d %08x\n", i, static_cast<uint32_t>(S.gregs[i]));
  }
  printf("D eip %08x\n", static_cast<uint32_t>(S.rip));
  printf("D eflags %08x\n", CTX->ReconstructCompactedEFLAGS(Thread, false, nullptr, 0));
  printf("D xmm %016llx\n", (unsigned long long)Fnv(S.xmm.sse.data, sizeof(uint64_t) * 2 * 8));
  printf("D mm %016llx\n", (unsigned long long)Fnv(S.mm, sizeof(S.mm)));
  printf("D mxcsr %08x fcw %04x\n", S.mxcsr, S.FCW);
  for (auto& [Start, Len] : M.Mapped) {
    printf("D mem%08llx %016llx\n", (unsigned long long)Start, (unsigned long long)Fnv(M.Host(Start), Len));
  }
}

int RunOne(const Options& Opt, const std::string& Bin, const std::string& Cfg) {
  InstallFaultHandlers();
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);

  TestConfig C;
  if (!ParseConfig(Cfg, C)) {
    fprintf(stderr, "cannot read %s\n", Cfg.c_str());
    return 6;
  }
  if (C.Is64) {
    return 3; // 32-bit suite only
  }

  std::ifstream BinIn(Bin, std::ios::binary);
  std::vector<char> Code((std::istreambuf_iterator<char>(BinIn)), std::istreambuf_iterator<char>());
  if (Code.empty()) {
    fprintf(stderr, "cannot read %s\n", Bin.c_str());
    return 6;
  }

  FEXCore::Config::Initialize();
  FEXCore::Config::ReloadMetaLayer();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MAXINST, std::to_string(Opt.MaxInst));
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, Opt.Multiblock ? "1" : "0");
  for (auto& [K, V] : C.Env) {
    bool Found = false;
#define OPT_BASE(type, group, enum, json, default)                     \
  if (!Found && K == "FEX_" #enum) {                                   \
    FEXCore::Config::Set(FEXCore::Config::CONFIG_##enum, V);           \
    Found = true;                                                      \
  }
#include <FEXCore/Config/ConfigValues.inl>
    if (!Found) {
      fprintf(stderr, "unknown test env option %s\n", K.c_str());
    }
  }

  auto HostFeatures = FEX::FetchHostFeatures();
  bool AVX = HostFeatures.SupportsAVX;
  bool Unsupported = ((C.Features & FEATURE_AVX) && !HostFeatures.SupportsAVX) || ((C.Features & FEATURE_RAND) && !HostFeatures.SupportsRAND) ||
                     ((C.Features & FEATURE_SHA) && !HostFeatures.SupportsSHA) ||
                     ((C.Features & FEATURE_CLZERO) && !HostFeatures.SupportsCLZERO) ||
                     ((C.Features & FEATURE_AES256) && !HostFeatures.SupportsAES256) ||
                     ((C.Features & FEATURE_AFP) && !HostFeatures.SupportsAFP) || (C.Features & FEATURE_LINUX);
  if (Unsupported) {
    return 3;
  }

  GuestMem M;
  M.Identity = Opt.Identity;
  if (!M.Reserve(Opt.Base)) {
    return 6;
  }
  bool Ok = M.Map(0xE000'0000, PAGE * 10) && M.Map(0xE800'0000 - PAGE, PAGE * 2) && M.Map(CODE_START, Code.size()) &&
            M.Map(STACK_OFFSET, PAGE);
  for (auto& [A, L] : C.Regions) {
    Ok = Ok && M.Map(A, L);
  }
  if (!Ok) {
    return 6;
  }
  memcpy(M.Host(CODE_START), Code.data(), Code.size());
  for (auto& D : C.Mem) {
    memcpy(M.Host(D.Addr), D.Bytes.data(), D.Bytes.size());
  }

  auto CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  HarnessSignals Signals;
  HarnessSyscalls Syscalls;
  CTX->SetSignalDelegator(&Signals);
  CTX->SetSyscallHandler(&Syscalls);
  CTX->EnableExitOnHLT();
  if (!M.Identity) {
    g_MemBase = reinterpret_cast<uint64_t>(M.Base);
    CTX->SetGuestMemoryBase(g_MemBase);
  }
  g_Mem = &M;
  if (Opt.SysOps) {
    CTX->EnableSystemOps();
    CTX->SetGuestCycleCounterRatio(TscRatioQ32());
  }
  if (!CTX->InitCore()) {
    fprintf(stderr, "InitCore failed\n");
    return 6;
  }

  auto* Thread = CTX->CreateThread();
  auto& State = Thread->CurrentFrame->State;

  // Call-ret stack with a guard page on each side, as FEX's front ends do.
  constexpr size_t CRS = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  auto* CrsAlloc = static_cast<uint8_t*>(mmap(nullptr, CRS + 2 * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Thread->CallRetStackBase = CrsAlloc + PAGE;
  mprotect(Thread->CallRetStackBase, CRS, PROT_READ | PROT_WRITE);
  State.callret_sp = reinterpret_cast<uint64_t>(Thread->CallRetStackBase) + CRS / 4;

  // Every selector is a flat 32-bit segment; LDT mirrors the GDT.
  static FEXCore::Core::CPUState::gdt_segment Gdt[32] {};
  for (auto& G : Gdt) {
    FEXCore::Core::CPUState::SetGDTBase(&G, 0);
    FEXCore::Core::CPUState::SetGDTLimit(&G, 0xF'FFFFu);
    G.L = 0;
    G.D = 1;
    G.P = 1;
    G.S = 1;
    G.G = 1;
  }
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = Gdt;
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = Gdt;
  State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  State.cs_cached = 0;
  State.rip = CODE_START;

  g_Thread = Thread;
  CTX->ExecuteThread(Thread);
  if (g_Irq.joinable()) {
    g_Irq.join();
  }

  FEXCore::Core::CPUState Out;
  memcpy(&Out, &State, sizeof(Out));
  if (AVX) {
    __uint128_t Lo[FEXCore::Core::CPUState::NUM_XMMS], Hi[FEXCore::Core::CPUState::NUM_XMMS];
    CTX->ReconstructXMMRegisters(Thread, Lo, Hi);
    for (size_t i = 0; i < FEXCore::Core::CPUState::NUM_XMMS; ++i) {
      memcpy(&Out.xmm.avx.data[i][0], &Lo[i], sizeof(__uint128_t));
      memcpy(&Out.xmm.avx.data[i][2], &Hi[i], sizeof(__uint128_t));
    }
  } else {
    CTX->ReconstructXMMRegisters(Thread, reinterpret_cast<__uint128_t*>(Out.xmm.sse.data), nullptr);
  }

  bool Match = true;
  for (auto& R : C.Regs) {
    const uint64_t* P = RegPtr(Out, R.Index, AVX);
    for (size_t j = 0; j < R.Values.size(); ++j) {
      if (P[j] != R.Values[j]) {
        Match = false;
        if (Opt.Verbose) {
          char N[32];
          fprintf(stderr, "  %s: 0x%016llx != 0x%016llx (expected)\n", RegName(R.Index, j, N, sizeof(N)), (unsigned long long)P[j],
                  (unsigned long long)R.Values[j]);
        }
      }
    }
  }
  if (Opt.Diff) {
    FEXCore::Core::CPUState Sse;
    memcpy(&Sse, &Out, sizeof(Sse));
    if (AVX) {
      for (size_t i = 0; i < FEXCore::Core::CPUState::NUM_XMMS; ++i) {
        memcpy(&Sse.xmm.sse.data[i][0], &Out.xmm.avx.data[i][0], 16);
      }
    }
    PrintDigest(CTX.get(), Thread, Sse, M);
  }
  fflush(stdout);
  fflush(stderr);
  return Match ? 0 : 1;
}

const char* VerdictOf(int Status) {
  if (WIFSIGNALED(Status)) {
    return WTERMSIG(Status) == SIGALRM ? "TIMEOUT" : "FAULT";
  }
  switch (WEXITSTATUS(Status)) {
  case 0: return "PASS";
  case 1: return "FAIL";
  case 2: return "FAULT";
  case 3: return "SKIP";
  default: return "ERROR";
  }
}

// Runs one mode in a child; returns its verdict and (with --diff) its digest lines.
const char* RunChild(const Options& Opt, const std::string& Base, std::map<std::string, std::string>* Digest) {
  int Pipe[2] = {-1, -1};
  if (Digest && pipe(Pipe) != 0) {
    return "ERROR";
  }
  fflush(stdout);
  fflush(stderr);
  pid_t Pid = fork();
  if (Pid == 0) {
    if (Digest) {
      close(Pipe[0]);
      dup2(Pipe[1], 1);
    }
    alarm(Opt.TimeoutSec);
    _exit(RunOne(Opt, Base + ".bin", Base + ".cfg"));
  }
  if (Digest) {
    close(Pipe[1]);
    std::string Text;
    char Buf[4096];
    ssize_t N;
    while ((N = read(Pipe[0], Buf, sizeof(Buf))) > 0) {
      Text.append(Buf, N);
    }
    close(Pipe[0]);
    std::istringstream S(Text);
    std::string Line;
    while (std::getline(S, Line)) {
      std::istringstream L(Line);
      std::string Tag, Part;
      if (L >> Tag >> Part && Tag == "D") {
        std::string Rest;
        std::getline(L, Rest);
        (*Digest)[Part] = Rest;
      }
    }
  }
  int Status = 0;
  waitpid(Pid, &Status, 0);
  return VerdictOf(Status);
}

} // namespace

int main(int argc, char** argv) {
  Options Opt;
  std::vector<std::string> Pos;
  for (int i = 1; i < argc; ++i) {
    std::string A = argv[i];
    auto Next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (A == "--base") {
      std::string V = Next();
      Opt.Identity = V == "identity";
      Opt.Base = (Opt.Identity || V == "auto") ? 0 : std::stoull(V, nullptr, 16);
    } else if (A == "--diff") {
      Opt.Diff = true;
    } else if (A == "--maxinst") {
      Opt.MaxInst = std::stoi(Next());
    } else if (A == "--multiblock") {
      Opt.Multiblock = std::stoi(Next());
    } else if (A == "--timeout") {
      Opt.TimeoutSec = std::stoi(Next());
    } else if (A == "--verbose") {
      Opt.Verbose = true;
    } else if (A == "--sysops") {
      Opt.SysOps = true;
    } else {
      Pos.push_back(A);
    }
  }
  if (Pos.size() != 2) {
    fprintf(stderr, "usage: fexrun [options] <test-dir> <manifest>\n");
    return 2;
  }

  std::ifstream Manifest(Pos[1]);
  std::string Line;
  std::map<std::string, int> Counts;
  setvbuf(stdout, nullptr, _IOLBF, 0);
  while (std::getline(Manifest, Line)) {
    std::istringstream S(Line);
    std::string Name, Tags, T;
    if (!(S >> Name)) {
      continue;
    }
    while (S >> T) {
      Tags += " " + T;
    }
    std::string Base = Pos[0] + "/" + Name;

    if (!Opt.Diff) {
      const char* Verdict = RunChild(Opt, Base, nullptr);
      Counts[Verdict]++;
      printf("%s %s%s\n", Verdict, Name.c_str(), Tags.c_str());
      continue;
    }

    Options IdOpt = Opt, MbOpt = Opt;
    IdOpt.Identity = true;
    MbOpt.Identity = false;
    std::map<std::string, std::string> IdDigest, MbDigest;
    const char* IdVerdict = RunChild(IdOpt, Base, &IdDigest);
    const char* MbVerdict = RunChild(MbOpt, Base, &MbDigest);
    std::string Parts;
    for (auto& [K, V] : IdDigest) {
      auto It = MbDigest.find(K);
      if (It == MbDigest.end() || It->second != V) {
        Parts += " " + K;
      }
    }
    for (auto& [K, V] : MbDigest) {
      if (!IdDigest.count(K)) {
        Parts += " " + K;
      }
    }
    bool Same = strcmp(IdVerdict, MbVerdict) == 0 && Parts.empty();
    const char* Verdict = Same ? "SAME" : "DIFF";
    Counts[Verdict]++;
    Counts[std::string("mb_") + MbVerdict]++;
    printf("%s %s %s/%s%s%s\n", Verdict, Name.c_str(), IdVerdict, MbVerdict, Parts.c_str(), Tags.c_str());
  }
  printf("summary");
  for (auto& [K, V] : Counts) {
    printf(" %s=%d", K.c_str(), V);
  }
  printf(" base=%s maxinst=%d multiblock=%d\n", Opt.Diff ? "diff" : Opt.Identity ? "identity" : "membase", Opt.MaxInst, Opt.Multiblock);
  return 0;
}
