<div align="center">
<p>
    <img width="100" src="https://raw.githubusercontent.com/flint-lang/logo/main/logo.svg">
    <h1>The Flint Interop Protocol</h1>
</p>

<p>
An approachable programming language to make power and performance accessible without bloat, in a high level package.

This repository is contains the Flint Interop Protocol implementation.

</p>

<p>
    <a href="#"><img src="https://img.shields.io/badge/c-%2300599C.svg?style=flat&logo=c%2B%2B&logoColor=white"></img></a>
    <a href="http://opensource.org/licenses/MIT"><img src="https://img.shields.io/github/license/flint-lang/fip?color=black"></img></a>
    <a href="#"><img src="https://img.shields.io/github/stars/flint-lang/fip"></img></a>
    <a href="#"><img src="https://img.shields.io/github/forks/flint-lang/fip"></img></a>
    <a href="#"><img src="https://img.shields.io/github/repo-size/flint-lang/fip"></img></a>
    <a href="https://github.com/flint-lang/flintc/graphs/contributors"><img src="https://img.shields.io/github/contributors/flint-lang/fip?color=blue"></img></a>
    <a href="https://github.com/flint-lang/fip/issues"><img src="https://img.shields.io/github/issues/flint-lang/fip"></img></a>
</p>

<p align="center">
  <a href="https://flint-lang.github.io/">Documentation</a> ·
  <a href="https://github.com/flint-lang/fip/issues">Report a Bug</a> ·
  <a href="https://github.com/flint-lang/fip/issues">Request Feature</a> ·
  <a href="https://github.com/flint-lang/fip/pulls">Send a Pull Request</a>
</p>

</div>

# Introduction

The Flint Interop Protocol (FIP) is a protocol aimed at gneralizing the communication of multiple compile modules for the Flint compiler. The Flint compiler handles all extern functions as black boxes. The FIP works like this:

1. The Flint Compiler (`flintc`) will spawn all enabled `fip` modules from the config located in `.fip/config/fip.toml`
2. The Compiler waits for all spawned Interop Modules to send a connect request to it
3. The FIP version information is checked, modules with non-matching versions are rejected
4. After the IMs connected to the compiler they will go through their source files and search for all symbols they can provide
5. The Flint Compiler (`flintc`) will come across an external function definition like `extern def foo(i32 x);` and it will broadcast a symbol resulution request to all active IMs
6. All IMs go through their symbols and check whether they provide the given symbol and send a message back to the compiler whether they provide the given symbol
7. This repeats for the whole parsing process and all external functions the compiler may come across
8. After parsing, the Flint Compiler (`flintc`) will send a compile request to all connected IMs. If the IMs provide symbols the compiler requested earlier, they will now compile their respective sources needed for the requested symbols into hashed files like `.fip/cache/AJKsdf2p.o` in the cache directory.
9. During the compilation of all IMs the Flint Compiler generates the Flint code and produces the `main.o` file used for linking
10. Before linking, the Flint Compiler sends a object request to all IMs and they return a list of 8-Byte hashes describing their compiled files.
11. The Flint Compiler then checks whether all extern code has compiled successfully, ensuring proper shutdown of the compiler when extern code is faulty
12. During the linking stage the Flint Compiler will link all externally compiled `.o` files to the `main.o` file and link all together, producing a final executable
13. The Flint Compiler (`flintc`) sends the kill message over the FIP to all the Interop Modules, telling them that they can shut down now.

The Flint compiler does not care into which language it calls, neither does it care where the `.o` files come from. This is the foundation of the FIP, because this way we can have a `fip-c` module responsible for parsing and checking C source files, being a "C expert" so to speak, and a different Interop Module, like `fip-rs` could be responsible for Rust. It is planned to provide a `fip-ft` Interop Module in the future too, to provide an interop module able to be used from other languages to call into Flint code.

# Minimal Setup

## `fip.toml`

This is the minimum setup required for FIP to work correctly. First, you need at least one config file in the `.fip/config/` directory, the `fip.toml` file. A minimal file looks like this:

```toml
[fip-c]
enable = true
```

The header, `fip-c` in this case, can be _any text_, it is simply the name of the IM binary to look for in the `PATH` variable. So, if you provide your own IM you can add the lines

```toml
[mymodule]
enable = true
```

to the `fip.toml` file and your module will pretty much be good to go, as long as it works properly with the FIP. So in this case the binary `mymodule` needs to be located and executable from your `PATH`.

## `fip-c.toml`

In addition to the `fip.toml` which is read and parsed by the Flint Compiler you also need to provide a configuration file for your Interop Module, for the `fip-c` module this configuration file must be named `fip-c.toml` and it must be located in the `.fip/config/` directory, next to the `fip.toml` file. All config files of FIP will land in this directory. The `fip-c.toml` file needs to look like this:

```toml
compiler = "clang"
sources = ["path/to/header.h", "path/to/source.c"]
compile_flags = ["-g", "-O0"]
```

The `compiler` field is just the compiler executable which will be executed. It can be `clang`, `gcc`, `filc`, `zig cc` or any other C compiler of your liking.
The `sources` field is a simple array of string values, each pointing to a source which will be parsed and scanned for definitions by the `fip-c` Interop Module
The `compile_flags` field is a simple array of string values for all compile flags you would normally call the compiler with

With these commands, the C compiler is called to produce the `.o` file(s) from the source(s). These are all configurations that are available at the moment, it will be expanded in the future. Also, these fields are **not** optional. The `fip-c` module will exit with an message of a faulty config if not all fields are provided (the arrays could be empty). For empty sources the `fip-c` Interop Module will exit too, as it does not have anything to do. Actually it idles because exiting would break the Flint Compiler as it would wait on a process which does no longer exist, so for now the IM just keeps running until the compiler is done and sends the kill message.

Different Modules can have vastly different configuration files, taylored to their specific language. The `fip-rs` module could have a `crates = []` field, for example. The module-specific configuration files are _not_ parsed by the compiler, they are _only_ parsed by their targeted Interop Module. When creating your own interop module named `mymodule` (for example) you should call the config file `mymodule.toml` too. Keep all names related.

## `fip-c`

Now let's come to the Interop Module itself. Because the `fip-c` executable depends on `libclang`, it has became quite large. Because of this the `fip-c` exectuable now needs to be installed system-wide. You just need to make sure that you put the binary into a directory present in your `PATH` variable. You can download the `fip-c` binary from the [Releases](https://github.com/flint-lang/fip/releases) page.

# Bindings

Because each Interop Module is a "master" in it's own language, you do not need to write bindings for external code at all. But, you sadly still need to declare each extern function you want to use, through an extern definition like

```rs
extern def foo(i32 x);
```

If you want to use a library with a lot of functions you will end up with a situation where you have a `raylib.ft` file just containing all definitions for the external functions. This is not optimal, and I am working on a solution to that problem, but it's design is not yet fully resolved. I was thinking about a flag of the Flint Compiler to tell it to generate "bindings" (a `.ft` file containing all extern definitions) of all extern code, but I want to limit this somehow. This problem is related to a different problem.

When you want to use the `SDL` and `raylib` library from C, for example, but you want them to be compiled using different compile flags, you simply cannot do it effectively as of now. The solution I imagine would look as follows:

```toml
[raylib]
compiler = "clang"
sources = ["/usr/include/raylib.h"]
compile_flags = ["-g", "-O1"]

[SDL]
compiler = "gcc"
sources = ["/usr/include/SDL3/SDL.h"]
compile_flags = ["-g", "-O3"]
```

Here, the whole `fip-c.toml` file can be repeated under a few different headers. If you do not add a header at all (in the above example) the header will just be called `fip-c` I think. But, if you then run the automatic bind gen from the Flint Compiler I couold very well imagine that it could produce one `.ft` file for each header. The header text could be arbitrary in this case, it is just the name of the "collection". For now the best directory to put it would probably be `.fip/bindings/raylib.ft` and `.fip/bindings/SDL.ft` but I am not sure about that yet.
I don't really like the idea that binding code you want to call is located in a hidden directory (`.fip`). For configuration, cache etc this is fine, but for files you actively want to use this feels wrong. I do not have a solution for this problem yet, but this is the direction in which FIP will evolve into. So, stay tuned for what FIP will become!

The best thing I could come up with is that a `binding_path` field in the `fip.toml` file could be added, like so:

```toml
[fip-c]
enable = true
binding_path = "src"
```

and the path provided there will always be a relative path based to the root directory (the directory the `.fip` directory is located in). So, if you then have a file structure like that:

```
ROOT/
 ├─ .fip/
 │  └─ config/
 │      ├─ fip.toml
 │      └─ fip-c.toml
 └─ src/
     └─ main.ft
```

The `fip-c.toml` file could look like above, with the `[raylib]` and `[SDL]` sections. Then, the `raylib.ft` and `SDL.ft` files are auto-generated and placed in the `src` directory... Not all edge-cases of this design have been considered yet, but I think it should be the right direction to move towards. This is only theoretical, though, and nothing about this does work yet.
