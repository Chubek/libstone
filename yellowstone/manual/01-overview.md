# Chapter 1: Overview and Design Goals

qobjfile is a layered object-file toolkit. qbfd provides format-neutral reading, qld provides linking, and the command line tools expose stable workflows.

Yellowstone also includes separate libraries for DWARF inspection, native
binary inspection, WebAssembly inspection, and offline runtime analysis.
See [Analysis tools](11-tools.md#analysis-tools) for their scope and usage.

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
