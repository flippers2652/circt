// REQUIRES: iverilog,cocotb

// RUN: circt-opt %s -pass-pipeline='builtin.module(hw.module(pipeline.pipeline(pipeline-explicit-regs, pipeline-ss-to-stage), lower-pipeline-to-hw{outline-stages}), lower-seq-to-sv, sv-trace-iverilog, export-verilog)' \
// RUN:     -o %t.mlir > %t.sv

// RUN: circt-cocotb-driver.py --objdir=%T --topLevel=simple \
// RUN:     --pythonModule=simple --pythonFolder="%S,%S/.." %t.sv 2>&1 | FileCheck %s

// CHECK: ** TEST
// CHECK: ** TESTS=[[N:.*]] PASS=[[N]] FAIL=0 SKIP=0

hw.module @simple(%arg0 : i32, %arg1 : i32, %go : i1, %clock : i1, %reset : i1) -> (out: i32) {
  %out = pipeline.pipeline(%arg0, %arg1, %go) clock %clock reset %reset : (i32, i32, i1) -> (i32) {
    ^bb0(%a0 : i32, %a1: i32, %g : i1):
      %add0 = comb.add %a0, %a1 : i32

      %s0_valid = pipeline.ss enable %g
      %add1 = comb.add %add0, %a0 : i32

      %s1_valid = pipeline.ss enable %s0_valid
      %add2 = comb.add %add1, %add0 : i32

      pipeline.return %add2 valid %s1_valid : i32
  }
  hw.output %out : i32
}
