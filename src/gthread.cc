#include "gthread.hh"

#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

#include "gstm.hh"
#include "log.h"
#include "util.hh"

GThread *GThread::GetInstance() {
  static char buffer[sizeof(GThread)];
  GThread *instance = new (buffer) GThread();
  instance->tid_ = getpid();
  instance->predecessor_ = 0;
  instance->AtomicBegin();
  return instance;
}

void GThread::Create(void *(*start_routine)(void *), void *args) {
  AtomicEnd();

  int child_pid = fork();
  REQUIRE(child_pid >= 0) << "fork failed: " << strerror(errno);

  if (child_pid > 0) {
    // Parent process
    predecessor_ = child_pid;

    AtomicBegin();
    return;
  } else {
    // Child process

    tid_ = getpid();

    AtomicBegin();
    // Execute thread function
    retval_ = start_routine(args);
    AtomicEnd();

    Gstm::Finalize();

    exit(0);

    return;
  }
}

void GThread::AtomicBegin() {
  ColorLog << "<a.beg>" << END;

  // Clear the local version mappings
  Gstm::read_set_version.clear();
  Gstm::write_set_version.clear();
  Gstm::local_page_version.clear();

  // Copy the global version mapping to local
  pthread_mutex_lock(Gstm::mutex);
  for (const auto &p : *Gstm::global_page_version) {
    Gstm::local_page_version.insert(p);
  }
  pthread_mutex_unlock(Gstm::mutex);

  // Unmap and map again at the beginning of the local heap to make sure the
  // local heap represent the latest view of the file THIS IS IMPORTANT since
  // whenther MAP_PRIVATE will reflect the latest state of the backed file is
  // unspecified and from experiment, it doesn't do that on Linux.
  REQUIRE(munmap(local_heap, HEAP_SIZE) == 0)
      << "munmap failed: " << strerror(errno);
  void *tmp = mmap(local_heap, HEAP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE,
                   shm_fd, 0);
  REQUIRE(tmp == local_heap) << "mmap failed: " << strerror(errno);

  // Turn off all permission on the local heap
  REQUIRE(mprotect(local_heap, HEAP_SIZE, PROT_NONE) == 0)
      << "mprotect failed: " << strerror(errno);

  context_.SaveContext();

  return;
}

void GThread::AtomicEnd() {
  ColorLog << "<a.end>" << END;
  if (!AtomicCommit()) {
    AtomicAbort();
  }
}

bool GThread::AtomicCommit() {
  // If we haven't read or written anything
  // we don't have to wait or commitUpdate local view of memory and return
  // true
  if (Gstm::read_set_version.empty() && Gstm::write_set_version.empty()) {
    // TODO: What do we need to update here?
    // Gstm::UpdateHeap();
    ColorLog << "<com.S>\t\tNo read & write" << END;
    return true;
  }

  // Wait for immediate predecessor to complete
  Gstm::WaitExited(predecessor_);

  // Now try to commit state. If and only if we succeed, return true

  // Lock to make sure only one process is commiting at a time
  pthread_mutex_lock(Gstm::mutex);
  bool commited = false;
  if (Gstm::IsHeapConsistent()) {
    Gstm::CommitHeap();
    ColorLog << "<com.S>\t\tconsistent heap" << END;
    commited = true;
  }
  pthread_mutex_unlock(Gstm::mutex);

  return commited;
}

void GThread::AtomicAbort() {
  ColorLog << "<rollback>" << END;
  context_.RestoreContext();
}

void GThread::Join() {
  AtomicEnd();
  Gstm::WaitExited(predecessor_);
  AtomicBegin();
}
