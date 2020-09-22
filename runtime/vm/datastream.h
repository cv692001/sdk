// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_DATASTREAM_H_
#define RUNTIME_VM_DATASTREAM_H_

#include "include/dart_api.h"
#include "platform/assert.h"
#include "platform/utils.h"
#include "vm/allocation.h"
#include "vm/exceptions.h"
#include "vm/globals.h"
#include "vm/os.h"

namespace dart {

// (S)LEB128 encodes 7 bits of data per byte (hence 128).
static constexpr uint8_t kDataBitsPerByte = 7;
static constexpr uint8_t kDataByteMask = (1 << kDataBitsPerByte) - 1;
// If more data follows a given data byte, the high bit is set.
static constexpr uint8_t kMoreDataMask = (1 << kDataBitsPerByte);
// For SLEB128, the high bit in the data of the last byte is the sign bit.
static constexpr uint8_t kSignMask = (1 << (kDataBitsPerByte - 1));

typedef uint8_t* (*ReAlloc)(uint8_t* ptr, intptr_t old_size, intptr_t new_size);
typedef void (*DeAlloc)(uint8_t* ptr);

// Stream for reading various types from a buffer.
class ReadStream : public ValueObject {
 public:
  ReadStream(const uint8_t* buffer, intptr_t size)
      : buffer_(buffer), current_(buffer), end_(buffer + size) {}

  void SetStream(const uint8_t* buffer, intptr_t size) {
    buffer_ = buffer;
    current_ = buffer;
    end_ = buffer + size;
  }

  template <int N, typename T>
  class Raw {};

  template <typename T>
  class Raw<1, T> {
   public:
    static T Read(ReadStream* st) { return bit_cast<T>(st->ReadByte()); }
  };

  template <typename T>
  class Raw<2, T> {
   public:
    static T Read(ReadStream* st) { return bit_cast<T>(st->Read16<int16_t>()); }
  };

  template <typename T>
  class Raw<4, T> {
   public:
    static T Read(ReadStream* st) { return bit_cast<T>(st->Read32<int32_t>()); }
  };

  template <typename T>
  class Raw<8, T> {
   public:
    static T Read(ReadStream* st) { return bit_cast<T>(st->Read64<int64_t>()); }
  };

  // Reads 'len' bytes from the stream.
  void ReadBytes(uint8_t* addr, intptr_t len) {
    ASSERT((end_ - current_) >= len);
    if (len != 0) {
      memmove(addr, current_, len);
    }
    current_ += len;
  }

  // Reads a value of type [T] assuming an encoding of LEB128 (whether or not
  // the type itself is unsigned).
  template <typename T = intptr_t>
  T ReadUnsigned() {
    if (std::is_unsigned<T>::value) {
      return ReadInternal<T>();
    } else {
      return bit_cast<T>(ReadUnsigned<typename std::make_unsigned<T>::type>());
    }
  }

  intptr_t Position() const { return current_ - buffer_; }
  void SetPosition(intptr_t value) {
    ASSERT((end_ - buffer_) > value);
    current_ = buffer_ + value;
  }

  void Align(intptr_t alignment) {
    intptr_t position_before = Position();
    intptr_t position_after = Utils::RoundUp(position_before, alignment);
    Advance(position_after - position_before);
  }

  const uint8_t* AddressOfCurrentPosition() const { return current_; }

  void Advance(intptr_t value) {
    ASSERT((end_ - current_) >= value);
    current_ = current_ + value;
  }

  intptr_t PendingBytes() const {
    ASSERT(end_ >= current_);
    return (end_ - current_);
  }

  // Reads a value of type [T] assuming an encoding of SLEB128 (whether or not
  // the type itself is signed).
  template <typename T>
  T Read() {
    if (std::is_signed<T>::value) {
      return ReadInternal<T>();
    } else {
      return bit_cast<T>(Read<typename std::make_signed<T>::type>());
    }
  }

  uword ReadWordWith32BitReads() {
    constexpr intptr_t kNumBytesPerRead32 = sizeof(uint32_t);
    constexpr intptr_t kNumRead32PerWord = sizeof(uword) / kNumBytesPerRead32;
    constexpr intptr_t kNumBitsPerRead32 = kNumBytesPerRead32 * kBitsPerByte;

    uword value = 0;
    for (intptr_t j = 0; j < kNumRead32PerWord; j++) {
      const auto partial_value = Raw<kNumBytesPerRead32, uint32_t>::Read(this);
      value |= (static_cast<uword>(partial_value) << (j * kNumBitsPerRead32));
    }
    return value;
  }

 private:
  template <typename T>
  T ReadInternal() {
    using Unsigned = typename std::make_unsigned<T>::type;
    const uint8_t* c = current_;
    Unsigned r = 0;
    uint8_t s = 0;
    uint8_t b;
    do {
      ASSERT(c < end_);
      b = *c++;
      r |= static_cast<Unsigned>(b & kDataByteMask) << s;
      s += kDataBitsPerByte;
    } while ((b & kMoreDataMask) != 0);
    current_ = c;
    // At this point, [s] contains how many data bits have made it into the
    // value. If the type is signed, the value negative, and the count of data
    // bits is less than the size of the value, then we need to extend the sign
    // by setting the remaining (unset) most significant bits (MSBs).
    Unsigned sign_bits = 0;
    const bool is_signed = std::is_signed<T>::value;
    if (is_signed && (b & kSignMask) != 0 && s < (kBitsPerByte * sizeof(T))) {
      // Create a bitmask for the current data bits and invert it.
      sign_bits = ~((static_cast<Unsigned>(1) << s) - 1);
    }
    return static_cast<T>(r | sign_bits);
  }

// Setting up needed variables for the unrolled loop sections below.
#define UNROLLED_INIT()                                                        \
  using Unsigned = typename std::make_unsigned<T>::type;                       \
  const uint8_t* c = current_;                                                 \
  uint8_t b;                                                                   \
  Unsigned r = 0;

// Part of the unrolled loop where the loop may stop, having read the last part,
// or continue reading. If stopping, extend the sign for signed values.
#define UNROLLED_BODY(bit_start)                                               \
  static_assert(bit_start % kDataBitsPerByte == 0,                             \
                "Bit start must be a multiple of the data bits per byte");     \
  static_assert(bit_start >= 0 && bit_start < kBitsPerByte * sizeof(T),        \
                "Starting unrolled body at invalid bit position");             \
  ASSERT(c < end_);                                                            \
  b = *c++;                                                                    \
  r |= static_cast<Unsigned>(b & kDataByteMask) << bit_start;                  \
  if ((b & kMoreDataMask) == 0) {                                              \
    current_ = c;                                                              \
    Unsigned sign_bits = 0;                                                    \
    if (std::is_signed<T>::value && (b & kSignMask) != 0) {                    \
      sign_bits = ~((static_cast<Unsigned>(1) << (bit_start + 7)) - 1);        \
    }                                                                          \
    return static_cast<T>(r | sign_bits);                                      \
  }

// If the unrolled end is reached, the last part always includes the original
// sign bit, so no need to sign extend.
#define UNROLLED_END(bit_start)                                                \
  static_assert(bit_start % kDataBitsPerByte == 0,                             \
                "Bit start must be a multiple of the data bits per byte");     \
  static_assert(bit_start >= 0 && bit_start < kBitsPerByte * sizeof(T),        \
                "Starting unrolled end at invalid bit position");              \
  static_assert(bit_start + kDataBitsPerByte >= kBitsPerByte * sizeof(T),      \
                "Unrolled end does not contain final bits in value");          \
  ASSERT(c < end_);                                                            \
  b = *c++;                                                                    \
  r |= static_cast<Unsigned>(b & kDataByteMask) << bit_start;                  \
  ASSERT_EQUAL((b & kMoreDataMask), 0);                                        \
  current_ = c;                                                                \
  return static_cast<T>(r);

  template <typename T>
  T Read16() {
    UNROLLED_INIT();
    UNROLLED_BODY(0);
    UNROLLED_BODY(7);
    UNROLLED_END(14);
  }

  template <typename T>
  T Read32() {
    UNROLLED_INIT();
    UNROLLED_BODY(0);
    UNROLLED_BODY(7);
    UNROLLED_BODY(14);
    UNROLLED_BODY(21);
    UNROLLED_END(28);
  }

  template <typename T>
  T Read64() {
    UNROLLED_INIT();
    UNROLLED_BODY(0);
    UNROLLED_BODY(7);
    UNROLLED_BODY(14);
    UNROLLED_BODY(21);
    UNROLLED_BODY(28);
    UNROLLED_BODY(35);
    UNROLLED_BODY(42);
    UNROLLED_BODY(49);
    UNROLLED_BODY(56);
    UNROLLED_END(63);
  }

#undef UNROLLED_END
#undef UNROLLED_BODY
#undef UNROLLED_INIT

  uint8_t ReadByte() {
    ASSERT(current_ < end_);
    return *current_++;
  }

 private:
  const uint8_t* buffer_;
  const uint8_t* current_;
  const uint8_t* end_;

  DISALLOW_COPY_AND_ASSIGN(ReadStream);
};

// Stream for writing various types into a buffer.
template <typename B>
class WriteStreamBase : public B {
 public:
  WriteStreamBase(uint8_t** buffer, ReAlloc alloc, intptr_t initial_size)
      : buffer_(buffer),
        end_(NULL),
        current_(NULL),
        current_size_(0),
        alloc_(alloc),
        initial_size_(initial_size) {
    ASSERT(buffer != NULL);
    ASSERT(alloc != NULL);
    *buffer_ = reinterpret_cast<uint8_t*>(alloc_(NULL, 0, initial_size_));
    if (*buffer_ == NULL) {
      Exceptions::ThrowOOM();
    }
    current_ = *buffer_;
    current_size_ = initial_size_;
    end_ = *buffer_ + initial_size_;
  }

  uint8_t* buffer() const { return *buffer_; }
  void set_buffer(uint8_t* value) { *buffer_ = value; }

  ReAlloc alloc() const { return alloc_; }
  intptr_t bytes_written() const { return current_ - *buffer_; }

  intptr_t Position() const { return current_ - *buffer_; }
  void SetPosition(intptr_t value) { current_ = *buffer_ + value; }

  void Align(intptr_t alignment) {
    intptr_t position_before = Position();
    intptr_t position_after = Utils::RoundUp(position_before, alignment);
    memset(current_, 0, position_after - position_before);
    SetPosition(position_after);
  }

  template <int N, typename T>
  class Raw {};

  template <typename T>
  class Raw<1, T> {
   public:
    static void Write(WriteStreamBase<B>* st, T value) {
      st->WriteByte(bit_cast<uint8_t>(value));
    }
  };

  template <typename T>
  class Raw<2, T> {
   public:
    static void Write(WriteStreamBase<B>* st, T value) {
      st->Write<int16_t>(bit_cast<int16_t>(value));
    }
  };

  template <typename T>
  class Raw<4, T> {
   public:
    static void Write(WriteStreamBase<B>* st, T value) {
      st->Write<int32_t>(bit_cast<int32_t>(value));
    }
  };

  template <typename T>
  class Raw<8, T> {
   public:
    static void Write(WriteStreamBase<B>* st, T value) {
      st->Write<int64_t>(bit_cast<int64_t>(value));
    }
  };

  void WriteWordWith32BitWrites(uword value) {
    constexpr intptr_t kNumBytesPerWrite32 = sizeof(uint32_t);
    constexpr intptr_t kNumWrite32PerWord = sizeof(uword) / kNumBytesPerWrite32;
    constexpr intptr_t kNumBitsPerWrite32 = kNumBytesPerWrite32 * kBitsPerByte;

    const uint32_t mask = Utils::NBitMask(kNumBitsPerWrite32);
    for (intptr_t j = 0; j < kNumWrite32PerWord; j++) {
      const uint32_t shifted_value = (value >> (j * kNumBitsPerWrite32));
      Raw<kNumBytesPerWrite32, uint32_t>::Write(this, shifted_value & mask);
    }
  }

  // Writes the LEB128 encoding of [value] to the stream (whether or not the
  // type [T] is unsigned).
  template <typename T>
  void WriteUnsigned(T value) {
    ASSERT(value >= 0);
    if (std::is_unsigned<T>::value) {
      WriteInternal<T>(value);
    } else {
      using Unsigned = typename std::make_unsigned<T>::type;
      WriteUnsigned<Unsigned>(bit_cast<Unsigned>(value));
    }
  }

  void WriteBytes(const void* addr, intptr_t len) {
    if ((end_ - current_) < len) {
      Resize(len);
    }
    ASSERT((end_ - current_) >= len);
    if (len != 0) {
      memmove(current_, addr, len);
    }
    current_ += len;
  }

  void WriteWord(uword value) {
    const intptr_t len = sizeof(uword);
    if ((end_ - current_) < len) {
      Resize(len);
    }
    ASSERT((end_ - current_) >= len);
    *reinterpret_cast<uword*>(current_) = value;
    current_ += len;
  }

  void WriteTargetWord(uword value) {
#if defined(IS_SIMARM_X64)
    RELEASE_ASSERT(Utils::IsInt(32, static_cast<word>(value)));
    const intptr_t len = sizeof(uint32_t);
    if ((end_ - current_) < len) {
      Resize(len);
    }
    ASSERT((end_ - current_) >= len);
    *reinterpret_cast<uint32_t*>(current_) = static_cast<uint32_t>(value);
    current_ += len;
#else   // defined(IS_SIMARM_X64)
    WriteWord(value);
#endif  // defined(IS_SIMARM_X64)
  }

  void Print(const char* format, ...) {
    va_list args;
    va_start(args, format);
    VPrint(format, args);
    va_end(args);
  }

  void VPrint(const char* format, va_list args) {
    // Measure.
    va_list measure_args;
    va_copy(measure_args, args);
    intptr_t len = Utils::VSNPrint(NULL, 0, format, measure_args);
    va_end(measure_args);

    // Alloc.
    if ((end_ - current_) < (len + 1)) {
      Resize(len + 1);
    }
    ASSERT((end_ - current_) >= (len + 1));

    // Print.
    va_list print_args;
    va_copy(print_args, args);
    Utils::VSNPrint(reinterpret_cast<char*>(current_), len + 1, format,
                    print_args);
    va_end(print_args);
    current_ += len;  // Not len + 1 to swallow the terminating NUL.
  }

  // Writes the SLEB128 encoding of [value] to the stream (whether or not the
  // type [T] is signed).
  template <typename T>
  void Write(T value) {
    if (std::is_signed<T>::value) {
      WriteInternal<T>(value);
    } else {
      using Signed = typename std::make_signed<T>::type;
      Write<Signed>(bit_cast<Signed>(value));
    }
  }

  template <typename T>
  void WriteFixed(T value) {
    const intptr_t len = sizeof(T);
    if ((end_ - current_) < len) {
      Resize(len);
    }
    ASSERT((end_ - current_) >= len);
    *reinterpret_cast<T*>(current_) = static_cast<T>(value);
    current_ += len;
  }

 private:
  template <typename T>
  void WriteInternal(T value) {
    T remainder = value;
    bool is_last_part;
    do {
      uint8_t part = static_cast<uint8_t>(remainder & kDataByteMask);
      remainder >>= kDataBitsPerByte;
      // For unsigned types, we're done when the remainder has no bits set.
      // For signed types, we're done when either:
      // - the remainder has no bits set and the part's sign bit is unset, or
      // - the remainder has all bits set and the part's sign bit is set.
      // If the remainder matches but the sign bit does not, we need one more
      // part to set the sign bit correctly.
      is_last_part =
          std::is_unsigned<T>::value
              ? remainder == static_cast<T>(0)
              : (remainder == static_cast<T>(0) && (part & kSignMask) == 0) ||
                    (remainder == ~static_cast<T>(0) &&
                     (part & kSignMask) != 0);
      if (!is_last_part) {
        // Mark this part as having more parts following it.
        part |= kMoreDataMask;
      }
      WriteByte(part);
    } while (!is_last_part);
  }

  DART_FORCE_INLINE void WriteByte(uint8_t value) {
    if (current_ >= end_) {
      Resize(1);
    }
    ASSERT(current_ < end_);
    *current_++ = value;
  }

  void Resize(intptr_t size_needed) {
    intptr_t position = current_ - *buffer_;
    intptr_t increment_size = current_size_;
    if (size_needed > increment_size) {
      increment_size = Utils::RoundUp(size_needed, initial_size_);
    }
    intptr_t new_size = current_size_ + increment_size;
    ASSERT(new_size > current_size_);
    *buffer_ =
        reinterpret_cast<uint8_t*>(alloc_(*buffer_, current_size_, new_size));
    if (*buffer_ == NULL) {
      Exceptions::ThrowOOM();
    }
    current_ = *buffer_ + position;
    current_size_ = new_size;
    end_ = *buffer_ + new_size;
    ASSERT(end_ > *buffer_);
  }

  uint8_t** const buffer_;
  uint8_t* end_;
  uint8_t* current_;
  intptr_t current_size_;
  ReAlloc alloc_;
  intptr_t initial_size_;

  DISALLOW_COPY_AND_ASSIGN(WriteStreamBase);
};

class WriteStream : public WriteStreamBase<ValueObject> {
 public:
  WriteStream(uint8_t** buffer, ReAlloc alloc, intptr_t initial_size)
      : WriteStreamBase<ValueObject>(buffer, alloc, initial_size) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(WriteStream);
};

class ZoneWriteStream : public WriteStreamBase<ZoneAllocated> {
 public:
  ZoneWriteStream(uint8_t** buffer, ReAlloc alloc, intptr_t initial_size)
      : WriteStreamBase<ZoneAllocated>(buffer, alloc, initial_size) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(ZoneWriteStream);
};

class StreamingWriteStream : public ValueObject {
 public:
  explicit StreamingWriteStream(intptr_t initial_capacity,
                                Dart_StreamingWriteCallback callback,
                                void* callback_data);
  ~StreamingWriteStream();

  intptr_t position() const { return flushed_size_ + (cursor_ - buffer_); }

  void Align(intptr_t alignment) {
    intptr_t padding = Utils::RoundUp(position(), alignment) - position();
    EnsureAvailable(padding);
    memset(cursor_, 0, padding);
    cursor_ += padding;
  }

  void Print(const char* format, ...) {
    va_list args;
    va_start(args, format);
    VPrint(format, args);
    va_end(args);
  }
  void VPrint(const char* format, va_list args);

  void WriteBytes(const uint8_t* buffer, intptr_t size) {
    EnsureAvailable(size);
    if (size != 0) {
      memmove(cursor_, buffer, size);
    }
    cursor_ += size;
  }

 private:
  void EnsureAvailable(intptr_t needed) {
    intptr_t available = limit_ - cursor_;
    if (available >= needed) return;
    EnsureAvailableSlowPath(needed);
  }

  void EnsureAvailableSlowPath(intptr_t needed);
  void Flush();

  uint8_t* buffer_;
  uint8_t* cursor_;
  uint8_t* limit_;
  intptr_t flushed_size_;
  Dart_StreamingWriteCallback callback_;
  void* callback_data_;

  DISALLOW_COPY_AND_ASSIGN(StreamingWriteStream);
};

}  // namespace dart

#endif  // RUNTIME_VM_DATASTREAM_H_
