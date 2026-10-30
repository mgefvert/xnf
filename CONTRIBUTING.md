# Contributing to XNF

Contributions of code, tests, documentation, bug reports, and thoughtful reviews
are welcome. XNF is an educational project, so clear explanations and tests are
as valuable as new features.

## Getting started

1. Read [KERNEL_OVERVIEW.md](KERNEL_OVERVIEW.md) and the guide for the subsystem
   you plan to change. The overview links to all of the technical guides.
2. For a substantial change, open an issue or start a discussion first to share
   the problem and approach. Small fixes can go straight to a pull request.
3. Set up CMake, NASM, MinGW GCC/binutils, QEMU, Python, and GDB as described in
   [KERNEL_BUILD.md](KERNEL_BUILD.md). Use the same compiler and generator for
   the normal and test builds.
4. Make your changes on a branch and open a pull request describing the problem,
   your solution, and the checks you ran. Include any known limitations or tests
   you could not run.

## Making changes

Keep changes focused and readable. Preserve the documented initialization,
ownership, ABI, and synchronization contracts, and update the relevant guide
when a contract changes. Add purpose headers to source, header, assembly, build,
and test files; explain non-obvious operations and their constraints. Keep
generated files in the build directory. See [AGENTS.md](AGENTS.md) for the
repository's detailed working rules.

## Testing

Add meaningful unit or subsystem tests for new behavior and regression tests for
bugs, including relevant boundary and failure cases. Tests should check observable
behavior and ownership, not just repeat the implementation. The existing kernel
tests live in `tests/kernel/` and run through the QEMU harnesses in `tests/`.
If a change cannot reasonably be tested automatically, explain why and describe
how you verified it in the pull request.

## Credit

Every contribution matters, including reports, documentation, tests, and review.
Use a Git commit name and email associated with your GitHub account if you want
your commits attributed there. Acknowledge collaborators in the pull request and,
for shared commits, use Git's `Co-authored-by` trailers with their preferred
names and email addresses. Please respect anyone's preference for how they are
credited.
