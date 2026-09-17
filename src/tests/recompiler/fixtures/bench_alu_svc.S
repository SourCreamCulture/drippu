// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Identical-PC JIT vs hybrid AOT benchmark fixture (drippu backlog #3).
// Not assembled by the build; encodings are hard-coded in recomp_stack_harness
// and translated through arm64_to_c.h. X1 is set to 1 by the harness so ADD
// cannot be constant-folded by the AOT C compiler.
//
// Guest PC offset: 0x2400 (past the 8-byte STR at 0x1FFC).

.global bench_alu_svc
bench_alu_svc:
    movz x0, #0
    .rept 512
    add x0, x0, x1
    .endr
    svc #3
