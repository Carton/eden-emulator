#!/usr/bin/env python3
"""Eden/TOTK 手动游玩卡顿哨兵 + wpr 内存环形缓冲自动抓 trace。(v2)

背景（PROFILE_PROGRESS.md 卡顿攻坚轮）：游玩中偶发 <1s 的可感知掉帧，需要
"机器感知 + 即刻冻结现场"。方案 = PresentMon(逐帧 CSV, QPC 时间戳) 做哨兵，
wpr 内存环形缓冲(常驻低开销)做现场，检测到严重卡顿立即 wpr -stop 落盘 ETL
——环形缓冲覆盖卡顿前 ~40s(CPU-only, ~24MB/s 事件量)。

本机踩坑记录（重要，改机器/改参数前必读）：
  1. C: 长期 99% 满 → wpr 内存模式 start/stop 都会在 C: 临时区 staging 上
     挂死/报 0x80070070。解法 = 所有 wpr 子进程 env 里 TMP/TEMP 指到 F:。
  2. PresentMon 自己持有 --output_file 的句柄，外部 tail 该文件会
     PermissionError(共享冲突)。解法 = 用 --output_stdout 管道，CSV 自己落盘。
  3. CPU+GPU 双 profile 事件量 ~200MB/s(环形只盖 ~5s)，默认只用 CPU profile；
     GPU 侧信号用 PresentMon 的 MsGPUActive/MsBetweenDisplayChange 列。
  4. wpr stop 落盘 ~25MB/s(F 盘)，~1GB 要 ~30s：抓取期间在后台线程做，
     主循环继续吃 PresentMon 帧；此窗口内不再触发新抓取。

必须以管理员运行（由 stutter_watch_start.bat 自动提权拉起）。
本脚本自身即那个"同一提权会话"：start/stop 都从这里发。

用法:
  python stutter_watch.py [--severe-ms 50] [--ratio 1.6] [--cooldown 5]
      [--max-captures 4] [--start-delay 15] [--duration 0] [--process-name eden]
      [--outdir F:\\prof\\stutters] [--with-gpu]

流程:
  1. 预检: wpr -cancel 自愈; 清残留 PresentMon; F 盘空间; 禁跑进程警告
  2. 起 PresentMon(--output_stdout 管道)盯目标进程, CSV 由本脚本转存 pm_live.csv
  3. 起 wpr -start CPU (内存环形)
  4. 帧间隔 >= severe_ms 且 >= ratio*滚动中位 且过冷却期 => 严重卡顿
     => 后台线程 wpr -stop 落盘 stutter_<时间>.etl + 侧车 json(帧上下文+锚点)
     => 完成后重启环形缓冲继续盯(直到 max-captures, 之后仅记录)
  5. 退出(Ctrl+C 或 --duration): 若环形还在, 补落 manual_<时间>.etl, 杀 PM, 摘要

输出: outdir/ 下 pm_live.csv / pm_console.log / stutter_*.etl+json /
      manual_*.etl / stutter_events.csv
后续: etl 交给 ETW MCP process_trace; 侧车 json 里 wpr_start_epoch +
detected_epoch + 行内 QPC 做切窗锚点。
"""
import argparse
import ctypes
import json
import os
import queue
import shutil
import statistics
import subprocess
import sys
import threading
import time
from collections import deque

PM_EXE = os.environ.get(
    "PM_EXE", r"G:\Tools\PresentMon\PresentMon-2.5.1-x64.exe")
WPR = "wpr.exe"
CREATE_NO_WINDOW = 0x08000000

HARD_WARN = {"losslessscalingsvc.exe", "losslessscaling.exe"}
SOFT_NOTE = {"nvcontainer.exe", "nvidia overlay.exe"}


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def is_admin():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


class Wpr:
    """wpr 控制；所有子进程 TMP/TEMP 重定向到 F:（C: 满会挂死，见文件头注 1）。"""

    def __init__(self, tmpdir, profiles):
        self.env = dict(os.environ, TMP=tmpdir, TEMP=tmpdir)
        self.profiles = profiles  # e.g. ["CPU"] 或 ["CPU", "GPU"]
        self.active = False
        self.started_epoch = None

    def _run(self, *args):
        return subprocess.run([WPR, *args], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", env=self.env)

    def cancel(self):
        return self._run("-cancel")

    def start(self):
        cmd = ["-start", self.profiles[0]]
        for p in self.profiles[1:]:
            cmd += ["-start", p]
        r = self._run(*cmd)
        self.active = r.returncode == 0
        self.started_epoch = time.time() if self.active else None
        if not self.active:
            log(f"wpr start 失败 rc={r.returncode} out={r.stdout!r} err={r.stderr!r}")
        return self.active

    def stop(self, etl_path):
        t0 = time.time()
        r = self._run("-stop", etl_path)
        size = os.path.getsize(etl_path) if os.path.exists(etl_path) else 0
        log(f"wpr stop -> {os.path.basename(etl_path)} rc={r.returncode} "
            f"{size/1e6:.0f}MB 用时{time.time()-t0:.0f}s")
        self.active = False
        return r.returncode == 0 and size > 0


def find_col(header, *cands, fuzzy=()):
    lo = [h.strip().lstrip(",").lower() for h in header]
    for c in cands:
        cl = c.lower()
        for i, h in enumerate(lo):
            if h == cl:
                return i
    for f in fuzzy:
        fl = f.lower()
        for i, h in enumerate(lo):
            if fl in h:
                return i
    return -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--process-name", default="eden")
    ap.add_argument("--severe-ms", type=float, default=50.0)
    ap.add_argument("--ratio", type=float, default=1.6)
    ap.add_argument("--cooldown", type=float, default=5.0)
    ap.add_argument("--max-captures", type=int, default=4)
    ap.add_argument("--start-delay", type=float, default=15.0,
                    help="启动后多少秒才武装检测（跳过开机画面）")
    ap.add_argument("--min-history", type=int, default=60,
                    help="滚动中位最少多少帧才武装")
    ap.add_argument("--duration", type=float, default=0.0,
                    help="多少秒后自动退出（0=跑到 Ctrl+C）")
    ap.add_argument("--outdir", default=r"F:\prof\stutters")
    ap.add_argument("--with-gpu", action="store_true",
                    help="环形加 GPU profile（事件量 ~200MB/s，覆盖只剩 ~5s，慎用）")
    ap.add_argument("--no-manual-flush", action="store_true")
    args = ap.parse_args()

    if not is_admin():
        print("必须以管理员运行（用 stutter_watch_start.bat）", flush=True)
        sys.exit(2)
    if not os.path.exists(PM_EXE):
        print(f"找不到 PresentMon: {PM_EXE}", flush=True)
        sys.exit(2)

    outdir = os.path.abspath(args.outdir)
    os.makedirs(outdir, exist_ok=True)
    tmpdir = os.path.join(outdir, "wpr_tmp")
    os.makedirs(tmpdir, exist_ok=True)
    session_tag = time.strftime("%Y%m%d_%H%M%S")
    events_csv = os.path.join(outdir, "stutter_events.csv")

    # ---- 预检 ----
    wpr = Wpr(tmpdir, ["CPU", "GPU"] if args.with_gpu else ["CPU"])
    wpr.cancel()  # 自愈僵尸会话
    subprocess.run(["taskkill", "/IM", os.path.basename(PM_EXE), "/F"],
                   capture_output=True)
    time.sleep(1)
    free_gb = shutil.disk_usage(outdir[:3]).free / 1e9
    if free_gb < 10:
        log(f"警告: {outdir[:2]} 只剩 {free_gb:.1f}GB，每份 ETL 约 0.7~1GB")
    tl = subprocess.run(["tasklist", "/FO", "CSV"], capture_output=True,
                        text=True, encoding="utf-8", errors="replace").stdout or ""
    low = {row.split('","')[0].strip('"').lower() for row in tl.splitlines()}
    for p in sorted(low & HARD_WARN):
        log(f"警告: {p} 在运行，建议测前关闭（会干扰帧率）")
    if low & SOFT_NOTE:
        log("提示: NVIDIA Overlay 在运行（实测不影响 med，忽略）")

    # ---- PresentMon: stdout 管道 + 读线程 ----
    pm_csv = os.path.join(outdir, "pm_live.csv")
    pm_log = os.path.join(outdir, "pm_console.log")
    pm = subprocess.Popen(
        [PM_EXE, "--process_name", args.process_name, "--output_stdout",
         "--qpc_time", "--stop_existing_session", "--no_console_stats"],
        stdout=subprocess.PIPE, stderr=open(pm_log, "w"),
        creationflags=CREATE_NO_WINDOW)
    lines = queue.Queue(maxsize=100000)

    def pm_reader():
        try:
            for raw in pm.stdout:
                lines.put(raw)
        except Exception:
            pass

    threading.Thread(target=pm_reader, daemon=True).start()
    log(f"PresentMon pid={pm.pid} 盯进程 {args.process_name}（游戏随时可启动）")

    ring_ok = wpr.start()
    csv_out = open(pm_csv, "w", buffering=1)

    hist = deque(maxlen=120)
    last_trigger = 0.0
    captures = 0
    capturing = False      # 后台 stop/restart 进行中，暂停触发
    t_watch0 = time.time()
    frames_seen = 0
    last_stat = 0.0
    cols = {}

    def resolve_cols(header):
        c = {
            "ms": find_col(header, "MsBetweenPresents"),
            "qpc": find_col(header, "CPUStartQPC", fuzzy=["qpc"]),
            "disp": find_col(header, "MsBetweenDisplayChange"),
            "api": find_col(header, "MsInPresentAPI"),
            "gpu": find_col(header, "MsGPUActive", fuzzy=["gpuactive"]),
        }
        if c["ms"] < 0:
            log(f"CSV 列识别失败，表头: {header}")
        else:
            log("CSV 列就绪: " + ", ".join(
                f"{k}={header[v]}" for k, v in c.items() if v >= 0))
        return c

    def do_capture(frame_ms, med, row, detected_epoch, wpr_start_epoch):
        """后台线程：stop 落盘 + 重启环形。主循环期间继续吃帧不丢 CSV。"""
        nonlocal captures, capturing, ring_ok
        tag = time.strftime("%Y%m%d_%H%M%S")
        ctx = {
            "detected_epoch": detected_epoch,
            "wpr_start_epoch": wpr_start_epoch,
            "frame_ms": frame_ms,
            "rolling_median_ms": round(med, 2),
            "threshold": {"severe_ms": args.severe_ms, "ratio": args.ratio},
            "recent_ms": [round(x, 1) for x in list(hist)[-30:]],
            "row": {},
        }
        for k in ("qpc", "disp", "api", "gpu"):
            i = cols.get(k, -1)
            if 0 <= i < len(row):
                ctx["row"][k] = row[i]
        etl = os.path.join(outdir, f"stutter_{tag}.etl")
        ok = ring_ok and wpr.stop(etl)
        ctx["etl"] = etl if ok else ("NO_RING" if not ring_ok else "STOP_FAILED")
        with open(os.path.join(outdir, f"stutter_{tag}.json"), "w") as f:
            json.dump(ctx, f, indent=1)
        with open(events_csv, "a") as f:
            f.write(f"{tag},{frame_ms:.1f},{med:.1f},{ctx['etl']}\n")
        captures += 1
        if captures < args.max_captures:
            time.sleep(2)
            ring_ok = wpr.start()
            log(f"环形已重启（剩 {args.max_captures - captures} 次配额）")
        else:
            log(f"已达 --max-captures {args.max_captures}，继续记录不再抓")
        capturing = False

    try:
        while True:
            if args.duration and time.time() - t_watch0 > args.duration:
                log(f"--duration {args.duration:.0f}s 到时，收尾")
                break
            if pm.poll() is not None:
                log(f"PresentMon 退出(code={pm.returncode})，看 {pm_log}")
                break

            # 吃管道里所有新行
            got = 0
            while True:
                try:
                    raw = lines.get_nowait()
                except queue.Empty:
                    break
                got += 1
                s = raw.decode("utf-8", "replace").strip()
                if not s:
                    continue
                csv_out.write(s + "\n")
                parts = s.split(",")
                if not cols:
                    header = [p.strip() for p in parts]
                    cols = resolve_cols(header)
                    continue
                i_ms = cols.get("ms", -1)
                if i_ms < 0 or len(parts) <= i_ms:
                    continue
                try:
                    ms = float(parts[i_ms] or "nan")
                except ValueError:
                    continue
                if ms != ms or ms <= 0:
                    continue
                frames_seen += 1
                hist.append(ms)
                if capturing:
                    continue
                armed = (frames_seen >= args.min_history
                         and time.time() - t_watch0 > args.start_delay
                         and len(hist) >= args.min_history)
                if not armed:
                    continue
                med = statistics.median(hist)
                now = time.time()
                if (ms >= args.severe_ms and ms >= args.ratio * med
                        and now - last_trigger > args.cooldown):
                    last_trigger = now
                    log(f"*** 严重卡顿 {ms:.0f}ms (med {med:.1f}) 抓取 #{captures + 1} ***")
                    capturing = True
                    threading.Thread(
                        target=do_capture,
                        args=(ms, med, parts, now, wpr.started_epoch),
                        daemon=True).start()

            if time.time() - last_stat > 15:
                last_stat = time.time()
                if hist:
                    srt = sorted(hist)
                    log(f"状态: {frames_seen} 帧 med {statistics.median(hist):.1f}ms "
                        f"p99 {srt[int(0.99*(len(srt)-1))]:.1f}ms "
                        f"抓取 {captures}/{args.max_captures}")
            time.sleep(0.25 if got else 0.5)
    except KeyboardInterrupt:
        log("Ctrl+C —— 收尾（若刚有漏检卡顿，manual etl 里还有）")
    finally:
        if capturing:
            time.sleep(1)  # 给后台 stop 一点时间；不等待完成
        if wpr.active and not args.no_manual_flush:
            etl = os.path.join(outdir, f"manual_{session_tag}.etl")
            if wpr.stop(etl):
                log(f"manual etl: {os.path.basename(etl)}")
        elif wpr.active:
            wpr.cancel()
        if pm.poll() is None:
            pm.terminate()
            try:
                pm.wait(5)
            except subprocess.TimeoutExpired:
                pm.kill()
        csv_out.close()
        log(f"会话结束: {frames_seen} 帧, {captures} 次抓取, 输出在 {outdir}")


if __name__ == "__main__":
    main()
