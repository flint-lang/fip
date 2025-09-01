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

1. The Flint Compiler (`flintc`) will spawn all enabled fip modules from the config located in `.fip/config/fip.toml`
2. The Compiler waits for all spawned Interop Modules to send a connect request to it
3. The FIP version information is checked, modules with non-matching versions are rejected
4. After the IMs connected to the compiler they will go through their source files and search for all symbols they can provide
5. The Flint Compiler (`flintc`) will come across an external function definition like `extern foo(i32 x)` and it will broadcast a symbol resulution request to all active IMs
6. All IMs go through their symbols and check whether they provide the given symbol and send a message back to the compiler whether they provide the given symbol
7. This repeats for the whole parsing process and all external functions the compiler may come across
8. After parsing, the Flint Compiler (`flintc`) will send a compile request to all connected IMs. If the IMs provide symbols the compiler requested earlier, they will now compile their respective sources needed for the requested symbols into hashed files like `.fip/cache/AJKsdf2p.o` in the cache directory.
9. During the compilation of all IMs the Flint Compiler generates the Flint code and produces the `main.o` file used for linking
10. Before linking, the Flint Compiler sends a object request to all IMs and they return a list of 8-Byte hashes describing their compiled files.
11. During linking stage the Flint Compiler will link all externally compiled `.o` files to the `main.o` file and link all together, producing a final executable
12. The Flint Compiler (`flintc`) sends the kill message over the FIP to all the Interop Modules, telling them that they can shut down now.

The Flint compiler does not care into which language it calls, neither does it care where the `.o` files come from. This is the foundation of the FIP, because this way we can have a `fip-c` module responsible for parsing and checking C source files, being a "C expert" so to speak, and a different Interop Module, like `fip-rs` could be responsible for Rust. It is planned to provide a `fip-ft` Interop Module in the future too, to provide an interop module able to be used from other languages to call into Flint functions.

# Minimal Setup

## `fip.toml`

This is the minimum setup required for FIP to work correctly. First, you need at least one config file in the `.fip/config/` directory, the `fip.toml` file. A minimal file looks like this:

```toml
[fip-c]
enable = true
```

The header, `fip-c` in this case, can be *any text*, it is simply the IM to look for. So, if you provide your own IM you can add the lines
```toml
[mymodule]
enable = true
```
to the `fip.toml` file and your module will pretty much be good to go, as long as it works properly with the FIP.

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

Different Modules can have vastly different configuration files, taylored to their specific language. The `fip-rs` module could have a `crates = []` field, for example. The module-specific configuration files are *not* parsed by the compiler, they are *only* parsed by their targetted Interop Module. When creating your own interop module named `mymodule` (for example) you should call the config file `mymodule.toml` too. Keep all names related.

## `fip-c`

Now let's come to the Interop Module itself. As of for now, the `fip-c` Interop Module is only `170 kB` in size, as it does not contain a compiler in of itself, it's just a small wrapper for the protocol and to call system commands itself. It is expected for all Interop Modules to be small in size, similar to the `fip-c` module.

The Interop Modules of FIP are stored in the `.fip/modules/` directory. So, `fip-c` needs to be stored in there too. It is the directory the Flint Compiler will try to spawn the Interop Modules found in the `fip.toml` file from. So, if you want to create your own Interop Module you need to add it to the `fip.toml` file, add a `mymodule.toml` config file and you need to place `mymodule` in the `.fip/modules/` path.

It was a deliberate design decision to require placing the IMs into the specific directory. This way it is not system-dependant, and all the fip-related configuration and modules of any project ly inside the single `.fip/` directory. 

# Module Manager

It is planned to integrate an Interop Module Manager into the `flint` executable itself (not the `flintc` executable). The `flint` executable will contain Wikis, Documentation, The IM-Manager, potentially a package manager and more, but that's a topic of the future, it does not even exist yet. So, for now you need to download the `fip-c` module or other modules directly from the releases page of this repository.

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

Here, the whole `fip-c.toml` file can be repeated under a few different headers. If you do not add a header at all (in the above example) the header will just be called `fip-c` I think. But, if you then run the automatic bind gen from the Flint Compiler I couold very well imagine that it could produce one `.ft` file for each header. The header text could be arbitrary in this case, it is just the name of the "collection". For now the best directory to put it would probably be `.fip/bindings/raylib.ft` and `.fip/bindings/SDL.ft` but i don't know yet. I don't really like the idea that binding code you want to call is located in a hidden directory (`.fip`). For configuration, cache etc this is fine, but for files you actively want to use this feels wrong. I do not have a solution for this problem yet, but this is the direction in which FIP will evolve into. So, stay tuned for what FIP will become!
