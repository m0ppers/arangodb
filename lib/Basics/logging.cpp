////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "Basics/logging.h"

#ifdef _WIN32
#include "Basics/win-utils.h"
#endif

#ifdef TRI_ENABLE_SYSLOG
#define SYSLOG_NAMES
#define prioritynames TRI_prioritynames
#define facilitynames TRI_facilitynames
#include <syslog.h>
#endif

#include "Basics/Exceptions.h"
#include "Basics/files.h"
#include "Basics/hashes.h"
#include "Basics/locks.h"
#include "Basics/Logger.h"
#include "Basics/Mutex.h"
#include "Basics/MutexLocker.h"
#include "Basics/shell-colors.h"
#include "Basics/Thread.h"
#include "Basics/tri-strings.h"
#include "Basics/vector.h"

using namespace arangodb;
using namespace arangodb::basics;

////////////////////////////////////////////////////////////////////////////////
/// @brief log appenders type
////////////////////////////////////////////////////////////////////////////////

typedef enum {
  APPENDER_TYPE_FILE,
  APPENDER_TYPE_SYSLOG
} TRI_log_appender_type_e;

////////////////////////////////////////////////////////////////////////////////
/// @brief message container
////////////////////////////////////////////////////////////////////////////////

struct log_message_t {
  log_message_t(log_message_t const&) = delete;
  log_message_t& operator=(log_message_t const&) = delete;

  log_message_t(TRI_log_level_e level, TRI_log_severity_e severity,
                char* message, size_t length, bool claimOwnership)
      : _level(level),
        _severity(severity),
        _message(message),
        _length(length),
        _freeMessage(false) {
    TRI_ASSERT(message != nullptr);

    if (!claimOwnership) {
      // need to copy the message before it is invalidated
      _message = TRI_DuplicateString(TRI_UNKNOWN_MEM_ZONE, message, length);
    }

    if (_message == nullptr) {
      _message = (char*)"out-of-memory";
      _freeMessage = false;
    } else {
      _freeMessage = true;
    }
  }

  ~log_message_t() {
    if (_freeMessage) {
      // free the message
      TRI_ASSERT(_message != nullptr);
      TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, _message);
    }
  }

  TRI_log_level_e const _level;
  TRI_log_severity_e const _severity;
  char* _message;
  size_t const _length;
  bool _freeMessage;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief base structure for log appenders
////////////////////////////////////////////////////////////////////////////////

struct TRI_log_appender_t {
  TRI_log_appender_t(char const*, TRI_log_severity_e, bool);
  virtual ~TRI_log_appender_t();

  virtual void logMessage(TRI_log_level_e, TRI_log_severity_e, char const* msg,
                          size_t length) = 0;
  virtual void reopenLog() = 0;
  virtual void closeLog() = 0;
  virtual std::string details() = 0;
  virtual TRI_log_appender_type_e type() = 0;
  virtual char const* typeName() = 0;

  char* _contentFilter;  // an optional content filter for log messages
  TRI_log_severity_e _severityFilter;  // appender will care only about message
                                       // with a specific severity. set to
                                       // TRI_LOG_SEVERITY_UNKNOWN to catch all
  bool _consume;  // whether or not the appender will consume the message (true)
                  // or let it through to other appenders (false)
};

////////////////////////////////////////////////////////////////////////////////
/// @brief create the appender
////////////////////////////////////////////////////////////////////////////////

TRI_log_appender_t::TRI_log_appender_t(char const* contentFilter,
                                       TRI_log_severity_e severityFilter,
                                       bool consume)
    : _contentFilter(nullptr),
      _severityFilter(severityFilter),
      _consume(consume) {
  if (contentFilter != nullptr) {
    _contentFilter = TRI_DuplicateString(TRI_UNKNOWN_MEM_ZONE, contentFilter);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the appender
////////////////////////////////////////////////////////////////////////////////

TRI_log_appender_t::~TRI_log_appender_t() {}

////////////////////////////////////////////////////////////////////////////////
/// @brief already initialized
////////////////////////////////////////////////////////////////////////////////

static volatile int Initialized = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief shutdown function already installed
////////////////////////////////////////////////////////////////////////////////

static volatile bool ShutdownInitalized = false;

////////////////////////////////////////////////////////////////////////////////
/// @brief name of first log file
////////////////////////////////////////////////////////////////////////////////

static char* LogfileName = nullptr;

////////////////////////////////////////////////////////////////////////////////
/// @brief log appenders
////////////////////////////////////////////////////////////////////////////////

static std::vector<TRI_log_appender_t*> Appenders;

////////////////////////////////////////////////////////////////////////////////
/// @brief log appenders
////////////////////////////////////////////////////////////////////////////////

static arangodb::Mutex AppendersLock;

////////////////////////////////////////////////////////////////////////////////
/// @brief maximal output length
////////////////////////////////////////////////////////////////////////////////

#define OUTPUT_MAX_LENGTH (256)

////////////////////////////////////////////////////////////////////////////////
/// @brief output buffer size
////////////////////////////////////////////////////////////////////////////////

#define OUTPUT_BUFFER_SIZE (1024)

////////////////////////////////////////////////////////////////////////////////
/// @brief output log levels
////////////////////////////////////////////////////////////////////////////////

#define OUTPUT_LOG_LEVELS (6)

////////////////////////////////////////////////////////////////////////////////
/// @brief current buffer lid
////////////////////////////////////////////////////////////////////////////////

static uint64_t BufferLID = 1;

////////////////////////////////////////////////////////////////////////////////
/// @brief current buffer position
////////////////////////////////////////////////////////////////////////////////

static size_t BufferCurrent[OUTPUT_LOG_LEVELS] = {0, 0, 0, 0, 0, 0};

////////////////////////////////////////////////////////////////////////////////
/// @brief current buffer output
////////////////////////////////////////////////////////////////////////////////

static TRI_log_buffer_t BufferOutput[OUTPUT_LOG_LEVELS][OUTPUT_BUFFER_SIZE];

////////////////////////////////////////////////////////////////////////////////
/// @brief buffer lock
////////////////////////////////////////////////////////////////////////////////

static arangodb::Mutex BufferLock;

////////////////////////////////////////////////////////////////////////////////
/// @brief condition variable for the logger
////////////////////////////////////////////////////////////////////////////////

static TRI_condition_t LogCondition;

////////////////////////////////////////////////////////////////////////////////
/// @brief message queue lock
////////////////////////////////////////////////////////////////////////////////

static arangodb::Mutex LogMessageQueueLock;

////////////////////////////////////////////////////////////////////////////////
/// @brief message queue
////////////////////////////////////////////////////////////////////////////////

static std::vector<log_message_t*> LogMessageQueue;

////////////////////////////////////////////////////////////////////////////////
/// @brief thread used for logging
////////////////////////////////////////////////////////////////////////////////

static TRI_thread_t LoggingThread;

////////////////////////////////////////////////////////////////////////////////
/// @brief thread used for logging
////////////////////////////////////////////////////////////////////////////////

static std::atomic<bool> LoggingThreadActive(false);

////////////////////////////////////////////////////////////////////////////////
/// @brief use local time for dates & times in log output
////////////////////////////////////////////////////////////////////////////////

static sig_atomic_t UseLocalTime = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief show line numbers, debug and trace always show the line numbers
////////////////////////////////////////////////////////////////////////////////

static sig_atomic_t ShowLineNumber = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief show thread identifier
////////////////////////////////////////////////////////////////////////////////

static sig_atomic_t ShowThreadIdentifier = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief output prefix
////////////////////////////////////////////////////////////////////////////////

static std::atomic<char*> OutputPrefix(nullptr);

////////////////////////////////////////////////////////////////////////////////
/// @brief logging active
////////////////////////////////////////////////////////////////////////////////

static sig_atomic_t LoggingActive = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief logging thread
////////////////////////////////////////////////////////////////////////////////

static bool ThreadedLogging = false;

////////////////////////////////////////////////////////////////////////////////
/// @brief stores output in a buffer
////////////////////////////////////////////////////////////////////////////////

static void StoreOutput(TRI_log_level_e level, time_t timestamp,
                        char const* text, size_t length) {
  size_t pos = (size_t)level;

  if (pos >= OUTPUT_LOG_LEVELS) {
    return;
  }

  char* msg;

  if (length > OUTPUT_MAX_LENGTH) {
    // use the UNKNOWN_MEM_ZONE here...
    // if we use CORE_MEM_ZONE and malloc fails, this fact would be logged.
    // but we are in the logging already...
    msg = static_cast<char*>(
        TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, OUTPUT_MAX_LENGTH + 1, false));

    if (msg != nullptr) {
      memcpy(msg, text, OUTPUT_MAX_LENGTH - 4);
      memcpy(msg + OUTPUT_MAX_LENGTH - 4, " ...", 4);
      // append the \0 byte, otherwise we have potentially unbounded strings
      msg[OUTPUT_MAX_LENGTH] = '\0';
    }
  } else {
    msg = TRI_DuplicateString(TRI_UNKNOWN_MEM_ZONE, text, length);
  }

  if (msg == nullptr) {
    // unable to allocate memory for the log message
    // do not try to log this (as we're in the logger ourselves)
    return;
  }

  char* old = nullptr;

  {
    MUTEX_LOCKER(mutexLocker, BufferLock);

    size_t oldPos = BufferCurrent[pos];
    BufferCurrent[pos] = (oldPos + 1) % OUTPUT_BUFFER_SIZE;
    size_t cur = BufferCurrent[pos];

    TRI_log_buffer_t* buf = &BufferOutput[pos][cur];

    // save the old value, so we can free it outside the mutex
    old = buf->_text;

    buf->_lid = BufferLID++;
    buf->_level = level;
    buf->_timestamp = timestamp;
    buf->_text = msg;
  }

  // now free the old value outside the mutex
  if (old != nullptr) {
    TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, old);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief compares the lid
////////////////////////////////////////////////////////////////////////////////

static int LidCompare(void const* l, void const* r) {
  TRI_log_buffer_t const* left = static_cast<TRI_log_buffer_t const*>(l);
  TRI_log_buffer_t const* right = static_cast<TRI_log_buffer_t const*>(r);

  return (int)(((int64_t)left->_lid) - ((int64_t)right->_lid));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generates a message string
////////////////////////////////////////////////////////////////////////////////

static int GenerateMessage(char* buffer, size_t size, int* offset,
                           char const* func, char const* file, int line,
                           TRI_log_level_e level, TRI_pid_t currentProcessId,
                           uint64_t threadNumber, char const* fmt, va_list ap) {
  int m;
  int n;

  // we store the "real" beginning of the message (without any prefixes) here
  *offset = 0;

  // .............................................................................
  // append the time prefix and output prefix
  // .............................................................................

  n = 0;

  {
    auto outputPrefix = OutputPrefix.load(std::memory_order_relaxed);

    if (outputPrefix != nullptr && *outputPrefix) {
      n = snprintf(buffer, size, "%s ", outputPrefix);
    }
  }

  if (n < 0) {
    return n;
  } else if ((int)size <= n) {
    return n;
  }

  m = n;

  // .............................................................................
  // append the process / thread identifier
  // .............................................................................

  if (ShowThreadIdentifier) {
    n = snprintf(buffer + m, size - m, "[%llu-%llu] ",
                 (unsigned long long)currentProcessId,
                 (unsigned long long)threadNumber);
  } else {
    n = snprintf(buffer + m, size - m, "[%llu] ",
                 (unsigned long long)currentProcessId);
  }

  if (n < 0) {
    return n;
  } else if ((int)size <= m + n) {
    return m + n;
  }

  m += n;

  // .............................................................................
  // append the log level
  // .............................................................................

  char const* ll = "UNKNOWN";

  switch (level) {
    case TRI_LOG_LEVEL_FATAL:
      ll = "FATAL";
      break;
    case TRI_LOG_LEVEL_ERROR:
      ll = "ERROR";
      break;
    case TRI_LOG_LEVEL_WARNING:
      ll = "WARNING";
      break;
    case TRI_LOG_LEVEL_INFO:
      ll = "INFO";
      break;
    case TRI_LOG_LEVEL_DEBUG:
      ll = "DEBUG";
      break;
    case TRI_LOG_LEVEL_TRACE:
      ll = "TRACE";
      break;
  }

  n = snprintf(buffer + m, size - m, "%s ", ll);

  if (n < 0) {
    return n;
  } else if ((int)size <= m + n) {
    return m + n;
  }

  m += n;

  // .............................................................................
  // check if we must display the line number
  // .............................................................................

  bool sln = ShowLineNumber > 0;

  if (level == TRI_LOG_LEVEL_DEBUG || level == TRI_LOG_LEVEL_TRACE) {
    sln = true;
  }

  // .............................................................................
  // append the file and line
  // .............................................................................

  if (sln) {
    n = snprintf(buffer + m, size - m, "[%s:%d] ", file, line);

    if (n < 0) {
      return n;
    } else if ((int)size <= m + n) {
      return m + n;
    }

    m += n;
  }

  // .............................................................................
  // append the message
  // .............................................................................

  // store the "real" beginning of the message (without any prefixes) in offset
  *offset = m;
  n = vsnprintf(buffer + m, size - m, fmt, ap);

  if (n < 0) {
    return n;
  }

  return m + n;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write to stderr
////////////////////////////////////////////////////////////////////////////////

static void WriteStderr(TRI_log_level_e level, char const* msg) {
  if (level == TRI_LOG_LEVEL_FATAL || level == TRI_LOG_LEVEL_ERROR) {
    fprintf(stderr, TRI_SHELL_COLOR_RED "%s" TRI_SHELL_COLOR_RESET "\n", msg);
  } else if (level == TRI_LOG_LEVEL_WARNING) {
    fprintf(stderr, TRI_SHELL_COLOR_YELLOW "%s" TRI_SHELL_COLOR_RESET "\n",
            msg);
  } else {
    fprintf(stderr, "%s\n", msg);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief outputs a message string to all appenders
////////////////////////////////////////////////////////////////////////////////

static void OutputMessage(TRI_log_level_e level, TRI_log_severity_e severity,
                          char* message, size_t length, size_t offset,
                          bool claimOwnership) {
  TRI_ASSERT(message != nullptr);

  if (!LoggingActive) {
    WriteStderr(level, message);

    if (claimOwnership) {
      TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, message);
    }

    return;
  }

  // copy message to ring buffer of recent log messages
  if (severity == TRI_LOG_SEVERITY_HUMAN) {
    // we start copying the message from the given offset to skip any irrelevant
    // or redundant message parts such as date, info etc. The offset might be 0
    // though.
    TRI_ASSERT(length >= offset);
    StoreOutput(level, time(0), message + offset, (size_t)(length - offset));
  }

  {
    MUTEX_LOCKER(mutexLocker, AppendersLock);

    if (Appenders.empty()) {
      WriteStderr(level, message);

      if (claimOwnership) {
        TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, message);
      }
      return;
    }
  }

  if (ThreadedLogging) {
    auto msg = std::make_unique<log_message_t>(level, severity, message, length,
                                               claimOwnership);

    try {
      MUTEX_LOCKER(mutexLocker, LogMessageQueueLock);
      LogMessageQueue.emplace_back(msg.get());
      msg.release();
    } catch (...) {
      if (claimOwnership) {
        TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, message);
      }
      // we can do nothing else here if we ran out of memory
    }
  } else {
    MUTEX_LOCKER(mutexLocker, AppendersLock);

    for (auto& it : Appenders) {
      // apply severity filter
      if (it->_severityFilter != TRI_LOG_SEVERITY_UNKNOWN &&
          it->_severityFilter != severity) {
        continue;
      }

      // apply content filter on log message
      if (it->_contentFilter != nullptr) {
        if (!TRI_IsContainedString(message, it->_contentFilter)) {
          continue;
        }
      }

      it->logMessage(level, severity, message, length);

      if (it->_consume) {
        break;
      }
    }

    if (claimOwnership) {
      TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, message);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief checks the message queue and sends message to appenders
////////////////////////////////////////////////////////////////////////////////

static void MessageQueueWorker(void* data) {
  int sl = 100;

  // now we're active
  LoggingThreadActive.store(true);

  while (true) {
    std::vector<log_message_t*> buffer;
    // copy the MessageQueue into the local buffer
    {
      MUTEX_LOCKER(mutexLocker, LogMessageQueueLock);
      buffer.swap(LogMessageQueue);
    }

    if (buffer.empty()) {
      sl += 1000;

      if (1000000 < sl) {
        sl = 1000000;
      }
    } else {
      TRI_ASSERT(!buffer.empty());

      // output messages using the appenders
      for (auto& msg : buffer) {
        {
          MUTEX_LOCKER(mutexLocker, AppendersLock);

          for (auto& it : Appenders) {
            // apply severity filter
            if (it->_severityFilter != TRI_LOG_SEVERITY_UNKNOWN &&
                it->_severityFilter != msg->_severity) {
              continue;
            }

            // apply content filter on log message
            if (it->_contentFilter != nullptr) {
              if (!TRI_IsContainedString(msg->_message, it->_contentFilter)) {
                continue;
              }
            }

            it->logMessage(msg->_level, msg->_severity, msg->_message,
                           msg->_length);

            if (it->_consume) {
              break;
            }
          }
        }

        delete msg;
      }

      buffer.clear();

      // sleep a little while
      sl = 100;
    }

    if (LoggingActive) {
      TRI_LockCondition(&LogCondition);
      TRI_TimedWaitCondition(&LogCondition, (uint64_t)sl);
      TRI_UnlockCondition(&LogCondition);
    } else {
      MUTEX_LOCKER(mutexLocker, LogMessageQueueLock);
      if (LogMessageQueue.empty()) {
        // queue is empty. we can leave this loop
        break;
      }
    }
  }

  // cleanup
  {
    MUTEX_LOCKER(mutexLocker, LogMessageQueueLock);
    for (auto& it : LogMessageQueue) {
      delete it;
    }
    LogMessageQueue.clear();
  }

  LoggingThreadActive.store(false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove all % from the format string so we can safely print it.
////////////////////////////////////////////////////////////////////////////////

static void DisarmFormatString(std::string& dangerousString) {
  std::replace(dangerousString.begin(), dangerousString.end(), '%', '^');
}

////////////////////////////////////////////////////////////////////////////////
/// @brief logs a new message with given thread information
////////////////////////////////////////////////////////////////////////////////

static void LogThread(char const* func, char const* file, int line,
                      TRI_log_level_e level, TRI_log_severity_e severity,
                      TRI_pid_t processId, uint64_t threadNumber,
                      char const* fmt, va_list ap) {
  static int const maxSize = 100 * 1024;
  va_list ap2;
  char buffer[2048];  // try a static buffer first
  time_t tt;
  struct tm tb;
  size_t len;
  int offset;
  int n;

  // .............................................................................
  // generate time prefix
  // .............................................................................

  tt = time(0);
  if (UseLocalTime == 0) {
    // use GMtime
    TRI_gmtime(tt, &tb);
    // write time in buffer
    len = strftime(buffer, 32, "%Y-%m-%dT%H:%M:%SZ ", &tb);
  } else {
    // use localtime
    TRI_localtime(tt, &tb);
    len = strftime(buffer, 32, "%Y-%m-%dT%H:%M:%S ", &tb);
  }

  errno = TRI_ERROR_NO_ERROR;
  va_copy(ap2, ap);
  n = GenerateMessage(buffer + len, sizeof(buffer) - len, &offset, func, file,
                      line, level, processId, threadNumber, fmt, ap2);
  va_end(ap2);

  if (n == -1) {
#ifdef _WIN32
    if (errno != EINVAL) {
      n = sizeof(buffer) * 2;
    } else
#endif
      try {
        std::string message("format string is corrupt: [");
        message += fmt + std::string("] - GenerateMessage failed");
        TRI_GetBacktrace(message);
        DisarmFormatString(message);
        TRI_Log(func, file, line, TRI_LOG_LEVEL_WARNING, TRI_LOG_SEVERITY_HUMAN,
                message.c_str());
      } catch (...) {
      }
    return;
  }
  if (n < (int)(sizeof(buffer) - len)) {
    // static buffer was big enough
    OutputMessage(level, severity, buffer, (size_t)(n + len),
                  (size_t)(offset + len), false);
    return;
  }

  // static buffer was not big enough
  while (n < maxSize) {
    int m;

    // allocate as much memory as we need
    char* p = static_cast<char*>(
        TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, n + len + 2, false));

    if (p == nullptr) {
      TRI_Log(func, file, line, TRI_LOG_LEVEL_ERROR, TRI_LOG_SEVERITY_HUMAN,
              "log message is too large (%d bytes)", n + len);
      return;
    }

    if (len > 0) {
      // copy still existing and unchanged time prefix into dynamic buffer
      memcpy(p, buffer, len);
    }

    errno = TRI_ERROR_NO_ERROR;
    va_copy(ap2, ap);
    m = GenerateMessage(p + len, n + 1, &offset, func, file, line, level,
                        processId, threadNumber, fmt, ap2);
    va_end(ap2);

    if (m == -1) {
#ifdef _WIN32
      if ((errno != EINVAL) && (n * 2 < maxSize)) {
        TRI_Free(TRI_UNKNOWN_MEM_ZONE, p);
        n *= 2;
      } else
#endif
      {
        TRI_Free(TRI_UNKNOWN_MEM_ZONE, p);
        try {
          std::string message("format string is corrupt: [");
          message += fmt + std::string("] ");
          TRI_GetBacktrace(message);
          DisarmFormatString(message);
          TRI_Log(func, file, line, TRI_LOG_LEVEL_WARNING,
                  TRI_LOG_SEVERITY_HUMAN, message.c_str());
        } catch (...) {
        }
        return;
      }
    } else if (m > n) {
      TRI_Free(TRI_UNKNOWN_MEM_ZONE, p);
      n = m;
      // again
    } else {
      // finally got a buffer big enough. p is freed in OutputMessage
      if (m + len - 1 > 0) {
        OutputMessage(level, severity, p, (size_t)(m + len),
                      (size_t)(offset + len), true);
      }

      return;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief closes all log appenders
////////////////////////////////////////////////////////////////////////////////

static void CloseLogging() {
  MUTEX_LOCKER(mutexLocker, AppendersLock);

  for (auto& it : Appenders) {
    delete it;
  }

  Appenders.clear();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the log severity
////////////////////////////////////////////////////////////////////////////////

void TRI_SetLogSeverityLogging(char const* severities) {
  TRI_vector_string_t split = TRI_SplitString(severities, ',');
  size_t const n = split._length;

  for (size_t i = 0; i < n; ++i) {
    char const* type = split._buffer[i];

    if (TRI_CaseEqualString(type, "usage")) {
      // IsUsage = 1;  // TODO: enable REQUESTS logging here
    } else if (TRI_CaseEqualString(type, "performance")) {
      // IsPerformance = 1;  // TODO: enable PERFORMANCE logging here
    }
  }

  TRI_DestroyVectorString(&split);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the output prefix
////////////////////////////////////////////////////////////////////////////////

void TRI_SetPrefixLogging(char const* prefix) {
  Logger::setOutputPrefix(prefix);

  char* outputPrefix = TRI_DuplicateString(TRI_UNKNOWN_MEM_ZONE, prefix);

  if (outputPrefix == nullptr) {
    return;
  }

  auto old = OutputPrefix.exchange(outputPrefix);

  if (old != nullptr) {
    TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, old);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the thread identifier visibility
////////////////////////////////////////////////////////////////////////////////

void TRI_SetThreadIdentifierLogging(bool show) {
  Logger::setShowThreadIdentifier(show);
  ShowThreadIdentifier = show ? 1 : 0;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief use local time?
////////////////////////////////////////////////////////////////////////////////

void TRI_SetUseLocalTimeLogging(bool value) {
  Logger::setUseLocalTime(value);
  UseLocalTime = value ? 1 : 0;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the line number visibility
////////////////////////////////////////////////////////////////////////////////

void TRI_SetLineNumberLogging(bool show) {
  Logger::setShowLineNumber(show);
  ShowLineNumber = show ? 1 : 0;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief logs a new message
////////////////////////////////////////////////////////////////////////////////

void TRI_Log(char const* func, char const* file, int line,
             TRI_log_level_e level, TRI_log_severity_e severity,
             char const* fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
#ifdef _WIN32
  if (level == TRI_LOG_LEVEL_FATAL || level == TRI_LOG_LEVEL_ERROR) {
    va_list wva;
    va_copy(wva, ap);
    TRI_LogWindowsEventlog(func, file, line, fmt, ap);
    va_end(wva);
  }
#endif
  if (!LoggingActive) {
    va_end(ap);
    return;
  }

  TRI_pid_t processId = TRI_CurrentProcessId();
  uint64_t threadNumber = Thread::currentThreadNumber();

  LogThread(func, file, line, level, severity, processId, threadNumber, fmt,
            ap);
  va_end(ap);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the last log entries
////////////////////////////////////////////////////////////////////////////////

TRI_vector_t* TRI_BufferLogging(TRI_log_level_e level, uint64_t start,
                                bool useUpto) {
  TRI_vector_t* result = static_cast<TRI_vector_t*>(
      TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, sizeof(TRI_vector_t), false));

  if (result == nullptr) {
    return nullptr;
  }

  TRI_InitVector(result, TRI_UNKNOWN_MEM_ZONE, sizeof(TRI_log_buffer_t));

  size_t begin = 0;
  size_t pos = (size_t)level;

  if (pos >= OUTPUT_LOG_LEVELS) {
    pos = OUTPUT_LOG_LEVELS - 1;
  }

  if (!useUpto) {
    begin = pos;
  }

  {
    MUTEX_LOCKER(mutexLocker, BufferLock);

    for (size_t i = begin; i <= pos; ++i) {
      for (size_t j = 0; j < OUTPUT_BUFFER_SIZE; ++j) {
        size_t cur = (BufferCurrent[i] + j) % OUTPUT_BUFFER_SIZE;
        TRI_log_buffer_t buf = BufferOutput[i][cur];

        if (buf._lid >= start && buf._text != nullptr && *buf._text != '\0') {
          buf._text = TRI_DuplicateString(TRI_UNKNOWN_MEM_ZONE, buf._text);

          if (buf._text != nullptr) {
            TRI_PushBackVector(result, &buf);
          }
        }
      }
    }
  }

  qsort(TRI_BeginVector(result), TRI_LengthVector(result),
        sizeof(TRI_log_buffer_t), LidCompare);

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief frees the log buffer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeBufferLogging(TRI_vector_t* buffer) {
  for (size_t i = 0; i < TRI_LengthVector(buffer); ++i) {
    TRI_log_buffer_t* buf =
        static_cast<TRI_log_buffer_t*>(TRI_AtVector(buffer, i));

    TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, buf->_text);
  }

  TRI_FreeVector(TRI_UNKNOWN_MEM_ZONE, buffer);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief structure for file log appenders
////////////////////////////////////////////////////////////////////////////////

struct log_appender_file_t : public TRI_log_appender_t {
 public:
  log_appender_file_t(char const*, TRI_log_severity_e, bool, bool, char const*);
  ~log_appender_file_t();

  void logMessage(TRI_log_level_e, TRI_log_severity_e, char const*,
                  size_t) override final;
  void reopenLog() override final;
  void closeLog() override final;
  std::string details() override final;

  TRI_log_appender_type_e type() override final { return APPENDER_TYPE_FILE; }

  char const* typeName() override final { return "file"; }

 private:
  void writeLogFile(int, char const*, ssize_t);

 private:
  std::string _filename;
  std::atomic<int> _fd;
  bool _fatal2stderr;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief create the appender
////////////////////////////////////////////////////////////////////////////////

log_appender_file_t::log_appender_file_t(char const* contentFilter,
                                         TRI_log_severity_e severityFilter,
                                         bool consume, bool fatal2stderr,
                                         char const* filename)
    : TRI_log_appender_t(contentFilter, severityFilter, consume),
      _filename(),
      _fd(-1),
      _fatal2stderr(fatal2stderr) {
  // logging to stdout
  if (TRI_EqualString(filename, "+")) {
    _fd.store(STDOUT_FILENO);
  }

  // logging to stderr
  else if (TRI_EqualString(filename, "-")) {
    _fd.store(STDERR_FILENO);
  }

  // logging to file
  else {
    int fd = TRI_CREATE(filename, O_APPEND | O_CREAT | O_WRONLY | TRI_O_CLOEXEC,
                        S_IRUSR | S_IWUSR | S_IRGRP);

    if (fd < 0) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_CANNOT_WRITE_FILE);
    }

    _fd.store(fd);

    _filename = filename;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the appender
////////////////////////////////////////////////////////////////////////////////

log_appender_file_t::~log_appender_file_t() { this->closeLog(); }

////////////////////////////////////////////////////////////////////////////////
/// @brief logs a message to a log file appender
////////////////////////////////////////////////////////////////////////////////

void log_appender_file_t::logMessage(TRI_log_level_e level,
                                     TRI_log_severity_e severity,
                                     char const* msg, size_t length) {
  int fd = _fd.load();

  if (fd < 0) {
    return;
  }

  if (level == TRI_LOG_LEVEL_FATAL && _fatal2stderr) {
    // a fatal error. always print this on stderr, too.
    WriteStderr(level, msg);

    // this function is already called when the appenders lock is held
    // no need to lock it again
    for (auto& it : Appenders) {
      std::string details = it->details();

      if (!details.empty()) {
        WriteStderr(TRI_LOG_LEVEL_INFO, details.c_str());
      }
    }

    if (_filename.empty() && (fd == STDOUT_FILENO || fd == STDERR_FILENO)) {
      // the logfile is either stdout or stderr. no need to print the message
      // again
      return;
    }
  }

  size_t escapedLength;
  char* escaped = TRI_EscapeControlsCString(TRI_UNKNOWN_MEM_ZONE, msg, length,
                                            &escapedLength, true);

  if (escaped != nullptr) {
    writeLogFile(fd, escaped, (ssize_t)escapedLength);
    TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, escaped);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reopens the log files appender
////////////////////////////////////////////////////////////////////////////////

void log_appender_file_t::reopenLog() {
  if (_filename.empty()) {
    return;
  }

  if (_fd <= STDERR_FILENO) {
    return;
  }

  // rename log file
  std::string backup(_filename);
  backup.append(".old");

  TRI_UnlinkFile(backup.c_str());
  TRI_RenameFile(_filename.c_str(), backup.c_str());

  // open new log file
  int fd = TRI_CREATE(_filename.c_str(),
                      O_APPEND | O_CREAT | O_WRONLY | TRI_O_CLOEXEC,
                      S_IRUSR | S_IWUSR | S_IRGRP);

  if (fd < 0) {
    TRI_RenameFile(backup.c_str(), _filename.c_str());
    return;
  }

  int old = std::atomic_exchange(&_fd, fd);

  TRI_CLOSE(old);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief closes a log file appender
////////////////////////////////////////////////////////////////////////////////

void log_appender_file_t::closeLog() {
  int fd = _fd.exchange(-1);

  if (fd > STDERR_FILENO) {
    TRI_CLOSE(fd);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief provide details about the logfile appender
////////////////////////////////////////////////////////////////////////////////

std::string log_appender_file_t::details() {
  if (_filename.empty()) {
    return "";
  }

  int fd = _fd.load();

  if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
    std::string buffer("More error details may be provided in the logfile '");
    buffer.append(_filename);
    buffer.append("'");

    return buffer;
  }

  return "";
}

////////////////////////////////////////////////////////////////////////////////
/// @brief writes to a log file
////////////////////////////////////////////////////////////////////////////////

void log_appender_file_t::writeLogFile(int fd, char const* buffer,
                                       ssize_t len) {
  bool giveUp = false;

  while (len > 0) {
    ssize_t n;

    n = TRI_WRITE(fd, buffer, len);

    if (n < 0) {
      fprintf(stderr, "cannot log data: %s\n", TRI_LAST_ERROR_STR);
      return;  // give up, but do not try to log failure
    } else if (n == 0) {
      if (!giveUp) {
        giveUp = true;
        continue;
      }
    }

    buffer += n;
    len -= n;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a log appender for file output
////////////////////////////////////////////////////////////////////////////////

int TRI_CreateLogAppenderFile(char const* filename, char const* contentFilter,
                              TRI_log_severity_e severityFilter, bool consume,
                              bool fatal2stderr) {
  // no logging
  if (filename == nullptr || *filename == '\0') {
    return TRI_ERROR_INTERNAL;
  }

  // allocate appender
  std::unique_ptr<log_appender_file_t> appender;
  try {
    appender.reset(new log_appender_file_t(contentFilter, severityFilter,
                                           consume, fatal2stderr, filename));
  } catch (...) {
    return TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
  }

  // and store it
  {
    MUTEX_LOCKER(mutexLocker, AppendersLock);
    try {
      Appenders.emplace_back(appender.get());
      appender.release();
    } catch (...) {
      return TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
    }
  }

  // register the name of the first logfile
  if (LogfileName == nullptr) {
    LogfileName = TRI_DuplicateString(TRI_UNKNOWN_MEM_ZONE, filename);
  }

  // and return base structure
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief structure for syslog appenders
////////////////////////////////////////////////////////////////////////////////

#ifdef TRI_ENABLE_SYSLOG

struct log_appender_syslog_t : public TRI_log_appender_t {
 public:
  log_appender_syslog_t(char const*, TRI_log_severity_e, bool, char const*,
                        char const*);
  ~log_appender_syslog_t();

  void logMessage(TRI_log_level_e, TRI_log_severity_e, char const* msg,
                  size_t length) override final;
  void reopenLog() override final;
  void closeLog() override final;
  std::string details() override final;

  TRI_log_appender_type_e type() override final { return APPENDER_TYPE_SYSLOG; }

  char const* typeName() override final { return "syslog"; }

 private:
  arangodb::Mutex _lock;
  bool _opened;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief create the appender
////////////////////////////////////////////////////////////////////////////////

log_appender_syslog_t::log_appender_syslog_t(char const* contentFilter,
                                             TRI_log_severity_e severityFilter,
                                             bool consume, char const* name,
                                             char const* facility)
    : TRI_log_appender_t(contentFilter, severityFilter, consume),
      _opened(false) {
  // no logging
  if (name == nullptr || *name == '\0') {
    name = "[arangod]";
  }

  // find facility
  int value = LOG_LOCAL0;

  if ('0' <= facility[0] && facility[0] <= '9') {
    value = atoi(facility);
  } else {
    CODE* ptr = reinterpret_cast<CODE*>(TRI_facilitynames);

    while (ptr->c_name != 0) {
      if (strcmp(ptr->c_name, facility) == 0) {
        value = ptr->c_val;
        break;
      }

      ++ptr;
    }
  }

  // and open logging, openlog does not have a return value...
  {
    MUTEX_LOCKER(mutexLocker, _lock);
    ::openlog(name, LOG_CONS | LOG_PID, value);
    _opened = true;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the appender
////////////////////////////////////////////////////////////////////////////////

log_appender_syslog_t::~log_appender_syslog_t() { this->closeLog(); }

////////////////////////////////////////////////////////////////////////////////
/// @brief logs a message to a syslog appender
////////////////////////////////////////////////////////////////////////////////

void log_appender_syslog_t::logMessage(TRI_log_level_e level,
                                       TRI_log_severity_e severity,
                                       char const* msg, size_t length) {
  int priority;

  switch (severity) {
    case TRI_LOG_SEVERITY_EXCEPTION:
      priority = LOG_CRIT;
      break;
    case TRI_LOG_SEVERITY_FUNCTIONAL:
      priority = LOG_NOTICE;
      break;
    case TRI_LOG_SEVERITY_USAGE:
      priority = LOG_INFO;
      break;
    case TRI_LOG_SEVERITY_TECHNICAL:
      priority = LOG_INFO;
      break;
    case TRI_LOG_SEVERITY_DEVELOPMENT:
      priority = LOG_DEBUG;
      break;
    default:
      priority = LOG_DEBUG;
      break;
  }

  if (severity == TRI_LOG_SEVERITY_HUMAN) {
    switch (level) {
      case TRI_LOG_LEVEL_FATAL:
        priority = LOG_CRIT;
        break;
      case TRI_LOG_LEVEL_ERROR:
        priority = LOG_ERR;
        break;
      case TRI_LOG_LEVEL_WARNING:
        priority = LOG_WARNING;
        break;
      case TRI_LOG_LEVEL_INFO:
        priority = LOG_NOTICE;
        break;
      case TRI_LOG_LEVEL_DEBUG:
        priority = LOG_INFO;
        break;
      case TRI_LOG_LEVEL_TRACE:
        priority = LOG_DEBUG;
        break;
    }
  }

  char const* ptr = strchr(msg, ']');

  if (ptr == nullptr) {
    ptr = msg;
  } else if (ptr[1] != '\0') {
    ptr += 2;
  }

  {
    MUTEX_LOCKER(mutexLocker, _lock);
    if (_opened) {
      ::syslog(priority, "%s", ptr);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reopens a syslog appender
////////////////////////////////////////////////////////////////////////////////

void log_appender_syslog_t::reopenLog() {}

////////////////////////////////////////////////////////////////////////////////
/// @brief closes a syslog appender
////////////////////////////////////////////////////////////////////////////////

void log_appender_syslog_t::closeLog() {
  MUTEX_LOCKER(mutexLocker, _lock);
  if (_opened) {
    ::closelog();
    _opened = false;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief provide details about the logfile appender
////////////////////////////////////////////////////////////////////////////////

std::string log_appender_syslog_t::details() {
  return "More error details may be provided in the syslog";
}

#endif

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a syslog appender
////////////////////////////////////////////////////////////////////////////////

#ifdef TRI_ENABLE_SYSLOG

int TRI_CreateLogAppenderSyslog(char const* name, char const* facility,
                                char const* contentFilter,
                                TRI_log_severity_e severityFilter,
                                bool consume) {
  TRI_ASSERT(facility != nullptr);
  TRI_ASSERT(*facility != '\0');

  // allocate appender
  std::unique_ptr<log_appender_syslog_t> appender;
  try {
    appender.reset(new log_appender_syslog_t(contentFilter, severityFilter,
                                             consume, name, facility));
  } catch (...) {
    return TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
  }

  // and store it
  {
    MUTEX_LOCKER(mutexLocker, AppendersLock);
    try {
      Appenders.emplace_back(appender.get());
      appender.release();
    } catch (...) {
      return TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
    }
  }

  // and return base structure
  return TRI_ERROR_NO_ERROR;
}

#endif

////////////////////////////////////////////////////////////////////////////////
/// @brief return global log file name
////////////////////////////////////////////////////////////////////////////////

char const* TRI_GetFilenameLogging() { return LogfileName; }

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes the logging components
////////////////////////////////////////////////////////////////////////////////

void TRI_InitializeLogging(bool threaded) {
  if (Initialized > 0) {
    return;
  }

  Initialized = 1;

  // logging is now active
  LoggingActive = 1;

  // generate threaded logging?
  ThreadedLogging = threaded;

  if (threaded) {
    TRI_InitCondition(&LogCondition);
    TRI_InitThread(&LoggingThread);
    TRI_StartThread(&LoggingThread, nullptr, "Logging", MessageQueueWorker, 0);

    while (true) {
      if (LoggingThreadActive.load()) {
        break;
      }
      usleep(5000);
    }
  }

  // always close logging at the end
  if (!ShutdownInitalized) {
    atexit((void (*)(void))TRI_ShutdownLogging);
    ShutdownInitalized = true;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief shut downs the logging components
////////////////////////////////////////////////////////////////////////////////

bool TRI_ShutdownLogging(bool clearBuffers) {
  if (Initialized != 1) {
    if (Initialized == 0) {
      return ThreadedLogging;
    }

    WriteStderr(TRI_LOG_LEVEL_ERROR, "race condition detected in logger");
    return false;
  }

  Initialized = 1;

  // logging is now inactive (this will terminate the logging thread)
  LoggingActive = 0;

  if (LogfileName != nullptr) {
    TRI_Free(TRI_UNKNOWN_MEM_ZONE, LogfileName);
    LogfileName = nullptr;
  }

  // join with the logging thread
  if (ThreadedLogging) {
    TRI_LockCondition(&LogCondition);
    TRI_SignalCondition(&LogCondition);
    TRI_UnlockCondition(&LogCondition);

    if (TRI_JoinThread(&LoggingThread) != TRI_ERROR_NO_ERROR) {
      // ignore all errors for now as we cannot log them anywhere...
    }

    TRI_DestroyCondition(&LogCondition);
  }

  // cleanup appenders
  CloseLogging();

  // cleanup prefix
  {
    auto prefix = OutputPrefix.exchange(nullptr);

    if (prefix != nullptr) {
      TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, prefix);
    }
  }

  if (clearBuffers) {
    // cleanup output buffers
    MUTEX_LOCKER(mutexLocker, BufferLock);

    for (size_t i = 0; i < OUTPUT_LOG_LEVELS; i++) {
      for (size_t j = 0; j < OUTPUT_BUFFER_SIZE; j++) {
        if (BufferOutput[i][j]._text != nullptr) {
          TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, BufferOutput[i][j]._text);
          BufferOutput[i][j]._text = nullptr;
        }
      }
    }
  }

  Initialized = 0;

  return ThreadedLogging;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reopens all log appenders
////////////////////////////////////////////////////////////////////////////////

void TRI_ReopenLogging() {
  MUTEX_LOCKER(mutexLocker, AppendersLock);

  for (auto& it : Appenders) {
    try {
      it->reopenLog();
    } catch (...) {
      // silently catch this error (we shouldn't try to log an error about a
      // logging error as this will get us into trouble with mutexes etc.)
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief makes sure all log messages are flushed
////////////////////////////////////////////////////////////////////////////////

void TRI_FlushLogging() {
  if (Initialized != 1) {
    return;
  }

  if (ThreadedLogging) {
    TRI_LockCondition(&LogCondition);
    TRI_SignalCondition(&LogCondition);
    TRI_UnlockCondition(&LogCondition);

    int tries = 0;
    while (++tries < 500) {
      {
        MUTEX_LOCKER(mutexLocker, LogMessageQueueLock);
        if (LogMessageQueue.empty()) {
          break;
        }
      }

      usleep(10000);
    }
  }
}
