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
    <a href="#"><img src="https://img.shields.io/badge/c++-%2300599C.svg?style=flat&logo=c%2B%2B&logoColor=white"></img></a>
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

The Flint Interop Protocol (FIP) is a protocol aimed at gneralizing the communication of multiple compile modules for the Flint compiler. The Flint compiler handles all extern functions as black boxes. The FIP should work like this:

1. At startup the compiler will send a broadcast message to check which Interop Modules exist and are running and then connects to them
2. The Flint Compiler (`flintc`) will come across an extern function definition, like `extern foo(i32 x);`
3. The Flint Compiler will send a symbol resolution request over the FIP, it akss "who provides `foo(i32)->void`?
4. The Interop Module which provides the given symbol will send back "I have the symbol you are searching for!"
5. The Flint Compiler continues it's compilation without further waiting for other modules
6. The Interop Module will compile the source code to a `.o` file to be linked later with the main program
7. The Flint Compiler will send a message "I am done compiling, now give me the `.o` files you got" and the Interop Modules will send their compiled `.o` files (the paths to them) over the FIP
8. The Flint Compiler links the recieved `.o` files to the program files in the linking stage and produces the executable from all `.o` files
