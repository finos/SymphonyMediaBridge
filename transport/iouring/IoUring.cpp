#include "transport/iouring/IoUring.h"
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

namespace iouring
{

int setup(unsigned entries, struct io_uring_params* p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

// https://manpages.ubuntu.com/manpages/kinetic/en/man2/io_uring_enter.2.html
int enter(int ring_fd, unsigned int to_submit, unsigned int min_complete, unsigned int flags)
{
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
}

// https://manpages.ubuntu.com/manpages/kinetic/en/man2/io_uring_register.2.html
int registerFileDescriptor(unsigned int ring_fd, unsigned int opcode, void* arg, unsigned int nr_args)
{
    return (int)syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args, NULL, 0);
}
} // namespace iouring
