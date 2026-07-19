from __future__ import annotations

import argparse
from dataclasses import asdict
import json
import queue
import sys
import threading
import traceback
from pathlib import Path

from scanner_core import (
    ScanCancelled,
    ScanError,
    ScanProgress,
    ScanResult,
    scan_library,
    verify_library,
)


def run_cli(args: argparse.Namespace) -> int:
    def log(message: str) -> None:
        print(message, flush=True)

    def progress(state: ScanProgress) -> None:
        if state.phase == "discover":
            print(f"\r正在枚举: {state.discovered}  {state.current_path[:80]:80}", end="", flush=True)
            return
        if state.total:
            print(
                f"\r{state.message}: {state.processed}/{state.total} "
                f"复用={state.reused} 新增={state.added} 修改={state.modified} "
                f"{state.current_path[:60]:60}",
                end="",
                flush=True,
            )

    try:
        if args.verify:
            result = verify_library(args.root)
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 0
        result = scan_library(
            args.root,
            force_full=args.full,
            strict_verify=args.strict,
            ultra_fast=args.ultra,
            progress=progress,
            log=log,
        )
        print()
        print(json.dumps(asdict(result), ensure_ascii=False, indent=2))
        return 0
    except KeyboardInterrupt:
        print("\n已取消")
        return 130
    except Exception as exc:
        print(f"\n错误: {exc}", file=sys.stderr)
        if args.debug:
            traceback.print_exc()
        return 1


class ScannerApp:
    def __init__(self) -> None:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk

        self.tk = tk
        self.ttk = ttk
        self.filedialog = filedialog
        self.messagebox = messagebox
        self.root = tk.Tk()
        self.root.title("ESP32-S3 曲库扫描器")
        self.root.geometry("900x680")
        self.root.minsize(760, 560)

        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.cancel_event = threading.Event()
        self.worker: threading.Thread | None = None

        self.path_var = tk.StringVar()
        self.phase_var = tk.StringVar(value="请选择 TF 卡根目录或 Music 目录")
        self.current_var = tk.StringVar(value="-")
        self.stats_var = tk.StringVar(value="发现 0　复用 0　新增 0　修改 0　删除 0")
        self.progress_var = tk.DoubleVar(value=0)

        self._build_ui()
        self.root.after(100, self._poll_events)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self) -> None:
        tk = self.tk
        ttk = self.ttk

        outer = ttk.Frame(self.root, padding=16)
        outer.pack(fill="both", expand=True)

        title = ttk.Label(outer, text="ESP32-S3 曲库扫描器", font=("Microsoft YaHei UI", 18, "bold"))
        title.pack(anchor="w")
        ttk.Label(
            outer,
            text="生成设备端兼容的 /System/library 索引与 /System/reports 扫描报告",
        ).pack(anchor="w", pady=(2, 14))

        path_frame = ttk.LabelFrame(outer, text="TF 卡目录", padding=10)
        path_frame.pack(fill="x")
        entry = ttk.Entry(path_frame, textvariable=self.path_var)
        entry.pack(side="left", fill="x", expand=True)
        ttk.Button(path_frame, text="选择目录", command=self._choose_directory).pack(side="left", padx=(8, 0))

        buttons = ttk.Frame(outer)
        buttons.pack(fill="x", pady=12)
        self.ultra_button = ttk.Button(buttons, text="超快速目录扫描", command=lambda: self._start_scan("ultra"))
        self.ultra_button.pack(side="left")
        self.incremental_button = ttk.Button(buttons, text="快速增量扫描", command=lambda: self._start_scan("fast"))
        self.incremental_button.pack(side="left", padx=(8, 0))
        self.strict_button = ttk.Button(buttons, text="严格增量扫描", command=lambda: self._start_scan("strict"))
        self.strict_button.pack(side="left", padx=(8, 0))
        self.full_button = ttk.Button(buttons, text="强制全量扫描", command=lambda: self._start_scan("full"))
        self.full_button.pack(side="left", padx=(8, 0))
        self.verify_button = ttk.Button(buttons, text="校验现有索引", command=self._start_verify)
        self.verify_button.pack(side="left", padx=(8, 0))
        self.cancel_button = ttk.Button(buttons, text="取消", command=self._cancel, state="disabled")
        self.cancel_button.pack(side="right")

        progress_frame = ttk.LabelFrame(outer, text="进度", padding=10)
        progress_frame.pack(fill="x")
        ttk.Label(progress_frame, textvariable=self.phase_var, font=("Microsoft YaHei UI", 11, "bold")).pack(anchor="w")
        ttk.Progressbar(progress_frame, variable=self.progress_var, maximum=100).pack(fill="x", pady=(8, 8))
        ttk.Label(progress_frame, textvariable=self.stats_var).pack(anchor="w")
        ttk.Label(progress_frame, textvariable=self.current_var, wraplength=830).pack(anchor="w", pady=(4, 0))

        log_frame = ttk.LabelFrame(outer, text="日志", padding=8)
        log_frame.pack(fill="both", expand=True, pady=(12, 0))
        self.log_text = tk.Text(log_frame, height=18, wrap="word", state="disabled", font=("Consolas", 10))
        scrollbar = ttk.Scrollbar(log_frame, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scrollbar.set)
        self.log_text.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        ttk.Label(
            outer,
            text="提示：首次运行或索引/Manifest 不匹配时，增量扫描会自动回退为全量扫描。",
        ).pack(anchor="w", pady=(10, 0))

    def _choose_directory(self) -> None:
        selected = self.filedialog.askdirectory(title="选择 TF 卡根目录或 Music 目录")
        if selected:
            self.path_var.set(selected)

    def _append_log(self, message: str) -> None:
        self.log_text.configure(state="normal")
        self.log_text.insert("end", message.rstrip() + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _set_busy(self, busy: bool) -> None:
        state = "disabled" if busy else "normal"
        self.ultra_button.configure(state=state)
        self.incremental_button.configure(state=state)
        self.strict_button.configure(state=state)
        self.full_button.configure(state=state)
        self.verify_button.configure(state=state)
        self.cancel_button.configure(state="normal" if busy else "disabled")

    def _validate_path(self) -> str | None:
        value = self.path_var.get().strip()
        if not value:
            self.messagebox.showwarning("缺少目录", "请先选择 TF 卡根目录或 Music 目录。")
            return None
        return value

    def _start_scan(self, mode: str) -> None:
        selected = self._validate_path()
        if selected is None or (self.worker and self.worker.is_alive()):
            return
        self.cancel_event.clear()
        self._set_busy(True)
        self.progress_var.set(0)
        self.phase_var.set("正在启动扫描")
        self.current_var.set("-")
        self.stats_var.set("发现 0　复用 0　新增 0　修改 0　删除 0")
        force_full = mode == "full"
        strict_verify = mode == "strict"
        ultra_fast = mode == "ultra"
        mode_label = {
            "ultra": "超快速目录扫描",
            "fast": "快速增量扫描",
            "strict": "严格增量扫描",
            "full": "强制全量扫描",
        }.get(mode, "快速增量扫描")
        self._append_log("=" * 72)
        self._append_log(f"开始{mode_label}")

        def progress(state: ScanProgress) -> None:
            self.events.put(("progress", state))

        def log(message: str) -> None:
            self.events.put(("log", message))

        def worker() -> None:
            try:
                result = scan_library(
                    selected,
                    force_full=force_full,
                    strict_verify=strict_verify,
                    ultra_fast=ultra_fast,
                    progress=progress,
                    log=log,
                    cancel_event=self.cancel_event,
                )
                self.events.put(("done", result))
            except ScanCancelled as exc:
                self.events.put(("cancelled", str(exc)))
            except Exception as exc:
                self.events.put(("error", (str(exc), traceback.format_exc())))

        self.worker = threading.Thread(target=worker, name="MusicScanner", daemon=True)
        self.worker.start()

    def _start_verify(self) -> None:
        selected = self._validate_path()
        if selected is None or (self.worker and self.worker.is_alive()):
            return
        self.cancel_event.clear()
        self._set_busy(True)
        self.phase_var.set("正在校验索引")
        self._append_log("=" * 72)
        self._append_log("校验现有索引与 Manifest")

        def worker() -> None:
            try:
                result = verify_library(selected)
                self.events.put(("verified", result))
            except Exception as exc:
                self.events.put(("error", (str(exc), traceback.format_exc())))

        self.worker = threading.Thread(target=worker, name="IndexVerifier", daemon=True)
        self.worker.start()

    def _cancel(self) -> None:
        self.cancel_event.set()
        self.phase_var.set("正在取消，请等待当前文件处理结束")
        self._append_log("收到取消请求")

    def _handle_progress(self, state: ScanProgress) -> None:
        self.phase_var.set(state.message or state.phase)
        self.current_var.set(state.current_path or "-")
        self.stats_var.set(
            f"发现 {state.discovered}　复用 {state.reused}　新增 {state.added}　"
            f"修改 {state.modified}　删除 {state.deleted}"
        )
        if state.total > 0:
            self.progress_var.set(min(100.0, state.processed * 100.0 / state.total))
        elif state.phase == "discover":
            self.progress_var.set(0)

    def _poll_events(self) -> None:
        while True:
            try:
                event, payload = self.events.get_nowait()
            except queue.Empty:
                break

            if event == "progress":
                self._handle_progress(payload)  # type: ignore[arg-type]
            elif event == "log":
                self._append_log(str(payload))
            elif event == "done":
                result: ScanResult = payload  # type: ignore[assignment]
                self._set_busy(False)
                self.progress_var.set(100)
                self.phase_var.set("扫描完成")
                mode_name = (
                    "全量"
                    if result.full_scan
                    else "超快速目录"
                    if result.ultra_fast_incremental
                    else "严格增量"
                    if result.strict_incremental
                    else "快速增量"
                )
                summary = (
                    f"模式：{mode_name}\n"
                    f"歌曲：{result.track_count}\n专辑：{result.album_count}\n歌手：{result.artist_count}\n"
                    f"复用：{result.reused}　新增：{result.added}　修改：{result.modified}　删除：{result.deleted}\n"
                    f"跳过目录：{result.skipped_directories}　无变化：{'是' if result.no_changes else '否'}\n"
                    f"用时：{result.elapsed_seconds:.2f} 秒\n\n"
                    f"索引：{result.index_path}\n清单：{result.manifest_path}"
                )
                self.messagebox.showinfo("扫描完成", summary)
            elif event == "verified":
                self._set_busy(False)
                self.phase_var.set("索引校验通过")
                result = payload
                self._append_log(json.dumps(result, ensure_ascii=False, indent=2))
                self.messagebox.showinfo(
                    "校验通过",
                    f"歌曲 {result['tracks']}\n专辑 {result['albums']}\n歌手 {result['artists']}\n"
                    f"Catalog CRC {result['catalog_crc32']}",
                )
            elif event == "cancelled":
                self._set_busy(False)
                self.phase_var.set("已取消")
                self._append_log(str(payload))
            elif event == "error":
                self._set_busy(False)
                message, detail = payload  # type: ignore[misc]
                self.phase_var.set("操作失败")
                self._append_log(detail)
                self.messagebox.showerror("操作失败", message)

        self.root.after(100, self._poll_events)

    def _on_close(self) -> None:
        if self.worker and self.worker.is_alive():
            if not self.messagebox.askyesno("扫描正在运行", "是否取消扫描并退出？"):
                return
            self.cancel_event.set()
        self.root.destroy()

    def run(self) -> None:
        self.root.mainloop()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ESP32-S3 V3 曲库电脑端扫描器")
    parser.add_argument("--root", help="TF 卡根目录或 Music 目录")
    parser.add_argument("--ultra", action="store_true", help="超快速目录扫描；平铺 /Music 未变化时整体跳过")
    parser.add_argument("--full", action="store_true", help="强制全量扫描")
    parser.add_argument("--strict", action="store_true", help="严格增量扫描，逐文件校验内容 CRC")
    parser.add_argument("--verify", action="store_true", help="仅校验现有索引")
    parser.add_argument("--debug", action="store_true", help="命令行错误时输出堆栈")
    parser.add_argument("--gui", action="store_true", help="强制打开图形界面")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.root and not args.gui:
        return run_cli(args)
    try:
        app = ScannerApp()
        app.run()
        return 0
    except Exception as exc:
        print(f"无法启动图形界面: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
