#include "gstm.hh"

#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <memory>

#include "color_log.hh"
#include "gthread.hh"
#include "log.h"
#include "util.hh"

// Initialize static members of Gstm
pthread_mutex_t* Gstm::mutex = nullptr;
Gstm::private_mapping_t* Gstm::read_set_version = nullptr;
Gstm::private_mapping_t* Gstm::write_set_version = nullptr;
Gstm::private_mapping_t* Gstm::local_page_version = nullptr;
Gstm::share_mapping_t* Gstm::global_page_version = nullptr;
size_t* Gstm::rollback_count_ = nullptr;

void Gstm::Initialize() {
  // Set up segfault handler
  struct sigaction sa;
  memset(&sa, 0, sizeof(struct sigaction));
  sa.sa_sigaction = Gstm::SegfaultHandler;
  sa.sa_flags = SA_SIGINFO;
  REQUIRE(sigaction(SIGSEGV, &sa, NULL) == 0)
      << "sigaction failed: " << strerror(errno);

  // Set up inter-process lock
  SetupInterProcessMutex();

  InitMapping();
}

void Gstm::InitMapping() {
  // Private mapping
  void* buffer = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  read_set_version = new (buffer) private_mapping_t();
  REQUIRE(buffer != MAP_FAILED) << "mmap failed: " << strerror(errno);

  buffer = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(buffer != MAP_FAILED) << "mmap failed: " << strerror(errno);
  write_set_version = new (buffer) private_mapping_t();

  buffer = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(buffer != MAP_FAILED) << "mmap failed: " << strerror(errno);
  local_page_version = new (buffer) private_mapping_t();

  // Shared mapping
  buffer = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);

  REQUIRE(buffer != MAP_FAILED) << "mmap failed: " << strerror(errno);
  global_page_version = new (buffer) share_mapping_t();

  buffer = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);

  REQUIRE(buffer != MAP_FAILED) << "mmap failed: " << strerror(errno);
  rollback_count_ = new (buffer) size_t();
}

void Gstm::SetupInterProcessMutex() {
  // This memory has to be accessed by different processes
  mutex = reinterpret_cast<pthread_mutex_t*>(
      mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
           -1, 0));
  REQUIRE(mutex != MAP_FAILED) << "mmap failed: " << strerror(errno);

  pthread_mutexattr_t mutex_attr;
  REQUIRE(pthread_mutexattr_init(&mutex_attr) == 0)
      << "pthread_mutexattr_init failed: " << strerror(errno);
  REQUIRE(pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED) ==
          0)
      << "pthread_mutexattr_setphared failed: " << strerror(errno);
  REQUIRE(pthread_mutex_init(mutex, &mutex_attr) == 0)
      << "pthread_mutex_init failed: " << strerror(errno);
  REQUIRE(pthread_mutexattr_destroy(&mutex_attr) == 0)
      << "pthread_mutex_attr_destroy failed: " << strerror(errno);
}

void Gstm::HandleReads(void* page) {
  size_t version_num = 0;
  if (local_page_version->find(page) != local_page_version->end()) {
    version_num = local_page_version->at(page);
  }
  ColorLog("<read>\t\t" << page << " version: " << version_num);

  (*read_set_version)[page] = version_num;
  REQUIRE(mprotect(page, PAGE_SIZE, PROT_READ) == 0)
      << "mprotect failed: " << strerror(errno);
}

void Gstm::HandleWrites(void* page) {
  size_t version_num = 1;
  if (local_page_version->find(page) != local_page_version->end()) {
    version_num = local_page_version->at(page) + 1;
  }

  ColorLog("<write>\t\t" << page << " version: " << version_num);

  (*write_set_version)[page] = version_num;

  REQUIRE(mprotect(page, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0)
      << "mprotect failed: " << strerror(errno);
}

void Gstm::SegfaultHandler(int signal, siginfo_t* info, void* ctx) {
  void* addr = info->si_addr;
  void* page = (void*)ROUND_DOWN((uintptr_t)addr, PAGE_SIZE);

  // If the memory does not come from bump allocator
  if ((uintptr_t)page < (uintptr_t)local_heap ||
      (uintptr_t)page > ((uintptr_t)local_heap) + HEAP_SIZE) {
    ColorLog("REAL segfault at " << addr);

    // Use _exit() instead of exit() since exit() is not safe in segfault
    // handler
    _exit(1);
  }

  if (read_set_version->find(page) != read_set_version->end()) {
    // If this is the second time this page enters the segfualt handler, it has
    // to be a write
    HandleWrites(page);
    return;
  } else {
    // If this is the first time that accesses this page, we consider it as a
    // to be a read
    HandleReads(page);
    return;
  }

  if (write_set_version->find(page) != write_set_version->end()) {
    // If this is the third time that triggers the fault, this is an actual
    // segfault
    ColorLog("REAL segfault at " << addr);

    // Use _exit() instead of exit() since exit() is not safe in segfault
    // handler
    _exit(1);
  }
}

void Gstm::WaitExited(pid_t predecessor) {
  if (predecessor == 0) {
    return;
  }
  int status;
  ColorLog("<wait>\t\tpid:" << predecessor);
  int ret = waitpid(predecessor, &status, 0);
  // It is possible this process has already used join to collect its immediate
  // child
  if (ret != predecessor) {
    if (errno == ECHILD) {
      // Keep checking if the predecessor has exited in the case that
      // predecessor is not a child of current process
      while (kill(predecessor, 0) != -1)
        ;
      if (errno == ESRCH) {
        ColorLog("<wait.S>\t\tpid:" << predecessor);
        return;
      }
    } else {
      ColorLog("Wait for processor " << predecessor
                                     << " failed: " << strerror(errno));
      _exit(0);
    }
  }
  ColorLog("<wait.S>\t\tpid:" << predecessor);
}

bool Gstm::IsHeapConsistent() {
  // Check for read set:
  //   If any page in the read set that is also in the global page set does not
  //   match the version number with the one in the global page set, then we
  //   have to rollback
  for (const auto& p : *read_set_version) {
    if (global_page_version->find(p.first) != global_page_version->end() &&
        global_page_version->at(p.first) != p.second) {
      ColorLog("<com.F>\t\tread "
               << p.first << " local version:" << p.second
               << ", global version: " << global_page_version->at(p.first));
      return false;
    }
  }

  return true;
}

void Gstm::CommitHeap() {
  for (const auto& p : *write_set_version) {
    // Calculate the offset between the local state and the global state
    size_t offset = reinterpret_cast<uintptr_t>(p.first) -
                    reinterpret_cast<uintptr_t>(local_heap);
    void* global_pos = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(global_heap) + offset);
    memcpy(global_pos, p.first, PAGE_SIZE);
    ColorLog("<copy>\t\t" << p.first << " to " << global_pos);

    // Update the page version number
    (*global_page_version)[p.first] = p.second;
    ColorLog("<commit>\t\t" << p.first << " version: " << p.second);
  }
  // Make sure the updates to the share mapping is actually synced to the
  // backing file
  REQUIRE(msync(global_heap, HEAP_SIZE, MS_SYNC) == 0)
      << "msync failed: " << strerror(errno);
  ColorLog("<msync.S>");

  // Update the local view of memory
  UpdateHeap();
}

void Gstm::UpdateHeap() {
  // Unmap and map again at the beginning of the local heap to make sure the
  // local heap represent the latest view of the file THIS IS IMPORTANT since
  // whenther MAP_PRIVATE will reflect the latest state of the backed file is
  // unspecified and from experiment, it doesn't do that on Linux.
  REQUIRE(munmap(local_heap, HEAP_SIZE) == 0)
      << "munmap failed: " << strerror(errno);
  void* tmp = mmap(local_heap, HEAP_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_FIXED, shm_fd, 0);
  REQUIRE(tmp == local_heap) << "mmap failed: " << strerror(errno);
}
