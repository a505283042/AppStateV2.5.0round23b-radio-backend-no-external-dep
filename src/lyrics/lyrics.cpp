#include "lyrics/lyrics.h"
#include "utils/log.h"
#include <algorithm>
#include <stdlib.h>
#include <SdFat.h>
#include "storage/storage_io.h"

extern SdFat sd;

LyricsDisplay g_lyricsDisplay;

static inline bool lyric_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool lyric_parse_uint(const char* s, size_t len, int* out_value) {
    if (!s || len == 0 || !out_value) return false;
    int value = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    *out_value = value;
    return true;
}

static char* lyrics_alloc_text_buffer(size_t len) {
    char* buf = (char*)ps_malloc(len + 1);
    if (!buf) {
        buf = (char*)malloc(len + 1);
    }
    return buf;
}

bool lyrics_read_file_to_alloc_buffer(const char* path, char** out_content, size_t* out_len) {
    if (!out_content || !out_len) return false;
    *out_content = nullptr;
    *out_len = 0;

    StorageSdLockGuard sd_lock(500);
    if (!sd_lock) {
        LOGW("[LYRICS] 获取 SD 锁超时");
        return false;
    }

    File32 file = sd.open(path, O_RDONLY);
    if (!file) {
        LOGW("[LYRICS] Failed to open: %s", path);
        return false;
    }

    const uint32_t file_size = file.fileSize();
    if (file_size == 0 || file_size > 65536) {
        file.close();
        LOGW("[LYRICS] Invalid file size: %u", (unsigned)file_size);
        return false;
    }

    char* buf = lyrics_alloc_text_buffer(file_size);
    if (!buf) {
        file.close();
        LOGW("[LYRICS] alloc failed size=%u", (unsigned)file_size);
        return false;
    }

    size_t copied = 0;
    bool ok = true;
    uint8_t chunk[512];
    while (file.available()) {
        const int bytes_read = file.read(chunk, sizeof(chunk));
        if (bytes_read <= 0) {
            ok = false;
            break;
        }
        memcpy(buf + copied, chunk, bytes_read);
        copied += (size_t)bytes_read;
    }
    file.close();

    if (!ok || copied != file_size) {
        free(buf);
        LOGW("[LYRICS] read failed copied=%u expected=%u", (unsigned)copied, (unsigned)file_size);
        return false;
    }

    buf[file_size] = '\0';
    *out_content = buf;
    *out_len = file_size;
    return true;
}

LyricsParser::LyricsParser()
    : m_text_buf(nullptr), m_text_len(0) {}

LyricsParser::~LyricsParser() {
    clear();
}

bool LyricsParser::loadFromFile(const char* path) {
    clear();

    const uint32_t t0 = millis();
    char* content = nullptr;
    size_t content_len = 0;
    if (!lyrics_read_file_to_alloc_buffer(path, &content, &content_len)) {
        return false;
    }

    const uint32_t t_after_read = millis();
    const bool result = parseOwnedBuffer(content, content_len);
    const uint32_t t_after_parse = millis();

    LOGI("[LYRICS] Loaded: %s (%u bytes, read=%lums parse=%lums total=%lums)",
         path,
         (unsigned)content_len,
         (unsigned long)(t_after_read - t0),
         (unsigned long)(t_after_parse - t_after_read),
         (unsigned long)(t_after_parse - t0));

    return result;
}



bool LyricsParser::parseOwnedBuffer(char* content, size_t len) {
    clear();

    if (!content || len == 0) {
        if (content) free(content);
        return false;
    }

    m_text_buf = content;
    m_text_len = len;

    size_t line_start = 0;
    for (size_t i = 0; i <= len; ++i) {
        const bool is_line_end = (i == len) || (m_text_buf[i] == '\n');
        if (!is_line_end) continue;

        size_t start = line_start;
        size_t end = i;

        while (start < end && lyric_is_space(m_text_buf[start])) ++start;
        while (end > start && lyric_is_space(m_text_buf[end - 1])) --end;

        if (end > start) {
            parseLineRange(start, end - start);
        }

        line_start = i + 1;
    }

    sortByTime();

    if (m_lines.empty()) {
        clear();
        return false;
    }

    LOGI("[LYRICS][PARSE] done text_len=%u lines=%d text_buf=%p",
         (unsigned)m_text_len, (int)m_lines.size(), m_text_buf);

    return true;
}

void LyricsParser::parseLineRange(size_t line_off, size_t line_len) {
    if (!m_text_buf || line_len == 0) return;

    const char* line = m_text_buf + line_off;
    size_t pos = 0;
    const size_t first_new_idx = m_lines.size();

    // 解析行首的 [mm:ss.xx] / [mm:ss:xx] 标签
    while (pos < line_len) {
        while (pos < line_len && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
        if (pos >= line_len || line[pos] != '[') {
            break;
        }

        size_t tag_end = pos + 1;
        while (tag_end < line_len && line[tag_end] != ']') ++tag_end;
        if (tag_end >= line_len) {
            break;
        }

        const uint32_t time_ms = parseTimeTagRange(line + pos, tag_end - pos + 1);
        if (time_ms != UINT32_MAX) {
            m_lines.emplace_back(time_ms, 0, 0);
        }

        pos = tag_end + 1;
    }

    if (m_lines.size() == first_new_idx) {
        return;
    }

    size_t text_begin = pos;
    size_t text_end = line_len;

    // 保持和旧逻辑一致：遇到下一个 '[' 以后截断
    for (size_t i = text_begin; i < line_len; ++i) {
        if (line[i] == '[') {
            text_end = i;
            break;
        }
    }

    while (text_begin < text_end && lyric_is_space(line[text_begin])) ++text_begin;
    while (text_end > text_begin && lyric_is_space(line[text_end - 1])) --text_end;

    if (text_begin >= text_end) {
        m_lines.resize(first_new_idx);
        return;
    }

    const size_t text_len = text_end - text_begin;
    if (text_len > 65535u) {
        m_lines.resize(first_new_idx);
        return;
    }

    // 关键：把这一句歌词在原始缓冲里原地终止，后面就能直接返回 const char*
    m_text_buf[line_off + text_end] = '\0';

    const uint32_t text_off = (uint32_t)(line_off + text_begin);
    for (size_t i = first_new_idx; i < m_lines.size(); ++i) {
        m_lines[i].text_off = text_off;
        m_lines[i].text_len = (uint16_t)text_len;
    }
}

uint32_t LyricsParser::parseTimeTagRange(const char* tag, size_t len) const {
    if (!tag || len < 7 || tag[0] != '[' || tag[len - 1] != ']') {
        return UINT32_MAX;
    }

    const char* inner = tag + 1;
    const size_t inner_len = len - 2;
    const size_t kNpos = (size_t)-1;

    size_t colon1 = kNpos;
    size_t sep2 = kNpos;

    for (size_t i = 0; i < inner_len; ++i) {
        if (inner[i] == ':') {
            if (colon1 == kNpos) {
                colon1 = i;
            } else {
                sep2 = i;
                break;
            }
        } else if (inner[i] == '.' && colon1 != kNpos) {
            sep2 = i;
            break;
        }
    }

    if (colon1 == kNpos) return UINT32_MAX;

    const size_t min_len = colon1;
    const size_t sec_start = colon1 + 1;
    const size_t sec_len = (sep2 == kNpos) ? (inner_len - sec_start) : (sep2 - sec_start);
    const size_t cs_start = (sep2 == kNpos) ? inner_len : (sep2 + 1);
    const size_t cs_len = (sep2 == kNpos) ? 0 : (inner_len - cs_start);

    int minutes = 0;
    int seconds = 0;
    int centiseconds = 0;

    if (!lyric_parse_uint(inner, min_len, &minutes)) return UINT32_MAX;
    if (!lyric_parse_uint(inner + sec_start, sec_len, &seconds)) return UINT32_MAX;
    if (cs_len > 0 && !lyric_parse_uint(inner + cs_start, cs_len, &centiseconds)) return UINT32_MAX;
    if (seconds < 0 || seconds >= 60) return UINT32_MAX;

    if (cs_len >= 3) {
        while (centiseconds >= 100) centiseconds /= 10;
    }

    return (uint32_t)((minutes * 60 + seconds) * 1000 + centiseconds * 10);
}

void LyricsParser::sortByTime() {
    std::sort(m_lines.begin(), m_lines.end(),
              [](const LyricLineRef& a, const LyricLineRef& b) {
                  return a.time_ms < b.time_ms;
              });
}

int LyricsParser::getCurrentIndex(uint32_t time_ms) const {
    if (m_lines.empty()) return -1;

    if (time_ms < m_lines[0].time_ms) {
        return 0;
    }

    auto it = std::upper_bound(
        m_lines.begin(), m_lines.end(), time_ms,
        [](uint32_t t, const LyricLineRef& line) { return t < line.time_ms; });

    const int idx = (int)std::distance(m_lines.begin(), it) - 1;
    return (idx < 0) ? 0 : idx;
}

uint32_t LyricsParser::getLineTimeMs(int index) const {
    if (index < 0 || index >= (int)m_lines.size()) {
        return 0;
    }
    return m_lines[index].time_ms;
}

const char* LyricsParser::getLineTextCStr(int index) const {
    if (!m_text_buf || index < 0 || index >= (int)m_lines.size()) {
        return "";
    }

    const LyricLineRef& line = m_lines[index];
    if (line.text_len == 0) {
        return "";
    }

    return m_text_buf + line.text_off;
}

String LyricsParser::getLineText(int index) const {
    return String(getLineTextCStr(index));
}

void LyricsParser::clear() {
    if (m_text_buf || !m_lines.empty()) {
        LOGI("[LYRICS][PARSE] clear text_buf=%p lines=%d",
             m_text_buf, (int)m_lines.size());
    }

    if (m_text_buf) {
        free(m_text_buf);
        m_text_buf = nullptr;
    }
    m_text_len = 0;
    m_lines.clear();
}

String LyricsParser::findLyricsFile(const char* audio_path) {
    String path(audio_path);

    int dotIndex = path.lastIndexOf('.');
    if (dotIndex > 0) {
        path = path.substring(0, dotIndex);
    }

    String lrcPath = path + ".lrc";
    String txtPath = path + ".txt";
    String lrcPathUpper = path + ".LRC";

    StorageSdLockGuard sd_lock(500);
    if (!sd_lock) {
        LOGW("[LYRICS] findLyricsFile lock timeout");
        return "";
    }

    File32 f;
    if ((f = sd.open(lrcPath.c_str(), O_RDONLY))) { f.close(); return lrcPath; }
    if ((f = sd.open(txtPath.c_str(), O_RDONLY))) { f.close(); return txtPath; }
    if ((f = sd.open(lrcPathUpper.c_str(), O_RDONLY))) { f.close(); return lrcPathUpper; }

    return "";
}

LyricsDisplay::LyricsDisplay() : m_currentIndex(-1), m_currentTime(0) {}
LyricsDisplay::~LyricsDisplay() {}

void LyricsDisplay::resetStateOnly() {
    m_currentIndex = -1;
    m_currentTime = 0;
}

bool LyricsDisplay::loadForTrack(const char* audio_path) {
    clear();

    String lyricsPath = LyricsParser::findLyricsFile(audio_path);
    if (lyricsPath.length() == 0) {
        return false;
    }

    return loadFromPath(lyricsPath.c_str());
}

bool LyricsDisplay::loadFromPath(const char* lrc_path) {
    resetStateOnly();

    if (!lrc_path || lrc_path[0] == '\0') {
        return false;
    }

    const bool success = m_parser.loadFromFile(lrc_path);
    if (success) {
        m_currentIndex = 0;
    } else {
        LOGW("[LYRICS] Failed: %s", lrc_path);
    }
    return success;
}



bool LyricsDisplay::loadFromOwnedTextBuffer(char* content, size_t len) {
    resetStateOnly();

    if (!content || len == 0) {
        if (content) free(content);
        return false;
    }

    const bool success = m_parser.parseOwnedBuffer(content, len);
    if (success) {
        m_currentIndex = 0;
    }
    return success;
}

void LyricsDisplay::updateTime(uint32_t time_ms) {
    m_currentTime = time_ms;
    const int newIndex = m_parser.getCurrentIndex(time_ms);

    if (newIndex != m_currentIndex) {
        m_currentIndex = newIndex;
        LOGD("[LYRICS] Index changed to: %d (time: %u ms)", m_currentIndex, time_ms);
    }
}

String LyricsDisplay::getCurrentLyric() const {
    return String(getCurrentLyricCStr());
}

String LyricsDisplay::getNextLyric() const {
    return String(getNextLyricCStr());
}

String LyricsDisplay::getFollowingLyric() const {
    return String(getFollowingLyricCStr());
}

const char* LyricsDisplay::getCurrentLyricCStr() const {
    return m_parser.getLineTextCStr(m_currentIndex);
}

const char* LyricsDisplay::getNextLyricCStr() const {
    return m_parser.getLineTextCStr(m_currentIndex + 1);
}

const char* LyricsDisplay::getFollowingLyricCStr() const {
    return m_parser.getLineTextCStr(m_currentIndex + 2);
}

uint32_t LyricsDisplay::getCurrentLyricStartTime() const {
    return m_parser.getLineTimeMs(m_currentIndex);
}

uint32_t LyricsDisplay::getNextLyricStartTime() const {
    return m_parser.getLineTimeMs(m_currentIndex + 1);
}

uint32_t LyricsDisplay::getFollowingLyricStartTime() const {
    return m_parser.getLineTimeMs(m_currentIndex + 2);
}

bool LyricsDisplay::hasLyrics() const {
    return m_parser.isLoaded();
}

void LyricsDisplay::clear() {
    m_parser.clear();
    m_currentIndex = -1;
    m_currentTime = 0;
}

uint32_t LyricsDisplay::getNextLyricTime() const {
    if (m_currentIndex + 1 < m_parser.getLineCount()) {
        return m_parser.getLineTimeMs(m_currentIndex + 1);
    }
    return m_currentTime + 5000;
}

LyricsDisplay::ScrollLyrics LyricsDisplay::getScrollLyrics() const {
    ScrollLyrics result;
    result.progress = 0.0f;

    if (!m_parser.isLoaded() || m_currentIndex < 0) {
        return result;
    }

    result.prev = m_parser.getLineTextCStr(m_currentIndex - 1);
    result.current = m_parser.getLineTextCStr(m_currentIndex);
    result.next = m_parser.getLineTextCStr(m_currentIndex + 1);

    LOGD("[LYRICS][UI] scroll ptrs prev=%p cur=%p next=%p", 
         (const void*)result.prev, 
         (const void*)result.current, 
         (const void*)result.next);

    const uint32_t currStart = m_parser.getLineTimeMs(m_currentIndex);
    const uint32_t nextStart = getNextLyricTime();

    if (nextStart > currStart && m_currentTime >= currStart) {
        const uint32_t duration = nextStart - currStart;
        const uint32_t elapsed = m_currentTime - currStart;
        result.progress = (float)elapsed / (float)duration;
        if (result.progress > 1.0f) result.progress = 1.0f;
    }

    return result;
}