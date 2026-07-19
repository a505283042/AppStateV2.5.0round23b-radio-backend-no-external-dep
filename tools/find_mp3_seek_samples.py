#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
扫描 MP3，寻找两类测试文件：
1. Xing + TOC，且采样到多个码率的 VBR MP3
2. 不含 Xing / Info / VBRI，且采样码率恒定的 CBR MP3 候选

用法：
    python find_mp3_seek_samples.py "D:\Music"
    python find_mp3_seek_samples.py "\\192.168.1.105\web\music"

结果保存在脚本当前目录：
    mp3_seek_candidates.csv
    xing_toc_vbr.txt
    cbr_no_index.txt
"""

from __future__ import annotations

import argparse
import csv
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Optional


BITRATE_MPEG1_L3 = (
    0, 32, 40, 48, 56, 64, 80, 96,
    112, 128, 160, 192, 224, 256, 320, 0
)
BITRATE_MPEG2_L3 = (
    0, 8, 16, 24, 32, 40, 48, 56,
    64, 80, 96, 112, 128, 144, 160, 0
)
BASE_SAMPLE_RATES = (44100, 48000, 32000)


@dataclass(frozen=True)
class FrameHeader:
    version: str
    version_id: int
    bitrate_kbps: int
    sample_rate: int
    padding: int
    channel_mode: int
    has_crc: bool
    frame_length: int
    samples_per_frame: int


@dataclass
class ScanResult:
    path: str
    category: str
    marker: str
    toc: bool
    sampled_frames: int
    bitrates: list[int]
    first_frame_offset: int
    id3_bytes: int
    note: str


def parse_header(data: bytes) -> Optional[FrameHeader]:
    if len(data) < 4:
        return None

    b0, b1, b2, b3 = data[:4]

    # 11 位同步字。
    if b0 != 0xFF or (b1 & 0xE0) != 0xE0:
        return None

    version_id = (b1 >> 3) & 0x03
    layer_id = (b1 >> 1) & 0x03
    protection_bit = b1 & 0x01

    # version_id=1 保留；Layer III 的 layer_id=1。
    if version_id == 1 or layer_id != 1:
        return None

    bitrate_index = (b2 >> 4) & 0x0F
    sample_rate_index = (b2 >> 2) & 0x03
    padding = (b2 >> 1) & 0x01

    if bitrate_index in (0, 15) or sample_rate_index == 3:
        return None

    if version_id == 3:
        version = "MPEG-1"
        bitrate_kbps = BITRATE_MPEG1_L3[bitrate_index]
        sample_rate = BASE_SAMPLE_RATES[sample_rate_index]
        samples_per_frame = 1152
        frame_length = (144000 * bitrate_kbps) // sample_rate + padding
    elif version_id == 2:
        version = "MPEG-2"
        bitrate_kbps = BITRATE_MPEG2_L3[bitrate_index]
        sample_rate = BASE_SAMPLE_RATES[sample_rate_index] // 2
        samples_per_frame = 576
        frame_length = (72000 * bitrate_kbps) // sample_rate + padding
    else:
        version = "MPEG-2.5"
        bitrate_kbps = BITRATE_MPEG2_L3[bitrate_index]
        sample_rate = BASE_SAMPLE_RATES[sample_rate_index] // 4
        samples_per_frame = 576
        frame_length = (72000 * bitrate_kbps) // sample_rate + padding

    if bitrate_kbps <= 0 or frame_length < 24:
        return None

    return FrameHeader(
        version=version,
        version_id=version_id,
        bitrate_kbps=bitrate_kbps,
        sample_rate=sample_rate,
        padding=padding,
        channel_mode=(b3 >> 6) & 0x03,
        has_crc=(protection_bit == 0),
        frame_length=frame_length,
        samples_per_frame=samples_per_frame,
    )


def read_header_at(fp: BinaryIO, offset: int) -> Optional[FrameHeader]:
    try:
        fp.seek(offset)
        return parse_header(fp.read(4))
    except OSError:
        return None


def valid_frame_at(fp: BinaryIO, offset: int, file_size: int) -> Optional[FrameHeader]:
    header = read_header_at(fp, offset)
    if header is None:
        return None

    next_offset = offset + header.frame_length
    if next_offset + 4 > file_size:
        return header

    next_header = read_header_at(fp, next_offset)
    if next_header is None:
        return None

    # 连续帧应保持 MPEG 版本和采样率一致。
    if (
        next_header.version_id != header.version_id
        or next_header.sample_rate != header.sample_rate
    ):
        return None

    return header


def synchsafe_to_int(data: bytes) -> int:
    if len(data) != 4 or any(byte & 0x80 for byte in data):
        return 0
    return (
        (data[0] << 21)
        | (data[1] << 14)
        | (data[2] << 7)
        | data[3]
    )


def id3_audio_start(fp: BinaryIO) -> tuple[int, int]:
    fp.seek(0)
    header = fp.read(10)
    if len(header) < 10 or header[:3] != b"ID3":
        return 0, 0

    tag_payload = synchsafe_to_int(header[6:10])
    total = 10 + tag_payload

    # ID3v2.4 footer 标志。大多数文件没有，但有时需要再跳过 10 字节。
    if header[3] == 4 and (header[5] & 0x10):
        total += 10

    return total, total


def find_frame(
    fp: BinaryIO,
    start: int,
    file_size: int,
    max_scan: int = 256 * 1024,
) -> Optional[tuple[int, FrameHeader]]:
    if start < 0:
        start = 0
    if start >= file_size:
        return None

    scan_size = min(max_scan, file_size - start)
    fp.seek(start)
    data = fp.read(scan_size)

    index = 0
    while True:
        index = data.find(b"\xFF", index)
        if index < 0:
            return None

        offset = start + index
        if index + 4 <= len(data):
            header = parse_header(data[index:index + 4])
            if header is not None:
                next_offset = offset + header.frame_length
                next_rel = next_offset - start

                if next_offset + 4 > file_size:
                    return offset, header

                if 0 <= next_rel <= len(data) - 4:
                    next_header = parse_header(data[next_rel:next_rel + 4])
                else:
                    next_header = read_header_at(fp, next_offset)

                if (
                    next_header is not None
                    and next_header.version_id == header.version_id
                    and next_header.sample_rate == header.sample_rate
                ):
                    return offset, header

        index += 1


def inspect_first_frame(
    fp: BinaryIO,
    frame_offset: int,
    header: FrameHeader,
) -> tuple[str, bool]:
    crc_bytes = 2 if header.has_crc else 0

    if header.version_id == 3:
        side_info = 17 if header.channel_mode == 3 else 32
    else:
        side_info = 9 if header.channel_mode == 3 else 17

    expected_xing = frame_offset + 4 + crc_bytes + side_info
    fp.seek(expected_xing)
    block = fp.read(120)

    marker = ""
    marker_offset = -1

    if block[:4] in (b"Xing", b"Info"):
        marker = block[:4].decode("ascii")
        marker_offset = expected_xing
    else:
        # 兼容少数编码器的非标准摆放，只在首帧前部寻找。
        fp.seek(frame_offset + 4)
        first_part = fp.read(min(header.frame_length - 4, 192))
        for signature in (b"Xing", b"Info", b"VBRI"):
            found = first_part.find(signature)
            if found >= 0:
                marker = signature.decode("ascii")
                marker_offset = frame_offset + 4 + found
                break

    if marker == "VBRI":
        return marker, False

    if marker not in ("Xing", "Info") or marker_offset < 0:
        return "", False

    fp.seek(marker_offset + 4)
    flags_raw = fp.read(4)
    if len(flags_raw) != 4:
        return marker, False

    flags = int.from_bytes(flags_raw, "big")
    has_toc = bool(flags & 0x0004)

    # 验证 TOC 字段实际存在。
    cursor = marker_offset + 8
    if flags & 0x0001:
        cursor += 4
    if flags & 0x0002:
        cursor += 4
    if has_toc:
        fp.seek(cursor)
        if len(fp.read(100)) != 100:
            has_toc = False

    return marker, has_toc


def sample_sequential(
    fp: BinaryIO,
    start_offset: int,
    file_size: int,
    frame_limit: int,
) -> tuple[list[int], int]:
    bitrates: list[int] = []
    offset = start_offset

    for _ in range(frame_limit):
        header = read_header_at(fp, offset)
        if header is None:
            break

        bitrates.append(header.bitrate_kbps)
        next_offset = offset + header.frame_length
        if next_offset <= offset or next_offset + 4 > file_size:
            break

        next_header = read_header_at(fp, next_offset)
        if next_header is None:
            break
        if (
            next_header.version_id != header.version_id
            or next_header.sample_rate != header.sample_rate
        ):
            break

        offset = next_offset

    return bitrates, offset


def sample_bitrates(
    fp: BinaryIO,
    first_frame_offset: int,
    file_size: int,
) -> tuple[list[int], int]:
    samples: list[int] = []

    # 开头连续采样约 200 帧。
    first_samples, _ = sample_sequential(
        fp, first_frame_offset, file_size, frame_limit=200
    )
    samples.extend(first_samples)

    audio_span = max(0, file_size - first_frame_offset)

    # 再检查文件中部多个位置，减少“开头暂时同码率”的误判。
    for fraction in (0.20, 0.40, 0.60, 0.80):
        approximate = first_frame_offset + int(audio_span * fraction)
        found = find_frame(fp, approximate, file_size, max_scan=128 * 1024)
        if found is None:
            continue
        window_samples, _ = sample_sequential(
            fp, found[0], file_size, frame_limit=30
        )
        samples.extend(window_samples)

    return sorted(set(samples)), len(samples)


def inspect_mp3(path: Path) -> Optional[ScanResult]:
    try:
        file_size = path.stat().st_size
        if file_size < 128:
            return None

        with path.open("rb") as fp:
            audio_start, id3_bytes = id3_audio_start(fp)
            found = find_frame(fp, audio_start, file_size)
            if found is None:
                return ScanResult(
                    path=str(path),
                    category="无法识别",
                    marker="",
                    toc=False,
                    sampled_frames=0,
                    bitrates=[],
                    first_frame_offset=-1,
                    id3_bytes=id3_bytes,
                    note="未找到连续合法 MP3 帧",
                )

            frame_offset, first_header = found
            marker, has_toc = inspect_first_frame(fp, frame_offset, first_header)
            bitrates, sampled_frames = sample_bitrates(
                fp, frame_offset, file_size
            )

            is_sampled_vbr = len(bitrates) > 1
            is_sampled_cbr = len(bitrates) == 1 and sampled_frames >= 100

            if marker == "Xing" and has_toc and is_sampled_vbr:
                category = "Xing TOC VBR"
                note = "最适合测试 类型=Xing TOC=1"
            elif marker == "Xing" and has_toc:
                category = "Xing TOC 候选"
                note = "有 Xing+TOC，但采样窗口未发现码率变化"
            elif marker == "" and is_sampled_cbr:
                category = "无索引 CBR"
                note = "无 Xing/Info/VBRI，多个位置采样码率恒定"
            elif marker == "Info":
                category = "Info"
                note = "通常是 CBR，已有 Info 头，不属于无索引 CBR"
            elif marker == "VBRI":
                category = "VBRI"
                note = "适合后续测试 VBRI 支持"
            elif is_sampled_vbr:
                category = "无索引 VBR"
                note = "采样到多个码率，但没有识别到 Xing/Info/VBRI"
            else:
                category = "其他"
                note = "未满足目标测试条件"

            return ScanResult(
                path=str(path),
                category=category,
                marker=marker,
                toc=has_toc,
                sampled_frames=sampled_frames,
                bitrates=bitrates,
                first_frame_offset=frame_offset,
                id3_bytes=id3_bytes,
                note=note,
            )

    except (OSError, PermissionError) as exc:
        return ScanResult(
            path=str(path),
            category="读取失败",
            marker="",
            toc=False,
            sampled_frames=0,
            bitrates=[],
            first_frame_offset=-1,
            id3_bytes=0,
            note=str(exc),
        )


def iter_mp3_files(root: Path):
    for directory, _, filenames in os.walk(root):
        for filename in filenames:
            if filename.lower().endswith(".mp3"):
                yield Path(directory) / filename


def write_text_list(path: Path, results: list[ScanResult]) -> None:
    with path.open("w", encoding="utf-8-sig", newline="\n") as fp:
        for item in results:
            fp.write(item.path)
            fp.write("\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="寻找 Xing TOC VBR 和无索引 CBR MP3 测试文件"
    )
    parser.add_argument("root", help="MP3 根目录，可使用本地路径或 SMB/UNC 路径")
    parser.add_argument(
        "--output",
        default=".",
        help="结果输出目录，默认是当前目录",
    )
    args = parser.parse_args()

    root = Path(args.root)
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    if not root.exists() or not root.is_dir():
        print(f"错误：目录不存在：{root}", file=sys.stderr)
        return 2

    files = list(iter_mp3_files(root))
    print(f"找到 {len(files)} 个 MP3，开始扫描……")

    results: list[ScanResult] = []
    for index, path in enumerate(files, start=1):
        result = inspect_mp3(path)
        if result is not None:
            results.append(result)

        if index % 50 == 0 or index == len(files):
            print(f"\r进度：{index}/{len(files)}", end="", flush=True)

    print()

    csv_path = output_dir / "mp3_seek_candidates.csv"
    with csv_path.open("w", encoding="utf-8-sig", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow([
            "类别",
            "标记",
            "TOC",
            "采样帧数",
            "采样码率_kbps",
            "首帧偏移",
            "ID3字节",
            "说明",
            "路径",
        ])
        for item in results:
            writer.writerow([
                item.category,
                item.marker,
                1 if item.toc else 0,
                item.sampled_frames,
                ",".join(str(value) for value in item.bitrates),
                item.first_frame_offset,
                item.id3_bytes,
                item.note,
                item.path,
            ])

    xing_results = [
        item for item in results if item.category == "Xing TOC VBR"
    ]
    cbr_results = [
        item for item in results if item.category == "无索引 CBR"
    ]
    vbri_results = [
        item for item in results if item.category == "VBRI"
    ]

    write_text_list(output_dir / "xing_toc_vbr.txt", xing_results)
    write_text_list(output_dir / "cbr_no_index.txt", cbr_results)
    write_text_list(output_dir / "vbri_candidates.txt", vbri_results)

    print(f"Xing TOC VBR：{len(xing_results)}")
    print(f"无索引 CBR：{len(cbr_results)}")
    print(f"VBRI 候选：{len(vbri_results)}")
    print(f"完整结果：{csv_path.resolve()}")

    if xing_results:
        print("\n前 5 个 Xing TOC VBR：")
        for item in xing_results[:5]:
            print(f"  {item.path}")

    if cbr_results:
        print("\n前 5 个无索引 CBR：")
        for item in cbr_results[:5]:
            print(f"  {item.path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
