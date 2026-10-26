# Elenchus

**ἔλεγχος** — Socratic cross-examination.

> No single question is decisive. The subject is caught when its answers
> across channels fail to cohere.

Elenchus is an adversarial measurement suite for the stealth of
SLAT-based hypervisors on Windows (Intel VT-x/EPT and AMD SVM/NPT),
companion artifact to the paper on maximally hardened, open
kernel-function hook frameworks. It interrogates a machine through
every sense it has — time, identity, memory, the debug subsystem —
and flags the places where the answers disagree with each other or
with the bare-metal baseline.

**Detection-only by design.** Every program here measures; nothing
conceals, injects, or hides. The suite contains no stealth code of
any kind. Its results are immediately deployable by defenders.

## Layout

```
Elenchus/
├── Driver/          one unified WDM driver (Driver.sys), four probe modules
│   ├── main.c             IRP dispatch routing
│   ├── mod_hptmap.c       A2: MMIO clock-counter mapping (HPET / PM_TMR)
│   ├── mod_rxdiff.c       C2: read-execute differential vs on-disk authority
│   ├── mod_kbench.c       PERF: per-hit kernel-side benchmark + inline baseline
│   ├── mod_idface.c       B2: MSR reads + VMX/SVM instruction faces
│   ├── idface.asm / kbench.asm   instruction stubs / target functions
│   └── Driver.sln         VS2022 + WDK solution
├── Executable/      ONE user-mode binary (Executable.exe), eight subcommands
│   ├── elenchus.c          dispatcher (cpuid/dwell/clk/tlb/rx/id/dbg/bench)
│   └── cpuid_dump.c dwell.c clkprobe.c tlbperturb.c
│       rxprobe.c idprobe.c dbgface.c bench.c
└── Script/          protocol + analysis
    ├── PROTOCOL.md       staged measurement protocol (v3) — read this first
    ├── snapshot.ps1      bare-metal system snapshot
    ├── diff_cpuid.py     CPUID diff adjudication
    └── analyze.py        A2 residual/monotonicity analysis
```

Install the driver once, all probes become available:

```powershell
sc create elenchus type= kernel start= demand binPath= "<path>\Driver.sys"
sc start elenchus
```

See [`Elenchus/Script/PROTOCOL.md`](Elenchus/Script/PROTOCOL.md) for the
full staged protocol (baseline → positive control → target forms, with
pre-registered expected losses).

## Requirements

- Windows 10 x64, admin shell; test-signing enabled
  (`bcdedit /set testsigning on`)
- Driver: VS2022 + WDK (open `Elenchus\Driver\Driver.sln`, Release/x64)
- Executable: VS2022 (open `Elenchus\Executable\Executable.sln`) — one binary, eight subcommands
- Python 3 (standard library only)

Run only on machines you own or are authorized to test.

## License

GPL-3.0-only ([LICENSE](LICENSE)). Same license as the frameworks
under evaluation, one ecosystem.

## Citation

```bibtex
@misc{elenchus,
  title  = {Elenchus: A Cross-Examination Suite for SLAT-Hypervisor Stealth},
  author = {Huai Tian},
  year   = {2026},
  note   = {Companion artifact; GPL-3.0},
  url    = {https://github.com/Huai-Tian/Elenchus}
}
```
