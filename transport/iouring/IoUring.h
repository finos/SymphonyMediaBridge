#pragma once
#include <cstdint>
#include <linux/io_uring.h>
// https://manpages.ubuntu.com/manpages/kinetic/en/man2/io_uring_setup.2.html

namespace iouring
{

int setup(unsigned entries, struct io_uring_params* p);
int enter(int ring_fd, unsigned int to_submit, unsigned int min_complete, unsigned int flags);

int registerFileDescriptor(unsigned int fd, unsigned int opcode, void* arg, unsigned int nr_args);

} // namespace iouring
