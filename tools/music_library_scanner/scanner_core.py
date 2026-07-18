from __future__ import annotations

import csv
import json
import os
import re
import shutil
import struct
import time
from datetime import datetime
import unicodedata
import zlib
from dataclasses import asdict, dataclass, field
from pathlib import Path, PurePosixPath
from typing import Callable, Iterable, Optional

# 与设备端 storage_types_v3.h / storage_manifest_v1.cpp 保持一致。
INDEX_V3_MAGIC = 0x4D494458  # MIDX
INDEX_V3_VERSION = 3
INDEX_V3_FLAG_CRC32 = 1 << 0
INDEX_V3_HEADER_SIZE = 36
INDEX_V3_SECTION_SIZE = 12
INDEX_V3_SECTION_COUNT = 4

SEC_V3_STR_POOL = 1
SEC_V3_ARTISTS = 2
SEC_V3_ALBUMS = 3
SEC_V3_TRACKS = 4

MANIFEST_MAGIC = 0x31464E4D  # MNF1
MANIFEST_LEGACY_VERSION = 1
MANIFEST_VERSION = 2
MANIFEST_FLAG_CRC32 = 1 << 0
MANIFEST_FLAG_CATALOG_CRC32 = 1 << 1
MANIFEST_HEADER_SIZE = 28
MANIFEST_FULL_CRC_MAX_BYTES = 16 * 1024
MANIFEST_SAMPLE_BYTES = 512

INVALID_OFF32 = 0
INVALID_ID32 = 0xFFFFFFFF

COVER_NONE = 0
COVER_MP3_APIC = 1
COVER_FLAC_PICTURE = 2
COVER_FILE_FALLBACK = 3

EXT_UNKNOWN = 0
EXT_MP3 = 1
EXT_FLAC = 2

TF_HAS_LRC = 1 << 0
TF_HAS_EMBED_COVER = 1 << 1
TF_HAS_FILE_COVER = 1 << 2
TF_IS_MP3 = 1 << 3
TF_IS_FLAC = 1 << 4

ARTIST_STRUCT = struct.Struct("<I")
ALBUM_STRUCT = struct.Struct("<III")
# 设备端 TrackRowV3 大小为 44 字节，末尾有 2 字节对齐填充。
TRACK_STRUCT = struct.Struct("<9IBBHH2x")
INDEX_HEADER_STRUCT = struct.Struct("<IHHIIIIIII")
SECTION_STRUCT = struct.Struct("<III")
MANIFEST_HEADER_STRUCT = struct.Struct("<IHHIIIII")
FINGERPRINT_V1_STRUCT = struct.Struct("<BIIII")
FINGERPRINT_V2_STRUCT = struct.Struct("<BBIHHIII")

if ARTIST_STRUCT.size != 4 or ALBUM_STRUCT.size != 12 or TRACK_STRUCT.size != 44:
    raise RuntimeError("V3 磁盘结构尺寸异常")
if FINGERPRINT_V1_STRUCT.size != 17 or FINGERPRINT_V2_STRUCT.size != 22:
    raise RuntimeError("Manifest 指纹结构尺寸异常")

ProgressCallback = Callable[["ScanProgress"], None]
LogCallback = Callable[[str], None]


class ScanError(RuntimeError):
    pass


class ScanCancelled(ScanError):
    pass


@dataclass(slots=True)
class FileFingerprint:
    present: bool = False
    attributes_valid: bool = False
    size: int = 0
    modify_date: int = 0
    modify_time: int = 0
    head_crc: int = 0
    middle_crc: int = 0
    tail_crc: int = 0

    def to_bytes(self) -> bytes:
        return FINGERPRINT_V2_STRUCT.pack(
            1 if self.present else 0,
            1 if self.attributes_valid else 0,
            self.size,
            self.modify_date,
            self.modify_time,
            self.head_crc,
            self.middle_crc,
            self.tail_crc,
        )

    @classmethod
    def from_bytes(
        cls, data: bytes, offset: int, version: int
    ) -> tuple["FileFingerprint", int]:
        if version <= MANIFEST_LEGACY_VERSION:
            present, size, head, middle, tail = FINGERPRINT_V1_STRUCT.unpack_from(
                data, offset
            )
            return (
                cls(
                    present=bool(present),
                    size=size,
                    head_crc=head,
                    middle_crc=middle,
                    tail_crc=tail,
                ),
                offset + FINGERPRINT_V1_STRUCT.size,
            )

        present, attributes_valid, size, modify_date, modify_time, head, middle, tail = (
            FINGERPRINT_V2_STRUCT.unpack_from(data, offset)
        )
        return (
            cls(
                present=bool(present),
                attributes_valid=bool(attributes_valid),
                size=size,
                modify_date=modify_date,
                modify_time=modify_time,
                head_crc=head,
                middle_crc=middle,
                tail_crc=tail,
            ),
            offset + FINGERPRINT_V2_STRUCT.size,
        )


@dataclass(slots=True)
class ManifestEntry:
    audio_rel: str
    audio_fingerprint: FileFingerprint
    lrc_fingerprint: FileFingerprint
    cover_fingerprint: FileFingerprint


@dataclass(slots=True)
class TrackTemp:
    title: str = ""
    artist: str = ""
    album: str = ""
    audio_rel: str = ""
    lrc_rel: str = ""
    cover_path_rel: str = ""
    cover_mime: str = ""
    cover_offset: int = 0
    cover_size: int = 0
    cover_source: int = COVER_NONE
    ext_code: int = EXT_UNKNOWN
    flags: int = 0


@dataclass(slots=True)
class ArtistRow:
    name_off: int


@dataclass(slots=True)
class AlbumRow:
    name_off: int
    primary_artist_off: int
    folder_cover_off: int


@dataclass(slots=True)
class TrackRow:
    title_off: int
    artist_off: int
    album_id: int
    audio_rel_off: int
    lrc_rel_off: int
    cover_path_off: int
    cover_offset: int
    cover_size: int
    mime_off: int
    cover_source: int
    ext_code: int
    flags: int
    reserved: int = 0


@dataclass(slots=True)
class Catalog:
    pool: bytes = b"\0"
    artists: list[ArtistRow] = field(default_factory=list)
    albums: list[AlbumRow] = field(default_factory=list)
    tracks: list[TrackRow] = field(default_factory=list)

    def pool_text(self, offset: int) -> str:
        if offset == INVALID_OFF32:
            return ""
        if offset < 0 or offset >= len(self.pool):
            raise ScanError(f"字符串池偏移越界: {offset}")
        end = self.pool.find(b"\0", offset)
        if end < 0:
            raise ScanError(f"字符串池缺少终止符: {offset}")
        return self.pool[offset:end].decode("utf-8", errors="replace")

    def track_to_temp(self, track_index: int) -> TrackTemp:
        row = self.tracks[track_index]
        album = ""
        if row.album_id != INVALID_ID32 and row.album_id < len(self.albums):
            album = self.pool_text(self.albums[row.album_id].name_off)
        return TrackTemp(
            title=self.pool_text(row.title_off),
            artist=self.pool_text(row.artist_off),
            album=album,
            audio_rel=self.pool_text(row.audio_rel_off),
            lrc_rel=self.pool_text(row.lrc_rel_off),
            cover_path_rel=self.pool_text(row.cover_path_off),
            cover_mime=self.pool_text(row.mime_off),
            cover_offset=row.cover_offset,
            cover_size=row.cover_size,
            cover_source=row.cover_source,
            ext_code=row.ext_code,
            flags=row.flags,
        )


@dataclass(slots=True)
class ScanProgress:
    phase: str
    message: str = ""
    current_path: str = ""
    processed: int = 0
    total: int = 0
    discovered: int = 0
    reused: int = 0
    added: int = 0
    modified: int = 0
    deleted: int = 0


@dataclass(slots=True)
class ScanResult:
    success: bool
    full_scan: bool
    forced_full_scan: bool
    strict_incremental: bool
    music_root: str
    system_root: str
    discovered: int
    reused: int
    added: int
    modified: int
    deleted: int
    track_count: int
    album_count: int
    artist_count: int
    index_size: int
    manifest_size: int
    elapsed_seconds: float
    index_path: str
    manifest_path: str
    report_path: str


@dataclass(slots=True)
class DiscoveredAudio:
    full_path: Path
    audio_rel: str
    filename: str
    fallback_artist: str
    fallback_album: str
    effective_cover: Optional[Path]


@dataclass(slots=True)
class ParsedCover:
    found: bool = False
    offset: int = 0
    size: int = 0
    mime: str = ""


@dataclass(slots=True)
class LoadedPair:
    catalog: Optional[Catalog] = None
    manifest_entries: dict[str, ManifestEntry] = field(default_factory=dict)
    valid_for_incremental: bool = False
    source_note: str = ""


def _default_log(_: str) -> None:
    pass


def _default_progress(_: ScanProgress) -> None:
    pass


def _check_cancel(cancel_event: object | None) -> None:
    if cancel_event is not None and bool(getattr(cancel_event, "is_set")()):
        raise ScanCancelled("用户取消扫描")


def _normalize_text(value: str) -> str:
    # 路径和标签统一 NFC，避免 Windows/macOS 的 Unicode 组合形式导致重复条目。
    return unicodedata.normalize("NFC", value).strip()


def _normalize_rel(path: Path, root: Path) -> str:
    rel = path.relative_to(root)
    return PurePosixPath(*rel.parts).as_posix()


def resolve_card_root(selected: str | os.PathLike[str]) -> tuple[Path, Path, Path]:
    path = Path(selected).expanduser().resolve()
    if not path.exists() or not path.is_dir():
        raise ScanError(f"目录不存在: {path}")

    if path.name.casefold() == "music":
        card_root = path.parent
        music_root = path
    else:
        card_root = path
        music_root = card_root / "Music"

    if not music_root.exists() or not music_root.is_dir():
        raise ScanError(f"未找到 Music 目录: {music_root}")

    system_root = card_root / "System"
    return card_root, music_root, system_root


def _ascii_fold_bytes(value: str) -> bytes:
    raw = value.encode("utf-8", errors="replace")
    return bytes((byte + 32 if 65 <= byte <= 90 else byte) for byte in raw)


def _trim_ascii(value: str) -> str:
    return value.strip(" \t")


def _primary_artist(value: str) -> str:
    value = _trim_ascii(value)
    if "/" in value:
        value = value.split("/", 1)[0]
    value = _trim_ascii(value)
    return value or "未知歌手"


def _track_number_hint(audio_rel: str) -> int:
    stem = PurePosixPath(audio_rel).stem.lstrip(" \t")
    match = re.match(r"(\d+)", stem)
    return int(match.group(1)) if match else 0x7FFFFFFF


def _track_sort_key(track: TrackTemp) -> tuple[bytes, bytes, int, bytes, bytes]:
    artist = _primary_artist(track.artist)
    album = _trim_ascii(track.album) or "未知专辑"
    return (
        _ascii_fold_bytes(artist),
        _ascii_fold_bytes(album),
        _track_number_hint(track.audio_rel),
        _ascii_fold_bytes(_trim_ascii(track.title)),
        _ascii_fold_bytes(_trim_ascii(track.audio_rel)),
    )


def _make_flags(track: TrackTemp) -> int:
    flags = 0
    if track.lrc_rel:
        flags |= TF_HAS_LRC
    if track.cover_source in (COVER_MP3_APIC, COVER_FLAC_PICTURE):
        flags |= TF_HAS_EMBED_COVER
    if track.cover_source == COVER_FILE_FALLBACK and track.cover_path_rel:
        flags |= TF_HAS_FILE_COVER
    if track.ext_code == EXT_MP3:
        flags |= TF_IS_MP3
    if track.ext_code == EXT_FLAC:
        flags |= TF_IS_FLAC
    return flags


class _StringPoolBuilder:
    def __init__(self) -> None:
        self.blob = bytearray(b"\0")
        self.offsets: dict[str, int] = {}

    def intern(self, value: str) -> int:
        # 字符池保留路径和显示文本本身，不在这里擅自裁剪文件名。
        value = unicodedata.normalize("NFC", value)
        if not value:
            return INVALID_OFF32
        existing = self.offsets.get(value)
        if existing is not None:
            return existing
        encoded = value.encode("utf-8", errors="replace")
        offset = len(self.blob)
        self.blob.extend(encoded)
        self.blob.append(0)
        self.offsets[value] = offset
        return offset


def build_catalog(tracks: Iterable[TrackTemp]) -> Catalog:
    sorted_tracks = sorted(tracks, key=_track_sort_key)
    if not sorted_tracks:
        raise ScanError("没有可构建的音频曲目")
    if len(sorted_tracks) > 0xFFFF:
        sorted_tracks = sorted_tracks[:0xFFFF]

    pool = _StringPoolBuilder()
    artists: list[ArtistRow] = []
    albums: list[AlbumRow] = []
    track_rows: list[TrackRow] = []
    artist_map: dict[str, int] = {}
    album_map: dict[tuple[str, str, str], int] = {}

    for track in sorted_tracks:
        primary_artist = _primary_artist(track.artist)
        album_name = _trim_ascii(track.album) or "未知专辑"
        folder_cover = track.cover_path_rel

        if primary_artist not in artist_map:
            artist_map[primary_artist] = len(artists)
            artists.append(ArtistRow(pool.intern(primary_artist)))

        album_key = (album_name, primary_artist, folder_cover)
        album_id = album_map.get(album_key)
        if album_id is None:
            album_id = len(albums)
            album_map[album_key] = album_id
            albums.append(
                AlbumRow(
                    pool.intern(album_name),
                    pool.intern(primary_artist),
                    pool.intern(folder_cover),
                )
            )

        track.flags = _make_flags(track)
        track_rows.append(
            TrackRow(
                title_off=pool.intern(track.title),
                artist_off=pool.intern(track.artist),
                album_id=album_id,
                audio_rel_off=pool.intern(track.audio_rel),
                lrc_rel_off=pool.intern(track.lrc_rel),
                cover_path_off=pool.intern(track.cover_path_rel),
                cover_offset=track.cover_offset,
                cover_size=track.cover_size,
                mime_off=pool.intern(track.cover_mime),
                cover_source=track.cover_source,
                ext_code=track.ext_code,
                flags=track.flags,
            )
        )

    return Catalog(bytes(pool.blob), artists, albums, track_rows)


def _serialize_artists(catalog: Catalog) -> bytes:
    return b"".join(ARTIST_STRUCT.pack(row.name_off) for row in catalog.artists)


def _serialize_albums(catalog: Catalog) -> bytes:
    return b"".join(
        ALBUM_STRUCT.pack(row.name_off, row.primary_artist_off, row.folder_cover_off)
        for row in catalog.albums
    )


def _serialize_tracks(catalog: Catalog) -> bytes:
    return b"".join(
        TRACK_STRUCT.pack(
            row.title_off,
            row.artist_off,
            row.album_id,
            row.audio_rel_off,
            row.lrc_rel_off,
            row.cover_path_off,
            row.cover_offset,
            row.cover_size,
            row.mime_off,
            row.cover_source,
            row.ext_code,
            row.flags,
            row.reserved,
        )
        for row in catalog.tracks
    )


def catalog_crc32(catalog: Catalog) -> int:
    counts = struct.pack(
        "<4I",
        len(catalog.tracks),
        len(catalog.albums),
        len(catalog.artists),
        len(catalog.pool),
    )
    crc_data = (
        counts
        + catalog.pool
        + _serialize_tracks(catalog)
        + _serialize_albums(catalog)
        + _serialize_artists(catalog)
    )
    return zlib.crc32(crc_data) & 0xFFFFFFFF


def serialize_index(catalog: Catalog) -> bytes:
    pool_blob = catalog.pool
    artist_blob = _serialize_artists(catalog)
    album_blob = _serialize_albums(catalog)
    track_blob = _serialize_tracks(catalog)

    cursor = INDEX_V3_HEADER_SIZE + INDEX_V3_SECTION_COUNT * INDEX_V3_SECTION_SIZE
    sections = [
        (SEC_V3_STR_POOL, cursor, len(pool_blob)),
    ]
    cursor += len(pool_blob)
    sections.append((SEC_V3_ARTISTS, cursor, len(artist_blob)))
    cursor += len(artist_blob)
    sections.append((SEC_V3_ALBUMS, cursor, len(album_blob)))
    cursor += len(album_blob)
    sections.append((SEC_V3_TRACKS, cursor, len(track_blob)))

    section_blob = b"".join(SECTION_STRUCT.pack(*item) for item in sections)
    payload = section_blob + pool_blob + artist_blob + album_blob + track_blob
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = INDEX_HEADER_STRUCT.pack(
        INDEX_V3_MAGIC,
        INDEX_V3_VERSION,
        INDEX_V3_FLAG_CRC32,
        INDEX_V3_HEADER_SIZE,
        INDEX_V3_SECTION_COUNT,
        len(catalog.tracks),
        len(catalog.albums),
        len(catalog.artists),
        len(pool_blob),
        crc,
    )
    return header + payload


def load_index(path: Path) -> Catalog:
    data = path.read_bytes()
    if len(data) < INDEX_V3_HEADER_SIZE + INDEX_V3_SECTION_COUNT * INDEX_V3_SECTION_SIZE:
        raise ScanError(f"索引过短: {path}")

    (
        magic,
        version,
        flags,
        header_size,
        section_count,
        track_count,
        album_count,
        artist_count,
        pool_size,
        expected_crc,
    ) = INDEX_HEADER_STRUCT.unpack_from(data, 0)

    if magic != INDEX_V3_MAGIC or version != INDEX_V3_VERSION:
        raise ScanError(f"索引版本不兼容: {path}")
    if header_size != INDEX_V3_HEADER_SIZE or section_count != INDEX_V3_SECTION_COUNT:
        raise ScanError(f"索引头尺寸不兼容: {path}")
    if (flags & INDEX_V3_FLAG_CRC32) == 0:
        raise ScanError(f"索引缺少 CRC: {path}")
    actual_crc = zlib.crc32(data[header_size:]) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise ScanError(
            f"索引 CRC 错误: expected=0x{expected_crc:08X} actual=0x{actual_crc:08X}"
        )

    sections: dict[int, tuple[int, int]] = {}
    offset = header_size
    for _ in range(section_count):
        sec_type, sec_offset, sec_size = SECTION_STRUCT.unpack_from(data, offset)
        offset += SECTION_STRUCT.size
        if sec_offset + sec_size > len(data):
            raise ScanError("索引区段越界")
        sections[sec_type] = (sec_offset, sec_size)

    required = {SEC_V3_STR_POOL, SEC_V3_ARTISTS, SEC_V3_ALBUMS, SEC_V3_TRACKS}
    if set(sections) != required:
        raise ScanError("索引区段缺失或重复")

    pool_off, pool_len = sections[SEC_V3_STR_POOL]
    artist_off, artist_len = sections[SEC_V3_ARTISTS]
    album_off, album_len = sections[SEC_V3_ALBUMS]
    track_off, track_len = sections[SEC_V3_TRACKS]

    if pool_len != pool_size:
        raise ScanError("字符串池尺寸与文件头不一致")
    if artist_len != artist_count * ARTIST_STRUCT.size:
        raise ScanError("歌手表尺寸不一致")
    if album_len != album_count * ALBUM_STRUCT.size:
        raise ScanError("专辑表尺寸不一致")
    if track_len != track_count * TRACK_STRUCT.size:
        raise ScanError("歌曲表尺寸不一致")

    pool = data[pool_off : pool_off + pool_len]
    if not pool or pool[0] != 0:
        raise ScanError("字符串池缺少空串保留项")

    artists = [
        ArtistRow(ARTIST_STRUCT.unpack_from(data, artist_off + i * ARTIST_STRUCT.size)[0])
        for i in range(artist_count)
    ]
    albums = [
        AlbumRow(*ALBUM_STRUCT.unpack_from(data, album_off + i * ALBUM_STRUCT.size))
        for i in range(album_count)
    ]
    tracks: list[TrackRow] = []
    for i in range(track_count):
        values = TRACK_STRUCT.unpack_from(data, track_off + i * TRACK_STRUCT.size)
        tracks.append(
            TrackRow(
                title_off=values[0],
                artist_off=values[1],
                album_id=values[2],
                audio_rel_off=values[3],
                lrc_rel_off=values[4],
                cover_path_off=values[5],
                cover_offset=values[6],
                cover_size=values[7],
                mime_off=values[8],
                cover_source=values[9],
                ext_code=values[10],
                flags=values[11],
                reserved=values[12],
            )
        )

    catalog = Catalog(pool, artists, albums, tracks)
    _validate_catalog(catalog)
    return catalog


def _validate_catalog(catalog: Catalog) -> None:
    offsets: set[int] = {0}
    cursor = 1
    while cursor < len(catalog.pool):
        offsets.add(cursor)
        end = catalog.pool.find(b"\0", cursor)
        if end < 0:
            raise ScanError("字符串池末尾不完整")
        cursor = end + 1

    def check_off(value: int) -> None:
        if value not in offsets:
            raise ScanError(f"字符串池偏移无效: {value}")

    for row in catalog.artists:
        check_off(row.name_off)
    for row in catalog.albums:
        check_off(row.name_off)
        check_off(row.primary_artist_off)
        check_off(row.folder_cover_off)
    for row in catalog.tracks:
        for value in (
            row.title_off,
            row.artist_off,
            row.audio_rel_off,
            row.lrc_rel_off,
            row.cover_path_off,
            row.mime_off,
        ):
            check_off(value)
        if row.album_id == INVALID_ID32 or row.album_id >= len(catalog.albums):
            raise ScanError("歌曲专辑 ID 无效")
        if not catalog.pool_text(row.audio_rel_off):
            raise ScanError("歌曲音频路径为空")


def serialize_manifest(entries: Iterable[ManifestEntry], catalog_crc: int) -> bytes:
    sorted_entries = sorted(entries, key=lambda item: item.audio_rel.encode("utf-8"))
    payload = bytearray()
    for entry in sorted_entries:
        encoded = entry.audio_rel.encode("utf-8")
        if not encoded or len(encoded) > 4096 or len(encoded) > 0xFFFF:
            raise ScanError(f"Manifest 路径长度无效: {entry.audio_rel}")
        payload.extend(struct.pack("<H", len(encoded)))
        payload.extend(encoded)
        payload.extend(entry.audio_fingerprint.to_bytes())
        payload.extend(entry.lrc_fingerprint.to_bytes())
        payload.extend(entry.cover_fingerprint.to_bytes())

    payload_crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = MANIFEST_HEADER_STRUCT.pack(
        MANIFEST_MAGIC,
        MANIFEST_VERSION,
        MANIFEST_FLAG_CRC32 | MANIFEST_FLAG_CATALOG_CRC32,
        MANIFEST_HEADER_SIZE,
        len(sorted_entries),
        len(payload),
        payload_crc,
        catalog_crc,
    )
    return header + payload


def load_manifest(path: Path) -> tuple[list[ManifestEntry], int]:
    data = path.read_bytes()
    if len(data) < MANIFEST_HEADER_SIZE:
        raise ScanError("Manifest 文件过短")
    magic, version, flags, header_size, count, payload_size, expected_crc, cat_crc = (
        MANIFEST_HEADER_STRUCT.unpack_from(data, 0)
    )
    if magic != MANIFEST_MAGIC or version not in (MANIFEST_LEGACY_VERSION, MANIFEST_VERSION):
        raise ScanError("Manifest 版本不兼容")
    if header_size != MANIFEST_HEADER_SIZE:
        raise ScanError("Manifest 缺少 Catalog CRC，需执行全量扫描升级")
    if (flags & (MANIFEST_FLAG_CRC32 | MANIFEST_FLAG_CATALOG_CRC32)) != (
        MANIFEST_FLAG_CRC32 | MANIFEST_FLAG_CATALOG_CRC32
    ):
        raise ScanError("Manifest 标志不完整")
    if len(data) != header_size + payload_size:
        raise ScanError("Manifest 文件尺寸不一致")
    payload = data[header_size:]
    actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise ScanError("Manifest CRC 错误")

    entries: list[ManifestEntry] = []
    offset = 0
    for _ in range(count):
        if offset + 2 > len(payload):
            raise ScanError("Manifest 路径长度越界")
        length = struct.unpack_from("<H", payload, offset)[0]
        offset += 2
        if length == 0 or offset + length > len(payload):
            raise ScanError("Manifest 路径越界")
        audio_rel = payload[offset : offset + length].decode("utf-8", errors="strict")
        offset += length
        audio_fp, offset = FileFingerprint.from_bytes(payload, offset, version)
        lrc_fp, offset = FileFingerprint.from_bytes(payload, offset, version)
        cover_fp, offset = FileFingerprint.from_bytes(payload, offset, version)
        entries.append(ManifestEntry(audio_rel, audio_fp, lrc_fp, cover_fp))
    if offset != len(payload):
        raise ScanError("Manifest 存在未解析数据")
    entries.sort(key=lambda item: item.audio_rel.encode("utf-8"))
    if len({entry.audio_rel for entry in entries}) != len(entries):
        raise ScanError("Manifest 存在重复路径")
    return entries, cat_crc


def _atomic_write(path: Path, data: bytes, verifier: Callable[[Path], None]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(str(path) + ".tmp")
    backup = Path(str(path) + ".bak")

    if tmp.exists():
        tmp.unlink()
    with tmp.open("wb") as file:
        file.write(data)
        file.flush()
        os.fsync(file.fileno())
    verifier(tmp)

    had_final = path.exists()
    if backup.exists():
        backup.unlink()
    if had_final:
        os.replace(path, backup)
    try:
        os.replace(tmp, path)
        verifier(path)
    except Exception:
        if path.exists():
            path.unlink()
        if had_final and backup.exists():
            os.replace(backup, path)
        raise


def _decode_legacy_bytes(data: bytes) -> str:
    data = data.split(b"\0", 1)[0].rstrip(b" \0")
    if not data:
        return ""
    for encoding in ("utf-8", "gb18030", "cp1252", "latin-1"):
        try:
            return _normalize_text(data.decode(encoding))
        except UnicodeDecodeError:
            continue
    return _normalize_text(data.decode("latin-1", errors="replace"))


def _syncsafe_u32(data: bytes) -> int:
    return ((data[0] & 0x7F) << 21) | ((data[1] & 0x7F) << 14) | ((data[2] & 0x7F) << 7) | (data[3] & 0x7F)


def _u32_be(data: bytes) -> int:
    return struct.unpack(">I", data)[0]


def _decode_id3_text(data: bytes) -> str:
    if not data:
        return ""
    encoding = data[0]
    payload = data[1:]
    try:
        if encoding == 0:
            return _decode_legacy_bytes(payload)
        if encoding == 3:
            return _normalize_text(payload.split(b"\0", 1)[0].decode("utf-8", errors="replace"))
        if encoding == 1:
            # utf-16 自动识别 BOM；无 BOM 时以小端兜底。
            if payload.startswith((b"\xff\xfe", b"\xfe\xff")):
                return _normalize_text(payload.decode("utf-16", errors="replace").split("\0", 1)[0])
            return _normalize_text(payload.decode("utf-16-le", errors="replace").split("\0", 1)[0])
        if encoding == 2:
            return _normalize_text(payload.decode("utf-16-be", errors="replace").split("\0", 1)[0])
    except Exception:
        return ""
    return ""


def read_id3_basic(path: Path) -> tuple[str, str, str]:
    title = artist = album = ""
    with path.open("rb") as file:
        header = file.read(10)
        if len(header) == 10 and header[:3] == b"ID3":
            version = header[3]
            flags = header[5]
            tag_size = _syncsafe_u32(header[6:10])
            position = 10
            if flags & 0x40:
                extended = file.read(4)
                if len(extended) == 4:
                    ext_size = _syncsafe_u32(extended) if version == 4 else _u32_be(extended)
                    position += 4 + ext_size if version == 4 else ext_size
                    file.seek(position)
            end = min(10 + tag_size, 10 + 64 * 1024)
            wanted = {b"TIT2": "title", b"TPE1": "artist", b"TALB": "album"}
            while position + 10 <= end:
                frame_header = file.read(10)
                if len(frame_header) != 10 or frame_header[:4] == b"\0\0\0\0":
                    break
                frame_id = frame_header[:4]
                frame_size = _syncsafe_u32(frame_header[4:8]) if version == 4 else _u32_be(frame_header[4:8])
                position += 10
                if frame_size <= 0 or position + frame_size > end:
                    break
                if frame_id in wanted:
                    content = file.read(min(frame_size, 512))
                    value = _decode_id3_text(content)
                    if frame_id == b"TIT2" and not title:
                        title = value
                    elif frame_id == b"TPE1" and not artist:
                        artist = value
                    elif frame_id == b"TALB" and not album:
                        album = value
                    if frame_size > len(content):
                        file.seek(frame_size - len(content), os.SEEK_CUR)
                else:
                    file.seek(frame_size, os.SEEK_CUR)
                position += frame_size
                if title and artist and album:
                    break

        # 与设备端一致：无论是否读到 ID3v2，都使用 ID3v1 补空字段。
        file.seek(0, os.SEEK_END)
        size = file.tell()
        if size >= 128:
            file.seek(size - 128)
            tag = file.read(128)
            if tag[:3] == b"TAG":
                if not title:
                    title = _decode_legacy_bytes(tag[3:33])
                if not artist:
                    artist = _decode_legacy_bytes(tag[33:63])
                if not album:
                    album = _decode_legacy_bytes(tag[63:93])
    return title, artist, album


def _skip_id3_description(data: bytes, encoding: int, start: int) -> int:
    if encoding in (0, 3):
        end = data.find(b"\0", start)
        return len(data) if end < 0 else end + 1
    cursor = start
    while cursor + 1 < len(data):
        if data[cursor] == 0 and data[cursor + 1] == 0:
            return cursor + 2
        cursor += 2
    return len(data)


def find_mp3_apic(path: Path) -> ParsedCover:
    with path.open("rb") as file:
        header = file.read(10)
        if len(header) != 10 or header[:3] != b"ID3":
            return ParsedCover()
        version = header[3]
        flags = header[5]
        tag_size = _syncsafe_u32(header[6:10])
        position = 10
        if flags & 0x40:
            extended = file.read(4)
            if len(extended) != 4:
                return ParsedCover()
            ext_size = _syncsafe_u32(extended) if version == 4 else _u32_be(extended)
            position += 4 + ext_size if version == 4 else ext_size
            file.seek(position)
        end = min(10 + tag_size, 10 + 512 * 1024)
        while position + 10 <= end:
            frame_header = file.read(10)
            if len(frame_header) != 10 or frame_header[:4] == b"\0\0\0\0":
                break
            frame_id = frame_header[:4]
            frame_size = _syncsafe_u32(frame_header[4:8]) if version == 4 else _u32_be(frame_header[4:8])
            position += 10
            if frame_size <= 0 or position + frame_size > end:
                break
            if frame_id != b"APIC":
                file.seek(frame_size, os.SEEK_CUR)
                position += frame_size
                continue
            if version == 4 and (frame_header[8] & 0x48):
                file.seek(frame_size, os.SEEK_CUR)
                position += frame_size
                continue
            frame_start = file.tell()
            content = file.read(frame_size)
            if len(content) != frame_size or not content:
                return ParsedCover()
            encoding = content[0]
            mime_end = content.find(b"\0", 1)
            if mime_end < 0 or mime_end + 1 >= len(content):
                return ParsedCover()
            mime = content[1:mime_end].decode("latin-1", errors="replace")
            description_start = mime_end + 2  # 跳过 picture type
            image_start = _skip_id3_description(content, encoding, description_start)
            if image_start >= len(content):
                return ParsedCover()
            return ParsedCover(True, frame_start + image_start, len(content) - image_start, mime)
    return ParsedCover()


def _read_u24_be(data: bytes) -> int:
    return (data[0] << 16) | (data[1] << 8) | data[2]


def read_flac_basic(path: Path) -> tuple[str, str, str]:
    title = artist = album = ""
    with path.open("rb") as file:
        if file.read(4) != b"fLaC":
            return title, artist, album
        for _ in range(64):
            header = file.read(4)
            if len(header) != 4:
                break
            is_last = bool(header[0] & 0x80)
            block_type = header[0] & 0x7F
            length = _read_u24_be(header[1:])
            block = file.read(length)
            if len(block) != length:
                break
            if block_type == 4 and length >= 8:
                cursor = 0
                vendor_len = struct.unpack_from("<I", block, cursor)[0]
                cursor += 4 + vendor_len
                if cursor + 4 > len(block):
                    break
                count = struct.unpack_from("<I", block, cursor)[0]
                cursor += 4
                for _comment in range(count):
                    if cursor + 4 > len(block):
                        break
                    item_len = struct.unpack_from("<I", block, cursor)[0]
                    cursor += 4
                    if cursor + item_len > len(block):
                        break
                    raw = block[cursor : cursor + min(item_len, 1024)]
                    cursor += item_len
                    text = raw.decode("utf-8", errors="replace")
                    if "=" not in text:
                        continue
                    key, value = text.split("=", 1)
                    value = _normalize_text(value)
                    upper = key.upper()
                    if upper == "TITLE" and not title:
                        title = value
                    elif upper == "ARTIST" and not artist:
                        artist = value
                    elif upper == "ALBUM" and not album:
                        album = value
                    if title and artist and album:
                        break
                break
            if is_last:
                break
    return title, artist, album


def find_flac_picture(path: Path) -> ParsedCover:
    fallback = ParsedCover()
    with path.open("rb") as file:
        if file.read(4) != b"fLaC":
            return ParsedCover()
        while True:
            header = file.read(4)
            if len(header) != 4:
                break
            is_last = bool(header[0] & 0x80)
            block_type = header[0] & 0x7F
            length = _read_u24_be(header[1:])
            block_start = file.tell()
            if block_type != 6:
                file.seek(length, os.SEEK_CUR)
            else:
                block = file.read(length)
                if len(block) != length:
                    break
                cursor = 0
                try:
                    picture_type = struct.unpack_from(">I", block, cursor)[0]
                    cursor += 4
                    mime_len = struct.unpack_from(">I", block, cursor)[0]
                    cursor += 4
                    mime = block[cursor : cursor + mime_len].decode("latin-1", errors="replace")
                    cursor += mime_len
                    desc_len = struct.unpack_from(">I", block, cursor)[0]
                    cursor += 4 + desc_len
                    cursor += 16  # width/height/depth/colors
                    data_len = struct.unpack_from(">I", block, cursor)[0]
                    cursor += 4
                except (struct.error, UnicodeDecodeError):
                    break
                if data_len > 0 and cursor + data_len <= len(block):
                    candidate = ParsedCover(True, block_start + cursor, data_len, mime)
                    if picture_type == 3:
                        return candidate
                    if not fallback.found:
                        fallback = candidate
            if is_last:
                break
    return fallback


def _fat_datetime(timestamp: float) -> tuple[int, int]:
    value = datetime.fromtimestamp(timestamp)
    year = min(2107, max(1980, value.year))
    fat_date = ((year - 1980) << 9) | (value.month << 5) | value.day
    fat_time = (value.hour << 11) | (value.minute << 5) | (value.second // 2)
    return fat_date, fat_time


def _file_fingerprint(
    path: Optional[Path], *, include_content: bool = True
) -> FileFingerprint:
    if path is None:
        return FileFingerprint()
    if not path.exists() or not path.is_file():
        raise ScanError(f"指纹文件不存在: {path}")
    stat = path.stat()
    size = stat.st_size
    if size > 0xFFFFFFFF:
        raise ScanError(f"文件超过 4GB，当前设备索引不支持: {path}")
    modify_date, modify_time = _fat_datetime(stat.st_mtime)
    base = FileFingerprint(
        present=True,
        attributes_valid=True,
        size=size,
        modify_date=modify_date,
        modify_time=modify_time,
    )
    if not include_content:
        return base

    with path.open("rb") as file:
        if size <= MANIFEST_FULL_CRC_MAX_BYTES:
            crc = zlib.crc32(file.read()) & 0xFFFFFFFF
            base.head_crc = crc
            base.middle_crc = crc
            base.tail_crc = crc
            return base
        sample = MANIFEST_SAMPLE_BYTES
        base.head_crc = zlib.crc32(file.read(sample)) & 0xFFFFFFFF
        middle_pos = size // 2 - sample // 2
        file.seek(middle_pos)
        base.middle_crc = zlib.crc32(file.read(sample)) & 0xFFFFFFFF
        file.seek(size - sample)
        base.tail_crc = zlib.crc32(file.read(sample)) & 0xFFFFFFFF
        return base


def _folder_cover(folder: Path, cache: dict[Path, Optional[Path]]) -> Optional[Path]:
    cached = cache.get(folder)
    if folder in cache:
        return cached

    fixed = (
        "cover.jpg",
        "cover.jpeg",
        "cover.png",
        "folder.jpg",
        "folder.jpeg",
        "folder.png",
        "front.jpg",
        "front.jpeg",
        "front.png",
    )
    try:
        entries = list(os.scandir(folder))
    except OSError as exc:
        raise ScanError(f"无法读取目录 {folder}: {exc}") from exc

    by_lower = {entry.name.casefold(): Path(entry.path) for entry in entries if entry.is_file()}
    for name in fixed:
        found = by_lower.get(name)
        if found is not None:
            cache[folder] = found
            return found

    fallback: Optional[Path] = None
    image_seen = 0
    entries_seen = 0
    for entry in entries:
        entries_seen += 1
        if not entry.is_file():
            continue
        lower = entry.name.casefold()
        if not lower.endswith((".jpg", ".jpeg", ".png")):
            continue
        image_seen += 1
        candidate = Path(entry.path)
        if fallback is None:
            fallback = candidate
        if any(token in lower for token in ("cover", "folder", "front", "album", "art")):
            cache[folder] = candidate
            return candidate
        if image_seen >= 12 or entries_seen >= 64:
            break

    cache[folder] = fallback
    return fallback


def _derive_hints(folder: Path, music_root: Path) -> tuple[str, str]:
    parts = folder.relative_to(music_root).parts
    if len(parts) >= 2:
        return _normalize_text(parts[0]), _normalize_text(parts[-1])
    if len(parts) == 1:
        return "", _normalize_text(parts[0])
    return "", ""


def discover_audio(
    music_root: Path,
    progress: ProgressCallback = _default_progress,
    cancel_event: object | None = None,
) -> list[DiscoveredAudio]:
    found: list[DiscoveredAudio] = []
    cover_cache: dict[Path, Optional[Path]] = {}

    def walk(folder: Path, inherited_cover: Optional[Path]) -> None:
        _check_cancel(cancel_event)
        local_cover = _folder_cover(folder, cover_cache)
        effective_cover = local_cover or inherited_cover
        fallback_artist, fallback_album = _derive_hints(folder, music_root)
        try:
            entries = list(os.scandir(folder))
        except OSError as exc:
            raise ScanError(f"无法读取目录 {folder}: {exc}") from exc

        for entry in entries:
            _check_cancel(cancel_event)
            if entry.is_dir(follow_symlinks=False):
                if not entry.name or entry.name.startswith("."):
                    continue
                walk(Path(entry.path), effective_cover)
                continue
            if not entry.is_file(follow_symlinks=False):
                continue
            suffix = Path(entry.name).suffix.casefold()
            if suffix not in (".mp3", ".flac"):
                continue
            full_path = Path(entry.path)
            audio_rel = _normalize_rel(full_path, music_root)
            found.append(
                DiscoveredAudio(
                    full_path=full_path,
                    audio_rel=audio_rel,
                    filename=entry.name,
                    fallback_artist=fallback_artist,
                    fallback_album=fallback_album,
                    effective_cover=effective_cover,
                )
            )
            progress(
                ScanProgress(
                    phase="discover",
                    message="正在枚举曲库",
                    current_path=audio_rel,
                    discovered=len(found),
                )
            )

    walk(music_root, None)
    return found


def _find_lrc(audio_path: Path) -> Optional[Path]:
    directory = audio_path.parent
    stem = audio_path.stem
    for ext in (".lrc", ".LRC", ".Lrc"):
        candidate = directory / f"{stem}{ext}"
        if candidate.exists() and candidate.is_file():
            return candidate
    # Windows 常用大小写不敏感；在其它系统也允许按 casefold 查找。
    target = f"{stem}.lrc".casefold()
    try:
        for entry in os.scandir(directory):
            if entry.is_file() and entry.name.casefold() == target:
                return Path(entry.path)
    except OSError:
        return None
    return None


def parse_audio(item: DiscoveredAudio, music_root: Path) -> TrackTemp:
    suffix = item.full_path.suffix.casefold()
    lrc_path = _find_lrc(item.full_path)
    track = TrackTemp(
        title=_normalize_text(item.full_path.stem),
        artist=item.fallback_artist,
        album=item.fallback_album,
        audio_rel=item.audio_rel,
        lrc_rel=_normalize_rel(lrc_path, music_root) if lrc_path else "",
        cover_path_rel=_normalize_rel(item.effective_cover, music_root) if item.effective_cover else "",
        cover_source=COVER_FILE_FALLBACK if item.effective_cover else COVER_NONE,
        ext_code=EXT_MP3 if suffix == ".mp3" else EXT_FLAC,
    )

    if suffix == ".mp3":
        cover = find_mp3_apic(item.full_path)
        title, artist, album = read_id3_basic(item.full_path)
        if cover.found:
            track.cover_source = COVER_MP3_APIC
            track.cover_offset = cover.offset
            track.cover_size = cover.size
            track.cover_mime = cover.mime
            track.cover_path_rel = ""
    else:
        cover = find_flac_picture(item.full_path)
        title, artist, album = read_flac_basic(item.full_path)
        if cover.found:
            track.cover_source = COVER_FLAC_PICTURE
            track.cover_offset = cover.offset
            track.cover_size = cover.size
            track.cover_mime = cover.mime
            track.cover_path_rel = ""

    if title:
        track.title = title
    if artist:
        track.artist = artist
    if album:
        track.album = album
    track.flags = _make_flags(track)
    return track


def _manifest_from_track(
    track: TrackTemp,
    audio_fp: FileFingerprint,
    lrc_fp: FileFingerprint,
    cover_fp: FileFingerprint,
) -> ManifestEntry:
    return ManifestEntry(track.audio_rel, audio_fp, lrc_fp, cover_fp)


def _track_map(catalog: Catalog) -> dict[str, tuple[int, TrackTemp]]:
    result: dict[str, tuple[int, TrackTemp]] = {}
    for index in range(len(catalog.tracks)):
        temp = catalog.track_to_temp(index)
        if temp.audio_rel in result:
            raise ScanError(f"旧索引存在重复路径: {temp.audio_rel}")
        result[temp.audio_rel] = (index, temp)
    return result


def _load_pair(system_root: Path, log: LogCallback) -> LoadedPair:
    index_candidates = [system_root / "music_index_v3.bin", system_root / "music_index_v3.bin.bak"]
    manifest_candidates = [system_root / "music_manifest_v1.bin", system_root / "music_manifest_v1.bin.bak"]

    catalog = None
    index_source = ""
    for path in index_candidates:
        if not path.exists():
            continue
        try:
            catalog = load_index(path)
            index_source = path.name
            break
        except Exception as exc:
            log(f"旧索引不可用 {path.name}: {exc}")

    entries: list[ManifestEntry] | None = None
    manifest_crc = 0
    manifest_source = ""
    for path in manifest_candidates:
        if not path.exists():
            continue
        try:
            entries, manifest_crc = load_manifest(path)
            manifest_source = path.name
            break
        except Exception as exc:
            log(f"旧清单不可用 {path.name}: {exc}")

    if catalog is None or entries is None:
        return LoadedPair(catalog, {}, False, "缺少可配对的旧索引或清单")

    actual_crc = catalog_crc32(catalog)
    if actual_crc != manifest_crc:
        return LoadedPair(catalog, {}, False, "旧索引与清单 Catalog CRC 不一致")
    if len(entries) != len(catalog.tracks):
        return LoadedPair(catalog, {}, False, "旧索引与清单条目数不一致")

    track_paths = set(_track_map(catalog))
    manifest_map = {entry.audio_rel: entry for entry in entries}
    if set(manifest_map) != track_paths:
        return LoadedPair(catalog, {}, False, "旧索引与清单路径集合不一致")

    return LoadedPair(
        catalog,
        manifest_map,
        True,
        f"增量基线: {index_source} + {manifest_source}",
    )


def _fingerprint_attributes_equal(a: FileFingerprint, b: FileFingerprint) -> bool:
    if a.present != b.present:
        return False
    if not a.present:
        return True
    return (
        a.attributes_valid
        and b.attributes_valid
        and a.size == b.size
        and a.modify_date == b.modify_date
        and a.modify_time == b.modify_time
    )


def _fingerprint_content_equal(a: FileFingerprint, b: FileFingerprint) -> bool:
    return (
        a.present == b.present
        and a.size == b.size
        and a.head_crc == b.head_crc
        and a.middle_crc == b.middle_crc
        and a.tail_crc == b.tail_crc
    )


def _entry_unchanged(
    old_entry: ManifestEntry,
    old_track: TrackTemp,
    audio_fp: FileFingerprint,
    current_lrc_rel: str,
    lrc_fp: FileFingerprint,
    current_cover_rel: str,
    cover_fp: FileFingerprint,
    *,
    attributes_only: bool,
) -> bool:
    compare = _fingerprint_attributes_equal if attributes_only else _fingerprint_content_equal
    if not compare(old_entry.audio_fingerprint, audio_fp):
        return False
    if old_track.lrc_rel != current_lrc_rel or not _fingerprint_content_equal(
        old_entry.lrc_fingerprint, lrc_fp
    ):
        return False
    if old_track.cover_source in (COVER_MP3_APIC, COVER_FLAC_PICTURE):
        return True
    if old_track.cover_source == COVER_FILE_FALLBACK:
        return old_track.cover_path_rel == current_cover_rel and _fingerprint_content_equal(
            old_entry.cover_fingerprint, cover_fp
        )
    return current_cover_rel == ""


def _write_report_csv(path: Path, tracks: Iterable[TrackTemp]) -> None:
    with path.open("w", encoding="utf-8-sig", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(
            [
                "audio_rel",
                "title",
                "artist",
                "album",
                "lrc_rel",
                "cover_source",
                "cover_path_rel",
                "cover_offset",
                "cover_size",
                "cover_mime",
            ]
        )
        for track in tracks:
            writer.writerow(
                [
                    track.audio_rel,
                    track.title,
                    track.artist,
                    track.album,
                    track.lrc_rel,
                    track.cover_source,
                    track.cover_path_rel,
                    track.cover_offset,
                    track.cover_size,
                    track.cover_mime,
                ]
            )


def scan_library(
    selected_root: str | os.PathLike[str],
    *,
    force_full: bool = False,
    strict_verify: bool = False,
    progress: ProgressCallback = _default_progress,
    log: LogCallback = _default_log,
    cancel_event: object | None = None,
) -> ScanResult:
    started = time.monotonic()
    card_root, music_root, system_root = resolve_card_root(selected_root)
    system_root.mkdir(parents=True, exist_ok=True)
    log(f"TF 根目录: {card_root}")
    log(f"音乐目录: {music_root}")

    pair = _load_pair(system_root, log)
    full_scan = force_full or not pair.valid_for_incremental
    if force_full:
        log("扫描模式: 强制全量")
    elif pair.valid_for_incremental:
        log(f"扫描模式: {'严格增量' if strict_verify else '快速增量'}；{pair.source_note}")
    else:
        log(f"扫描模式: 自动全量；原因={pair.source_note}")

    discovered = discover_audio(music_root, progress, cancel_event)
    if not discovered:
        raise ScanError("Music 目录中没有 MP3 或 FLAC 文件")
    total = len(discovered)
    log(f"发现音频: {total}")

    old_tracks = _track_map(pair.catalog) if pair.valid_for_incremental and pair.catalog else {}
    old_manifest = pair.manifest_entries if pair.valid_for_incremental else {}
    seen_old: set[str] = set()

    tracks: list[TrackTemp] = []
    next_manifest: list[ManifestEntry] = []
    reused = added = modified = 0
    cover_fp_cache: dict[Path, tuple[FileFingerprint, bool]] = {}

    def cover_fingerprint(path: Optional[Path], include_content: bool) -> FileFingerprint:
        if path is None:
            return FileFingerprint()
        cached = cover_fp_cache.get(path)
        if cached is not None and (not include_content or cached[1]):
            return cached[0]
        value = _file_fingerprint(path, include_content=include_content)
        cover_fp_cache[path] = (value, include_content)
        return value

    for processed, item in enumerate(discovered, start=1):
        _check_cancel(cancel_event)
        lrc_path = _find_lrc(item.full_path)
        lrc_rel = _normalize_rel(lrc_path, music_root) if lrc_path else ""
        cover_rel = _normalize_rel(item.effective_cover, music_root) if item.effective_cover else ""

        old_entry = old_manifest.get(item.audio_rel)
        old_track_tuple = old_tracks.get(item.audio_rel)
        has_old = not full_scan and old_entry is not None and old_track_tuple is not None
        include_content = full_scan or strict_verify or not has_old

        audio_fp = _file_fingerprint(item.full_path, include_content=include_content)
        # 歌词通常很小，目录封面又按路径缓存；快速模式也完整校验它们。
        lrc_fp = _file_fingerprint(lrc_path, include_content=True)
        cover_fp = cover_fingerprint(item.effective_cover, True)

        reused_track: TrackTemp | None = None
        if has_old:
            old_track = old_track_tuple[1]
            unchanged = _entry_unchanged(
                old_entry,
                old_track,
                audio_fp,
                lrc_rel,
                lrc_fp,
                cover_rel,
                cover_fp,
                attributes_only=not strict_verify,
            )
            if not unchanged and not strict_verify:
                audio_fp = _file_fingerprint(item.full_path, include_content=True)
                lrc_fp = _file_fingerprint(lrc_path, include_content=True)
                cover_fp = cover_fingerprint(item.effective_cover, True)
                unchanged = _entry_unchanged(
                    old_entry,
                    old_track,
                    audio_fp,
                    lrc_rel,
                    lrc_fp,
                    cover_rel,
                    cover_fp,
                    attributes_only=False,
                )
            if unchanged:
                reused_track = old_track

        if reused_track is not None:
            track = reused_track
            if include_content or not _fingerprint_attributes_equal(
                old_entry.audio_fingerprint, audio_fp
            ):
                entry = _manifest_from_track(track, audio_fp, lrc_fp, cover_fp)
            else:
                entry = old_entry
            reused += 1
            seen_old.add(item.audio_rel)
        else:
            if not include_content:
                audio_fp = _file_fingerprint(item.full_path, include_content=True)
                lrc_fp = _file_fingerprint(lrc_path, include_content=True)
                cover_fp = cover_fingerprint(item.effective_cover, True)
            track = parse_audio(item, music_root)
            parsed_lrc_path = music_root / PurePosixPath(track.lrc_rel) if track.lrc_rel else None
            parsed_lrc_fp = _file_fingerprint(parsed_lrc_path, include_content=True)
            if track.cover_source == COVER_FILE_FALLBACK and track.cover_path_rel:
                parsed_cover_path = music_root / PurePosixPath(track.cover_path_rel)
                parsed_cover_fp = cover_fingerprint(parsed_cover_path, True)
            else:
                parsed_cover_fp = FileFingerprint()
            entry = _manifest_from_track(track, audio_fp, parsed_lrc_fp, parsed_cover_fp)
            if not full_scan and item.audio_rel in old_manifest:
                modified += 1
                seen_old.add(item.audio_rel)
            else:
                added += 1

        tracks.append(track)
        next_manifest.append(entry)
        progress(
            ScanProgress(
                phase="scan",
                message="正在解析曲库" if reused_track is None else "正在复用旧索引",
                current_path=item.audio_rel,
                processed=processed,
                total=total,
                discovered=total,
                reused=reused,
                added=added,
                modified=modified,
                deleted=0,
            )
        )

    deleted = 0 if full_scan else len(set(old_manifest) - seen_old)
    _check_cancel(cancel_event)
    progress(
        ScanProgress(
            phase="build",
            message="正在生成 V3 索引",
            processed=total,
            total=total,
            discovered=total,
            reused=reused,
            added=added,
            modified=modified,
            deleted=deleted,
        )
    )
    catalog = build_catalog(tracks)
    index_data = serialize_index(catalog)
    cat_crc = catalog_crc32(catalog)
    manifest_data = serialize_manifest(next_manifest, cat_crc)

    index_path = system_root / "music_index_v3.bin"
    manifest_path = system_root / "music_manifest_v1.bin"
    _atomic_write(index_path, index_data, lambda path: load_index(path))
    # 索引写成功后再写 Manifest，与设备端保存顺序一致。
    _atomic_write(manifest_path, manifest_data, lambda path: load_manifest(path))

    report_csv = system_root / "music_scan_tracks.csv"
    _write_report_csv(report_csv, tracks)
    elapsed = time.monotonic() - started
    report_path = system_root / "music_scan_report.json"

    result = ScanResult(
        success=True,
        full_scan=full_scan,
        forced_full_scan=force_full,
        strict_incremental=strict_verify and not full_scan,
        music_root=str(music_root),
        system_root=str(system_root),
        discovered=total,
        reused=reused,
        added=added,
        modified=modified,
        deleted=deleted,
        track_count=len(catalog.tracks),
        album_count=len(catalog.albums),
        artist_count=len(catalog.artists),
        index_size=len(index_data),
        manifest_size=len(manifest_data),
        elapsed_seconds=elapsed,
        index_path=str(index_path),
        manifest_path=str(manifest_path),
        report_path=str(report_path),
    )
    report_path.write_text(
        json.dumps(asdict(result), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    progress(
        ScanProgress(
            phase="done",
            message="扫描完成",
            processed=total,
            total=total,
            discovered=total,
            reused=reused,
            added=added,
            modified=modified,
            deleted=deleted,
        )
    )
    log(
        "完成: "
        f"模式={'全量' if full_scan else '增量'} "
        f"发现={total} 复用={reused} 新增={added} 修改={modified} 删除={deleted} "
        f"歌曲={len(catalog.tracks)} 专辑={len(catalog.albums)} 歌手={len(catalog.artists)} "
        f"用时={elapsed:.2f}s"
    )
    return result


def verify_library(selected_root: str | os.PathLike[str]) -> dict[str, object]:
    _, _, system_root = resolve_card_root(selected_root)
    index_path = system_root / "music_index_v3.bin"
    manifest_path = system_root / "music_manifest_v1.bin"
    catalog = load_index(index_path)
    entries, manifest_crc = load_manifest(manifest_path)
    actual_crc = catalog_crc32(catalog)
    if actual_crc != manifest_crc:
        raise ScanError(
            f"Catalog CRC 不一致: index=0x{actual_crc:08X} manifest=0x{manifest_crc:08X}"
        )
    if len(entries) != len(catalog.tracks):
        raise ScanError("索引与 Manifest 条目数量不一致")
    index_paths = {catalog.track_to_temp(i).audio_rel for i in range(len(catalog.tracks))}
    manifest_paths = {entry.audio_rel for entry in entries}
    if index_paths != manifest_paths:
        raise ScanError("索引与 Manifest 路径集合不一致")
    return {
        "ok": True,
        "tracks": len(catalog.tracks),
        "albums": len(catalog.albums),
        "artists": len(catalog.artists),
        "pool_bytes": len(catalog.pool),
        "catalog_crc32": f"0x{actual_crc:08X}",
        "index_path": str(index_path),
        "manifest_path": str(manifest_path),
    }
