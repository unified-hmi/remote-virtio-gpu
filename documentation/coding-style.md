# Remote VirtIO GPU Device Coding Style

[Linux kernel coding style](https://www.kernel.org/doc/html/v5.18/process/coding-style.html) is used for RVGPU development. The basic idea is following:

* Linux kernel coding style is a mature and fully validated coding style.
* RVGPU in fact provides a low-level system function on Linux and also includes
  a Linux kernel module. It has a good fit for Linux kernel coding style.
* Linux kernel provides the coding style definition for clang-format which is
  an excellent tool to format C code.

We have included the .clang-format file from the Linux kernel v5.18 source tree
to the root of RVGPU source. So you can use clang-format tools to format the
source code and the patches to be committed.

## Using clang-format

In this section, a simple usage of clang-format is described. You can install
clang-format as following in Ubuntu:

```
	sudo apt install clang-format
```

which will install following two tools:

* clang-format
* git-clang-format

clang-format can be used to formatting the specified source code file as follow:

```
	clang-format src/rvgpu-renderer/rvgpu-renderer.c
```

Formatted rvgpu-renderer.c will be outputted to the standard output. Note that
because clang-format will use the coding style defined in the .clang-format
file of the current directory, please make sure clang-format is executed from
the root of RVGPU source tree.

If a "-i" option is used, clang-format will modify the source file directly
instead of print out to standard output.

```
	clang-format -i src/rvgpu-renderer/rvgpu-renderer.c
```

When using together with git, you can only format the modified part instead of
the whole source code file by using git-clang-format. This does great help to
create a clean commit.

git-clang-format can be used as follow:

    # Modify source code
    vim src/rvgpu-renderer/rvgpu-renderer.c

    # Stage the modification
    git add src/rvgpu-renderer/rvgpu-renderer.c

    # Format the modification
    git clang-format

If git-clang-format modified the source code,
src/rvgpu-renderer/rvgpu-renderer.c will turn to be "modified" state again.
Just confirm and git-add it again.

It is strongly recommended to use git-clang-format check the modifications you
made before committing them.
