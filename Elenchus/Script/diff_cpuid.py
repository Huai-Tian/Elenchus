#!/usr/bin/env python3
# =====================================================================
# diff_cpuid.py — 比较两份 cpuid_dump CSV（B1 向量判读）
#
# 用法: python3 diff_cpuid.py bare.csv target.csv
# 输出: 逐行差异（leaf,subleaf + 两侧寄存器值）+ 差异计数
# 判读: 裸机 vs 框架驻留 期望仅 hypervisor 位/签名 leaf/maxleaf 变化；
#       大面积差异 = 身份面泄漏
# =====================================================================
import sys

def load(path):
    d = {}
    with open(path) as f:
        for line in f:
            parts = [p.strip() for p in line.strip().split(",")]
            if len(parts) != 6 or parts[0] == "leaf":
                continue
            try:
                d[(int(parts[0], 0), int(parts[1], 0))] = parts[2:6]
            except ValueError:
                continue
    return d

def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = load(sys.argv[1]), load(sys.argv[2])
    diffs = 0
    only_a = [k for k in a if k not in b]
    only_b = [k for k in b if k not in a]
    for k in sorted(set(a) & set(b)):
        if a[k] != b[k]:
            diffs += 1
            print(f"DIFF leaf={k[0]:#x} sub={k[1]}: "
                  f"bare eax={a[k][0]} ebx={a[k][1]} ecx={a[k][2]} edx={a[k][3]} | "
                  f"target eax={b[k][0]} ebx={b[k][1]} ecx={b[k][2]} edx={b[k][3]}")
    for k in sorted(only_a)[:10]:
        print(f"ONLY-BARE leaf={k[0]:#x} sub={k[1]}")
    for k in sorted(only_b)[:10]:
        print(f"ONLY-TARGET leaf={k[0]:#x} sub={k[1]}")
    print(f"\n总计: {diffs} 处寄存器差异, {len(only_a)} leaf 仅裸机, {len(only_b)} leaf 仅目标")
    return 0

if __name__ == "__main__":
    sys.exit(main())
