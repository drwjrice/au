#pragma once

#include "au/AuByteSource.h"
#include "au/ParseError.h"

#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <optional>
#include <unistd.h>
#include <sys/stat.h>

namespace au {

class FileByteSource : public AuByteSource { // TODO rename this and FileByteSourceImpl
protected:
  static constexpr size_t MIN_HIST_SIZE = 1 * 1024;

  const size_t INIT_BUFFER_SIZE;
  std::string name_;
  size_t bufSize_; //< Allocated size of current working buffer
  char *buf_;      //< Working buffer
  size_t pos_;     //< Current position in the underlying data stream
  char *cur_;      //< Current position in the working buffer
  char *limit_;    //< End of the current working buffer
  std::optional<size_t> pinPos_;

  bool waitForData_;

public:
  explicit FileByteSource(const std::string &fname, bool waitForData,
                          size_t bufferSizeInK = 256)
      : INIT_BUFFER_SIZE(bufferSizeInK * 1024),
        name_(fname == "-" ? "<stdin>" : fname),
        bufSize_(INIT_BUFFER_SIZE),
        buf_(static_cast<char *>(malloc(bufSize_))),
        pos_(0), cur_(buf_), limit_(buf_), waitForData_(waitForData) {}

  FileByteSource(const FileByteSource &) = delete;
  FileByteSource(FileByteSource &&) = delete;
  FileByteSource &operator=(const FileByteSource &) = delete;
  FileByteSource &operator=(FileByteSource &&) = delete;

  ~FileByteSource() override {
    free(buf_);
  }

  std::string name() const override {
    return name_;
  }

  /// Position in the underlying data stream
  size_t pos() const override { return pos_; }

  Byte next() override {
    while (cur_ == limit_) if (!read()) return Byte::Eof();
    pos_++;
    return Byte(*cur_++);
  }

  Byte peek() override {
    while (cur_ == limit_) if (!read()) return Byte::Eof();
    return Byte(*cur_);
  }

  void readFunc(size_t len, Fn &&func) override {
    while (len) {
      while (cur_ == limit_)
        if (!read())
          AU_THROW("reached eof while trying to read " << len << " bytes");
      // limit_ > cur_, so cast to size_t is fine...
      auto first = std::min(len, static_cast<size_t>(limit_ - cur_));
      func(std::string_view(cur_, first));
      pos_ += first;
      cur_ += first;
      len -= first;
    }
  }

  void skip(size_t len) override {
    // this implementation might conceivably fail if we're processing a
    // non-seekable stream like stdin. this has never been an issue in practice,
    // since we almost always call skip() with small values of len. but it's
    // worth noting, in case it ever comes up.
    seek(pos_ + len);
  }

  void setPin(size_t abspos) override final {
    // pin should be within the current buffer, but certainly ahead of the
    // current start of buffer
    assert(abspos >= pos_ - static_cast<size_t>(cur_ - buf_));
    pinPos_ = abspos;
  }

  void clearPin() override final {
    pinPos_.reset();
  }

  void seek(size_t abspos) override {
    if (abspos < pos_ && pos_ - abspos <= static_cast<size_t>(cur_ - buf_)) {
      auto relseek = pos_ - abspos;
      cur_ -= relseek;
      pos_ -= relseek;
    } else {
      doSeek(abspos);
      cur_ = limit_ = buf_;
      pos_ = abspos;
      clearPin(); // if we actually have to seek the stream, just forget the pin
      if (!read())
        THROW_RT("failed to read from new location");
    }
  }

  bool scanTo(std::string_view needle) override {
    while (true) {
      while (buffAvail() < needle.length()) {
        // we might have just done a seek that left us with a very small
        // amount of buffered data. alternatively, we might have already
        // attempted to find 'needle' and failed. then we've done a seek to
        // very near the end of the buffer, leaving just len(needle)-1 bytes.
        // the seek automatically clears the buffer and re-reads, but the
        // UNDERLYING source might just return the same few bytes again, in
        // which case we'll fail on the next iteration. the contract is that
        // the underlying source can return any non-zero number of bytes on a
        // read(), but it won't return 0 unless it really actually has no more
        // bytes to give us. therefore...

        // we attempt to keep reading until we either have enough buffer
        // to scan, or until we really can't get anything more.
        // this is a crappy way of doing this, but given the
        // current design, it's the simplest solution. this whole thing should
        // be refactored...
        if (!read())
          return false;
      }
      auto found = static_cast<char *>(memmem(cur_, buffAvail(), needle.data(),
        needle.length()));
      if (found) {
        assert(found >= cur_);
        size_t offset = static_cast<size_t>(found - cur_);
        pos_ += offset;
        cur_ += offset;
        return true;
      } else {
        // TODO for zipped files where seeking may expensive, it'll be better
        // perhaps to copy the last few bytes to the start of buffer and just
        // read again, since they're contiguous reads and we're not really seeking.
        // but the zipped file source may hide that anyway using a context like
        // zindex does
        skip(buffAvail()-(needle.length()-1));
      }
    }
  }

private:
  virtual ssize_t doRead(char *buf, size_t len) = 0;
  virtual void doSeek(size_t abspos) = 0;

  /// Free space in the buffer
  size_t buffFree() const {
    return bufSize_ - static_cast<size_t>(limit_ - buf_);
  }

  /// Available to be consumed
  size_t buffAvail() const {
    return static_cast<size_t>(limit_ - cur_);
  }

private:
  /// @return true if some data was read, false if 0 bytes were read.
  bool read() {
    // Keep a minimum amount of consumed data in the buffer so we can seek back
    // even in non-seekable data streams.
    auto histSz = MIN_HIST_SIZE;
    // and if the pinned position extends that history, so be it.
    if (pinPos_ && *pinPos_ < pos_) {
        auto pinnedHistSz = pos_ - *pinPos_;
        histSz = std::max(histSz, pinnedHistSz);
    }
    if (cur_ > buf_ + histSz) {
      auto startOfHistory = cur_ - histSz;
      memmove(buf_, startOfHistory,
              static_cast<size_t>(limit_ - startOfHistory));
      auto shift = startOfHistory - buf_;
      cur_ -= shift;
      limit_ -= shift;
    }

    // now see if we need to increase the size of the buffer.
    if (buffFree() == 0) {
        auto curPos = cur_ - buf_;
        auto limitPos = limit_ - buf_;
        // always grow the buffer by a constant amount, there's no particular
        // reason to believe it's going to need to grow exponentially or
        // anything like that.
        bufSize_ += INIT_BUFFER_SIZE;
        buf_ = static_cast<char *>(realloc(buf_, bufSize_));
        if (!buf_)
            THROW_RT("Unable to grow buffer to " << bufSize_ << " bytes!");
        cur_ = buf_ + curPos;
        limit_ = buf_ +  limitPos;
    }

    ssize_t bytesRead = 0;
    do {
      bytesRead = doRead(limit_, buffFree());
      if (bytesRead < 0) // TODO: && errno != EAGAIN ?
        THROW_RT("Error reading file: " << strerror(errno));
      if (bytesRead == 0 && waitForData_)
        sleep(1);
    } while (!bytesRead && waitForData_);

    if (!bytesRead) return false;
    limit_ += bytesRead;
    return true;
  }
};

class FileByteSourceImpl : public FileByteSource {
  int fd_;

public:
  explicit FileByteSourceImpl(const std::string &fname, bool waitForData,
                              size_t bufferSizeInK = 256)
      : FileByteSource(fname, waitForData, bufferSizeInK) {
    if (fname == "-") {
      fd_ = fileno(stdin);
    } else {
      fd_ = ::open(fname.c_str(), O_RDONLY);
    }
    if (fd_ == -1)
      THROW_RT("open: " << strerror(errno) << " (" << fname << ")");
#ifndef __APPLE__
    ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);  // TODO report error?
#endif
  }

  ~FileByteSourceImpl() override {
    close(fd_); // TODO report error?
  }

  size_t endPos() const override {
    struct stat stat;
    if (auto res = fstat(fd_, &stat); res < 0)
      THROW_RT("failed to stat file: " << strerror(errno));
    if (stat.st_size < 0)
      THROW_RT("file size was negative!");
    return static_cast<size_t>(stat.st_size);
  }

  bool isSeekable() const override {
    return lseek(fd_, 0, SEEK_CUR) == 0;
  }

private:
  ssize_t doRead(char *buf, size_t len) override {
    return ::read(fd_, buf, len);
  }

  void doSeek(size_t abspos) override {
    auto pos = lseek(fd_, static_cast<off_t>(abspos), SEEK_SET);
    if (pos < 0) {
      THROW_RT("failed to seek to desired location: " << strerror(errno));
    }
  }
};

}