# Register Allocation (RegAlloc) Methods

Register allocation is the compiler backend process of assigning virtual registers, temporaries, and program values to physical CPU registers, spilling values to memory when necessary.

The following are major register allocation algorithms, techniques, and heuristics, grouped by family.

## 1. Graph-Coloring Register Allocation

Models register interference as a graph, where vertices represent live ranges and edges represent values that cannot share a register.

1. Chaitin's graph-coloring allocation
2. Chaitin–Briggs register allocation
3. Briggs's optimistic coloring
4. George–Appel iterated register coalescing
5. Conservative graph coloring
6. Aggressive graph coloring
7. Priority-based graph coloring
8. Simplify/select graph coloring
9. Optimistic register coloring
10. Kempe-chain recoloring
11. Preference-based graph coloring
12. Hierarchical graph coloring
13. Region-based graph coloring
14. Precolored graph coloring
15. Register-pair-aware graph coloring

## 2. Linear-Scan Register Allocation

Uses live intervals to allocate registers efficiently, commonly in JIT compilers.

16. Basic linear scan
17. Poletto–Sarkar linear scan
18. Traub-style linear scan with lifetime holes
19. Extended linear scan
20. Second-chance binpacking
21. Wimmer–Franz linear scan
22. SSA-based linear scan
23. Backtracking linear scan
24. Priority-based linear scan
25. Interval-splitting linear scan
26. Active/inactive interval linear scan
27. Register-class-aware linear scan
28. Trace-based linear scan
29. Region-based linear scan
30. Hot/cold-aware linear scan

## 3. SSA-Based Register Allocation

Takes advantage of Static Single Assignment form and its interference-graph properties.

31. SSA chordal graph coloring
32. Dominance-tree register allocation
33. SSA destruction-based allocation
34. SSA-aware greedy allocation
35. SSA-based spilling
36. SSA-based live-range splitting
37. SSA interference graph coloring
38. Phi-aware register coalescing
39. SSA-based register assignment
40. Register allocation via chordal graph elimination

## 4. Greedy and Heuristic Allocation

Uses local decisions, priorities, and cost functions.

41. Greedy register allocation
42. LLVM-style greedy allocation
43. Priority-queue allocation
44. Spill-cost-based allocation
45. Use-density-based allocation
46. Frequency-weighted allocation
47. Loop-depth-weighted allocation
48. Rematerialization-aware allocation
49. Register-pressure-aware allocation
50. Eviction-based allocation
51. Eviction-chain allocation
52. Local greedy allocation
53. Global greedy allocation
54. Register-bank-aware greedy allocation
55. Affinity-driven allocation

## 5. Local and Global Allocation

These describe the scope over which registers are assigned, rather than a single algorithm.

56. Basic-block-local allocation
57. Global register allocation
58. Top-down register allocation
59. Bottom-up register allocation
60. Usage-count-based allocation
61. Frequency-count-based allocation
62. Next-use-based allocation
63. Belady-style furthest-next-use allocation
64. Loop-local allocation
65. Superblock register allocation
66. Hyperblock register allocation
67. Trace-based allocation
68. Region-based allocation

## 6. Optimal and Mathematical Allocation

Formulates allocation as an optimization problem.

69. Integer Linear Programming (ILP) allocation
70. Mixed-Integer Programming (MIP) allocation
71. Partitioned Boolean Quadratic Programming (PBQP)
72. Constraint Programming allocation
73. SAT-based register allocation
74. SMT-based register allocation
75. Branch-and-bound allocation
76. Dynamic-programming allocation
77. Tree-decomposition-based allocation
78. Minimum-cost-flow-based allocation
79. Constraint-graph-based allocation
80. Multiobjective register allocation

## 7. Register Allocation Through Other Models

81. Puzzle-solving register allocation
82. Multi-commodity-flow register allocation
83. Bipartite-matching-based allocation
84. Weighted matching allocation
85. Register-allocation-by-instruction-scheduling
86. Integrated instruction selection and allocation
87. Integrated register allocation and scheduling
88. Combinatorial optimization-based allocation
89. Genetic-algorithm-based allocation
90. Simulated-annealing-based allocation
91. Reinforcement-learning-guided allocation

## 8. Specialized Register Allocation

92. Stack-machine register allocation
93. GPU register allocation
94. SIMD register allocation
95. Vector register allocation
96. Predicate register allocation
97. Rotating-register allocation
98. Register-window allocation
99. Heterogeneous register-file allocation
100. Bank-aware register allocation
101. Aliased-register allocation
102. Subregister-aware allocation
103. Register-pair allocation
104. Register-tuple allocation
105. Irregular-register-architecture allocation
106. Calling-convention-aware allocation
107. JIT-specific register allocation
108. Trace-JIT register allocation
109. Register allocation for VLIW processors
110. Register allocation for embedded processors

## 9. Supporting Register Allocation Techniques

These aren't standalone allocators, but are essential components of sophisticated allocation systems.

| Technique                    | Purpose                                         |
| ---------------------------- | ----------------------------------------------- |
| Liveness analysis            | Determine when values are alive                 |
| Live-range splitting         | Divide long live ranges                         |
| Live-range coalescing        | Eliminate unnecessary register moves            |
| Copy coalescing              | Assign the same register to copy-related values |
| Spilling                     | Move register-resident values to memory         |
| Spill placement              | Decide where loads and stores belong            |
| Spill-code optimization      | Reduce spill overhead                           |
| Rematerialization            | Recompute values instead of reloading           |
| Register scavenging          | Obtain a temporary physical register            |
| Register eviction            | Free a register by relocating another value     |
| Register recoloring          | Change assignments to resolve conflicts         |
| Register pressure estimation | Predict simultaneous register demand            |
| Live-range shrinking         | Shorten value lifetimes                         |
| Register-class inference     | Determine which registers values can use        |
| Phi elimination              | Lower SSA phi operations into moves             |
| Parallel-copy resolution     | Implement simultaneous assignments              |
| Register packing             | Place compatible values into registers or lanes |
| Callee-save optimization     | Reduce save/restore overhead                    |
| Spill-slot coloring          | Reuse stack slots with nonoverlapping lifetimes |
| Register allocation hints    | Prefer certain physical registers               |

## Choosing an allocator

| Algorithm                 | Compilation speed | Allocation quality            | Suitable use                                |
| ------------------------- | ----------------- | ----------------------------- | ------------------------------------------- |
| Basic linear scan         | Very fast         | Moderate                      | Baseline JIT                                |
| Extended linear scan      | Fast              | Good                          | Optimizing JIT                              |
| Graph coloring            | Moderate to slow  | Good–high                     | AOT compiler                                |
| SSA chordal coloring      | Moderate          | Good–high                     | SSA-based compiler                          |
| LLVM-style greedy         | Moderate          | High                          | Optimizing compiler                         |
| PBQP                      | Slow              | Potentially high              | Irregular register constraints              |
| ILP                       | Very slow         | Optimal for modeled objective | Research and small functions                |
| Puzzle-solving            | Fast              | Architecture-dependent        | Suitable constrained register architectures |
| Local next-use allocation | Very fast         | Limited globally              | Simple code generators                      |

For a new compiler backend, I recommend implementing basic linear scan first, followed by live-range splitting, coalescing, rematerialization, and a more sophisticated greedy or SSA-based allocator.

One important distinction: graph coloring, linear scan, greedy allocation, and mathematical optimization are algorithm families, whereas spilling, coalescing, splitting, and recoloring are supporting techniques that can be combined with several allocators.