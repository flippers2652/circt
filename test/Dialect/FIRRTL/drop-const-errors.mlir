// RUN: circt-opt --pass-pipeline='builtin.module(firrtl.circuit(any(firrtl-drop-const)))' -verify-diagnostics --split-input-file %s

firrtl.circuit "CheckConstAssignInNonConstCondition" {
firrtl.module @CheckConstAssignInNonConstCondition(in %p: !firrtl.uint<1>, in %in: !firrtl.const.uint<2>, out %out: !firrtl.const.uint<2>) {
  firrtl.when %p : !firrtl.uint<1> {
    // expected-error @+1 {{assignment to 'const' type '!firrtl.const.uint<2>' is dependent on a non-'const' condition}}
    firrtl.connect %out, %in : !firrtl.const.uint<2>, !firrtl.const.uint<2>
  } else {
    // expected-error @+1 {{assignment to 'const' type '!firrtl.const.uint<2>' is dependent on a non-'const' condition}}
    firrtl.connect %out, %in : !firrtl.const.uint<2>, !firrtl.const.uint<2>
  }
}
}

// -----

firrtl.circuit "CheckNestedConstAssignInNonConstCondition" {
firrtl.module @CheckNestedConstAssignInNonConstCondition(in %constP: !firrtl.const.uint<1>, in %p: !firrtl.uint<1>, in %in: !firrtl.const.uint<2>, out %out: !firrtl.const.uint<2>) {
  firrtl.when %p : !firrtl.uint<1> {
    firrtl.when %constP : !firrtl.const.uint<1> {
      // expected-error @+1 {{assignment to 'const' type '!firrtl.const.uint<2>' is dependent on a non-'const' condition}}
      firrtl.connect %out, %in : !firrtl.const.uint<2>, !firrtl.const.uint<2>
    }
  }
}
}

// -----

firrtl.circuit "CheckBundleConstFieldAssignInNonConstCondition" {
firrtl.module @CheckBundleConstFieldAssignInNonConstCondition(in %p: !firrtl.uint<1>, in %in: !firrtl.bundle<a: const.uint<2>>, out %out: !firrtl.bundle<a: const.uint<2>>) {
  firrtl.when %p : !firrtl.uint<1> {
    // expected-error @+1 {{assignment to nested 'const' member of type '!firrtl.bundle<a: const.uint<2>>' is dependent on a non-'const' condition}}
    firrtl.connect %out, %in : !firrtl.bundle<a: const.uint<2>>, !firrtl.bundle<a: const.uint<2>>
  } else {
    // expected-error @+1 {{assignment to nested 'const' member of type '!firrtl.bundle<a: const.uint<2>>' is dependent on a non-'const' condition}}
    firrtl.connect %out, %in :!firrtl.bundle<a: const.uint<2>>, !firrtl.bundle<a: const.uint<2>>
  }
}
}
