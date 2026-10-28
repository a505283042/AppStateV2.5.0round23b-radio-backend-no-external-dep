#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""将 PlatformIO 生成的 4 个 ESP32-S3 固件镜像合并为可从 0x0000 烧录的完整镜像。"""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import shlex
import sys
from pathlib import Path

DEFAULT_ENV = "esp32-s3-devkitc-1"
OUTPUT_NAME = "VinylDeck_AppStateV2.5.0round23b_Full.bin"
PARTITIONS_FILE = "partitions_16mb_coredump.csv"

# 烧录参数中的镜像名 -> 合并角色。Arduino 框架下 OTA 初始数据在 flash_args 里
# 叫 boot_app0.bin，ESP-IDF 原生构建里叫 ota_data_initial.bin，两者等价。
ROLE_BY_IMAGE = {
    "bootloader.bin": "bootloader",
    "partitions.bin": "partitions",
    "ota_data_initial.bin": "otadata",
    "boot_app0.bin": "otadata",
    "firmware.bin": "firmware",
}
OTA_INIT_NAMES = ("ota_data_initial.bin", "boot_app0.bin")


def parse_int(value: str) -> int:
    return int(value.strip(), 0)


def parse_partition_offsets(partitions_csv: Path) -> dict[str, int]:
    offsets: dict[str, int] = {}
    with partitions_csv.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.reader(handle):
            if not row or row[0].lstrip().startswith("#") or len(row) < 4:
                continue
            name = row[0].strip()
            offset = row[3].strip()
            if name and offset:
                offsets[name] = parse_int(offset)
    return offsets


def parse_flash_args(build_dir: Path) -> dict[str, int]:
    """优先读取 PlatformIO/ESP-IDF 生成的实际烧录参数，避免硬编码偏移。"""
    required = {"bootloader", "partitions", "otadata", "firmware"}
    result: dict[str, int] = {}

    for name in ("flash_args", "flash_project_args"):
        path = build_dir / name
        if not path.is_file():
            continue

        try:
            tokens = shlex.split(path.read_text(encoding="utf-8", errors="ignore"))
        except OSError:
            continue

        for index in range(len(tokens) - 1):
            token = tokens[index]
            role = ROLE_BY_IMAGE.get(Path(tokens[index + 1]).name)
            if role is None:
                continue
            try:
                result[role] = parse_int(token)
            except ValueError:
                continue

        if required.issubset(result):
            return result

    return result


def resolve_partitions_csv(project_root: Path) -> Path:
    partitions_csv = project_root / PARTITIONS_FILE
    if not partitions_csv.is_file():
        raise FileNotFoundError(f"找不到分区表：{partitions_csv}")
    return partitions_csv


def resolve_offsets(project_root: Path, build_dir: Path) -> tuple[dict[str, int], str]:
    flash_offsets = parse_flash_args(build_dir)
    required = {"bootloader", "partitions", "otadata", "firmware"}
    if required.issubset(flash_offsets):
        return flash_offsets, "PlatformIO flash_args"

    partition_offsets = parse_partition_offsets(resolve_partitions_csv(project_root))
    if "otadata" not in partition_offsets or "app0" not in partition_offsets:
        raise ValueError("分区表缺少 otadata 或 app0，无法安全确定合并偏移")

    # 当前工程为 ESP32-S3 + ESP-IDF 默认启动布局：Bootloader=0x0000，分区表=0x8000。
    # otadata 与 app0 则直接从当前项目分区表读取，避免与工程配置漂移。
    offsets = {
        "bootloader": 0x0000,
        "partitions": 0x8000,
        "otadata": partition_offsets["otadata"],
        "firmware": partition_offsets["app0"],
    }
    return offsets, f"{PARTITIONS_FILE} + ESP32-S3 默认启动偏移"


def find_ota_init_image(build_dir: Path) -> Path:
    """OTA 初始数据镜像：优先构建目录，其次 Arduino 框架包内的 boot_app0.bin。"""
    for name in OTA_INIT_NAMES:
        candidate = build_dir / name
        if candidate.is_file():
            return candidate

    core_dir = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    framework_partitions = (
        core_dir / "packages" / "framework-arduinoespressif32" / "tools" / "partitions"
    )
    for name in OTA_INIT_NAMES:
        candidate = framework_partitions / name
        if candidate.is_file():
            return candidate

    raise FileNotFoundError(
        f"缺少 OTA 初始数据镜像（{' / '.join(OTA_INIT_NAMES)}），"
        f"已查找 {build_dir} 与 {framework_partitions}"
    )


def resolve_image_paths(build_dir: Path) -> dict[str, Path]:
    return {
        "bootloader": build_dir / "bootloader.bin",
        "partitions": build_dir / "partitions.bin",
        "otadata": find_ota_init_image(build_dir),
        "firmware": build_dir / "firmware.bin",
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def merge_images(output_path: Path, offsets: dict[str, int], image_paths: dict[str, Path]) -> None:
    images: list[tuple[int, Path, int]] = []
    for role, offset in offsets.items():
        image_path = image_paths[role]
        if not image_path.is_file():
            raise FileNotFoundError(f"缺少编译产物：{image_path}")
        images.append((offset, image_path, image_path.stat().st_size))

    images.sort(key=lambda item: item[0])

    previous_end = 0
    for offset, path, size in images:
        if offset < previous_end:
            raise ValueError(
                f"镜像发生地址重叠：{path.name} 起始=0x{offset:X}，前一镜像结束=0x{previous_end:X}"
            )
        previous_end = offset + size

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("wb") as output:
        current = 0
        ff_block = b"\xFF" * 65536

        for offset, image_path, _ in images:
            gap = offset - current
            while gap > 0:
                chunk_size = min(gap, len(ff_block))
                output.write(ff_block[:chunk_size])
                gap -= chunk_size
                current += chunk_size

            with image_path.open("rb") as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    output.write(chunk)
                    current += len(chunk)

    print("VinylDeck 完整固件合并完成")
    print(f"输出文件：{output_path}")
    print()
    for offset, image_path, size in images:
        print(
            f"  0x{offset:06X}  {image_path.name:<22} "
            f"{size:>9,} B  结束=0x{offset + size:06X}"
        )
    print()
    print(f"合并后大小：{output_path.stat().st_size:,} B (0x{output_path.stat().st_size:X})")
    print(f"SHA256：{sha256_file(output_path)}")
    print("烧录起始地址：0x0000")


def main() -> int:
    script_path = Path(__file__).resolve()
    project_root = script_path.parents[2]

    parser = argparse.ArgumentParser(description="合并 VinylDeck ESP32-S3 完整固件")
    parser.add_argument("--env", default=DEFAULT_ENV, help=f"PlatformIO 环境名，默认 {DEFAULT_ENV}")
    parser.add_argument(
        "--build-dir",
        type=Path,
        help="编译输出目录，默认 .pio/build/<env>",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help=f"输出文件，默认写入项目根目录/{OUTPUT_NAME}",
    )
    args = parser.parse_args()

    build_dir = (args.build_dir or (project_root / ".pio" / "build" / args.env)).resolve()
    output_path = (args.output or (project_root / OUTPUT_NAME)).resolve()

    try:
        offsets, source = resolve_offsets(project_root, build_dir)
        image_paths = resolve_image_paths(build_dir)
        print(f"偏移来源：{source}")
        merge_images(output_path, offsets, image_paths)
    except (FileNotFoundError, ValueError, OSError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
