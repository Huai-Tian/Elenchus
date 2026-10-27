# Elenchus 执行协议（v3 — 统一驱动版）

论文实验全套采集器。所有产物 CSV/控制台输出直接回填论文表格。

**结构**：`Elenchus/Driver`（统一驱动 Driver.sys）/ `Elenchus/Executable`（统一程序 Executable.exe，八个子命令）/ `Elenchus/Script`（脚本与协议）。

**形态标注纪律（v3.2）**：隐蔽框架（GnptHooks/GeptHooks）不置 CPUID hypervisor 位，
全自动模式的形态判定靠 CPUID 成本快测（中位 >600 cycles = 有拦截）。若框架不拦 CPUID
则快测失灵，须显式指定：`Executable.exe auto fw`。误标形态的轮次数据有效、标签作废，
文件名 `-replace '_bare_','_fw_'` 补救即可。

**驱动一次装载，全部探测可用**：

```powershell
sc create elenchus type= kernel start= demand binPath= "<path>\Driver.sys"
sc start elenchus
# ... 所有实验 ...
sc stop elenchus
sc delete elenchus
```

## 构建

- **驱动**：VS2022 + WDK 打开 `Elenchus\Driver\Driver.sln`，Release/x64 构建
  （源：main.c 分发路由 + mod_hptmap/mod_rxdiff/mod_kbench/mod_idface + idface.asm/kbench.asm）
- **R3**：VS 打开 `Elenchus\Executable\Executable.sln` 生成（产出单个 Executable.exe，八个子命令：cpuid/dwell/clk/tlb/rx/id/dbg/bench）
- **脚本**：Python 3 纯标准库；snapshot.ps1 需管理员 PowerShell

## v3 变更摘要（时事核查驱动）

- 修 bug：CPUID 采集补越界/管理程序 leaf 区（Ophion 2026 回声探针）
- 新增 A4：exit 诱发 TLB 扰动（tlbperturb）
- 新增形态：dwell `-i rdtscaux`（TSC_AUX 一致性）
- idprobe 默认 MSR 表含 VMX 能力区 + VM_CR(0xC0010114, APM §15.30.1)
- C1 分析含近重复页关联（EAC 实战向量）
- 统一驱动架构（v3.1）：四模块共用 \Device\Elenchus

## ⚠ 平台约束（先读）

**一台机器只能跑同平台的虚拟机**：
- Intel 机 → VMware guest 虚拟 Intel CPU → GeptHooks 的正对照
- AMD 机 → VMware guest 嵌套 AMD → GnptHooks 的正对照
- VMFUNC 基线（HyperDbg）仅 Intel；AMD 无此原语 = 架构缺席（论文数据点）
- Zen3 PMC 边界 = 文档化（5800H 已退货）
- 部署前提：Win11 22H2+ 默认 VBS/HVCI 须关闭（root 姿态要求，论文 Limitations）

## 阶段协议

### 阶段 0：裸机基线 ⚠️ 装框架之前必采，不可补采

```powershell
cd Executable
Executable.exe cpuid -o bare_cpuid.csv
Executable.exe dwell -n 1000000 -c 0 -i cpuid -o bare_dwell_c0.csv
Executable.exe dwell -n 1000000 -c 0 -i sgdt  -o bare_sgdt_c0.csv
Executable.exe dwell -n 1000000 -c 0 -i rdtscaux                 # PASS/FAIL
Executable.exe tlb -t none  -c 0                            # 噪声底
Executable.exe tlb -t cpuid -c 0                            # 裸机形态
# 多核抽样: -c 1/2/... 至少 4 核
# 管理员 PowerShell: cd Elenchus\Script; .\snapshot.ps1 -Tag bare
```

### 阶段 1：A2 签名实验（含 A3 单调性 + 真值交叉验证）

```powershell
# 正对照: 本机 VMware guest 内跑同一探测器
Executable.exe clk -h -n 1000000 -c 0 -o vm_hpet.csv
Executable.exe clk -p -n 1000000 -c 0 -o vm_pmtmr.csv    # Zen5 无 PM_TMR 时自动记 N/A 跳过(合法形态)
Executable.exe clk -q -n 1000000 -c 0 -o vm_qpc.csv

# 目标形态: 装框架(Debug 构建) + elenchus 驱动
sc start elenchus
sc start GeptHooks        # Intel 机; AMD 机 = gnpt
Executable.exe clk -h -n 1000000 -c 0 -o fw_hpet.csv
Executable.exe clk -p -n 1000000 -c 0 -o fw_pmtmr.csv
Executable.exe clk -q -n 1000000 -c 0 -o fw_qpc.csv
Executable.exe dwell -n 1000000 -c 0 -i cpuid -o fw_dwell_c0.csv
Executable.exe dwell -n 1000000 -c 0 -i sgdt  -o fw_sgdt_c0.csv
Executable.exe dwell -n 1000000 -c 0 -i rdtscaux
Executable.exe tlb -t cpuid -c 0
Executable.exe tlb -t clock -c 0        # 时钟封堵路径 exit 的扰动(重点)

# ⚠ 真值交叉验证(必做): 卸载框架后查会话日志 'y' 事件数>0
#   Gept: gept_log.txt;  Gnpt: gnpt_log.txt
#   'y'=0 而 A2 PASS → 采样未触发封堵路径, 该轮作废重跑

python3 Elenchus\Script\analyze.py bare_hpet.csv vm_hpet.csv fw_hpet.csv
```

### 阶段 2：C2 读-执行差分

```powershell
sc start elenchus     # 若已装载则跳过
Executable.exe rx -c 0      # 逐核 0..N-1, 每核 3 轮
# 形态矩阵:
#   Gnpt 普通模式(demo 冷靶) → 预期驻留核 FAIL(预注册输格)
#   NtClose 挂 TRANSPARENT → 预期全核 PASS
#   Gept HideRead=1 → PASS; HideRead=0 → 视核形态
Executable.exe rx -c 0 -e 100
```

### 阶段 3：性能基线（Intel 5 形态 / AMD 4 形态）

```powershell
sc start elenchus
Executable.exe bench kaddr                      # KbTarget VA
# 共同形态:
#   裸机:   Executable.exe bench k -n 1000000 ×3
#   inline: Executable.exe bench k -n 1000000 --inline ×3 (自动装/卸)
#   框架×3: demo main.c 挂 KbTarget VA(shadow/TRANSPARENT/NX-FENCE 各一轮)
#           → sc start 框架 → Executable.exe bench k → sc stop
#   端到端: demo 挂 NtClose → Executable.exe bench hits/batch/flood/ctxsw
# 仅 Intel: VMFUNC 式(HyperDbg EPT hook 同靶) → Executable.exe bench k
```

### 阶段 4：B 族 + C3 全程窗口 + 长浸泡

```powershell
sc start elenchus
Executable.exe id -o bare      # 三形态: bare / vm / fw
python3 Elenchus\Script\diff_cpuid.py bare_cpuid.csv fw_cpuid.csv
Executable.exe dbg -n 100       # 三形态各跑
# B4: 全核驻留会话中截图任务管理器"虚拟化: 已禁用"

# C1 物理扫描: 单次/平台; 分析含 SLAT 签名 + 近重复页关联
#   (采集器未随套件, 附录级, 需要再写)

# C3 全程窗口(两终端):
#   终端1: Executable.exe dwell -w -i cpuid -o win_full.csv
#   终端2: sc start GeptHooks → 60s → sc stop GeptHooks
#   终端1: Ctrl-C (install/uninstall 两窗口毛刺)
```

### D1 长浸泡（分块生存协议——两台机器各 ≥12h 累计）

```powershell
# 每段 = 独立 install→浸泡→关机 会话; 建议:
#   Day A: 8h 段(睡眠/唤醒 ×1 + 高负载段 + 空闲段)
#   Day B: 4h 段(全新冷启 = 覆盖重启边界)
# 剂量论证: PG 分钟级随机间隔 → 12h ≈ 每目标 50-150 独立检查周期
# 慢性泄漏不靠墙钟: 卸载时 'H' 隐藏页计数 + C1 池岛 diff 覆盖
# AMD: 一次干净 12h 冷窗会话优先(已知 6h demo 记录的延长)
# 段末分类(事件日志):
#   Event 1001 BugCheck 0x109        → PG 杀死(死亡事件)
#   Event 41 无 BugCheck             → 断电时存活(时长计入)
#   无 0x109 无干净关机               → 右删失
# 记录: 每段 (日期, 形态, 时长, 上下文, 'H'计数, 结局)
# D2 PMC/IBS: 仅 Zen5; Zen3 = 文档化
```

## 数据回传

命名 `<向量>_<形态>_<机器>_<轮次>.csv`（如 `a2_hpet_fw_amd_r1.csv`）+
控制台摘要 + 框架日志 'y'/'a'/'H' 计数。

## 纪律

- 每格 ≥3 轮（C1 单次）；裸机基线先于一切框架安装
- A2 的 PASS 必须附 'y'>0 证据
- AMD 长浸泡守冷启窗口纪律；死亡后不立即重启复测
- 正对照 VMware 每台机器跑自己的（同平台约束）
- 异常输出 = 真实数据（如 STGI 在现代 AMD 裸机合法执行非 #UD）——如实记录
