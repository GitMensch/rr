/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "ProcessSyscallRec"

#include "record_syscall.h"

#include <arpa/inet.h>
#include <asm/ldt.h>
#include <assert.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/futex.h>
#include <linux/if.h>
#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/net.h>
#include <linux/prctl.h>
#include <linux/sem.h>
#include <linux/shm.h>
#include <linux/sockios.h>
#include <linux/wireless.h>
#include <poll.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/quota.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <termios.h>

#include <limits>
#include <utility>

#include <rr/rr.h>

#include "preload/preload_interface.h"

#include "AutoRemoteSyscalls.h"
#include "drm.h"
#include "Flags.h"
#include "kernel_abi.h"
#include "kernel_metadata.h"
#include "log.h"
#include "RecordSession.h"
#include "Scheduler.h"
#include "task.h"
#include "TraceStream.h"
#include "util.h"

using namespace std;
using namespace rr;

/**
 * Modes used to register syscall memory parameter with TaskSyscallState.
 */
enum ArgMode {
  // Syscall memory parameter is an in-parameter only.
  // This is only important when we want to move the buffer to scratch memory
  // so we can modify it without making the modifications potentially visible
  // to user code. Otherwise, such parameters can be ignored.
  IN,
  // Syscall memory parameter is out-parameter only.
  OUT,
  // Syscall memory parameter is an in-out parameter.
  IN_OUT,
  // Syscall memory parameter is an in-out parameter but we must not use
  // scratch (e.g. for futexes, we must use the actual memory word).
  IN_OUT_NO_SCRATCH
};

/**
 * Specifies how to determine the size to record for a syscall memory
 * parameter. There is a static max_size determined before the syscall
 * executes (which we need in order to allocate scratch memory), combined
 * with an optional dynamic size taken from the syscall result or a specific
 * memory location after the syscall has executed. The minimum of the static
 * and dynamic size (if any) is used.
 */
struct ParamSize {
  ParamSize(size_t max_size = size_t(-1))
      : max_size(max_size), from_syscall(false) {}
  template <typename T>
  static ParamSize from_initialized_mem(Task* t, remote_ptr<T> p) {
    ParamSize r(p.is_null() ? size_t(0) : size_t(t->read_mem(p)));
    r.mem_ptr = p;
    r.read_size = sizeof(T);
    return r;
  }
  template <typename T> static ParamSize from_mem(remote_ptr<T> p) {
    ParamSize r(size_t(-1));
    r.mem_ptr = p;
    r.read_size = sizeof(T);
    return r;
  }
  template <typename T>
  static ParamSize from_syscall_result(size_t max_size = size_t(-1)) {
    ParamSize r(max_size);
    r.from_syscall = true;
    r.read_size = sizeof(T);
    return r;
  }
  ParamSize limit_size(size_t max) const {
    ParamSize r(*this);
    r.max_size = min(r.max_size, max);
    return r;
  }

  /**
   * Return true if 'other' takes its dynamic size from the same source as
   * this.
   * When multiple syscall memory parameters take their dynamic size from the
   * same source, the source size is distributed among them, with the first
   * registered parameter taking up to its max_size bytes, followed by the next,
   * etc. This lets us efficiently record iovec buffers.
   */
  bool is_same_source(const ParamSize& other) const {
    return ((!mem_ptr.is_null() && other.mem_ptr == mem_ptr) ||
            (from_syscall && other.from_syscall)) &&
           (read_size == other.read_size);
  }
  /**
   * Compute the actual size after the syscall has executed.
   * 'already_consumed' bytes are subtracted from the dynamic part of the size.
   */
  size_t eval(Task* t, size_t already_consumed) const;

  /** explicit size or max size if mem_ptr/from_syscall_result is specified */
  size_t max_size;
  /** read size from this location */
  remote_ptr<void> mem_ptr;
  /** number of bytes to read to get the size */
  size_t read_size;
  /** when true, read size from syscall result register */
  bool from_syscall;
};

/**
 * When tasks enter syscalls that may block and so must be
 * prepared for a context-switch, and the syscall params
 * include (in)outparams that point to buffers, we need to
 * redirect those arguments to scratch memory.  This allows rr
 * to serialize execution of what may be multiple blocked
 * syscalls completing "simultaneously" (from rr's
 * perspective).  After the syscall exits, we restore the data
 * saved in scratch memory to the original buffers.
 *
 * Then during replay, we simply restore the saved data to the
 * tracee's passed-in buffer args and continue on.
 *
 * This is implemented by having rec_prepare_syscall_arch set up
 * a record in param_list for syscall in-memory  parameter (whether
 * "in" or "out"). Then done_preparing is called, which does the actual
 * scratch setup. process_syscall_results is called when the syscall is
 * done, to write back scratch results to the real parameters and
 * clean everything up.
 *
 * ... a fly in this ointment is may-block buffered syscalls.
 * If a task blocks in one of those, it will look like it just
 * entered a syscall that needs a scratch buffer.  However,
 * it's too late at that point to fudge the syscall args,
 * because processing of the syscall has already begun in the
 * kernel.  But that's OK: the syscallbuf code has already
 * swapped out the original buffer-pointers for pointers into
 * the syscallbuf (which acts as its own scratch memory).  We
 * just have to worry about setting things up properly for
 * replay.
 *
 * The descheduled syscall will "abort" its commit into the
 * syscallbuf, so the outparam data won't actually be saved
 * there (and thus, won't be restored during replay).  During
 * replay, we have to restore them like we restore the
 * non-buffered-syscall scratch data. This is done by recording
 * the relevant syscallbuf record data in rec_process_syscall_arch.
 */
struct TaskSyscallState {
  void init(Task* t) {
    if (preparation_done) {
      return;
    }
    this->t = t;
    scratch = t->scratch_ptr;
  }

  /**
   * Identify a syscall memory parameter whose address is in register 'arg'
   * with type T.
   * Returns a remote_ptr to the data in the child (before scratch relocation)
   * or null if parameters have already been prepared (the syscall is
   * resuming).
   */
  template <typename T>
  remote_ptr<T> reg_parameter(int arg, ArgMode mode = OUT) {
    return reg_parameter(arg, sizeof(T), mode).cast<T>();
  }
  /**
   * Identify a syscall memory parameter whose address is in register 'arg'
   * with size 'size'.
   * Returns a remote_ptr to the data in the child (before scratch relocation)
   * or null if parameters have already been prepared (the syscall is
   * resuming).
   */
  remote_ptr<void> reg_parameter(int arg, const ParamSize& size,
                                 ArgMode mode = OUT);
  /**
   * Identify a syscall memory parameter whose address is in memory at
   * location 'addr_of_buf_ptr' with type T.
   * Returns a remote_ptr to the data in the child (before scratch relocation)
   * or null if parameters have already been prepared (the syscall is
   * resuming).
   * addr_of_buf_ptr must be in a buffer identified by some init_..._parameter
   * call.
   */
  template <typename T>
  remote_ptr<T> mem_ptr_parameter(remote_ptr<void> addr_of_buf_ptr,
                                  ArgMode mode = OUT) {
    return mem_ptr_parameter(addr_of_buf_ptr, sizeof(T), mode).cast<T>();
  }
  /**
   * Identify a syscall memory parameter whose address is in memory at
   * location 'addr_of_buf_ptr' with type T.
   * Returns a remote_ptr to the data in the child (before scratch relocation)
   * or null if parameters have already been prepared (the syscall is
   * resuming).
   * addr_of_buf_ptr must be in a buffer identified by some init_..._parameter
   * call.
   */
  template <typename Ptr>
  remote_ptr<typename Ptr::Referent> mem_ptr_parameter_inferred(
      remote_ptr<Ptr> addr_of_buf_ptr, ArgMode mode = OUT) {
    remote_ptr<void> p =
        mem_ptr_parameter(addr_of_buf_ptr, Ptr::referent_size(), mode);
    return p.cast<typename Ptr::Referent>();
  }
  /**
   * Identify a syscall memory parameter whose address is in memory at
   * location 'addr_of_buf_ptr' with size 'size'.
   * Returns a remote_ptr to the data in the child (before scratch relocation)
   * or null if parameters have already been prepared (the syscall is
   * resuming).
   * addr_of_buf_ptr must be in a buffer identified by some init_..._parameter
   * call.
   */
  remote_ptr<void> mem_ptr_parameter(remote_ptr<void> addr_of_buf_ptr,
                                     const ParamSize& size, ArgMode mode = OUT);
  /**
   * Internal method that takes 'ptr', an address within some memory parameter,
   * and relocates it to the parameter's location in scratch memory.
   */
  remote_ptr<void> relocate_pointer_to_scratch(remote_ptr<void> ptr);
  /**
   * Internal method that takes the index of a MemoryParam and a vector
   * containing the actual sizes assigned to each param < param_index, and
   * computes the actual size to use for parameter param_index.
   */
  size_t eval_param_size(size_t param_index, vector<size_t>& actual_sizes);
  /**
   * Called when all memory parameters have been identified. If 'sw' is
   * ALLOW_SWITCH, sets up scratch memory and updates registers etc as
   * necessary.
   * If scratch can't be used for some reason, returns PREVENT_SWITCH,
   * otherwise returns 'sw'.
   */
  Switchable done_preparing(Switchable sw);
  enum WriteBack {
    WRITE_BACK,
    NO_WRITE_BACK
  };
  /**
   * Called when a syscall exits to copy results from scratch memory to their
   * original destinations, update registers, etc.
   * Pass NO_WRITE_BACK to indicate that the kernel did not write anything.
   */
  void process_syscall_results(WriteBack write_back = WRITE_BACK);

  /**
   * Upon successful syscall completion, each RestoreAndRecordScratch record
   * in param_list consumes num_bytes from the t->scratch_ptr
   * buffer, copying the data to remote_dest and recording the data at
   * remote_dest. If ptr_in_reg is greater than zero, updates the task's
   * ptr_in_reg register with 'remote_dest'. If ptr_in_memory is non-null,
   * updates the ptr_in_memory location with the value 'remote_dest'.
   */
  struct MemoryParam {
    MemoryParam() : ptr_in_reg(0) {}

    remote_ptr<void> dest;
    remote_ptr<void> scratch;
    ParamSize num_bytes;
    remote_ptr<void> ptr_in_memory;
    int ptr_in_reg;
    ArgMode mode;
  };

  Task* t;

  vector<MemoryParam> param_list;
  /** Tracks the position in t's scratch_ptr buffer where we should allocate
   *  the next scratch area.
   */
  remote_ptr<void> scratch;

  std::unique_ptr<TraceTaskEvent> exec_saved_event;

  /** Saved syscall-entry registers, used by a couple of code paths that
   *  modify the registers temporarily.
   */
  std::unique_ptr<Registers> syscall_entry_registers;

  /** When nonzero, syscall is expected to return the given errno and we should
   *  die if it does not. This is set when we detect an error condition during
   *  syscall-enter preparation.
   */
  int expect_errno;

  /** Records whether the syscall is switchable. Only valid when
   *  preparation_done is true. */
  Switchable switchable;
  /** When true, this syscall has already been prepared and should not
   *  be set up again.
   */
  bool preparation_done;
  /** When true, the scratch area is enabled, otherwise we're letting
   *  syscall outputs be written directly to their destinations.
   *  Only valid when preparation_done is true.
   */
  bool scratch_enabled;
  /** When true, we'll record the page of memory below the stack pointer.
   *  Some ioctls seem to modify this for no good reason.
   */
  bool record_page_below_stack_ptr;

  TaskSyscallState()
      : t(nullptr),
        expect_errno(0),
        preparation_done(false),
        scratch_enabled(false),
        record_page_below_stack_ptr(false) {}
};

static const Property<TaskSyscallState, Task> syscall_state_property;

template <typename Arch>
static void rec_before_record_syscall_entry_arch(Task* t, int syscallno) {
  if (Arch::write != syscallno) {
    return;
  }
  int fd = t->regs().arg1_signed();
  if (RR_MAGIC_SAVE_DATA_FD != fd) {
    return;
  }
  remote_ptr<void> buf = t->regs().arg2();
  size_t len = t->regs().arg3();

  ASSERT(t, buf) << "Can't save a null buffer";

  t->record_remote(buf, len);
}

void rec_before_record_syscall_entry(Task* t, int syscallno) {
  RR_ARCH_FUNCTION(rec_before_record_syscall_entry_arch, t->arch(), t,
                   syscallno)
}

template <typename Arch>
static void set_remote_ptr_arch(Task* t, remote_ptr<void> addr,
                                remote_ptr<void> value) {
  auto typed_addr = addr.cast<typename Arch::unsigned_word>();
  t->write_mem(typed_addr, (typename Arch::unsigned_word)value.as_int());
}

static void set_remote_ptr(Task* t, remote_ptr<void> addr,
                           remote_ptr<void> value) {
  RR_ARCH_FUNCTION(set_remote_ptr_arch, t->arch(), t, addr, value);
}

template <typename Arch>
static remote_ptr<void> get_remote_ptr_arch(Task* t, remote_ptr<void> addr) {
  auto typed_addr = addr.cast<typename Arch::unsigned_word>();
  auto old = t->read_mem(typed_addr);
  return remote_ptr<void>(old);
}

static remote_ptr<void> get_remote_ptr(Task* t, remote_ptr<void> addr) {
  RR_ARCH_FUNCTION(get_remote_ptr_arch, t->arch(), t, addr);
}

static void align_scratch(remote_ptr<void>* scratch, uintptr_t amount = 8) {
  *scratch = (scratch->as_int() + amount - 1) & ~(amount - 1);
}

remote_ptr<void> TaskSyscallState::reg_parameter(int arg, const ParamSize& size,
                                                 ArgMode mode) {
  if (preparation_done) {
    return remote_ptr<void>();
  }

  MemoryParam param;
  param.dest = t->regs().arg(arg);
  if (param.dest.is_null()) {
    return remote_ptr<void>();
  }
  param.num_bytes = size;
  param.mode = mode;
  if (mode != IN_OUT_NO_SCRATCH) {
    param.scratch = scratch;
    scratch += param.num_bytes.max_size;
    align_scratch(&scratch);
    param.ptr_in_reg = arg;
  }
  param_list.push_back(param);
  return param.dest;
}

remote_ptr<void> TaskSyscallState::mem_ptr_parameter(
    remote_ptr<void> addr_of_buf_ptr, const ParamSize& size, ArgMode mode) {
  if (preparation_done) {
    return remote_ptr<void>();
  }

  MemoryParam param;
  param.dest = get_remote_ptr(t, addr_of_buf_ptr);
  if (param.dest.is_null()) {
    return remote_ptr<void>();
  }
  param.num_bytes = size;
  param.mode = mode;
  if (mode != IN_OUT_NO_SCRATCH) {
    param.scratch = scratch;
    scratch += param.num_bytes.max_size;
    align_scratch(&scratch);
    param.ptr_in_memory = addr_of_buf_ptr;
  }
  param_list.push_back(param);
  return param.dest;
}

remote_ptr<void> TaskSyscallState::relocate_pointer_to_scratch(
    remote_ptr<void> ptr) {
  int num_relocations = 0;
  remote_ptr<void> result;
  for (auto& param : param_list) {
    if (param.dest <= ptr && ptr < param.dest + param.num_bytes.max_size) {
      result = param.scratch + (ptr - param.dest);
      ++num_relocations;
    }
  }
  assert(num_relocations > 0 &&
         "Pointer in non-scratch memory being updated to point to scratch?");
  assert(num_relocations <= 1 &&
         "Overlapping buffers containing relocated pointer?");
  return result;
}

Switchable TaskSyscallState::done_preparing(Switchable sw) {
  if (preparation_done) {
    return switchable;
  }
  preparation_done = true;

  ssize_t scratch_num_bytes = scratch - t->scratch_ptr;
  ASSERT(t, scratch_num_bytes >= 0);
  if (sw == ALLOW_SWITCH && scratch_num_bytes > t->scratch_size) {
    LOG(warn)
        << "`" << t->syscall_name(t->ev().Syscall().number)
        << "' needed a scratch buffer of size " << scratch_num_bytes
        << ", but only " << t->scratch_size
        << " was available.  Disabling context switching: deadlock may follow.";
    switchable = PREVENT_SWITCH;
  } else {
    switchable = sw;
  }
  if (switchable == PREVENT_SWITCH || param_list.empty()) {
    return switchable;
  }

  scratch_enabled = true;

  // Step 1: Copy all IN/IN_OUT parameters to their scratch areas
  for (auto& param : param_list) {
    ASSERT(t, param.num_bytes.max_size < size_t(-1));
    if (param.mode == IN_OUT || param.mode == IN) {
      // Initialize scratch buffer with input data
      t->remote_memcpy(param.scratch, param.dest, param.num_bytes.max_size);
    }
  }
  // Step 2: Update pointers in registers/memory to point to scratch areas
  Registers r = t->regs();
  for (auto& param : param_list) {
    if (param.ptr_in_reg) {
      r.set_arg(param.ptr_in_reg, param.scratch.as_int());
    }
    if (!param.ptr_in_memory.is_null()) {
      // Pointers being relocated must themselves be in scratch memory.
      // We don't want to modify non-scratch memory. Find the pointer's location
      // in scratch memory.
      auto p = relocate_pointer_to_scratch(param.ptr_in_memory);
      // Update pointer to point to scratch.
      // Note that this can only happen after step 1 is complete and all
      // parameter data has been copied to scratch memory.
      set_remote_ptr(t, p, param.scratch);
    }
    // If the number of bytes to record is coming from a memory location,
    // update that location to scratch.
    if (!param.num_bytes.mem_ptr.is_null()) {
      param.num_bytes.mem_ptr =
          relocate_pointer_to_scratch(param.num_bytes.mem_ptr);
    }
  }
  t->set_regs(r);
  return switchable;
}

template <typename Arch>
static void prepare_recvmsg(Task* t, TaskSyscallState& syscall_state,
                            remote_ptr<typename Arch::msghdr> msgp,
                            const ParamSize& io_size) {
  auto namelen_ptr = REMOTE_PTR_FIELD(msgp, msg_namelen);
  syscall_state.mem_ptr_parameter(
      REMOTE_PTR_FIELD(msgp, msg_name),
      ParamSize::from_initialized_mem(t, namelen_ptr));

  auto msg = t->read_mem(msgp);
  remote_ptr<void> iovecsp_void = syscall_state.mem_ptr_parameter(
      REMOTE_PTR_FIELD(msgp, msg_iov),
      sizeof(typename Arch::iovec) * msg.msg_iovlen, IN);
  auto iovecsp = iovecsp_void.cast<typename Arch::iovec>();
  auto iovecs = t->read_mem(iovecsp, msg.msg_iovlen);
  for (size_t i = 0; i < msg.msg_iovlen; ++i) {
    syscall_state.mem_ptr_parameter(REMOTE_PTR_FIELD(iovecsp + i, iov_base),
                                    io_size.limit_size(iovecs[i].iov_len));
  }

  auto controllen_ptr = REMOTE_PTR_FIELD(msgp, msg_controllen);
  syscall_state.mem_ptr_parameter(
      REMOTE_PTR_FIELD(msgp, msg_control),
      ParamSize::from_initialized_mem(t, controllen_ptr));
}

template <typename Arch>
static void prepare_recvmmsg(Task* t, TaskSyscallState& syscall_state,
                             remote_ptr<typename Arch::mmsghdr> mmsgp,
                             unsigned int vlen) {
  for (unsigned int i = 0; i < vlen; ++i) {
    auto msgp = mmsgp + i;
    prepare_recvmsg<Arch>(t, syscall_state, REMOTE_PTR_FIELD(msgp, msg_hdr),
                          ParamSize::from_mem(REMOTE_PTR_FIELD(msgp, msg_len)));
  }
}

template <typename Arch>
static Switchable prepare_socketcall(Task* t, TaskSyscallState& syscall_state) {
  /* int socketcall(int call, unsigned long *args) {
   *   long a[6];
   *   copy_from_user(a,args);
   *   sys_recv(a0, (void __user *)a1, a[2], a[3]);
   * }
   *
   *  (from http://lxr.linux.no/#linux+v3.6.3/net/socket.c#L2354)
   */
  switch ((int)t->regs().arg1_signed()) {
    /* int socket(int domain, int type, int protocol); */
    case SYS_SOCKET:
    /* int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
     */
    case SYS_CONNECT:
    /* int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen); */
    case SYS_BIND:
    /* int listen(int sockfd, int backlog) */
    case SYS_LISTEN:
    /* ssize_t send(int sockfd, const void *buf, size_t len, int flags) */
    case SYS_SEND:
    /* ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const
     * struct sockaddr *dest_addr, socklen_t addrlen); */
    case SYS_SENDTO:
    /* int setsockopt(int sockfd, int level, int optname, const void *optval,
     * socklen_t optlen); */
    case SYS_SETSOCKOPT:
    /* int shutdown(int socket, int how) */
    case SYS_SHUTDOWN:
      break;

    /*  int getsockopt(int sockfd, int level, int optname, const void *optval,
     * socklen_t* optlen);
     */
    case SYS_GETSOCKOPT: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::getsockopt_args>(2, IN);
      auto optlen_ptr = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, optlen), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, optval),
          ParamSize::from_initialized_mem(t, optlen_ptr));
      break;
    }

    /* int socketpair(int domain, int type, int protocol, int sv[2]);
     *
     * values returned in sv
     */
    case SYS_SOCKETPAIR: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::socketpair_args>(2, IN);
      syscall_state.mem_ptr_parameter(REMOTE_PTR_FIELD(argsp, sv),
                                      sizeof(int) * 2);
      break;
    }

    /* int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
     */
    case SYS_GETPEERNAME:
    /* int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
     */
    case SYS_GETSOCKNAME: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::getsockname_args>(2, IN);
      auto addrlen_ptr = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, addrlen), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, addr),
          ParamSize::from_initialized_mem(t, addrlen_ptr));
      break;
    }

    /* ssize_t recv([int sockfd, void *buf, size_t len, int flags]) */
    case SYS_RECV: {
      auto argsp = syscall_state.reg_parameter<typename Arch::recv_args>(2, IN);
      auto args = t->read_mem(argsp);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, buf),
          ParamSize::from_syscall_result<typename Arch::ssize_t>(args.len));
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }

    /* int accept([int sockfd, struct sockaddr *addr, socklen_t *addrlen]) */
    case SYS_ACCEPT: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::accept_args>(2, IN);
      auto addrlen_ptr = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, addrlen), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, addr),
          ParamSize::from_initialized_mem(t, addrlen_ptr));
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }

    /* int accept4([int sockfd, struct sockaddr *addr, socklen_t *addrlen, int
     * flags]) */
    case SYS_ACCEPT4: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::accept4_args>(2, IN);
      auto addrlen_ptr = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, addrlen), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, addr),
          ParamSize::from_initialized_mem(t, addrlen_ptr));
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }

    case SYS_RECVFROM: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::recvfrom_args>(2, IN);
      auto args = t->read_mem(argsp);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, buf),
          ParamSize::from_syscall_result<typename Arch::ssize_t>(args.len));
      auto addrlen_ptr = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, addrlen), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, src_addr),
          ParamSize::from_initialized_mem(t, addrlen_ptr));
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }

    case SYS_RECVMSG: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::recvmsg_args>(2, IN);
      auto msgp = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, msg), IN_OUT);
      prepare_recvmsg<Arch>(
          t, syscall_state, msgp,
          ParamSize::from_syscall_result<typename Arch::ssize_t>());

      auto args = t->read_mem(argsp);
      if (!(args.flags & MSG_DONTWAIT)) {
        return syscall_state.done_preparing(ALLOW_SWITCH);
      }
      break;
    }

    case SYS_RECVMMSG: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::recvmmsg_args>(2, IN);
      auto args = t->read_mem(argsp);
      remote_ptr<void> mmsgp_void = syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, msgvec),
          sizeof(typename Arch::mmsghdr) * args.vlen, IN_OUT);
      auto mmsgp = mmsgp_void.cast<typename Arch::mmsghdr>();
      prepare_recvmmsg<Arch>(t, syscall_state, mmsgp, args.vlen);
      if (!(args.flags & MSG_DONTWAIT)) {
        return syscall_state.done_preparing(ALLOW_SWITCH);
      }
      break;
    }

    /* ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) */
    case SYS_SENDMSG: {
      auto argsp = remote_ptr<typename Arch::sendmsg_args>(t->regs().arg2());
      auto args = t->read_mem(argsp);
      if (!(args.flags & MSG_DONTWAIT)) {
        return syscall_state.done_preparing(ALLOW_SWITCH);
      }
      break;
    }

    case SYS_SENDMMSG: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::sendmmsg_args>(2, IN);
      auto args = t->read_mem(argsp);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, msgvec),
          sizeof(typename Arch::mmsghdr) * args.vlen, IN_OUT);
      if (!(args.flags & MSG_DONTWAIT)) {
        return syscall_state.done_preparing(ALLOW_SWITCH);
      }
      break;
    }

    default:
      syscall_state.expect_errno = EINVAL;
      break;
  }
  return syscall_state.done_preparing(PREVENT_SWITCH);
}

template <typename Arch>
static Switchable prepare_msgctl(Task* t, TaskSyscallState& syscall_state,
                                 int cmd, int buf_ptr_reg) {
  switch (cmd) {
    case IPC_STAT:
    case MSG_STAT:
      syscall_state.reg_parameter<typename Arch::msqid64_ds>(buf_ptr_reg);
      break;
    case IPC_INFO:
    case MSG_INFO:
      syscall_state.reg_parameter<typename Arch::msginfo>(buf_ptr_reg);
      break;
  }
  return syscall_state.done_preparing(PREVENT_SWITCH);
}

template <typename Arch>
static void prepare_ioctl(Task* t, TaskSyscallState& syscall_state) {
  int request = (int)t->regs().arg2_signed();
  int type = _IOC_TYPE(request);
  int nr = _IOC_NR(request);
  int dir = _IOC_DIR(request);
  int size = _IOC_SIZE(request);

  LOG(debug) << "handling ioctl(" << HEX(request) << "): type:" << HEX(type)
             << " nr:" << HEX(nr) << " dir:" << HEX(dir) << " size:" << size;

  ASSERT(t, !t->is_desched_event_syscall())
      << "Failed to skip past desched ioctl()";

  /* Some ioctl()s are irregular and don't follow the _IOC()
   * conventions.  Special case them here. */
  switch (request) {
    case SIOCETHTOOL: {
      auto ifrp = syscall_state.reg_parameter<typename Arch::ifreq>(3, IN);
      syscall_state.mem_ptr_parameter<typename Arch::ethtool_cmd>(
          REMOTE_PTR_FIELD(ifrp, ifr_ifru.ifru_data));
      syscall_state.record_page_below_stack_ptr = true;
      return;
    }

    case SIOCGIFCONF: {
      auto ifconfp = syscall_state.reg_parameter<typename Arch::ifconf>(3);
      auto ifconf = t->read_mem(ifconfp);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(ifconfp, ifc_ifcu.ifcu_buf), ifconf.ifc_len);
      syscall_state.record_page_below_stack_ptr = true;
      return;
    }

    case SIOCGIFADDR:
    case SIOCGIFFLAGS:
    case SIOCGIFINDEX:
    case SIOCGIFMTU:
    case SIOCGIFNAME:
      syscall_state.reg_parameter<typename Arch::ifreq>(3);
      syscall_state.record_page_below_stack_ptr = true;
      return;

    case SIOCGIWRATE:
      // SIOCGIWRATE hasn't been observed to write beyond
      // tracees' stacks, but we record a stack page here
      // just in case the behavior is driver-dependent.
      syscall_state.reg_parameter<typename Arch::iwreq>(3);
      syscall_state.record_page_below_stack_ptr = true;
      return;

    case TCGETS:
      syscall_state.reg_parameter<typename Arch::termios>(3);
      return;

    case TIOCINQ:
      syscall_state.reg_parameter<int>(3);
      return;

    case TIOCGWINSZ:
      syscall_state.reg_parameter<typename Arch::winsize>(3);
      return;
  }

  /* In ioctl language, "_IOC_READ" means "outparam".  Both
   * READ and WRITE can be set for inout params. */
  if (!(_IOC_READ & dir)) {
    /* If the kernel isn't going to write any data back to
     * us, we hope and pray that the result of the ioctl
     * (observable to the tracee) is deterministic. */
    LOG(debug) << "  (deterministic ioctl, nothing to do)";
    return;
  }

  /* The following are thought to be "regular" ioctls, the
   * processing of which is only known to (observably) write to
   * the bytes in the structure passed to the kernel.  So all we
   * need is to record |size| bytes.*/
  switch (request) {
    /* TODO: what are the 0x46 ioctls? */
    case 0xc020462b:
    case 0xc048464d:
    case 0xc0204637:
    case 0xc0304627:
      FATAL() << "Unknown 0x46-series ioctl nr " << HEX(nr);
      break; /* not reached */

    /* The following are ioctls for the linux Direct Rendering
     * Manager (DRM).  The ioctl "type" is 0x64 (100, or ASCII 'd'
     * as they docs helpfully declare it :/).  The ioctl numbers
     * are allocated as follows
     *
     *  [0x00, 0x40) -- generic commands
     *  [0x40, 0xa0) -- device-specific commands
     *  [0xa0, 0xff) -- more generic commands
     *
     * Chasing down unknown ioctls is somewhat annoying in this
     * scheme, but here's an example: request "0xc0406481".  "0xc"
     * means it's a read/write ioctl, and "0x0040" is the size of
     * the payload.  The actual ioctl request is "0x6481".
     *
     * As we saw above, "0x64" is the DRM type.  So now we need to
     * see what command "0x81" is.  It's in the
     * device-specific-command space, so we can start by
     * subtracting "0x40" to get a command "0x41".  Then
     *
     *  $ cd
     *  $ grep -rn 0x41 *
     *  nouveau_drm.h:200:#define DRM_NOUVEAU_GEM_PUSHBUF        0x41
     *
     * Well that was lucky!  So the command is
     * DRM_NOUVEAU_GEM_PUSHBUF, and the parameters etc can be
     * tracked down from that.
     */

    /* TODO: At least one of these ioctl()s, most likely
     * NOUVEAU_GEM_NEW, opens a file behind rr's back on behalf of
     * the callee.  That wreaks havoc later on in execution, so we
     * disable the whole lot for now until rr can handle that
     * behavior (by recording access to shmem segments). */
    case DRM_IOCTL_VERSION:
    case DRM_IOCTL_NOUVEAU_GEM_NEW:
    case DRM_IOCTL_NOUVEAU_GEM_PUSHBUF:
      FATAL() << "Intentionally unhandled DRM(0x64) ioctl nr " << HEX(nr);
      break;

    case DRM_IOCTL_GET_MAGIC:
    case DRM_IOCTL_RADEON_INFO:
    case DRM_IOCTL_I915_GEM_PWRITE:
    case DRM_IOCTL_GEM_OPEN:
    case DRM_IOCTL_I915_GEM_MMAP:
    case DRM_IOCTL_RADEON_GEM_CREATE:
    case DRM_IOCTL_RADEON_GEM_GET_TILING:
      FATAL() << "Not-understood DRM(0x64) ioctl nr " << HEX(nr);
      break; /* not reached */

    case 0x4010644d:
    case 0xc0186441:
    case 0x80086447:
    case 0xc0306449:
    case 0xc030644b:
      FATAL() << "Unknown DRM(0x64) ioctl nr " << HEX(nr);
      break; /* not reached */

    default:
      t->regs().print_register_file(stderr);
      ASSERT(t, false) << "Unknown ioctl(" << HEX(request)
                       << "): type:" << HEX(type) << " nr:" << HEX(nr)
                       << " dir:" << HEX(dir) << " size:" << size
                       << " addr:" << HEX(t->regs().arg3());
  }
}

static const int RR_KCMP_FILE = 0;

template <typename Arch> static bool is_stdio_fd(Task* t, int fd) {
  int pid = getpid();

  int r = syscall(Arch::kcmp, pid, t->rec_tid, RR_KCMP_FILE, STDOUT_FILENO, fd);
  if (r < 0 && errno == ENOSYS) {
    return fd == STDOUT_FILENO || fd == STDERR_FILENO;
  }
  if (r == 0) {
    return true;
  }
  if (r < 0 && errno == EBADF) {
    // Tracees may try to write to invalid fds.
    return false;
  }
  ASSERT(t, r >= 0) << "kcmp failed";

  r = syscall(Arch::kcmp, pid, t->rec_tid, RR_KCMP_FILE, STDERR_FILENO, fd);
  if (r == 0) {
    return true;
  }
  if (r < 0 && errno == EBADF) {
    // Tracees may try to write to invalid fds.
    return false;
  }
  ASSERT(t, r >= 0) << "kcmp failed";

  return false;
}

/**
 * |t| was descheduled while in a buffered syscall.  We don't
 * use scratch memory for the call, because the syscallbuf itself
 * is serving that purpose. More importantly, we *can't* set up
 * scratch for |t|, because it's already in the syscall. Instead, we will
 * record the syscallbuf memory in rec_process_syscall_arch.
 *
 * Returns ALLOW_SWITCH if the syscall should be interruptible, PREVENT_SWITCH
 * otherwise.
 */
template <typename Arch>
static Switchable prepare_deschedule(Task* t, TaskSyscallState& syscall_state,
                                     int syscallno) {
  const struct syscallbuf_record* rec = t->desched_rec();

  assert(rec);
  ASSERT(t, syscallno == rec->syscallno) << "Syscallbuf records syscall "
                                         << t->syscall_name(rec->syscallno)
                                         << ", but expecting "
                                         << t->syscall_name(syscallno);

  switch (syscallno) {
    case Arch::write:
    case Arch::writev:
      return is_stdio_fd<Arch>(t, (int)t->regs().arg1_signed()) ? PREVENT_SWITCH
                                                                : ALLOW_SWITCH;
    default:
      return ALLOW_SWITCH;
  }
}

static bool exec_file_supported(const string& file_name) {
#if defined(__i386__)
  /* All this function does is reject 64-bit ELF binaries. Everything
     else we (optimistically) indicate support for. Missing or corrupt
     files will cause execve to fail normally. When we support 64-bit,
     this entire function can be removed. */
  return read_elf_class(file_name) != ELFCLASS64;
#elif defined(__x86_64__)
  // We support 32-bit and 64-bit binaries.
  return true;
#else
#error unknown architecture
#endif
}

template <typename Arch> static Switchable rec_prepare_syscall_arch(Task* t) {
  int syscallno = t->ev().Syscall().number;

  auto& syscall_state = syscall_state_property.get_or_create(*t);
  syscall_state.init(t);

  if (t->desched_rec()) {
    return prepare_deschedule<Arch>(t, syscall_state, syscallno);
  }

  if (syscallno < 0) {
    // Invalid syscall. Don't let it accidentally match a
    // syscall number below that's for an undefined syscall.
    return PREVENT_SWITCH;
  }

  switch (syscallno) {
    case Arch::splice: {
      syscall_state.reg_parameter<loff_t>(2, IN_OUT);
      syscall_state.reg_parameter<loff_t>(4, IN_OUT);
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }

    case Arch::sendfile: {
      syscall_state.reg_parameter<typename Arch::off_t>(3, IN_OUT);
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }
    case Arch::sendfile64: {
      syscall_state.reg_parameter<typename Arch::off64_t>(3, IN_OUT);
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }

    case Arch::clone: {
      syscall_state.syscall_entry_registers =
          unique_ptr<Registers>(new Registers(t->regs()));
      unsigned long flags = t->regs().arg1();
      if (flags & CLONE_UNTRACED) {
        Registers r = t->regs();
        // We can't let tracees clone untraced tasks,
        // because they can create nondeterminism that
        // we can't replay.  So unset the UNTRACED bit
        // and then cover our tracks on exit from
        // clone().
        r.set_arg1(flags & ~CLONE_UNTRACED);
        t->set_regs(r);
      }
      return PREVENT_SWITCH;
    }

    case Arch::exit:
      t->stable_exit = true;
      if (t->task_group()->task_set().size() == 1) {
        t->task_group()->exit_code = (int)t->regs().arg1();
      }
      destroy_buffers(t);
      return PREVENT_SWITCH;

    case Arch::exit_group:
      if (t->task_group()->task_set().size() == 1) {
        t->stable_exit = true;
      }
      t->task_group()->exit_code = (int)t->regs().arg1();
      return PREVENT_SWITCH;

    case Arch::execve: {
      if (!syscall_state.syscall_entry_registers) {
        syscall_state.syscall_entry_registers =
            unique_ptr<Registers>(new Registers(t->regs()));
      }

      t->pre_exec();

      Registers r = t->regs();
      string raw_filename = t->read_c_str(r.arg1());
      uintptr_t end = r.arg1() + raw_filename.length();
      if (!exec_file_supported(t->exec_file())) {
        // Force exec to fail with ENOENT by advancing arg1 to
        // the null byte
        r.set_arg1(end);
        t->set_regs(r);
      }

      vector<string> cmd_line;
      remote_ptr<typename Arch::unsigned_word> argv = r.arg2();
      while (true) {
        auto p = t->read_mem(argv);
        if (!p) {
          break;
        }
        cmd_line.push_back(t->read_c_str(p));
        argv++;
      }
      // Save the event. We can't record it here because the exec might fail.
      syscall_state.exec_saved_event = unique_ptr<TraceTaskEvent>(
          new TraceTaskEvent(t->tid, raw_filename, cmd_line));

      return PREVENT_SWITCH;
    }

    case Arch::fcntl:
    case Arch::fcntl64:
      switch ((int)t->regs().arg2_signed()) {
        case Arch::DUPFD:
        case Arch::GETFD:
        case Arch::GETFL:
        case Arch::SETFL:
        case Arch::SETFD:
        case Arch::SETLK:
        case Arch::SETLK64:
        case Arch::SETOWN:
        case Arch::SETOWN_EX:
        case Arch::SETSIG:
          break;

        case Arch::GETLK:
          syscall_state.reg_parameter<typename Arch::flock>(3, IN_OUT);
          break;

        case Arch::GETLK64:
          // flock and flock64 better be different on 32-bit architectures, but
          // on 64-bit architectures, it's OK if they're the same.
          static_assert(
              sizeof(typename Arch::flock) < sizeof(typename Arch::flock64) ||
                  Arch::elfclass == ELFCLASS64,
              "struct flock64 not declared differently from struct flock");
          syscall_state.reg_parameter<typename Arch::flock64>(3, IN_OUT);
          break;

        case Arch::GETOWN_EX:
          syscall_state.reg_parameter<typename Arch::f_owner_ex>(3);
          break;

        case Arch::SETLKW:
        case Arch::SETLKW64:
          // SETLKW blocks, but doesn't write any
          // outparam data to the |struct flock|
          // argument, so no need for scratch.
          return syscall_state.done_preparing(ALLOW_SWITCH);

        default:
          // Unknown command should trigger EINVAL.
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return syscall_state.done_preparing(PREVENT_SWITCH);

    /* int futex(int *uaddr, int op, int val, const struct timespec *timeout,
     *           int *uaddr2, int val3);
     * futex parameters are in-out but they can't be moved to scratch
     * addresses. */
    case Arch::futex:
      switch ((int)t->regs().arg2_signed() & FUTEX_CMD_MASK) {
        case FUTEX_WAIT:
        case FUTEX_WAIT_BITSET:
          syscall_state.reg_parameter<int>(1, IN_OUT_NO_SCRATCH);
          return syscall_state.done_preparing(ALLOW_SWITCH);

        case FUTEX_CMP_REQUEUE:
        case FUTEX_WAKE_OP:
          syscall_state.reg_parameter<int>(1, IN_OUT_NO_SCRATCH);
          syscall_state.reg_parameter<int>(5, IN_OUT_NO_SCRATCH);
          break;

        case FUTEX_WAKE:
          syscall_state.reg_parameter<int>(1, IN_OUT_NO_SCRATCH);
          break;

        default:
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return syscall_state.done_preparing(PREVENT_SWITCH);

    case Arch::ipc:
      switch (t->regs().arg1_signed()) {
        case MSGCTL: {
          int cmd = (int)t->regs().arg3_signed() & ~IPC_64;
          return prepare_msgctl<Arch>(t, syscall_state, cmd, 5);
        }

        case MSGGET:
          break;

        case MSGSND:
          return syscall_state.done_preparing(ALLOW_SWITCH);

        case MSGRCV: {
          size_t msgsize = t->regs().arg3();
          auto kluge_args =
              syscall_state.reg_parameter<typename Arch::ipc_kludge_args>(5,
                                                                          IN);
          syscall_state.mem_ptr_parameter(REMOTE_PTR_FIELD(kluge_args, msgbuf),
                                          sizeof(typename Arch::signed_long) +
                                              msgsize);
          return syscall_state.done_preparing(ALLOW_SWITCH);
        }

        default:
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return syscall_state.done_preparing(PREVENT_SWITCH);

    case Arch::msgctl:
      return prepare_msgctl<Arch>(t, syscall_state,
                                  (int)t->regs().arg2_signed(), 3);

    case Arch::msgrcv: {
      size_t msgsize = t->regs().arg3();
      syscall_state.reg_parameter(2,
                                  sizeof(typename Arch::signed_long) + msgsize);
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }

    case Arch::msgsnd:
      return syscall_state.done_preparing(ALLOW_SWITCH);

    case Arch::socketcall:
      return prepare_socketcall<Arch>(t, syscall_state);

    case Arch::select:
    case Arch::_newselect:
      if (syscallno == Arch::select &&
          Arch::select_semantics == Arch::SelectStructArguments) {
        auto argsp =
            syscall_state.reg_parameter<typename Arch::select_args>(1, IN);
        syscall_state.mem_ptr_parameter_inferred(
            REMOTE_PTR_FIELD(argsp, read_fds), IN_OUT);
        syscall_state.mem_ptr_parameter_inferred(
            REMOTE_PTR_FIELD(argsp, write_fds), IN_OUT);
        syscall_state.mem_ptr_parameter_inferred(
            REMOTE_PTR_FIELD(argsp, except_fds), IN_OUT);
        syscall_state.mem_ptr_parameter_inferred(
            REMOTE_PTR_FIELD(argsp, timeout), IN_OUT);
      } else {
        syscall_state.reg_parameter<typename Arch::fd_set>(2, IN_OUT);
        syscall_state.reg_parameter<typename Arch::fd_set>(3, IN_OUT);
        syscall_state.reg_parameter<typename Arch::fd_set>(4, IN_OUT);
        syscall_state.reg_parameter<typename Arch::timeval>(5, IN_OUT);
      }
      return syscall_state.done_preparing(ALLOW_SWITCH);

    case Arch::recvfrom: {
      syscall_state.reg_parameter(
          2, ParamSize::from_syscall_result<typename Arch::size_t>(
                 t->regs().arg3()));
      auto addrlen_ptr =
          syscall_state.reg_parameter<typename Arch::socklen_t>(6, IN_OUT);
      syscall_state.reg_parameter(
          5, ParamSize::from_initialized_mem(t, addrlen_ptr));
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }

    case Arch::recvmsg: {
      auto msgp = syscall_state.reg_parameter<typename Arch::msghdr>(2, IN_OUT);
      prepare_recvmsg<Arch>(
          t, syscall_state, msgp,
          ParamSize::from_syscall_result<typename Arch::ssize_t>());
      if (!((int)t->regs().arg3() & MSG_DONTWAIT)) {
        return syscall_state.done_preparing(ALLOW_SWITCH);
      }
      return syscall_state.done_preparing(PREVENT_SWITCH);
    }

    case Arch::recvmmsg: {
      auto vlen = (unsigned int)t->regs().arg3();
      auto mmsgp =
          syscall_state.reg_parameter(2, sizeof(typename Arch::mmsghdr) * vlen,
                                      IN_OUT).cast<typename Arch::mmsghdr>();
      prepare_recvmmsg<Arch>(t, syscall_state, mmsgp, vlen);
      if (!((unsigned int)t->regs().arg4() & MSG_DONTWAIT)) {
        return syscall_state.done_preparing(ALLOW_SWITCH);
      }
      return syscall_state.done_preparing(PREVENT_SWITCH);
    }

    case Arch::sendmsg:
      if (!((unsigned int)t->regs().arg4() & MSG_DONTWAIT)) {
        return syscall_state.done_preparing(ALLOW_SWITCH);
      }
      return syscall_state.done_preparing(PREVENT_SWITCH);

    case Arch::sendmmsg: {
      auto vlen = (unsigned int)t->regs().arg3();
      syscall_state.reg_parameter(2, sizeof(typename Arch::mmsghdr) * vlen,
                                  IN_OUT);
      if (!((unsigned int)t->regs().arg4() & MSG_DONTWAIT)) {
        return syscall_state.done_preparing(ALLOW_SWITCH);
      }
      return syscall_state.done_preparing(PREVENT_SWITCH);
    }

    case Arch::getsockname:
    case Arch::getpeername: {
      auto addrlen_ptr =
          syscall_state.reg_parameter<typename Arch::socklen_t>(3, IN_OUT);
      syscall_state.reg_parameter(
          2, ParamSize::from_initialized_mem(t, addrlen_ptr));
      return syscall_state.done_preparing(PREVENT_SWITCH);
    }

    case Arch::getsockopt: {
      auto optlen_ptr =
          syscall_state.reg_parameter<typename Arch::socklen_t>(5, IN_OUT);
      syscall_state.reg_parameter(
          4, ParamSize::from_initialized_mem(t, optlen_ptr));
      return syscall_state.done_preparing(PREVENT_SWITCH);
    }

    case Arch::pread64:
    /* ssize_t read(int fd, void *buf, size_t count); */
    case Arch::read:
      syscall_state.reg_parameter(
          2, ParamSize::from_syscall_result<typename Arch::size_t>(
                 (size_t)t->regs().arg3()));
      return syscall_state.done_preparing(ALLOW_SWITCH);

    case Arch::accept:
    case Arch::accept4: {
      auto addrlen_ptr =
          syscall_state.reg_parameter<typename Arch::socklen_t>(3, IN_OUT);
      syscall_state.reg_parameter(
          2, ParamSize::from_initialized_mem(t, addrlen_ptr));
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }

    case Arch::getcwd: {
      syscall_state.reg_parameter(
          1, ParamSize::from_syscall_result<typename Arch::ssize_t>(
                 (size_t)t->regs().arg2()));
      return syscall_state.done_preparing(PREVENT_SWITCH);
    }

    case Arch::getdents:
    case Arch::getdents64: {
      syscall_state.reg_parameter(2, ParamSize::from_syscall_result<int>(
                                         (unsigned int)t->regs().arg3()));
      return syscall_state.done_preparing(PREVENT_SWITCH);
    }

    case Arch::readlink: {
      syscall_state.reg_parameter(
          2, ParamSize::from_syscall_result<typename Arch::ssize_t>(
                 (size_t)t->regs().arg3()));
      return syscall_state.done_preparing(PREVENT_SWITCH);
    }

    case Arch::write:
    case Arch::writev: {
      int fd = (int)t->regs().arg1_signed();
      maybe_mark_stdio_write(t, fd);
      // Tracee writes to rr's stdout/stderr are echoed during replay.
      // We want to ensure that these writes are replayed in the same
      // order as they were performed during recording. If we treat
      // those writes as interruptible, we can get into a difficult
      // situation: we start the system call, it gets interrupted,
      // we switch to another thread that starts its own write, and
      // at that point we don't know which order the kernel will
      // actually perform the writes in.
      // We work around this problem by making writes to rr's
      // stdout/stderr non-interruptible. This theoretically
      // introduces the possibility of deadlock between rr's
      // tracee and some external program reading rr's output
      // via a pipe ... but that seems unlikely to bite in practice.
      return is_stdio_fd<Arch>(t, fd) ? PREVENT_SWITCH : ALLOW_SWITCH;
      // Note that the determination of whether fd maps to rr's
      // stdout/stderr is exact, using kcmp, whereas our decision
      // to echo is currently based on the simple heuristic of
      // whether fd is STDOUT_FILENO/STDERR_FILENO (which can be
      // wrong due to those fds being dup'ed, redirected, etc).
      // We could use kcmp for the echo decision too, except
      // when writes are buffered by syscallbuf it gets rather
      // complex. A better solution is probably for the replayer
      // to track metadata for each tracee fd, tracking whether the
      // fd points to rr's stdout/stderr.
    }

    /* ssize_t readv(int fd, const struct iovec *iov, int iovcnt); */
    case Arch::readv:
    /* ssize_t preadv(int fd, const struct iovec *iov, int iovcnt,
                      off_t offset); */
    case Arch::preadv: {
      int iovcnt = (int)t->regs().arg3_signed();
      remote_ptr<void> iovecsp_void = syscall_state.reg_parameter(
          2, sizeof(typename Arch::iovec) * iovcnt, IN);
      auto iovecsp = iovecsp_void.cast<typename Arch::iovec>();
      auto iovecs = t->read_mem(iovecsp, iovcnt);
      ParamSize io_size =
          ParamSize::from_syscall_result<typename Arch::size_t>();
      for (size_t i = 0; i < iovcnt; ++i) {
        syscall_state.mem_ptr_parameter(REMOTE_PTR_FIELD(iovecsp + i, iov_base),
                                        io_size.limit_size(iovecs[i].iov_len));
      }
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }

    /* pid_t waitpid(pid_t pid, int *status, int options); */
    /* pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage);
     */
    case Arch::waitpid:
    case Arch::wait4:
      syscall_state.reg_parameter<int>(2);
      if (syscallno == Arch::wait4) {
        syscall_state.reg_parameter<typename Arch::rusage>(4);
      }
      return syscall_state.done_preparing(ALLOW_SWITCH);

    case Arch::waitid:
      syscall_state.reg_parameter<typename Arch::siginfo_t>(3);
      return syscall_state.done_preparing(ALLOW_SWITCH);

    case Arch::pause:
      return syscall_state.done_preparing(ALLOW_SWITCH);

    /* int poll(struct pollfd *fds, nfds_t nfds, int timeout) */
    /* int ppoll(struct pollfd *fds, nfds_t nfds,
     *           const struct timespec *timeout_ts,
     *           const sigset_t *sigmask); */
    case Arch::poll:
    case Arch::ppoll: {
      auto nfds = (nfds_t)t->regs().arg2();
      syscall_state.reg_parameter(1, sizeof(typename Arch::pollfd) * nfds,
                                  IN_OUT);
      return syscall_state.done_preparing(ALLOW_SWITCH);
    }

    /* int prctl(int option, unsigned long arg2, unsigned long arg3, unsigned
     * long arg4, unsigned long arg5); */
    case Arch::prctl:
      switch ((int)t->regs().arg1_signed()) {
        case PR_GET_ENDIAN:
        case PR_GET_FPEMU:
        case PR_GET_FPEXC:
        case PR_GET_PDEATHSIG:
        case PR_GET_TSC:
        case PR_GET_UNALIGN:
          syscall_state.reg_parameter<int>(2);
          break;

        case PR_GET_NAME:
          syscall_state.reg_parameter(2, 16);
          break;

        case PR_SET_NAME:
          t->update_prname(t->regs().arg2());
          break;

        case PR_SET_SECCOMP:
          break;

        default:
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return syscall_state.done_preparing(PREVENT_SWITCH);

    case Arch::ioctl:
      prepare_ioctl<Arch>(t, syscall_state);
      return syscall_state.done_preparing(PREVENT_SWITCH);

    case Arch::_sysctl: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::__sysctl_args>(1, IN);
      auto oldlenp = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, oldlenp), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, oldval),
          ParamSize::from_initialized_mem(t, oldlenp));
      return syscall_state.done_preparing(PREVENT_SWITCH);
    }

    case Arch::quotactl:
      switch ((int)t->regs().arg1_signed() & SUBCMDMASK) {
        case Q_GETQUOTA:
          syscall_state.reg_parameter<typename Arch::dqblk>(4);
          break;
        case Q_GETINFO:
          syscall_state.reg_parameter<typename Arch::dqinfo>(4);
          break;
        case Q_GETFMT:
          syscall_state.reg_parameter<int>(4);
          break;
        case Q_SETQUOTA:
          FATAL() << "Trying to set disk quota usage, this may interfere with "
                     "rr recording";
        // not reached
        case Q_QUOTAON:
        case Q_QUOTAOFF:
        case Q_SETINFO:
        case Q_SYNC:
          break;
        default:
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return syscall_state.done_preparing(PREVENT_SWITCH);

    /* int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int
     * timeout); */
    case Arch::epoll_wait:
      syscall_state.reg_parameter(2, sizeof(typename Arch::epoll_event) *
                                         t->regs().arg3_signed());
      return syscall_state.done_preparing(ALLOW_SWITCH);

    /* The following two syscalls enable context switching not for
     * liveness/correctness reasons, but rather because if we
     * didn't context-switch away, rr might end up busy-waiting
     * needlessly.  In addition, albeit far less likely, the
     * client program may have carefully optimized its own context
     * switching and we should take the hint. */

    /* int nanosleep(const struct timespec *req, struct timespec *rem); */
    case Arch::nanosleep:
      syscall_state.reg_parameter<typename Arch::timespec>(2);
      return syscall_state.done_preparing(ALLOW_SWITCH);

    case Arch::sched_yield:
      // Force |t| to be context-switched if another thread
      // of equal or higher priority is available.  We set
      // the counter to INT_MAX / 2 because various other
      // irrelevant events intervening between now and
      // scheduling may increment t's event counter, and we
      // don't want it to overflow.
      t->succ_event_counter = numeric_limits<int>::max() / 2;
      // We're just pretending that t is blocked.  The next
      // time its scheduling slot opens up, it's OK to
      // blocking-waitpid on t to see its status change.
      t->pseudo_blocked = true;
      t->record_session().scheduler().schedule_one_round_robin(t);
      return ALLOW_SWITCH;

    case Arch::rt_sigpending:
      syscall_state.reg_parameter(1, (size_t)t->regs().arg2());
      return syscall_state.done_preparing(PREVENT_SWITCH);

    case Arch::rt_sigtimedwait:
      syscall_state.reg_parameter<typename Arch::siginfo_t>(2);
      return syscall_state.done_preparing(ALLOW_SWITCH);

    case Arch::rt_sigsuspend:
    case Arch::sigsuspend:
      t->sigsuspend_blocked_sigs = unique_ptr<sig_set_t>(
          new sig_set_t(t->read_mem(remote_ptr<sig_set_t>(t->regs().arg1()))));
      return syscall_state.done_preparing(ALLOW_SWITCH);

    case Arch::getxattr:
    case Arch::lgetxattr:
    case Arch::fgetxattr:
      syscall_state.reg_parameter(
          3, ParamSize::from_syscall_result<size_t>(t->regs().arg4()));
      return syscall_state.done_preparing(PREVENT_SWITCH);

    case Arch::sched_setaffinity: {
      syscall_state.syscall_entry_registers =
          unique_ptr<Registers>(new Registers(t->regs()));
      // Ignore all sched_setaffinity syscalls. They might interfere
      // with our own affinity settings.
      Registers r = t->regs();
      // Set arg1 to an invalid PID to ensure this syscall is ignored.
      r.set_arg1(-1);
      t->set_regs(r);
      return PREVENT_SWITCH;
    }

    default:
      return PREVENT_SWITCH;
  }
}

Switchable rec_prepare_syscall(Task* t) {
  RR_ARCH_FUNCTION(rec_prepare_syscall_arch, t->arch(), t)
}

template <typename Arch> static void rec_prepare_restart_syscall_arch(Task* t) {
  int syscallno = t->ev().Syscall().number;
  auto& syscall_state = *syscall_state_property.get(*t);
  switch (syscallno) {
    case Arch::nanosleep: {
      /* Hopefully uniquely among syscalls, nanosleep()
       * requires writing to its remaining-time outparam
       * *only if* the syscall fails with -EINTR.  When a
       * nanosleep() is interrupted by a signal, we don't
       * know a priori whether it's going to be eventually
       * restarted or not.  (Not easily, anyway.)  So we
       * don't know whether it will eventually return -EINTR
       * and would need the outparam written.  To resolve
       * that, we do what the kernel does, and update the
       * outparam at the -ERESTART_RESTART interruption
       * regardless. */
      syscall_state.process_syscall_results();
      break;
    }
  }

  syscall_state_property.remove(*t);
}

void rec_prepare_restart_syscall(Task* t) {
  RR_ARCH_FUNCTION(rec_prepare_restart_syscall_arch, t->arch(), t)
}

template <typename Arch> static void init_scratch_memory(Task* t) {
  const int scratch_size = 512 * page_size();
  size_t sz = scratch_size;
  // The PROT_EXEC looks scary, and it is, but it's to prevent
  // this region from being coalesced with another anonymous
  // segment mapped just after this one.  If we named this
  // segment, we could remove this hack.
  int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  {
    /* initialize the scratchpad for blocking system calls */
    AutoRemoteSyscalls remote(t);

    t->scratch_ptr =
        remote.mmap_syscall(remote_ptr<void>(), sz, prot, flags, -1, 0);
    t->scratch_size = scratch_size;
  }
  // record this mmap for the replay
  Registers r = t->regs();
  uintptr_t saved_result = r.syscall_result();
  r.set_syscall_result(t->scratch_ptr);
  t->set_regs(r);

  char filename[PATH_MAX];
  sprintf(filename, "scratch for thread %d", t->tid);
  struct stat stat;
  memset(&stat, 0, sizeof(stat));
  TraceMappedRegion file(string(filename), stat, t->scratch_ptr,
                         t->scratch_ptr + scratch_size);
  auto record_in_trace =
      t->trace_writer().write_mapped_region(file, prot, flags);
  ASSERT(t, record_in_trace == TraceWriter::DONT_RECORD_IN_TRACE);

  r.set_syscall_result(saved_result);
  t->set_regs(r);

  t->vm()->map(t->scratch_ptr, sz, prot, flags, 0,
               MappableResource::scratch(t->rec_tid));
}

size_t ParamSize::eval(Task* t, size_t already_consumed) const {
  size_t s = max_size;
  if (!mem_ptr.is_null()) {
    size_t mem_size;
    switch (read_size) {
      case 4:
        mem_size = t->read_mem(mem_ptr.cast<uint32_t>());
        break;
      case 8:
        mem_size = t->read_mem(mem_ptr.cast<uint64_t>());
        break;
      default:
        ASSERT(t, false) << "Unknown read_size";
        return 0;
    }
    ASSERT(t, already_consumed <= mem_size);
    s = min(s, mem_size - already_consumed);
  }
  if (from_syscall) {
    size_t syscall_size = t->regs().syscall_result();
    switch (read_size) {
      case 4:
        syscall_size = uint32_t(syscall_size);
        break;
      case 8:
        syscall_size = uint64_t(syscall_size);
        break;
      default:
        ASSERT(t, false) << "Unknown read_size";
        return 0;
    }
    ASSERT(t, already_consumed <= syscall_size);
    s = min(s, syscall_size - already_consumed);
  }
  ASSERT(t, s < size_t(-1));
  return s;
}

size_t TaskSyscallState::eval_param_size(size_t i,
                                         vector<size_t>& actual_sizes) {
  assert(actual_sizes.size() == i);

  size_t already_consumed = 0;
  for (size_t j = 0; j < i; ++j) {
    if (param_list[j].num_bytes.is_same_source(param_list[i].num_bytes)) {
      already_consumed += actual_sizes[j];
    }
  }
  size_t size = param_list[i].num_bytes.eval(t, already_consumed);
  actual_sizes.push_back(size);
  return size;
}

void TaskSyscallState::process_syscall_results(WriteBack write_back) {
  ASSERT(t, preparation_done);

  // XXX what's the best way to handle failed syscalls? Currently we just
  // record everything as if it succeeded. That handles failed syscalls that
  // wrote partial results, but doesn't handle syscalls that failed with
  // EFAULT.
  vector<size_t> actual_sizes;
  if (scratch_enabled) {
    size_t scratch_num_bytes = scratch - t->scratch_ptr;
    auto data = t->read_mem(t->scratch_ptr.cast<uint8_t>(), scratch_num_bytes);
    Registers r = t->regs();
    // Step 1: compute actual sizes of all buffers and copy outputs
    // from scratch back to their origin
    for (size_t i = 0; i < param_list.size(); ++i) {
      auto& param = param_list[i];
      size_t size = eval_param_size(i, actual_sizes);
      if (write_back == WRITE_BACK &&
          (param.mode == IN_OUT || param.mode == OUT)) {
        const uint8_t* d = data.data() + (param.scratch - t->scratch_ptr);
        t->write_bytes_helper(param.dest, size, d);
      }
    }
    bool memory_cleaned_up = false;
    // Step 2: restore modified in-memory pointers and registers
    for (size_t i = 0; i < param_list.size(); ++i) {
      auto& param = param_list[i];
      if (param.ptr_in_reg) {
        r.set_arg(param.ptr_in_reg, param.dest.as_int());
      }
      if (!param.ptr_in_memory.is_null()) {
        memory_cleaned_up = true;
        set_remote_ptr(t, param.ptr_in_memory, param.dest);
      }
    }
    if (write_back == WRITE_BACK) {
      // Step 3: record all output memory areas
      for (size_t i = 0; i < param_list.size(); ++i) {
        auto& param = param_list[i];
        size_t size = actual_sizes[i];
        if (param.mode == IN_OUT_NO_SCRATCH) {
          t->record_remote(param.dest, size);
        } else if (param.mode == IN_OUT || param.mode == OUT) {
          // If pointers in memory were fixed up in step 2, then record
          // from tracee memory to ensure we record such fixes. Otherwise we
          // can record from our local data.
          // XXX This optimization can be improved if necessary...
          if (memory_cleaned_up) {
            t->record_remote(param.dest, size);
          } else {
            const uint8_t* d = data.data() + (param.scratch - t->scratch_ptr);
            t->record_local(param.dest, size, d);
          }
        }
      }
    }
    t->set_regs(r);
  } else {
    for (size_t i = 0; i < param_list.size(); ++i) {
      auto& param = param_list[i];
      size_t size = eval_param_size(i, actual_sizes);
      t->record_remote(param.dest, size);
    }
  }

  if (record_page_below_stack_ptr) {
    /* Record.the page above the top of |t|'s stack.  The SIOC* ioctls
     * have been observed to write beyond the end of tracees' stacks, as
     * if they had allocated scratch space for themselves.  All we can do
     * for now is try to record the scratch data.
     */
    t->record_remote(t->regs().sp() - page_size(), page_size());
  }
}

// We have |keys_length| instead of using array_length(keys) to work
// around a gcc bug.
template <typename Arch> struct elf_auxv_ordering {
  static const unsigned int keys[];
  static const size_t keys_length;
};

template <>
const unsigned int elf_auxv_ordering<X86Arch>::keys[] = {
  AT_SYSINFO, AT_SYSINFO_EHDR, AT_HWCAP, AT_PAGESZ, AT_CLKTCK, AT_PHDR,
  AT_PHENT,   AT_PHNUM,        AT_BASE,  AT_FLAGS,  AT_ENTRY,  AT_UID,
  AT_EUID,    AT_GID,          AT_EGID,  AT_SECURE
};
template <>
const size_t elf_auxv_ordering<X86Arch>::keys_length = array_length(keys);

template <>
const unsigned int elf_auxv_ordering<X64Arch>::keys[] = {
  AT_SYSINFO_EHDR, AT_HWCAP, AT_PAGESZ, AT_CLKTCK, AT_PHDR,
  AT_PHENT,        AT_PHNUM, AT_BASE,   AT_FLAGS,  AT_ENTRY,
  AT_UID,          AT_EUID,  AT_GID,    AT_EGID,   AT_SECURE,
};
template <>
const size_t elf_auxv_ordering<X64Arch>::keys_length = array_length(keys);

template <typename Arch>
static void process_execve(Task* t, TaskSyscallState& syscall_state) {
  Registers r = t->regs();
  if (r.syscall_failed()) {
    if (r.arg1() != syscall_state.syscall_entry_registers->arg1()) {
      LOG(warn)
          << "Blocked attempt to execve 64-bit image (not yet supported by rr)";
      // Restore arg1, which we clobbered.
      r.set_arg1(syscall_state.syscall_entry_registers->arg1());
      t->set_regs(r);
    }
    return;
  }

  // XXX what does this signifiy?
  if (r.arg1() != 0) {
    return;
  }

  t->record_session().trace_writer().write_task_event(
      *syscall_state.exec_saved_event);

  t->post_exec_syscall();

  remote_ptr<typename Arch::unsigned_word> stack_ptr = t->regs().sp();

  /* start_stack points to argc - iterate over argv pointers */

  /* FIXME: there are special cases, like when recording gcc,
   *        where the stack pointer does not point to argc. For example,
   *        it may point to &argc.
   */
  // long* argc = (long*)t->read_word((uint8_t*)stack_ptr);
  // stack_ptr += *argc + 1;
  auto argc = t->read_mem(stack_ptr);
  stack_ptr += argc + 1;

  // unsigned long* null_ptr = read_child_data(t, sizeof(void*), stack_ptr);
  // assert(*null_ptr == 0);
  auto null_ptr = t->read_mem(stack_ptr);
  assert(null_ptr == 0);
  stack_ptr++;

  /* should now point to envp (pointer to environment strings) */
  while (0 != t->read_mem(stack_ptr)) {
    stack_ptr++;
  }
  stack_ptr++;
  /* should now point to ELF Auxiliary Table */

  struct ElfEntry {
    typename Arch::unsigned_word key;
    typename Arch::unsigned_word value;
  };
  union {
    ElfEntry entries[elf_auxv_ordering<Arch>::keys_length];
    uint8_t bytes[sizeof(entries)];
  } table;
  t->read_bytes(stack_ptr, table.bytes);
  stack_ptr += 2 * array_length(elf_auxv_ordering<Arch>::keys);

  for (size_t i = 0; i < array_length(elf_auxv_ordering<Arch>::keys); ++i) {
    auto expected_field = elf_auxv_ordering<Arch>::keys[i];
    const ElfEntry& entry = table.entries[i];
    ASSERT(t, expected_field == entry.key)
        << "Elf aux entry " << i << " should be " << HEX(expected_field)
        << ", but is " << HEX(entry.key);
  }

  auto at_random = t->read_mem(stack_ptr);
  stack_ptr++;
  ASSERT(t, AT_RANDOM == at_random) << "ELF item should be " << HEX(AT_RANDOM)
                                    << ", but is " << HEX(at_random);

  remote_ptr<void> rand_addr = t->read_mem(stack_ptr);
  // XXX where does the magic number come from?
  t->record_remote(rand_addr, 16);

  init_scratch_memory<Arch>(t);
}

static void process_mmap(Task* t, int syscallno, size_t length, int prot,
                         int flags, int fd, off_t offset_pages) {
  size_t size = ceil_page_size(length);
  off64_t offset = offset_pages * 4096;

  if (t->regs().syscall_failed()) {
    // We purely emulate failed mmaps.
    return;
  }
  remote_ptr<void> addr = t->regs().syscall_result();
  if (flags & MAP_ANONYMOUS) {
    // Anonymous mappings are by definition not
    // backed by any file-like object, and are
    // initialized to zero, so there's no
    // nondeterminism to record.
    // assert(!(flags & MAP_UNINITIALIZED));
    t->vm()->map(addr, size, prot, flags, 0, MappableResource::anonymous());
    return;
  }

  ASSERT(t, fd >= 0) << "Valid fd required for file mapping";
  assert(!(flags & MAP_GROWSDOWN));

  // TODO: save a reflink copy of the resource to the
  // trace directory as |fs/[st_dev].[st_inode]|.  Then
  // we wouldn't have to care about looking up a name
  // for the resource.
  auto result = t->fstat(fd);
  TraceMappedRegion file(result.file_name, result.st, addr, addr + size,
                         offset_pages);
  if (t->trace_writer().write_mapped_region(file, prot, flags) ==
      TraceWriter::RECORD_IN_TRACE) {
    off64_t end = (off64_t)result.st.st_size - offset;
    t->record_remote(addr, min(end, (off64_t)size));
  }

  if ((prot & PROT_WRITE) && (flags & MAP_SHARED)) {
    LOG(debug) << result.file_name
               << " is SHARED|WRITEABLE; that's not handled "
                  "correctly yet. Optimistically hoping it's not "
                  "written by programs outside the rr tracee "
                  "tree.";
  }

  t->vm()->map(addr, size, prot, flags, offset,
               MappableResource(FileId(result.st), result.file_name));
}

template <typename Arch>
static void before_syscall_exit(Task* t, int syscallno) {
  t->maybe_update_vm(syscallno, SYSCALL_EXIT);

  switch (syscallno) {
    case Arch::setpriority: {
      // The syscall might have failed due to insufficient
      // permissions (e.g. while trying to decrease the nice value
      // while not root).
      // We'll choose to honor the new value anyway since we'd like
      // to be able to test configurations where a child thread
      // has a lower nice value than its parent, which requires
      // lowering the child's nice value.
      if ((int)t->regs().arg1_signed() == PRIO_PROCESS) {
        Task* target =
            (int)t->regs().arg2_signed()
                ? t->session().find_task((int)t->regs().arg2_signed())
                : t;
        if (target) {
          LOG(debug) << "Setting nice value for tid " << t->tid << " to "
                     << t->regs().arg3();
          target->record_session().scheduler().update_task_priority(
              target, (int)t->regs().arg3_signed());
        }
      }
      return;
    }
    case Arch::set_robust_list:
      t->set_robust_list(t->regs().arg1(), (size_t)t->regs().arg2());
      return;

    case Arch::set_thread_area:
      t->set_thread_area(t->regs().arg1());
      return;

    case Arch::set_tid_address:
      t->set_tid_addr(t->regs().arg1());
      return;

    case Arch::sigaction:
    case Arch::rt_sigaction:
      // TODO: SYS_signal
      t->update_sigaction(t->regs());
      return;

    case Arch::sigprocmask:
    case Arch::rt_sigprocmask:
      t->update_sigmask(t->regs());
      return;
  }
}

static void check_syscall_rejected(Task* t) {
  // Invalid syscalls return -ENOSYS. Assume any such
  // result means the syscall was completely ignored by the
  // kernel so it's OK for us to not do anything special.
  // Other results mean we probably need to understand this
  // syscall, but we don't.
  if (t->regs().syscall_result_signed() != -ENOSYS) {
    t->regs().print_register_file(stderr);
    int syscallno = t->ev().Syscall().number;
    ASSERT(t, false) << "Unhandled syscall " << t->syscall_name(syscallno)
                     << "(" << syscallno << ") returned "
                     << t->regs().syscall_result_signed();
  }
}

template <typename Arch> static void rec_process_syscall_arch(Task* t) {
  int syscallno = t->ev().Syscall().number;

  LOG(debug) << t->tid << ": processing: " << t->ev()
             << " -- time: " << t->trace_time();

  auto& syscall_state = *syscall_state_property.get(*t);

  before_syscall_exit<Arch>(t, syscallno);

  if (const struct syscallbuf_record* rec = t->desched_rec()) {
    t->record_local(t->syscallbuf_child +
                        (rec->extra_data - (uint8_t*)t->syscallbuf_hdr),
                    rec->size - sizeof(*rec), (uint8_t*)rec->extra_data);
    syscall_state_property.remove(*t);
    return;
  }

  if (syscallno < 0) {
    check_syscall_rejected(t);
    syscall_state_property.remove(*t);
    return;
  }

  if (syscall_state.expect_errno) {
    ASSERT(t, t->regs().syscall_result_signed() == -syscall_state.expect_errno)
        << "Expected " << errno_name(syscall_state.expect_errno) << " for '"
        << t->syscall_name(syscallno) << "' but got result "
        << t->regs().syscall_result_signed();
    syscall_state_property.remove(*t);
    return;
  }

  switch (syscallno) {

// All the regular syscalls are handled here.
#include "SyscallRecordCase.generated"

    case Arch::clone: {
      long new_tid = t->regs().syscall_result_signed();
      Task* new_task = t->session().find_task(new_tid);
      uintptr_t flags = syscall_state.syscall_entry_registers->arg1();

      if (flags & CLONE_UNTRACED) {
        Registers r = t->regs();
        r.set_arg1(flags);
        t->set_regs(r);
      }

      if (new_tid < 0)
        break;

      new_task->push_event(SyscallEvent(syscallno, t->arch()));

      /* record child id here */
      remote_ptr<void>* stack_not_needed = nullptr;
      remote_ptr<typename Arch::pid_t> parent_tid_in_parent,
          parent_tid_in_child;
      remote_ptr<void> tls_in_parent, tls_in_child;
      remote_ptr<typename Arch::pid_t> child_tid_in_parent, child_tid_in_child;
      extract_clone_parameters(t, stack_not_needed, &parent_tid_in_parent,
                               &tls_in_parent, &child_tid_in_parent);
      extract_clone_parameters(new_task, stack_not_needed, &parent_tid_in_child,
                               &tls_in_child, &child_tid_in_child);
      t->record_remote_even_if_null(parent_tid_in_parent);

      if (Arch::clone_tls_type == Arch::UserDescPointer) {
        t->record_remote_even_if_null(
            tls_in_parent.cast<typename Arch::user_desc>());
        new_task->record_remote_even_if_null(
            tls_in_child.cast<typename Arch::user_desc>());
      } else {
        assert(Arch::clone_tls_type == Arch::PthreadStructurePointer);
      }
      new_task->record_remote_even_if_null(parent_tid_in_child);
      new_task->record_remote_even_if_null(child_tid_in_child);

      new_task->pop_syscall();

      t->record_session().trace_writer().write_task_event(
          TraceTaskEvent(new_tid, t->tid, flags));

      init_scratch_memory<Arch>(new_task);
      // The new tracee just "finished" a clone that was
      // started by its parent.  It has no pending events,
      // so it can be context-switched out.
      new_task->switchable = ALLOW_SWITCH;

      break;
    }
    case Arch::execve:
      process_execve<Arch>(t, syscall_state);
      break;

    case Arch::mmap:
      switch (Arch::mmap_semantics) {
        case Arch::StructArguments: {
          auto args = t->read_mem(
              remote_ptr<typename Arch::mmap_args>(t->regs().arg1()));
          process_mmap(t, syscallno, args.len, args.prot, args.flags, args.fd,
                       args.offset / 4096);
          break;
        }
        case Arch::RegisterArguments:
          process_mmap(t, syscallno, (size_t)t->regs().arg2(),
                       (int)t->regs().arg3_signed(),
                       (int)t->regs().arg4_signed(),
                       (int)t->regs().arg5_signed(),
                       ((off_t)t->regs().arg6_signed()) / 4096);
          break;
      }
      break;
    case Arch::mmap2:
      process_mmap(t, syscallno, (size_t)t->regs().arg2(),
                   (int)t->regs().arg3_signed(), (int)t->regs().arg4_signed(),
                   (int)t->regs().arg5_signed(),
                   (off_t)t->regs().arg6_signed());
      break;

    case Arch::nanosleep: {
      /* If the sleep completes, the kernel doesn't
       * write back to the remaining-time
       * argument. */
      syscall_state.process_syscall_results(
          0 != (int)t->regs().syscall_result_signed()
              ? TaskSyscallState::WRITE_BACK
              : TaskSyscallState::NO_WRITE_BACK);
      break;
    }
    case Arch::open: {
      string pathname = t->read_c_str(remote_ptr<void>(t->regs().arg1()));
      if (is_blacklisted_filename(pathname.c_str())) {
        /* NB: the file will still be open in the
         * process's file table, but let's hope this
         * gross hack dies before we have to worry
         * about that. */
        LOG(warn) << "Cowardly refusing to open " << pathname;
        Registers r = t->regs();
        r.set_syscall_result(-ENOENT);
        t->set_regs(r);
      }
      break;
    }
    case Arch::_newselect:
    case Arch::_sysctl:
    case Arch::accept:
    case Arch::accept4:
    case Arch::epoll_wait:
    case Arch::fcntl:
    case Arch::fcntl64:
    case Arch::fgetxattr:
    case Arch::futex:
    case Arch::getcwd:
    case Arch::getdents:
    case Arch::getdents64:
    case Arch::getsockname:
    case Arch::getsockopt:
    case Arch::getpeername:
    case Arch::getxattr:
    case Arch::ioctl:
    case Arch::ipc:
    case Arch::lgetxattr:
    case Arch::msgctl:
    case Arch::msgrcv:
    case Arch::poll:
    case Arch::ppoll:
    case Arch::prctl:
    case Arch::pread64:
    case Arch::preadv:
    case Arch::quotactl:
    case Arch::read:
    case Arch::readv:
    case Arch::recvfrom:
    case Arch::recvmsg:
    case Arch::recvmmsg:
    case Arch::rt_sigpending:
    case Arch::rt_sigtimedwait:
    case Arch::select:
    case Arch::sendfile:
    case Arch::sendfile64:
    case Arch::sendmmsg:
    case Arch::socketcall:
    case Arch::splice:
    case Arch::waitid:
    case Arch::waitpid:
    case Arch::wait4:
      syscall_state.process_syscall_results();
      break;

    case Arch::write:
    case Arch::writev:
      break;

    case Arch::rt_sigsuspend:
    case Arch::sigsuspend:
      t->sigsuspend_blocked_sigs = nullptr;
      break;

    case Arch::sched_setaffinity: {
      // Restore the register that we altered.
      Registers r = t->regs();
      r.set_arg1(syscall_state.syscall_entry_registers->arg1());
      // Pretend the syscall succeeded.
      r.set_syscall_result(0);
      t->set_regs(r);
      break;
    }

    case SYS_rrcall_init_buffers:
      t->init_buffers(nullptr, SHARE_DESCHED_EVENT_FD);
      break;

    case SYS_rrcall_init_preload: {
      t->vm()->at_preload_init(t);

      Registers r = t->regs();
      r.set_syscall_result(0);
      t->set_regs(r);
      break;
    }

    default:
      check_syscall_rejected(t);
      break;
  }

  syscall_state_property.remove(*t);
}

void rec_process_syscall(Task* t) {
  RR_ARCH_FUNCTION(rec_process_syscall_arch, t->arch(), t)
}
