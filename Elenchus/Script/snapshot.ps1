# =====================================================================
# snapshot.ps1 — 系统基线快照（S0 阶段，装框架前必采）
#
# 用法（管理员 PowerShell）:
#   powershell -ExecutionPolicy Bypass -File .\snapshot.ps1 [-Tag bare]
# 产物: snapshot_<host>_<tag>_<时间戳>\ 目录
# =====================================================================
param([string]$Tag = "bare")

$ErrorActionPreference = "Continue"
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$dir = Join-Path (Get-Location) "snapshot_$($env:COMPUTERNAME)_${Tag}_$stamp"
New-Item -ItemType Directory -Path $dir -Force | Out-Null

Write-Host "==> 快照目录: $dir"

# CPU 与平台身份
Get-CimInstance Win32_Processor | Format-List * | Out-File "$dir\cpu.txt" -Encoding utf8
Get-CimInstance Win32_ComputerSystem | Format-List * | Out-File "$dir\cs.txt" -Encoding utf8

# 虚拟化可见状态（B4 行的裸机基线）
"=== Win32_ComputerSystem.HypervisorPresent ===" | Out-File "$dir\virt.txt" -Encoding utf8
(Get-CimInstance Win32_ComputerSystem).HypervisorPresent | Out-File "$dir\virt.txt" -Append -Encoding utf8
"=== systeminfo | findstr 虚拟化相关 ===" | Out-File "$dir\virt.txt" -Append -Encoding utf8
systeminfo | Select-String -Pattern "Hyper-V|虚拟化|Virtualization|固件|Firmware" |
    Out-File "$dir\virt.txt" -Append -Encoding utf8

# 启动配置与驱动清单（框架是否在位/残留）
bcdedit /enum | Out-File "$dir\bcdedit.txt" -Encoding utf8
driverquery /v | Out-File "$dir\drivers.txt" -Encoding utf8

# 事件日志基线（后续 DWM 崩溃族对照用）
"=== System 最近 200 条 ===" | Out-File "$dir\events.txt" -Encoding utf8
Get-WinEvent -LogName System -MaxEvents 200 -ErrorAction SilentlyContinue |
    Format-Table TimeCreated, Id, LevelDisplayName, ProviderName -AutoSize |
    Out-File "$dir\events.txt" -Append -Encoding utf8
"=== Application 最近 200 条 ===" | Out-File "$dir\events_app.txt" -Encoding utf8
Get-WinEvent -LogName Application -MaxEvents 200 -ErrorAction SilentlyContinue |
    Format-Table TimeCreated, Id, LevelDisplayName, ProviderName -AutoSize |
    Out-File "$dir\events_app.txt" -Append -Encoding utf8

# 进程基线
Get-Process | Sort-Object CPU -Descending |
    Format-Table Name, Id, CPU, WS -AutoSize | Out-File "$dir\procs.txt" -Encoding utf8

Write-Host "==> 完成。归档整个目录（对照组基因，不可补采）"
