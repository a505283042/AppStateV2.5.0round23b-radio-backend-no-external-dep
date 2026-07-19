#include "meta/meta_flac_cover.h"
#include "storage/storage_io.h"

namespace {

static uint32_t read_u24_be(const uint8_t* p)
{
  return ((uint32_t)p[0] << 16) |
         ((uint32_t)p[1] << 8) |
         (uint32_t)p[2];
}

static bool read_exact(FlacByteReader& reader, void* dst, size_t n)
{
  return reader.read(dst, n);
}

static bool read_u32_be_reader(FlacByteReader& reader, uint32_t& value)
{
  uint8_t b[4];
  if (!reader.read(b, sizeof(b))) return false;

  value = ((uint32_t)b[0] << 24) |
          ((uint32_t)b[1] << 16) |
          ((uint32_t)b[2] << 8) |
          (uint32_t)b[3];
  return true;
}

class SdFileFlacReader final : public FlacByteReader {
public:
  explicit SdFileFlacReader(File32& file) : m_file(file) {}

  bool read(void* dst, size_t n) override
  {
    return (size_t)m_file.read(dst, n) == n;
  }

  bool seek(uint32_t pos) override
  {
    return m_file.seekSet(pos);
  }

  bool skip(uint32_t n) override
  {
    while (n > 0) {
      const uint32_t step = n > 0x7FFFFFFFu ? 0x7FFFFFFFu : n;
      if (!m_file.seekCur((int32_t)step)) return false;
      n -= step;
    }
    return true;
  }

  uint32_t position() const override
  {
    return (uint32_t)m_file.position();
  }

private:
  File32& m_file;
};

}  // namespace

bool flac_find_picture_from_reader(FlacByteReader& reader, FlacCoverLoc& out)
{
  out = {};

  if (!reader.seek(0)) return false;

  uint8_t magic[4];
  if (!read_exact(reader, magic, sizeof(magic))) return false;

  if (!(magic[0] == 'f' && magic[1] == 'L' &&
        magic[2] == 'a' && magic[3] == 'C')) {
    // 不是 FLAC 不属于 I/O 失败，只是没有可用 PICTURE。
    return true;
  }

  FlacCoverLoc fallback{};
  bool has_fallback = false;

  while (true) {
    uint8_t block_header[4];
    if (!read_exact(reader, block_header, sizeof(block_header))) break;

    const bool is_last = (block_header[0] & 0x80u) != 0;
    const uint8_t type = block_header[0] & 0x7Fu;
    const uint32_t length = read_u24_be(block_header + 1);

    if (type != 6 /* PICTURE */) {
      if (!reader.skip(length)) break;
      if (is_last) break;
      continue;
    }

    const uint32_t block_start = reader.position();
    uint32_t picture_type = 0;
    uint32_t mime_len = 0;
    uint32_t description_len = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t colors = 0;
    uint32_t data_len = 0;

    if (!read_u32_be_reader(reader, picture_type) ||
        !read_u32_be_reader(reader, mime_len)) {
      break;
    }

    String mime;
    if (mime_len > 0) {
      if (mime_len <= 127) {
        char tmp[128];
        if (!reader.read(tmp, mime_len)) break;
        tmp[mime_len] = '\0';
        mime = String(tmp);
      } else if (!reader.skip(mime_len)) {
        break;
      }
    }

    if (!read_u32_be_reader(reader, description_len)) break;
    if (description_len > 0 && !reader.skip(description_len)) break;

    if (!read_u32_be_reader(reader, width) ||
        !read_u32_be_reader(reader, height) ||
        !read_u32_be_reader(reader, depth) ||
        !read_u32_be_reader(reader, colors) ||
        !read_u32_be_reader(reader, data_len)) {
      break;
    }

    (void)width;
    (void)height;
    (void)depth;
    (void)colors;

    const uint32_t data_offset = reader.position();
    if (data_len > 0) {
      FlacCoverLoc candidate{};
      candidate.found = true;
      candidate.offset = data_offset;
      candidate.size = data_len;
      candidate.mime = mime;

      if (picture_type == 3 /* front cover */) {
        out = candidate;
        return true;
      }

      if (!has_fallback) {
        fallback = candidate;
        has_fallback = true;
      }
    }

    const uint32_t current = reader.position();
    if (current < block_start) break;

    const uint32_t consumed = current - block_start;
    if (consumed > length) break;

    const uint32_t remaining = length - consumed;
    if (remaining > 0 && !reader.skip(remaining)) break;
    if (is_last) break;
  }

  if (has_fallback) out = fallback;
  return true;
}

bool flac_find_picture(SdFat& sd, const char* path, FlacCoverLoc& out)
{
  out = {};

  StorageSdLockGuard sd_lock(1000);
  if (!sd_lock) return false;

  File32 file = sd.open(path, O_RDONLY);
  if (!file) return false;

  SdFileFlacReader reader(file);
  const bool ok = flac_find_picture_from_reader(reader, out);
  file.close();
  return ok;
}
