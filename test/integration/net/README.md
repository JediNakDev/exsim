# Network tests

These integration tests use loopback sockets and temporary ports. They cover the admin socket, TCP connections, partial writes, bounded buffers, stale readiness tokens, UDP, multicast, and protocol runtime behavior.

```sh
make -j4 integration
make -j4 integration MODE=debug
```

macOS uses kqueue. Linux uses epoll. Allocation checks count C++ heap allocations after setup. Sanitizer builds skip allocation counting and still check memory safety.

The integration target also runs the Python process tests in `exsim_exchanged_test.py`. See [test commands](../../README.md) for filtering and build options.
