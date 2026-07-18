#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include <esp_heap_caps.h>

/**
 * @brief 优先从 PSRAM 分配的 STL allocator。
 *
 * 扫描临时容器允许在 PSRAM 不可用时回落内部 8-bit heap，避免因为单次外部内存
 * 分配失败直接破坏 STL 容器。运行时可通过容器 data() 指针归属日志确认实际位置。
 */
template <typename T>
class PsramAllocator {
public:
  using value_type = T;
  using pointer = T*;
  using const_pointer = const T*;
  using reference = T&;
  using const_reference = const T&;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;

  template <typename U>
  struct rebind {
    using other = PsramAllocator<U>;
  };

  PsramAllocator() noexcept = default;

  template <typename U>
  PsramAllocator(const PsramAllocator<U>&) noexcept {}

  T* allocate(size_type count)
  {
    if (count == 0) return nullptr;
    if (count > max_size()) std::abort();

    const size_type bytes = count * sizeof(T);
    void* memory = heap_caps_malloc(
        bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!memory) {
      memory = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }

    if (!memory) std::abort();
    return static_cast<T*>(memory);
  }

  void deallocate(T* pointer, size_type) noexcept
  {
    heap_caps_free(pointer);
  }

  size_type max_size() const noexcept
  {
    return std::numeric_limits<size_type>::max() / sizeof(T);
  }
};

template <typename T, typename U>
inline bool operator==(const PsramAllocator<T>&,
                       const PsramAllocator<U>&) noexcept
{
  return true;
}

template <typename T, typename U>
inline bool operator!=(const PsramAllocator<T>&,
                       const PsramAllocator<U>&) noexcept
{
  return false;
}

template <typename T>
using PsramVector = std::vector<T, PsramAllocator<T>>;

template <typename Key,
          typename Value,
          typename Compare = std::less<Key>>
using PsramMap = std::map<
    Key,
    Value,
    Compare,
    PsramAllocator<std::pair<const Key, Value>>>;

/**
 * @brief 扫描阶段使用的 PSRAM 文本。
 *
 * Arduino String 的字符缓冲走默认 heap；仅把外层 vector 放入 PSRAM 仍会让标题、路径、
 * 歌手和专辑长期占用内部 RAM。本类型把字符缓冲本身也优先放入 PSRAM，并提供扫描与
 * builder 当前需要的最小 String 兼容接口。
 */
class PsramString {
public:
  PsramString() noexcept = default;

  PsramString(const char* text)
  {
    assign_or_abort(text, text ? std::strlen(text) : 0);
  }

  PsramString(const String& text)
  {
    assign_or_abort(text.c_str(), text.length());
  }

  PsramString(const PsramString& other)
  {
    assign_or_abort(other.c_str(), other.length());
  }

  PsramString(PsramString&& other) noexcept
      : data_(other.data_), length_(other.length_)
  {
    other.data_ = nullptr;
    other.length_ = 0;
  }

  ~PsramString()
  {
    release();
  }

  PsramString& operator=(const PsramString& other)
  {
    if (this != &other) {
      assign_or_abort(other.c_str(), other.length());
    }
    return *this;
  }

  PsramString& operator=(PsramString&& other) noexcept
  {
    if (this != &other) {
      release();
      data_ = other.data_;
      length_ = other.length_;
      other.data_ = nullptr;
      other.length_ = 0;
    }
    return *this;
  }

  PsramString& operator=(const String& text)
  {
    assign_or_abort(text.c_str(), text.length());
    return *this;
  }

  PsramString& operator=(const char* text)
  {
    assign_or_abort(text, text ? std::strlen(text) : 0);
    return *this;
  }

  bool assign(const char* text, size_t length)
  {
    if (!text || length == 0) {
      clear();
      return true;
    }

    char* next = allocate_chars(length + 1);
    if (!next) return false;

    std::memcpy(next, text, length);
    next[length] = '\0';

    release();
    data_ = next;
    length_ = length;
    return true;
  }

  bool resize_for_write(size_t length)
  {
    if (length == 0) {
      clear();
      return true;
    }

    char* next = allocate_chars(length + 1);
    if (!next) return false;
    next[length] = '\0';

    release();
    data_ = next;
    length_ = length;
    return true;
  }

  void clear() noexcept
  {
    release();
  }

  bool isEmpty() const noexcept
  {
    return length_ == 0;
  }

  size_t length() const noexcept
  {
    return length_;
  }

  const char* c_str() const noexcept
  {
    return data_ ? data_ : "";
  }

  char* data() noexcept
  {
    return data_;
  }

  const char* data() const noexcept
  {
    return c_str();
  }

  bool isExternal() const noexcept
  {
    return !data_ || esp_ptr_external_ram(data_);
  }

  int compareTo(const PsramString& other) const noexcept
  {
    return std::strcmp(c_str(), other.c_str());
  }

  int compareTo(const String& other) const noexcept
  {
    return std::strcmp(c_str(), other.c_str());
  }

  int compareTo(const char* other) const noexcept
  {
    return std::strcmp(c_str(), other ? other : "");
  }

  void swap(PsramString& other) noexcept
  {
    std::swap(data_, other.data_);
    std::swap(length_, other.length_);
  }

private:
  static char* allocate_chars(size_t bytes)
  {
    if (bytes == 0) return nullptr;

    void* memory = heap_caps_malloc(
        bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!memory) {
      memory = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }

    return static_cast<char*>(memory);
  }

  void assign_or_abort(const char* text, size_t length)
  {
    if (!assign(text, length)) std::abort();
  }

  void release() noexcept
  {
    if (data_) {
      heap_caps_free(data_);
      data_ = nullptr;
    }
    length_ = 0;
  }

  char* data_ = nullptr;
  size_t length_ = 0;
};

inline bool operator==(const PsramString& left,
                       const PsramString& right) noexcept
{
  return left.compareTo(right) == 0;
}

inline bool operator!=(const PsramString& left,
                       const PsramString& right) noexcept
{
  return !(left == right);
}

inline bool operator<(const PsramString& left,
                      const PsramString& right) noexcept
{
  return left.compareTo(right) < 0;
}

inline bool operator==(const PsramString& left,
                       const String& right) noexcept
{
  return left.compareTo(right) == 0;
}

inline bool operator!=(const PsramString& left,
                       const String& right) noexcept
{
  return !(left == right);
}

inline bool operator==(const String& left,
                       const PsramString& right) noexcept
{
  return right == left;
}

inline bool operator!=(const String& left,
                       const PsramString& right) noexcept
{
  return !(right == left);
}

inline bool operator==(const PsramString& left,
                       const char* right) noexcept
{
  return left.compareTo(right) == 0;
}

inline bool operator!=(const PsramString& left,
                       const char* right) noexcept
{
  return !(left == right);
}

inline void swap(PsramString& left, PsramString& right) noexcept
{
  left.swap(right);
}
