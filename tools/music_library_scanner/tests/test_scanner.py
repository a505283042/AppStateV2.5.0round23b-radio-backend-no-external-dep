from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from scanner_core import (
    COVER_FILE_FALLBACK,
    COVER_FLAC_PICTURE,
    COVER_MP3_APIC,
    catalog_crc32,
    load_index,
    load_manifest,
    scan_library,
    verify_library,
)


def syncsafe(value: int) -> bytes:
    return bytes(
        [
            (value >> 21) & 0x7F,
            (value >> 14) & 0x7F,
            (value >> 7) & 0x7F,
            value & 0x7F,
        ]
    )


def id3_text_frame(frame_id: bytes, value: str) -> bytes:
    body = b"\x03" + value.encode("utf-8")
    return frame_id + struct.pack(">I", len(body)) + b"\0\0" + body


def make_mp3(path: Path, title: str, artist: str, album: str, *, apic: bool) -> None:
    frames = [
        id3_text_frame(b"TIT2", title),
        id3_text_frame(b"TPE1", artist),
        id3_text_frame(b"TALB", album),
    ]
    if apic:
        image = b"\xff\xd8\xff\xe0" + b"JPEGDATA" * 10 + b"\xff\xd9"
        body = b"\x00image/jpeg\x00\x03\x00" + image
        frames.append(b"APIC" + struct.pack(">I", len(body)) + b"\0\0" + body)
    payload = b"".join(frames)
    path.write_bytes(b"ID3\x03\x00\x00" + syncsafe(len(payload)) + payload + b"\x00" * 1024)


def flac_block(block_type: int, data: bytes, *, last: bool = False) -> bytes:
    first = block_type | (0x80 if last else 0)
    return bytes([first]) + len(data).to_bytes(3, "big") + data


def make_flac(path: Path, title: str, artist: str, album: str) -> None:
    vendor = b"scanner-test"
    comments = [
        f"TITLE={title}".encode(),
        f"ARTIST={artist}".encode(),
        f"ALBUM={album}".encode(),
    ]
    vorbis = struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", len(comments))
    for comment in comments:
        vorbis += struct.pack("<I", len(comment)) + comment

    image = b"\x89PNG\r\n\x1a\n" + b"PNGDATA" * 10
    mime = b"image/png"
    picture = (
        struct.pack(">I", 3)
        + struct.pack(">I", len(mime))
        + mime
        + struct.pack(">I", 0)
        + struct.pack(">IIII", 100, 100, 24, 0)
        + struct.pack(">I", len(image))
        + image
    )
    data = b"fLaC" + flac_block(0, b"\0" * 34) + flac_block(4, vorbis) + flac_block(6, picture, last=True)
    path.write_bytes(data + b"\0" * 2048)


class ScannerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / "TF"
        self.album = self.root / "Music" / "Artist A" / "Album A"
        self.album.mkdir(parents=True)
        (self.album / "cover.jpg").write_bytes(b"\xff\xd8" + b"COVER" * 100 + b"\xff\xd9")
        make_mp3(self.album / "01-song.mp3", "歌曲一", "歌手甲/歌手乙", "专辑甲", apic=True)
        make_mp3(self.album / "02-plain.mp3", "歌曲二", "歌手甲", "专辑甲", apic=False)
        make_flac(self.album / "03-song.flac", "歌曲三", "歌手丙", "专辑乙")
        (self.album / "01-song.lrc").write_text("[00:00.00]歌词一\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_full_incremental_modify_delete(self) -> None:
        first = scan_library(self.root)
        self.assertTrue(first.full_scan)
        self.assertEqual(first.track_count, 3)
        self.assertEqual(first.added, 3)

        catalog = load_index(self.root / "System" / "music_index_v3.bin")
        entries, manifest_crc = load_manifest(self.root / "System" / "music_manifest_v1.bin")
        self.assertEqual(len(entries), 3)
        self.assertEqual(catalog_crc32(catalog), manifest_crc)

        by_path = {catalog.track_to_temp(i).audio_rel: catalog.track_to_temp(i) for i in range(3)}
        self.assertEqual(by_path["Artist A/Album A/01-song.mp3"].cover_source, COVER_MP3_APIC)
        self.assertEqual(by_path["Artist A/Album A/02-plain.mp3"].cover_source, COVER_FILE_FALLBACK)
        self.assertEqual(by_path["Artist A/Album A/03-song.flac"].cover_source, COVER_FLAC_PICTURE)
        self.assertEqual(by_path["Artist A/Album A/01-song.mp3"].title, "歌曲一")

        second = scan_library(self.root)
        self.assertFalse(second.full_scan)
        self.assertEqual(second.reused, 3)
        self.assertEqual(second.added, 0)
        self.assertEqual(second.modified, 0)
        self.assertEqual(second.deleted, 0)

        strict = scan_library(self.root, strict_verify=True)
        self.assertFalse(strict.full_scan)
        self.assertTrue(strict.strict_incremental)
        self.assertEqual(strict.reused, 3)

        # 等长修改，验证快速模式仍会完整校验小型歌词文件。
        lrc = self.album / "01-song.lrc"
        old = lrc.read_bytes()
        replacement = old.replace("一".encode("utf-8"), "二".encode("utf-8"))
        self.assertEqual(len(old), len(replacement))
        lrc.write_bytes(replacement)
        third = scan_library(self.root)
        self.assertFalse(third.full_scan)
        self.assertEqual(third.modified, 1)
        self.assertEqual(third.reused, 2)

        (self.album / "03-song.flac").unlink()
        fourth = scan_library(self.root)
        self.assertFalse(fourth.full_scan)
        self.assertEqual(fourth.deleted, 1)
        self.assertEqual(fourth.track_count, 2)

        verified = verify_library(self.root)
        self.assertTrue(verified["ok"])
        self.assertEqual(verified["tracks"], 2)


if __name__ == "__main__":
    unittest.main()
