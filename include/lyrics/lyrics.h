#pragma once
#include <Arduino.h>
#include <vector>

struct LyricLineRef {
    uint32_t time_ms;
    uint32_t text_off;
    uint16_t text_len;

    LyricLineRef(uint32_t t = 0, uint32_t off = 0, uint16_t len = 0)
        : time_ms(t), text_off(off), text_len(len) {}
};

class LyricsParser {
public:
    LyricsParser();
    ~LyricsParser();

    // 从文件加载歌词（支持 .lrc 和 .txt）
    bool loadFromFile(const char* path);

    // 直接接管一块已分配的歌词文本缓冲
    // 成功或失败后，这块缓冲都由 parser 负责释放
    bool parseOwnedBuffer(char* content, size_t len);

    // 获取当前时间对应的歌词索引
    int getCurrentIndex(uint32_t time_ms) const;

    // 按索引取时间 / 文本
    uint32_t getLineTimeMs(int index) const;
    String getLineText(int index) const;
    const char* getLineTextCStr(int index) const;

    int getLineCount() const { return (int)m_lines.size(); }

    void clear();

    bool isLoaded() const { return !m_lines.empty() && m_text_buf != nullptr; }

    static String findLyricsFile(const char* audio_path);

private:
    std::vector<LyricLineRef> m_lines;
    char* m_text_buf;
    size_t m_text_len;

    uint32_t parseTimeTagRange(const char* tag, size_t len) const;
    void parseLineRange(size_t line_off, size_t line_len);
    void sortByTime();
};

class LyricsDisplay {
public:
    LyricsDisplay();
    ~LyricsDisplay();

    bool loadForTrack(const char* audio_path);
    bool loadFromPath(const char* lrc_path);

    // 直接接管缓冲区
    bool loadFromOwnedTextBuffer(char* content, size_t len);

    void updateTime(uint32_t time_ms);

    const char* getCurrentLyricCStr() const;
    const char* getNextLyricCStr() const;
    const char* getFollowingLyricCStr() const;

    String getCurrentLyric() const;
    String getNextLyric() const;
    String getFollowingLyric() const;

    uint32_t getCurrentLyricStartTime() const;
    uint32_t getNextLyricStartTime() const;
    uint32_t getFollowingLyricStartTime() const;

    bool hasLyrics() const;
    void clear();

    struct ScrollLyrics {
        const char* prev;
        const char* current;
        const char* next;
        float progress;

        ScrollLyrics()
            : prev(""), current(""), next(""), progress(0.0f) {}
    };
    ScrollLyrics getScrollLyrics() const;

private:
    LyricsParser m_parser;
    int m_currentIndex;
    uint32_t m_currentTime;

    void resetStateOnly();
    uint32_t getNextLyricTime() const;
};

extern LyricsDisplay g_lyricsDisplay;

// 只负责从 SD 读取歌词文件到内存；不做解析
bool lyrics_read_file_to_alloc_buffer(const char* path, char** out_content, size_t* out_len);
