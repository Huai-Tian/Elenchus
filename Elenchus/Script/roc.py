#!/usr/bin/env python3
# =====================================================================
# roc.py — S3 归因否认实验: 离线 ROC/AUC 分析
#
# 输入: detect 子命令的 obs.csv(每机器×每形态一行特征)
#   tag,form,hvbit,hvmaxleaf,hvvendor,cpuid_med,cpuid_p90,cpuid_p99,
#   cpuid_over2x,rdtsc_med,ratio_med,sgdt_med,sgdt_p99,xc_med,xc_p99,
#   resid_ns_med,resid_ns_p99,tsc_mhz,tsc_drift_ppm
#
# 用法:
#   python roc.py obs.csv                       # 全特征 AUC 报告
#   python roc.py obs.csv --pos fw              # 正类形态(默认 fw)
#   python roc.py obs.csv --pos fw --neg bare   # 负类形态(默认 bare)
#   python roc.py obs.csv --plot roc.png        # ROC 图(需 matplotlib)
#
# 方法: 逐特征全阈值扫描 ROC(不假设分布); 组合得分 = 各特征
#   min-max 归一(bare 校准集内)后求和; 报告 AUC 与混淆矩阵。
#   vm 形态自动作"检测器有效性验证"列(AUC 应显著高于 fw)。
# =====================================================================
import sys
import csv
import argparse
import math

# 数值特征(参与 ROC; hvbit 等单独报)
FEATURES = [
    "cpuid_med", "cpuid_p90", "cpuid_p99", "cpuid_over2x",
    "rdtsc_med", "ratio_med", "sgdt_med", "sgdt_p99",
    "xc_med", "xc_p99", "resid_ns_med", "resid_ns_p99",
    "tsc_drift_ppm",
]
INVALID = 0xFFFFFFFFFFFFFFFF  # detect 的单核哨兵值


def load(path):
    rows = []
    with open(path, newline="", encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def valid(v):
    try:
        x = float(v)
    except (ValueError, TypeError):
        return None
    if x >= INVALID or math.isnan(x):
        return None
    return x


def auc_from_scores(pos, neg):
    """AUC = P(pos score > neg score) + 0.5 P(==), 双向扫全阈值。"""
    if not pos or not neg:
        return None
    wins = ties = 0
    for p in pos:
        for n in neg:
            if p > n:
                wins += 1
            elif p == n:
                ties += 1
    return (wins + 0.5 * ties) / (len(pos) * len(neg))


def roc_points(pos, neg):
    """全阈值 ROC 点列表(FPR, TPR), 阈值降序。"""
    scores = sorted(set(pos) | set(neg), reverse=True)
    pts = [(1.0, 1.0)]  # 阈值低于全部: 全判正
    for th in scores:
        tp = sum(1 for p in pos if p >= th)
        fp = sum(1 for n in neg if n >= th)
        pts.append((fp / len(neg), tp / len(pos)))
    pts.append((0.0, 0.0))
    return pts


def mann_whitney_ci(pos, neg, auc):
    """AUC 的 95% CI(Hanley-McNeil 简化)。"""
    n1, n2 = len(pos), len(neg)
    if n1 < 2 or n2 < 2 or auc is None:
        return None, None
    q1 = auc / (2 - auc)
    q2 = 2 * auc * auc / (1 + auc)
    se = math.sqrt((auc * (1 - auc) + (n1 - 1) * (q1 - auc * auc)
                    + (n2 - 1) * (q2 - auc * auc)) / (n1 * n2))
    return max(0.0, auc - 1.96 * se), min(1.0, auc + 1.96 * se)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--pos", default="fw", help="正类形态(默认 fw)")
    ap.add_argument("--neg", default="bare", help="负类形态(默认 bare)")
    ap.add_argument("--cal", default="bare",
                    help="min-max 归一校准形态(默认 bare)")
    ap.add_argument("--plot", default=None, help="ROC 图输出 png")
    args = ap.parse_args()

    rows = load(args.csv)
    pos = [r for r in rows if r["form"] == args.pos]
    neg = [r for r in rows if r["form"] == args.neg]
    vm = [r for r in rows if r["form"] == "vm"]

    print(f"样本: {args.neg}={len(neg)}  {args.pos}={len(pos)}  vm={len(vm)}")
    if not pos:
        sys.exit(f"无正类形态 '{args.pos}' 样本")
    if not neg:
        sys.exit(f"无负类形态 '{args.neg}' 样本")

    print(f"\n{'特征':<16}{'AUC(' + args.pos + ')':>14}{'95%CI':>18}"
          f"{'AUC(vm验证)':>13}")
    print("-" * 62)

    per_feat_auc = {}
    for feat in FEATURES:
        p = [valid(r[feat]) for r in pos]
        n = [valid(r[feat]) for r in neg]
        p = [x for x in p if x is not None]
        n = [x for x in n if x is not None]
        a = auc_from_scores(p, n) if p and n else None
        lo, hi = mann_whitney_ci(p, n, a) if a is not None else (None, None)
        per_feat_auc[feat] = a

        vm_v = None
        if vm:
            vv = [valid(r[feat]) for r in vm]
            vv = [x for x in vv if x is not None]
            vm_v = auc_from_scores(vv, n) if vv else None

        a_s = f"{a:.3f}" if a is not None else " n/a"
        ci_s = f"[{lo:.2f},{hi:.2f}]" if lo is not None else ""
        v_s = f"{vm_v:.3f}" if vm_v is not None else " n/a"
        print(f"{feat:<16}{a_s:>14}{ci_s:>18}{v_s:>13}")

    # 身份探针直报
    hv = [(r["form"], int(r["hvbit"])) for r in rows if r["hvbit"] != ""]
    if hv:
        leak = [f for f, b in hv if b == 1 and f == args.pos]
        print(f"\nhvbit 暴露: {args.pos} 形态 {len(leak)}/{len(pos)} 例"
              + ("  ← 身份面泄漏!" if leak else "  (清洁 ✓)"))

    # ── 组合得分(校准集 min-max 归一后求和) ──
    cal = [r for r in rows if r["form"] == args.cal] or neg
    combos_pos, combos_neg, combos_vm = [], [], []
    for r in pos:
        s = 0.0
        for feat in FEATURES:
            cvals = [valid(x[feat]) for x in cal]
            cvals = [x for x in cvals if x is not None]
            v = valid(r[feat])
            if not cvals or v is None:
                continue
            lo, hi = min(cvals), max(cvals)
            s += (v - lo) / (hi - lo) if hi > lo else 0.0
        combos_pos.append(s)
    for r in neg:
        s = 0.0
        for feat in FEATURES:
            cvals = [valid(x[feat]) for x in cal]
            cvals = [x for x in cvals if x is not None]
            v = valid(r[feat])
            if not cvals or v is None:
                continue
            lo, hi = min(cvals), max(cvals)
            s += (v - lo) / (hi - lo) if hi > lo else 0.0
        combos_neg.append(s)
    for r in vm:
        s = 0.0
        for feat in FEATURES:
            cvals = [valid(x[feat]) for x in cal]
            cvals = [x for x in cvals if x is not None]
            v = valid(r[feat])
            if not cvals or v is None:
                continue
            lo, hi = min(cvals), max(cvals)
            s += (v - lo) / (hi - lo) if hi > lo else 0.0
        combos_vm.append(s)

    a_combo = auc_from_scores(combos_pos, combos_neg)
    lo, hi = mann_whitney_ci(combos_pos, combos_neg, a_combo)
    a_vm = auc_from_scores(combos_vm, combos_neg) if combos_vm else None
    print(f"\n组合得分 AUC({args.pos}) = {a_combo:.3f}  "
          f"95%CI [{lo:.2f},{hi:.2f}]"
          + (f"   vm 验证 AUC = {a_vm:.3f}" if a_vm is not None else ""))

    # ── 混淆矩阵(组合得分, 阈值取 Youden J 最大) ──
    pts = roc_points(combos_pos, combos_neg)
    best = max(pts, key=lambda p: p[1] - p[0])
    thr = None
    allv = sorted(combos_pos + combos_neg, reverse=True)
    # 找到对应 FPR/TPR 的阈值近似: 重扫
    for t in allv:
        tp = sum(1 for x in combos_pos if x >= t) / len(combos_pos)
        fp = sum(1 for x in combos_neg if x >= t) / len(combos_neg)
        if abs(tp - best[1]) < 1e-9 and abs(fp - best[0]) < 1e-9:
            thr = t
            break
    if thr is not None:
        print(f"\n最优阈值(Youden J) = {thr:.2f} 混淆矩阵:")
        for name, arr in [(args.neg, combos_neg), (args.pos, combos_pos)]:
            tp_ = sum(1 for x in arr if x >= thr)
            print(f"  {name:<6} 判虚拟化 {tp_:>3}/{len(arr)}")

    # ── 可选画图 ──
    if args.plot:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
            fig, ax = plt.subplots(figsize=(4.2, 4.2))
            for feat in ("cpuid_med", "xc_med", "resid_ns_med"):
                p = [valid(r[feat]) for r in pos]
                n = [valid(r[feat]) for r in neg]
                p = [x for x in p if x is not None]
                n = [x for x in n if x is not None]
                if p and n:
                    pts_f = roc_points(p, n)
                    ax.plot([x for x, _ in pts_f], [y for _, y in pts_f],
                            label=feat, lw=1.2)
            if combos_pos and combos_neg:
                pts_c = roc_points(combos_pos, combos_neg)
                ax.plot([x for x, _ in pts_c], [y for _, y in pts_c],
                        label="combined", lw=2.0, color="k")
            ax.plot([0, 1], [0, 1], ":", color="gray", lw=0.8)
            ax.set_xlabel("FPR")
            ax.set_ylabel("TPR")
            ax.set_title(f"ROC: {args.pos} vs {args.neg}")
            ax.legend(fontsize=8)
            fig.tight_layout()
            fig.savefig(args.plot, dpi=150)
            print(f"\nROC 图 → {args.plot}")
        except ImportError:
            print("\n(未装 matplotlib, 跳过画图)")


if __name__ == "__main__":
    main()
