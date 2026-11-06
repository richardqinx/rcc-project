This directory contains the RCC Project.

The RCC Project is a collection of systems software, from compilers to
terminal applications.

The RCC Project is free software, distributed under the GNU General Public
License, version 3. See the file COPYING for the license terms.

The RCC Project currently includes rcc, proofsearch, ricochet, raytracing,
rv64cpu, muqemu, and other projects.

rcc
    A C compiler project, including a frontend, middle end, and backend.

proofsearch
    An automated theorem prover for first-order logic using
    clause-saturation-based proof search.

ricochet
    A terminal-based web browser.

raytracing
    A ray tracer written in modern C++.

rv64cpu
    An RV64 RISC-V processor core written in SystemVerilog.

muqemu
    A simplified model of QEMU written in modern C++.

Projects for binary utilities, memory allocators, and other systems software
will be added to the repository as subprojects.

Each subproject maintains its own build entry points, build dependencies,
and runtime behavior.

For specific build instructions, see the build files and documentation in
the corresponding subproject directory.
