<div align="center">
<p>
    <img width="100" src="https://raw.githubusercontent.com/flint-lang/logo/main/logo.svg">
    <h1>The Flint Interop Protocol</h1>
</p>

<p>
A high level language with transparency at its core.

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

## Introduction

The Flint Interop Protocol (FIP) is a protocol aimed at generalizing the communication of multiple compile modules for the Flint compiler. The Flint compiler handles all extern functions as black boxes. The FIP works like this:

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
