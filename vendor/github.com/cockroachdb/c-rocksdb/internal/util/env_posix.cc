//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#if defined(OS_LINUX)
#include <linux/fs.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifdef OS_LINUX
#include <sys/statfs.h>
#include <sys/syscall.h>
#endif
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <algorithm>
// Get nano time includes
#if defined(OS_LINUX) || defined(OS_FREEBSD)
#elif defined(__MACH__)
#include <mach/clock.h>
#include <mach/mach.h>
#else
#include <chrono>
#endif
#include <deque>
#include <set>
#include "port/port.h"
#include "rocksdb/slice.h"
#include "util/coding.h"
#include "util/io_posix.h"
#include "util/iostats_context_imp.h"
#include "util/logging.h"
#include "util/posix_logger.h"
#include "util/string_util.h"
#include "util/sync_point.h"
#include "util/thread_status_updater.h"
#include "util/thread_status_util.h"

#if !defined(TMPFS_MAGIC)
#define TMPFS_MAGIC 0x01021994
#endif
#if !defined(XFS_SUPER_MAGIC)
#define XFS_SUPER_MAGIC 0x58465342
#endif
#if !defined(EXT4_SUPER_MAGIC)
#define EXT4_SUPER_MAGIC 0xEF53
#endif

namespace rocksdb {

namespace {

ThreadStatusUpdater* CreateThreadStatusUpdater() {
  return new ThreadStatusUpdater();
}

// list of pathnames that are locked
static std::set<std::string> lockedFiles;
static port::Mutex mutex_lockedFiles;

static int LockOrUnlock(const std::string& fname, int fd, bool lock) {
  mutex_lockedFiles.Lock();
  if (lock) {
    // If it already exists in the lockedFiles set, then it is already locked,
    // and fail this lock attempt. Otherwise, insert it into lockedFiles.
    // This check is needed because fcntl() does not detect lock conflict
    // if the fcntl is issued by the same thread that earlier acquired
    // this lock.
    if (lockedFiles.insert(fname).second == false) {
      mutex_lockedFiles.Unlock();
      errno = ENOLCK;
      return -1;
    }
  } else {
    // If we are unlocking, then verify that we had locked it earlier,
    // it should already exist in lockedFiles. Remove it from lockedFiles.
    if (lockedFiles.erase(fname) != 1) {
      mutex_lockedFiles.Unlock();
      errno = ENOLCK;
      return -1;
    }
  }
  errno = 0;
  struct flock f;
  memset(&f, 0, sizeof(f));
  f.l_type = (lock ? F_WRLCK : F_UNLCK);
  f.l_whence = SEEK_SET;
  f.l_start = 0;
  f.l_len = 0;        // Lock/unlock entire file
  int value = fcntl(fd, F_SETLK, &f);
  if (value == -1 && lock) {
    // if there is an error in locking, then remove the pathname from lockedfiles
    lockedFiles.erase(fname);
  }
  mutex_lockedFiles.Unlock();
  return value;
}

void PthreadCall(const char* label, int result) {
  if (result != 0) {
    fprintf(stderr, "pthread %s: %s\n", label, strerror(result));
    abort();
  }
}

class PosixFileLock : public FileLock {
 public:
  int fd_;
  std::string filename;
};

class PosixEnv : public Env {
 public:
  PosixEnv();

  virtual ~PosixEnv() {
    for (const auto tid : threads_to_join_) {
      pthread_join(tid, nullptr);
    }
    for (int pool_id = 0; pool_id < Env::Priority::TOTAL; ++pool_id) {
      thread_pools_[pool_id].JoinAllThreads();
    }
    // All threads must be joined before the deletion of
    // thread_status_updater_.
    delete thread_status_updater_;
  }

  void SetFD_CLOEXEC(int fd, const EnvOptions* options) {
    if ((options == nullptr || options->set_fd_cloexec) && fd > 0) {
      fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }
  }

  virtual Status NewSequentialFile(const std::string& fname,
                                   unique_ptr<SequentialFile>* result,
                                   const EnvOptions& options) override {
    result->reset();
    FILE* f = nullptr;
    do {
      IOSTATS_TIMER_GUARD(open_nanos);
      f = fopen(fname.c_str(), "r");
    } while (f == nullptr && errno == EINTR);
    if (f == nullptr) {
      *result = nullptr;
      return IOError(fname, errno);
    } else {
      int fd = fileno(f);
      SetFD_CLOEXEC(fd, &options);
      result->reset(new PosixSequentialFile(fname, f, options));
      return Status::OK();
    }
  }

  virtual Status NewRandomAccessFile(const std::string& fname,
                                     unique_ptr<RandomAccessFile>* result,
                                     const EnvOptions& options) override {
    result->reset();
    Status s;
    int fd;
    {
      IOSTATS_TIMER_GUARD(open_nanos);
      fd = open(fname.c_str(), O_RDONLY);
    }
    SetFD_CLOEXEC(fd, &options);
    if (fd < 0) {
      s = IOError(fname, errno);
    } else if (options.use_mmap_reads && sizeof(void*) >= 8) {
      // Use of mmap for random reads has been removed because it
      // kills performance when storage is fast.
      // Use mmap when virtual address-space is plentiful.
      uint64_t size;
      s = GetFileSize(fname, &size);
      if (s.ok()) {
        void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (base != MAP_FAILED) {
          result->reset(new PosixMmapReadableFile(fd, fname, base,
                                                  size, options));
        } else {
          s = IOError(fname, errno);
        }
      }
      close(fd);
    } else {
      result->reset(new PosixRandomAccessFile(fname, fd, options));
    }
    return s;
  }

  virtual Status NewWritableFile(const std::string& fname,
                                 unique_ptr<WritableFile>* result,
                                 const EnvOptions& options) override {
    result->reset();
    Status s;
    int fd = -1;
    do {
      IOSTATS_TIMER_GUARD(open_nanos);
      fd = open(fname.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
      s = IOError(fname, errno);
    } else {
      SetFD_CLOEXEC(fd, &options);
      if (options.use_mmap_writes) {
        if (!checkedDiskForMmap_) {
          // this will be executed once in the program's lifetime.
          // do not use mmapWrite on non ext-3/xfs/tmpfs systems.
          if (!SupportsFastAllocate(fname)) {
            forceMmapOff = true;
          }
          checkedDiskForMmap_ = true;
        }
      }
      if (options.use_mmap_writes && !forceMmapOff) {
        result->reset(new PosixMmapFile(fname, fd, page_size_, options));
      } else {
        // disable mmap writes
        EnvOptions no_mmap_writes_options = options;
        no_mmap_writes_options.use_mmap_writes = false;

        result->reset(new PosixWritableFile(fname, fd, no_mmap_writes_options));
      }
    }
    return s;
  }

  virtual Status ReuseWritableFile(const std::string& fname,
                                   const std::string& old_fname,
                                   unique_ptr<WritableFile>* result,
                                   const EnvOptions& options) override {
    result->reset();
    Status s;
    int fd = -1;
    do {
      IOSTATS_TIMER_GUARD(open_nanos);
      fd = open(old_fname.c_str(), O_RDWR, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
      s = IOError(fname, errno);
    } else {
      SetFD_CLOEXEC(fd, &options);
      // rename into place
      if (rename(old_fname.c_str(), fname.c_str()) != 0) {
        Status r = IOError(old_fname, errno);
        close(fd);
        return r;
      }
      if (options.use_mmap_writes) {
        if (!checkedDiskForMmap_) {
          // this will be executed once in the program's lifetime.
          // do not use mmapWrite on non ext-3/xfs/tmpfs systems.
          if (!SupportsFastAllocate(fname)) {
            forceMmapOff = true;
          }
          checkedDiskForMmap_ = true;
        }
      }
      if (options.use_mmap_writes && !forceMmapOff) {
        result->reset(new PosixMmapFile(fname, fd, page_size_, options));
      } else {
        // disable mmap writes
        EnvOptions no_mmap_writes_options = options;
        no_mmap_writes_options.use_mmap_writes = false;

        result->reset(new PosixWritableFile(fname, fd, no_mmap_writes_options));
      }
    }
    return s;
  }

  virtual Status NewDirectory(const std::string& name,
                              unique_ptr<Directory>* result) override {
    result->reset();
    int fd;
    {
      IOSTATS_TIMER_GUARD(open_nanos);
      fd = open(name.c_str(), 0);
    }
    if (fd < 0) {
      return IOError(name, errno);
    } else {
      result->reset(new PosixDirectory(fd));
    }
    return Status::OK();
  }

  virtual Status FileExists(const std::string& fname) override {
    int result = access(fname.c_str(), F_OK);

    if (result == 0) {
      return Status::OK();
    }

    switch (errno) {
      case EACCES:
      case ELOOP:
      case ENAMETOOLONG:
      case ENOENT:
      case ENOTDIR:
        return Status::NotFound();
      default:
        assert(result == EIO || result == ENOMEM);
        return Status::IOError("Unexpected error(" + ToString(result) +
                               ") accessing file `" + fname + "' ");
    }
  }

  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) override {
    result->clear();
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
      return IOError(dir, errno);
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      result->push_back(entry->d_name);
    }
    closedir(d);
    return Status::OK();
  }

  virtual Status DeleteFile(const std::string& fname) override {
    Status result;
    if (unlink(fname.c_str()) != 0) {
      result = IOError(fname, errno);
    }
    return result;
  };

  virtual Status CreateDir(const std::string& name) override {
    Status result;
    if (mkdir(name.c_str(), 0755) != 0) {
      result = IOError(name, errno);
    }
    return result;
  };

  virtual Status CreateDirIfMissing(const std::string& name) override {
    Status result;
    if (mkdir(name.c_str(), 0755) != 0) {
      if (errno != EEXIST) {
        result = IOError(name, errno);
      } else if (!DirExists(name)) { // Check that name is actually a
                                     // directory.
        // Message is taken from mkdir
        result = Status::IOError("`"+name+"' exists but is not a directory");
      }
    }
    return result;
  };

  virtual Status DeleteDir(const std::string& name) override {
    Status result;
    if (rmdir(name.c_str()) != 0) {
      result = IOError(name, errno);
    }
    return result;
  };

  virtual Status GetFileSize(const std::string& fname,
                             uint64_t* size) override {
    Status s;
    struct stat sbuf;
    if (stat(fname.c_str(), &sbuf) != 0) {
      *size = 0;
      s = IOError(fname, errno);
    } else {
      *size = sbuf.st_size;
    }
    return s;
  }

  virtual Status GetFileModificationTime(const std::string& fname,
                                         uint64_t* file_mtime) override {
    struct stat s;
    if (stat(fname.c_str(), &s) !=0) {
      return IOError(fname, errno);
    }
    *file_mtime = static_cast<uint64_t>(s.st_mtime);
    return Status::OK();
  }
  virtual Status RenameFile(const std::string& src,
                            const std::string& target) override {
    Status result;
    if (rename(src.c_str(), target.c_str()) != 0) {
      result = IOError(src, errno);
    }
    return result;
  }

  virtual Status LinkFile(const std::string& src,
                          const std::string& target) override {
    Status result;
    if (link(src.c_str(), target.c_str()) != 0) {
      if (errno == EXDEV) {
        return Status::NotSupported("No cross FS links allowed");
      }
      result = IOError(src, errno);
    }
    return result;
  }

  virtual Status LockFile(const std::string& fname, FileLock** lock) override {
    *lock = nullptr;
    Status result;
    int fd;
    {
      IOSTATS_TIMER_GUARD(open_nanos);
      fd = open(fname.c_str(), O_RDWR | O_CREAT, 0644);
    }
    if (fd < 0) {
      result = IOError(fname, errno);
    } else if (LockOrUnlock(fname, fd, true) == -1) {
      result = IOError("lock " + fname, errno);
      close(fd);
    } else {
      SetFD_CLOEXEC(fd, nullptr);
      PosixFileLock* my_lock = new PosixFileLock;
      my_lock->fd_ = fd;
      my_lock->filename = fname;
      *lock = my_lock;
    }
    return result;
  }

  virtual Status UnlockFile(FileLock* lock) override {
    PosixFileLock* my_lock = reinterpret_cast<PosixFileLock*>(lock);
    Status result;
    if (LockOrUnlock(my_lock->filename, my_lock->fd_, false) == -1) {
      result = IOError("unlock", errno);
    }
    close(my_lock->fd_);
    delete my_lock;
    return result;
  }

  virtual void Schedule(void (*function)(void* arg1), void* arg,
                        Priority pri = LOW, void* tag = nullptr) override;

  virtual int UnSchedule(void* arg, Priority pri) override;

  virtual void StartThread(void (*function)(void* arg), void* arg) override;

  virtual void WaitForJoin() override;

  virtual unsigned int GetThreadPoolQueueLen(Priority pri = LOW) const override;

  virtual Status GetTestDirectory(std::string* result) override {
    const char* env = getenv("TEST_TMPDIR");
    if (env && env[0] != '\0') {
      *result = env;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "/tmp/rocksdbtest-%d", int(geteuid()));
      *result = buf;
    }
    // Directory may already exist
    CreateDir(*result);
    return Status::OK();
  }

  virtual Status GetThreadList(
      std::vector<ThreadStatus>* thread_list) override {
    assert(thread_status_updater_);
    return thread_status_updater_->GetThreadList(thread_list);
  }

  static uint64_t gettid(pthread_t tid) {
    uint64_t thread_id = 0;
    memcpy(&thread_id, &tid, std::min(sizeof(thread_id), sizeof(tid)));
    return thread_id;
  }

  static uint64_t gettid() {
    pthread_t tid = pthread_self();
    return gettid(tid);
  }

  virtual uint64_t GetThreadID() const override {
    return gettid(pthread_self());
  }

  virtual Status NewLogger(const std::string& fname,
                           shared_ptr<Logger>* result) override {
    FILE* f;
    {
      IOSTATS_TIMER_GUARD(open_nanos);
      f = fopen(fname.c_str(), "w");
    }
    if (f == nullptr) {
      result->reset();
      return IOError(fname, errno);
    } else {
      int fd = fileno(f);
#ifdef ROCKSDB_FALLOCATE_PRESENT
      fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, 4 * 1024);
#endif
      SetFD_CLOEXEC(fd, nullptr);
      result->reset(new PosixLogger(f, &PosixEnv::gettid, this));
      return Status::OK();
    }
  }

  virtual uint64_t NowMicros() override {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }

  virtual uint64_t NowNanos() override {
#if defined(OS_LINUX) || defined(OS_FREEBSD)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#elif defined(__MACH__)
    clock_serv_t cclock;
    mach_timespec_t ts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &ts);
    mach_port_deallocate(mach_task_self(), cclock);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
       std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
  }

  virtual void SleepForMicroseconds(int micros) override { usleep(micros); }

  virtual Status GetHostName(char* name, uint64_t len) override {
    int ret = gethostname(name, static_cast<size_t>(len));
    if (ret < 0) {
      if (errno == EFAULT || errno == EINVAL)
        return Status::InvalidArgument(strerror(errno));
      else
        return IOError("GetHostName", errno);
    }
    return Status::OK();
  }

  virtual Status GetCurrentTime(int64_t* unix_time) override {
    time_t ret = time(nullptr);
    if (ret == (time_t) -1) {
      return IOError("GetCurrentTime", errno);
    }
    *unix_time = (int64_t) ret;
    return Status::OK();
  }

  virtual Status GetAbsolutePath(const std::string& db_path,
                                 std::string* output_path) override {
    if (db_path.find('/') == 0) {
      *output_path = db_path;
      return Status::OK();
    }

    char the_path[256];
    char* ret = getcwd(the_path, 256);
    if (ret == nullptr) {
      return Status::IOError(strerror(errno));
    }

    *output_path = ret;
    return Status::OK();
  }

  // Allow increasing the number of worker threads.
  virtual void SetBackgroundThreads(int num, Priority pri) override {
    assert(pri >= Priority::LOW && pri <= Priority::HIGH);
    thread_pools_[pri].SetBackgroundThreads(num);
  }

  // Allow increasing the number of worker threads.
  virtual void IncBackgroundThreadsIfNeeded(int num, Priority pri) override {
    assert(pri >= Priority::LOW && pri <= Priority::HIGH);
    thread_pools_[pri].IncBackgroundThreadsIfNeeded(num);
  }

  virtual void LowerThreadPoolIOPriority(Priority pool = LOW) override {
    assert(pool >= Priority::LOW && pool <= Priority::HIGH);
#ifdef OS_LINUX
    thread_pools_[pool].LowerIOPriority();
#endif
  }

  virtual std::string TimeToString(uint64_t secondsSince1970) override {
    const time_t seconds = (time_t)secondsSince1970;
    struct tm t;
    int maxsize = 64;
    std::string dummy;
    dummy.reserve(maxsize);
    dummy.resize(maxsize);
    char* p = &dummy[0];
    localtime_r(&seconds, &t);
    snprintf(p, maxsize,
             "%04d/%02d/%02d-%02d:%02d:%02d ",
             t.tm_year + 1900,
             t.tm_mon + 1,
             t.tm_mday,
             t.tm_hour,
             t.tm_min,
             t.tm_sec);
    return dummy;
  }

  EnvOptions OptimizeForLogWrite(const EnvOptions& env_options,
                                 const DBOptions& db_options) const override {
    EnvOptions optimized = env_options;
    optimized.use_mmap_writes = false;
    optimized.bytes_per_sync = db_options.wal_bytes_per_sync;
    // TODO(icanadi) it's faster if fallocate_with_keep_size is false, but it
    // breaks TransactionLogIteratorStallAtLastRecord unit test. Fix the unit
    // test and make this false
    optimized.fallocate_with_keep_size = true;
    return optimized;
  }

  EnvOptions OptimizeForManifestWrite(
      const EnvOptions& env_options) const override {
    EnvOptions optimized = env_options;
    optimized.use_mmap_writes = false;
    optimized.fallocate_with_keep_size = true;
    return optimized;
  }

 private:
  bool checkedDiskForMmap_;
  bool forceMmapOff; // do we override Env options?


  // Returns true iff the named directory exists and is a directory.
  virtual bool DirExists(const std::string& dname) {
    struct stat statbuf;
    if (stat(dname.c_str(), &statbuf) == 0) {
      return S_ISDIR(statbuf.st_mode);
    }
    return false; // stat() failed return false
  }

  bool SupportsFastAllocate(const std::string& path) {
#ifdef ROCKSDB_FALLOCATE_PRESENT
    struct statfs s;
    if (statfs(path.c_str(), &s)){
      return false;
    }
    switch (s.f_type) {
      case EXT4_SUPER_MAGIC:
        return true;
      case XFS_SUPER_MAGIC:
        return true;
      case TMPFS_MAGIC:
        return true;
      default:
        return false;
    }
#else
    return false;
#endif
  }

  size_t page_size_;


  class ThreadPool {
   public:
    ThreadPool()
        : total_threads_limit_(1),
          bgthreads_(0),
          queue_(),
          queue_len_(0),
          exit_all_threads_(false),
          low_io_priority_(false),
          env_(nullptr) {
      PthreadCall("mutex_init", pthread_mutex_init(&mu_, nullptr));
      PthreadCall("cvar_init", pthread_cond_init(&bgsignal_, nullptr));
    }

    ~ThreadPool() {
      assert(bgthreads_.size() == 0U);
    }

    void JoinAllThreads() {
      PthreadCall("lock", pthread_mutex_lock(&mu_));
      assert(!exit_all_threads_);
      exit_all_threads_ = true;
      PthreadCall("signalall", pthread_cond_broadcast(&bgsignal_));
      PthreadCall("unlock", pthread_mutex_unlock(&mu_));
      for (const auto tid : bgthreads_) {
        pthread_join(tid, nullptr);
      }
      bgthreads_.clear();
    }

    void SetHostEnv(Env* env) {
      env_ = env;
    }

    void LowerIOPriority() {
#ifdef OS_LINUX
      PthreadCall("lock", pthread_mutex_lock(&mu_));
      low_io_priority_ = true;
      PthreadCall("unlock", pthread_mutex_unlock(&mu_));
#endif
    }

    // Return true if there is at least one thread needs to terminate.
    bool HasExcessiveThread() {
      return static_cast<int>(bgthreads_.size()) > total_threads_limit_;
    }

    // Return true iff the current thread is the excessive thread to terminate.
    // Always terminate the running thread that is added last, even if there are
    // more than one thread to terminate.
    bool IsLastExcessiveThread(size_t thread_id) {
      return HasExcessiveThread() && thread_id == bgthreads_.size() - 1;
    }

    // Is one of the threads to terminate.
    bool IsExcessiveThread(size_t thread_id) {
      return static_cast<int>(thread_id) >= total_threads_limit_;
    }

    // Return the thread priority.
    // This would allow its member-thread to know its priority.
    Env::Priority GetThreadPriority() {
      return priority_;
    }

    // Set the thread priority.
    void SetThreadPriority(Env::Priority priority) {
      priority_ = priority;
    }

    void BGThread(size_t thread_id) {
      bool low_io_priority = false;
      while (true) {
        // Wait until there is an item that is ready to run
        PthreadCall("lock", pthread_mutex_lock(&mu_));
        // Stop waiting if the thread needs to do work or needs to terminate.
        while (!exit_all_threads_ && !IsLastExcessiveThread(thread_id) &&
               (queue_.empty() || IsExcessiveThread(thread_id))) {
          PthreadCall("wait", pthread_cond_wait(&bgsignal_, &mu_));
        }
        if (exit_all_threads_) { // mechanism to let BG threads exit safely
          PthreadCall("unlock", pthread_mutex_unlock(&mu_));
          break;
        }
        if (IsLastExcessiveThread(thread_id)) {
          // Current thread is the last generated one and is excessive.
          // We always terminate excessive thread in the reverse order of
          // generation time.
          auto terminating_thread = bgthreads_.back();
          pthread_detach(terminating_thread);
          bgthreads_.pop_back();
          if (HasExcessiveThread()) {
            // There is still at least more excessive thread to terminate.
            WakeUpAllThreads();
          }
          PthreadCall("unlock", pthread_mutex_unlock(&mu_));
          break;
        }
        void (*function)(void*) = queue_.front().function;
        void* arg = queue_.front().arg;
        queue_.pop_front();
        queue_len_.store(static_cast<unsigned int>(queue_.size()),
                         std::memory_order_relaxed);

        bool decrease_io_priority = (low_io_priority != low_io_priority_);
        PthreadCall("unlock", pthread_mutex_unlock(&mu_));

#ifdef OS_LINUX
        if (decrease_io_priority) {
          #define IOPRIO_CLASS_SHIFT               (13)
          #define IOPRIO_PRIO_VALUE(class, data)   \
              (((class) << IOPRIO_CLASS_SHIFT) | data)
          // Put schedule into IOPRIO_CLASS_IDLE class (lowest)
          // These system calls only have an effect when used in conjunction
          // with an I/O scheduler that supports I/O priorities. As at
          // kernel 2.6.17 the only such scheduler is the Completely
          // Fair Queuing (CFQ) I/O scheduler.
          // To change scheduler:
          //  echo cfq > /sys/block/<device_name>/queue/schedule
          // Tunables to consider:
          //  /sys/block/<device_name>/queue/slice_idle
          //  /sys/block/<device_name>/queue/slice_sync
          syscall(SYS_ioprio_set,
                  1,  // IOPRIO_WHO_PROCESS
                  0,  // current thread
                  IOPRIO_PRIO_VALUE(3, 0));
          low_io_priority = true;
        }
#else
        (void)decrease_io_priority; // avoid 'unused variable' error
#endif
        (*function)(arg);
      }
    }

    // Helper struct for passing arguments when creating threads.
    struct BGThreadMetadata {
      ThreadPool* thread_pool_;
      size_t thread_id_;  // Thread count in the thread.
      explicit BGThreadMetadata(ThreadPool* thread_pool, size_t thread_id)
          : thread_pool_(thread_pool), thread_id_(thread_id) {}
    };

    static void* BGThreadWrapper(void* arg) {
      BGThreadMetadata* meta = reinterpret_cast<BGThreadMetadata*>(arg);
      size_t thread_id = meta->thread_id_;
      ThreadPool* tp = meta->thread_pool_;
#if ROCKSDB_USING_THREAD_STATUS
      // for thread-status
      ThreadStatusUtil::RegisterThread(tp->env_,
          (tp->GetThreadPriority() == Env::Priority::HIGH ?
              ThreadStatus::HIGH_PRIORITY :
              ThreadStatus::LOW_PRIORITY));
#endif
      delete meta;
      tp->BGThread(thread_id);
#if ROCKSDB_USING_THREAD_STATUS
      ThreadStatusUtil::UnregisterThread();
#endif
      return nullptr;
    }

    void WakeUpAllThreads() {
      PthreadCall("signalall", pthread_cond_broadcast(&bgsignal_));
    }

    void SetBackgroundThreadsInternal(int num, bool allow_reduce) {
      PthreadCall("lock", pthread_mutex_lock(&mu_));
      if (exit_all_threads_) {
        PthreadCall("unlock", pthread_mutex_unlock(&mu_));
        return;
      }
      if (num > total_threads_limit_ ||
          (num < total_threads_limit_ && allow_reduce)) {
        total_threads_limit_ = std::max(1, num);
        WakeUpAllThreads();
        StartBGThreads();
      }
      PthreadCall("unlock", pthread_mutex_unlock(&mu_));
    }

    void IncBackgroundThreadsIfNeeded(int num) {
      SetBackgroundThreadsInternal(num, false);
    }

    void SetBackgroundThreads(int num) {
      SetBackgroundThreadsInternal(num, true);
    }

    void StartBGThreads() {
      // Start background thread if necessary
      while ((int)bgthreads_.size() < total_threads_limit_) {
        pthread_t t;
        PthreadCall(
            "create thread",
            pthread_create(&t, nullptr, &ThreadPool::BGThreadWrapper,
                           new BGThreadMetadata(this, bgthreads_.size())));

        // Set the thread name to aid debugging
#if defined(_GNU_SOURCE) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 12)
        char name_buf[16];
        snprintf(name_buf, sizeof name_buf, "rocksdb:bg%" ROCKSDB_PRIszt,
                 bgthreads_.size());
        name_buf[sizeof name_buf - 1] = '\0';
        pthread_setname_np(t, name_buf);
#endif
#endif

        bgthreads_.push_back(t);
      }
    }

    void Schedule(void (*function)(void* arg1), void* arg, void* tag) {
      PthreadCall("lock", pthread_mutex_lock(&mu_));

      if (exit_all_threads_) {
        PthreadCall("unlock", pthread_mutex_unlock(&mu_));
        return;
      }

      StartBGThreads();

      // Add to priority queue
      queue_.push_back(BGItem());
      queue_.back().function = function;
      queue_.back().arg = arg;
      queue_.back().tag = tag;
      queue_len_.store(static_cast<unsigned int>(queue_.size()),
                       std::memory_order_relaxed);

      if (!HasExcessiveThread()) {
        // Wake up at least one waiting thread.
        PthreadCall("signal", pthread_cond_signal(&bgsignal_));
      } else {
        // Need to wake up all threads to make sure the one woken
        // up is not the one to terminate.
        WakeUpAllThreads();
      }

      PthreadCall("unlock", pthread_mutex_unlock(&mu_));
    }

    int UnSchedule(void* arg) {
      int count = 0;
      PthreadCall("lock", pthread_mutex_lock(&mu_));

      // Remove from priority queue
      BGQueue::iterator it = queue_.begin();
      while (it != queue_.end()) {
        if (arg == (*it).tag) {
          it = queue_.erase(it);
          count++;
        } else {
          it++;
        }
      }
      queue_len_.store(static_cast<unsigned int>(queue_.size()),
                       std::memory_order_relaxed);
      PthreadCall("unlock", pthread_mutex_unlock(&mu_));
      return count;
    }

    unsigned int GetQueueLen() const {
      return queue_len_.load(std::memory_order_relaxed);
    }

   private:
    // Entry per Schedule() call
    struct BGItem {
      void* arg;
      void (*function)(void*);
      void* tag;
    };
    typedef std::deque<BGItem> BGQueue;

    pthread_mutex_t mu_;
    pthread_cond_t bgsignal_;
    int total_threads_limit_;
    std::vector<pthread_t> bgthreads_;
    BGQueue queue_;
    std::atomic_uint queue_len_;  // Queue length. Used for stats reporting
    bool exit_all_threads_;
    bool low_io_priority_;
    Env::Priority priority_;
    Env* env_;
  };

  std::vector<ThreadPool> thread_pools_;

  pthread_mutex_t mu_;
  std::vector<pthread_t> threads_to_join_;

};

PosixEnv::PosixEnv()
    : checkedDiskForMmap_(false),
      forceMmapOff(false),
      page_size_(getpagesize()),
      thread_pools_(Priority::TOTAL) {
  PthreadCall("mutex_init", pthread_mutex_init(&mu_, nullptr));
  for (int pool_id = 0; pool_id < Env::Priority::TOTAL; ++pool_id) {
    thread_pools_[pool_id].SetThreadPriority(
        static_cast<Env::Priority>(pool_id));
    // This allows later initializing the thread-local-env of each thread.
    thread_pools_[pool_id].SetHostEnv(this);
  }
  thread_status_updater_ = CreateThreadStatusUpdater();
}

void PosixEnv::Schedule(void (*function)(void* arg1), void* arg, Priority pri,
                        void* tag) {
  assert(pri >= Priority::LOW && pri <= Priority::HIGH);
  thread_pools_[pri].Schedule(function, arg, tag);
}

int PosixEnv::UnSchedule(void* arg, Priority pri) {
  return thread_pools_[pri].UnSchedule(arg);
}

unsigned int PosixEnv::GetThreadPoolQueueLen(Priority pri) const {
  assert(pri >= Priority::LOW && pri <= Priority::HIGH);
  return thread_pools_[pri].GetQueueLen();
}

struct StartThreadState {
  void (*user_function)(void*);
  void* arg;
};

static void* StartThreadWrapper(void* arg) {
  StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);
  state->user_function(state->arg);
  delete state;
  return nullptr;
}

void PosixEnv::StartThread(void (*function)(void* arg), void* arg) {
  pthread_t t;
  StartThreadState* state = new StartThreadState;
  state->user_function = function;
  state->arg = arg;
  PthreadCall("start thread",
              pthread_create(&t, nullptr,  &StartThreadWrapper, state));
  PthreadCall("lock", pthread_mutex_lock(&mu_));
  threads_to_join_.push_back(t);
  PthreadCall("unlock", pthread_mutex_unlock(&mu_));
}

void PosixEnv::WaitForJoin() {
  for (const auto tid : threads_to_join_) {
    pthread_join(tid, nullptr);
  }
  threads_to_join_.clear();
}

}  // namespace

std::string Env::GenerateUniqueId() {
  std::string uuid_file = "/proc/sys/kernel/random/uuid";

  Status s = FileExists(uuid_file);
  if (s.ok()) {
    std::string uuid;
    s = ReadFileToString(this, uuid_file, &uuid);
    if (s.ok()) {
      return uuid;
    }
  }
  // Could not read uuid_file - generate uuid using "nanos-random"
  Random64 r(time(nullptr));
  uint64_t random_uuid_portion =
    r.Uniform(std::numeric_limits<uint64_t>::max());
  uint64_t nanos_uuid_portion = NowNanos();
  char uuid2[200];
  snprintf(uuid2,
           200,
           "%lx-%lx",
           (unsigned long)nanos_uuid_portion,
           (unsigned long)random_uuid_portion);
  return uuid2;
}

Env* Env::Default() {
  static PosixEnv default_env;
  return &default_env;
}

}  // namespace rocksdb