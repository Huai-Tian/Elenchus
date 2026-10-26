#!/usr/bin/env python3
# =====================================================================
# analyze.py — A2 跨时钟域相干性残差分析
#
# 输入: clkprobe 输出的 CSV("tsc,other" 每行一对)
# 输出: 线性回归 other = a*tsc + b 的残差统计 + ASCII 直方图
#
# 判读:
#   裸机             → 残差 σ ≈ 计数器量化噪声, 单峰窄分布
#   未补偿 hypervisor → σ 显著放大 / 双峰 / 台阶(每次 exit = 一个台阶)
#   被测框架         → 与裸机同形 = 时间轴封堵成功的证据
#
# 用法: python3 analyze.py bare.csv vmware.csv framework.csv [--col tsc]
#       单文件也行: python3 analyze.py run.csv
# =====================================================================
import sys
import math

def load(path):
    xs, ys = [], []
    with open(path) as f:
        for line in f:
            parts = line.strip().split(",")
            if len(parts) != 2:
                continue
            try:
                x, y = float(parts[0]), float(parts[1])
            except ValueError:
                continue
            xs.append(x)
            ys.append(y)
    return xs, ys

def regress(xs, ys):
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    a = sxy / sxx if sxx else 0.0
    b = my - a * mx
    return a, b

def stats(resid):
    n = len(resid)
    mu = sum(resid) / n
    var = sum((r - mu) ** 2 for r in resid) / n
    sigma = math.sqrt(var)
    srt = sorted(resid)
    p50 = srt[n // 2]
    p95 = srt[int(n * 0.95)]
    p999 = srt[min(int(n * 0.999), n - 1)]
    # 最大间隙: 排序残差中相邻差值最大者(台阶检测——
    # 未补偿 exit 在残差轴上表现为若干聚簇, 簇间大间隙)
    maxgap, gi = 0.0, -1
    for i in range(1, n):
        g = srt[i] - srt[i - 1]
        if g > maxgap:
            maxgap, gi = g, i
    return mu, sigma, p50, p95, p999, maxgap, gi

def hist_ascii(resid, bins=40, width=60):
    srt = sorted(resid)
    lo, hi = srt[0], srt[-1]
    if hi == lo:
        hi = lo + 1
    cnt = [0] * bins
    for r in resid:
        idx = int((r - lo) / (hi - lo) * bins)
        cnt[min(idx, bins - 1)] += 1
    peak = max(cnt)
    lines = []
    for i in range(bins):
        bar = "#" * max(0, int(cnt[i] / peak * width))
        lo_b = lo + (hi - lo) * i / bins
        lines.append(f"{lo_b:>12.1f} |{bar:<{width}} {cnt[i]}")
    return "\n".join(lines)

def bimodality(resid, sigma):
    # 简易双峰性: 中位数两侧各找局部计数比(非严格 dip test, 够判读用)
    srt = sorted(resid)
    n = len(srt)
    med = srt[n // 2]
    if sigma == 0:
        return 0.0
    return abs(med - (srt[0] + srt[-1]) / 2) / sigma

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    print(f"{'file':<28} {'n':>8} {'slope':>14} {'sigma':>10} {'p95|r|':>10} {'p99.9|r|':>10} {'maxgap':>10} {'skew~':>7} {'mono':>6}")
    print("-" * 112)
    hists = []
    for path in sys.argv[1:]:
        if path.startswith("--"):
            continue
        xs, ys = load(path)
        if len(xs) < 100:
            print(f"{path}: 样本不足({len(xs)})")
            continue
        a, b = regress(xs, ys)
        resid = [y - (a * x + b) for x, y in zip(xs, ys)]
        mu, sigma, p50, p95, p999, maxgap, gi = stats(resid)
        skew = bimodality(resid, sigma)
        # A3 单调性: 计数器列(ys)出现倒退的次数(快速连读虚拟计数器倒退=
        # 补偿非单调=检测向量; 裸机硬件计数器从不倒退)
        mono_bad = sum(1 for i in range(1, len(ys)) if ys[i] < ys[i - 1])
        print(f"{path.split('/')[-1]:<28} {len(xs):>8} {a:>14.6g} {sigma:>10.2f} {abs(p95 - mu):>10.2f} {abs(p999 - mu):>10.2f} {maxgap:>10.2f} {skew:>7.2f} {mono_bad:>6}")
        hists.append((path, resid))
    for path, resid in hists:
        print(f"\n== {path} 残差直方图 ==")
        print(hist_ascii(resid))
    print("\n判读: sigma/maxgap/mono 与裸机同量级 → 封堵成功; sigma 放大、直方图台阶/双峰、")
    print("      或 mono>0(计数器倒退) → 时间轴泄漏。")
    print("      ⚠ 真值交叉验证: 框架形态必须同时查框架日志 'y' 事件数>0(证明采样确实")
    print("      经过 root 仿真路径), 否则 PASS 无效(可能未触发封堵)。")
    return 0

if __name__ == "__main__":
    sys.exit(main())
