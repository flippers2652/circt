//===- Reduction.cpp - Reductions for circt-reduce ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines abstract reduction patterns for the 'circt-reduce' tool.
//
//===----------------------------------------------------------------------===//

#include "Reduction.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/InitAllDialects.h"
#include "mlir/IR/AsmState.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Reducer/Tester.h"
#include "mlir/Support/FileUtilities.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "circt-reduce"

using namespace llvm;
using namespace mlir;
using namespace circt;

//===----------------------------------------------------------------------===//
// Reduction
//===----------------------------------------------------------------------===//

Reduction::~Reduction() {}

//===----------------------------------------------------------------------===//
// Pass Reduction
//===----------------------------------------------------------------------===//

PassReduction::PassReduction(MLIRContext *context, std::unique_ptr<Pass> pass,
                             bool canIncreaseSize, bool oneShot)
    : context(context), canIncreaseSize(canIncreaseSize), oneShot(oneShot) {
  passName = pass->getArgument();
  if (passName.empty())
    passName = pass->getName();

  if (auto opName = pass->getOpName())
    pm = std::make_unique<PassManager>(context, *opName);
  else
    pm = std::make_unique<PassManager>(context);
  pm->addPass(std::move(pass));
}

bool PassReduction::match(Operation *op) const {
  return op->getName().getIdentifier() == pm->getOpName(*context);
}

LogicalResult PassReduction::rewrite(Operation *op) const {
  return pm->run(op);
}

std::string PassReduction::getName() const { return passName.str(); }

//===----------------------------------------------------------------------===//
// Concrete Sample Reductions (to later move into the dialects)
//===----------------------------------------------------------------------===//

/// A sample reduction pattern that maps `firrtl.module` to `firrtl.extmodule`.
struct ModuleExternalizer : public Reduction {
  bool match(Operation *op) const override {
    return isa<firrtl::FModuleOp>(op);
  }
  LogicalResult rewrite(Operation *op) const override {
    auto module = cast<firrtl::FModuleOp>(op);
    OpBuilder builder(module);
    builder.create<firrtl::FExtModuleOp>(
        module->getLoc(),
        module->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName()),
        module.getPorts(), StringRef(), module.annotationsAttr());
    module->erase();
    return success();
  }
  std::string getName() const override { return "module-externalizer"; }
};

/// Invalidate all the leaf fields of a value with a given flippedness by
/// connecting an invalid value to them. This is useful for ensuring that all
/// output ports of an instance or memory (including those nested in bundles)
/// are properly invalidated.
static void invalidateOutputs(ImplicitLocOpBuilder &builder, Value value,
                              SmallDenseMap<Type, Value, 8> &invalidCache,
                              bool flip = false) {
  auto type = value.getType().dyn_cast<firrtl::FIRRTLType>();
  if (!type)
    return;

  // Descend into bundles by creating subfield ops.
  if (auto bundleType = type.dyn_cast<firrtl::BundleType>()) {
    for (auto &element : llvm::enumerate(bundleType.getElements())) {
      auto subfield =
          builder.createOrFold<firrtl::SubfieldOp>(value, element.index());
      invalidateOutputs(builder, subfield, invalidCache,
                        flip ^ element.value().isFlip);
      if (subfield.use_empty())
        subfield.getDefiningOp()->erase();
    }
    return;
  }

  // Descend into vectors by creating subindex ops.
  if (auto vectorType = type.dyn_cast<firrtl::FVectorType>()) {
    for (unsigned i = 0, e = vectorType.getNumElements(); i != e; ++i) {
      auto subindex = builder.createOrFold<firrtl::SubfieldOp>(value, i);
      invalidateOutputs(builder, subindex, invalidCache, flip);
      if (subindex.use_empty())
        subindex.getDefiningOp()->erase();
    }
    return;
  }

  // Only drive outputs.
  if (flip)
    return;
  Value invalid = invalidCache.lookup(type);
  if (!invalid) {
    invalid = builder.create<firrtl::InvalidValueOp>(type);
    invalidCache.insert({type, invalid});
  }
  builder.create<firrtl::ConnectOp>(value, invalid);
}

/// Reduce all leaf fields of a value through an XOR tree.
static void reduceXor(ImplicitLocOpBuilder &builder, Value &into, Value value) {
  auto type = value.getType().dyn_cast<firrtl::FIRRTLType>();
  if (!type)
    return;
  if (auto bundleType = type.dyn_cast<firrtl::BundleType>()) {
    for (auto &element : llvm::enumerate(bundleType.getElements()))
      reduceXor(
          builder, into,
          builder.createOrFold<firrtl::SubfieldOp>(value, element.index()));
    return;
  }
  if (auto vectorType = type.dyn_cast<firrtl::FVectorType>()) {
    for (unsigned i = 0, e = vectorType.getNumElements(); i != e; ++i)
      reduceXor(builder, into,
                builder.createOrFold<firrtl::SubfieldOp>(value, i));
    return;
  }
  if (type.isa<firrtl::UIntType, firrtl::SIntType>())
    into = into ? builder.createOrFold<firrtl::XorPrimOp>(into, value) : value;
}

/// A sample reduction pattern that maps `firrtl.instance` to a set of
/// invalidated wires. This often shortcuts a long iterative process of connect
/// invalidation, module externalization, and wire stripping
struct InstanceStubber : public Reduction {
  bool match(Operation *op) const override {
    return isa<firrtl::InstanceOp>(op);
  }
  LogicalResult rewrite(Operation *op) const override {
    auto instOp = cast<firrtl::InstanceOp>(op);
    LLVM_DEBUG(llvm::dbgs() << "Stubbing instance `" << instOp.name() << "`\n");
    ImplicitLocOpBuilder builder(instOp.getLoc(), instOp);
    SmallDenseMap<Type, Value, 8> invalidCache;
    for (unsigned i = 0, e = instOp.getNumResults(); i != e; ++i) {
      auto result = instOp.getResult(i);
      auto name = builder.getStringAttr(Twine(instOp.name()) + "_" +
                                        instOp.getPortNameStr(i));
      auto wire = builder.create<firrtl::WireOp>(
          result.getType(), name, instOp.getPortAnnotation(i), StringAttr{});
      invalidateOutputs(builder, wire, invalidCache,
                        instOp.getPortDirection(i) == firrtl::Direction::In);
      result.replaceAllUsesWith(wire);
    }
    auto moduleOp = instOp.getReferencedModule();
    instOp->erase();
    if (SymbolTable::symbolKnownUseEmpty(
            moduleOp, moduleOp->getParentOfType<ModuleOp>())) {
      LLVM_DEBUG(llvm::dbgs() << "- Removing now unused module `"
                              << moduleOp.moduleName() << "`\n");
      moduleOp->erase();
    }
    return success();
  }
  std::string getName() const override { return "instance-stubber"; }
  bool acceptSizeIncrease() const override { return true; }
};

/// A sample reduction pattern that maps `firrtl.mem` to a set of invalidated
/// wires.
struct MemoryStubber : public Reduction {
  bool match(Operation *op) const override { return isa<firrtl::MemOp>(op); }
  LogicalResult rewrite(Operation *op) const override {
    auto memOp = cast<firrtl::MemOp>(op);
    LLVM_DEBUG(llvm::dbgs() << "Stubbing memory `" << memOp.name() << "`\n");
    ImplicitLocOpBuilder builder(memOp.getLoc(), memOp);
    SmallDenseMap<Type, Value, 8> invalidCache;
    Value xorInputs;
    SmallVector<Value> outputs;
    for (unsigned i = 0, e = memOp.getNumResults(); i != e; ++i) {
      auto result = memOp.getResult(i);
      auto name = builder.getStringAttr(Twine(memOp.name()) + "_" +
                                        memOp.getPortNameStr(i));
      auto wire = builder.create<firrtl::WireOp>(
          result.getType(), name, memOp.getPortAnnotation(i), StringAttr{});
      invalidateOutputs(builder, wire, invalidCache, true);
      result.replaceAllUsesWith(wire);

      // Isolate the input and output data fields of the port.
      Value input, output;
      switch (memOp.getPortKind(i)) {
      case firrtl::MemOp::PortKind::Read:
        output = builder.createOrFold<firrtl::SubfieldOp>(wire, 3);
        break;
      case firrtl::MemOp::PortKind::Write:
        input = builder.createOrFold<firrtl::SubfieldOp>(wire, 3);
        break;
      case firrtl::MemOp::PortKind::ReadWrite:
        input = builder.createOrFold<firrtl::SubfieldOp>(wire, 5);
        output = builder.createOrFold<firrtl::SubfieldOp>(wire, 3);
        break;
      }

      // Reduce all input ports to a single one through an XOR tree.
      unsigned numFields =
          wire.getType().cast<firrtl::BundleType>().getNumElements();
      for (unsigned i = 0; i != numFields; ++i) {
        if (i != 2 && i != 3 && i != 5)
          reduceXor(builder, xorInputs,
                    builder.createOrFold<firrtl::SubfieldOp>(wire, i));
      }
      if (input)
        reduceXor(builder, xorInputs, input);

      // Track the output port to hook it up to the XORd input later.
      if (output)
        outputs.push_back(output);
    }

    // Hook up the outputs.
    for (auto output : outputs)
      builder.create<firrtl::ConnectOp>(output, xorInputs);

    memOp->erase();
    return success();
  }
  std::string getName() const override { return "memory-stubber"; }
  bool acceptSizeIncrease() const override { return true; }
};

/// Starting at the given `op`, traverse through it and its operands and erase
/// operations that have no more uses.
static void pruneUnusedOps(Operation *initialOp) {
  SmallVector<Operation *> worklist;
  SmallSet<Operation *, 4> handled;
  worklist.push_back(initialOp);
  while (!worklist.empty()) {
    auto op = worklist.pop_back_val();
    if (!op->use_empty())
      continue;
    for (auto arg : op->getOperands())
      if (auto argOp = arg.getDefiningOp())
        if (handled.insert(argOp).second)
          worklist.push_back(argOp);
    op->erase();
  }
}

/// A sample reduction pattern that replaces operations with a constant zero of
/// their type.
struct Constantifier : public Reduction {
  bool match(Operation *op) const override {
    if (op->getNumResults() != 1)
      return false;
    if (isa<firrtl::WireOp, firrtl::RegOp, firrtl::RegResetOp,
            firrtl::InstanceOp, firrtl::SubfieldOp, firrtl::SubindexOp,
            firrtl::SubaccessOp>(op))
      return false;
    auto type = op->getResult(0).getType().dyn_cast<firrtl::FIRRTLType>();
    return type && type.isa<firrtl::UIntType, firrtl::SIntType>();
  }
  LogicalResult rewrite(Operation *op) const override {
    assert(match(op));
    OpBuilder builder(op);
    auto type = op->getResult(0).getType().cast<firrtl::FIRRTLType>();
    auto width = type.getBitWidthOrSentinel();
    if (width == -1)
      width = 64;
    auto newOp = builder.create<firrtl::ConstantOp>(
        op->getLoc(), type, APSInt(width, type.isa<firrtl::UIntType>()));
    op->replaceAllUsesWith(newOp);
    pruneUnusedOps(op);
    return success();
  }
  std::string getName() const override { return "constantifier"; }
};

/// A sample reduction pattern that replaces the right-hand-side of
/// `firrtl.connect` and `firrtl.partialconnect` operations with a
/// `firrtl.invalidvalue`. This removes uses from the fanin cone to these
/// connects and creates opportunities for reduction in DCE/CSE.
struct ConnectInvalidator : public Reduction {
  bool match(Operation *op) const override {
    return isa<firrtl::ConnectOp, firrtl::PartialConnectOp>(op) &&
           !op->getOperand(1).getDefiningOp<firrtl::InvalidValueOp>();
  }
  LogicalResult rewrite(Operation *op) const override {
    assert(match(op));
    auto rhs = op->getOperand(1);
    OpBuilder builder(op);
    auto invOp =
        builder.create<firrtl::InvalidValueOp>(rhs.getLoc(), rhs.getType());
    auto rhsOp = rhs.getDefiningOp();
    op->setOperand(1, invOp);
    if (rhsOp)
      pruneUnusedOps(rhsOp);
    return success();
  }
  std::string getName() const override { return "connect-invalidator"; }
};

/// A sample reduction pattern that removes operations which either produce no
/// results or their results have no users.
struct OperationPruner : public Reduction {
  bool match(Operation *op) const override {
    return !isa<ModuleOp>(op) &&
           !op->hasAttr(SymbolTable::getSymbolAttrName()) &&
           (op->getNumResults() == 0 || op->use_empty());
  }
  LogicalResult rewrite(Operation *op) const override {
    assert(match(op));
    pruneUnusedOps(op);
    return success();
  }
  std::string getName() const override { return "operation-pruner"; }
};

/// A sample reduction pattern that removes ports from the root `firrtl.module`
/// if the port is not used or just invalidated.
struct RootPortPruner : public Reduction {
  bool match(Operation *op) const override {
    auto module = dyn_cast<firrtl::FModuleOp>(op);
    if (!module)
      return false;
    auto circuit = module->getParentOfType<firrtl::CircuitOp>();
    if (!circuit)
      return false;
    return circuit.nameAttr() == module.getNameAttr();
  }
  LogicalResult rewrite(Operation *op) const override {
    assert(match(op));
    auto module = cast<firrtl::FModuleOp>(op);
    SmallVector<unsigned> dropPorts;
    for (unsigned i = 0, e = module.getNumPorts(); i != e; ++i) {
      bool onlyInvalidated =
          llvm::all_of(module.getArgument(i).getUses(), [](OpOperand &use) {
            auto *op = use.getOwner();
            if (!isa<firrtl::ConnectOp, firrtl::PartialConnectOp>(op))
              return false;
            if (use.getOperandNumber() != 0)
              return false;
            if (!op->getOperand(1).getDefiningOp<firrtl::InvalidValueOp>())
              return false;
            return true;
          });
      if (onlyInvalidated) {
        dropPorts.push_back(i);
        for (auto user : module.getArgument(i).getUsers())
          user->erase();
      }
    }
    module.erasePorts(dropPorts);
    return success();
  }
  std::string getName() const override { return "root-port-pruner"; }
};

/// A sample reduction pattern that replaces instances of `firrtl.extmodule`
/// with wires.
struct ExtmoduleInstanceRemover : public Reduction {
  bool match(Operation *op) const override {
    if (auto instOp = dyn_cast<firrtl::InstanceOp>(op))
      return isa<firrtl::FExtModuleOp>(instOp.getReferencedModule());
    return false;
  }
  LogicalResult rewrite(Operation *op) const override {
    auto instOp = cast<firrtl::InstanceOp>(op);
    auto portInfo = instOp.getReferencedModule().getPorts();
    ImplicitLocOpBuilder builder(instOp.getLoc(), instOp);
    SmallVector<Value> replacementWires;
    for (firrtl::PortInfo info : portInfo) {
      auto wire = builder.create<firrtl::WireOp>(
          info.type, (Twine(instOp.name()) + "_" + info.getName()).str());
      if (info.isOutput()) {
        auto inv = builder.create<firrtl::InvalidValueOp>(info.type);
        builder.create<firrtl::ConnectOp>(wire, inv);
      }
      replacementWires.push_back(wire);
    }
    instOp.replaceAllUsesWith(std::move(replacementWires));
    instOp->erase();
    return success();
  }
  std::string getName() const override { return "extmodule-instance-remover"; }
  bool acceptSizeIncrease() const override { return true; }
};

//===----------------------------------------------------------------------===//
// Reduction Registration
//===----------------------------------------------------------------------===//

static std::unique_ptr<Pass> createSimpleCanonicalizerPass() {
  GreedyRewriteConfig config;
  config.useTopDownTraversal = true;
  config.enableRegionSimplification = false;
  return createCanonicalizerPass(config);
}

void circt::createAllReductions(
    MLIRContext *context,
    llvm::function_ref<void(std::unique_ptr<Reduction>)> add) {
  // Gather a list of reduction patterns that we should try. Ideally these are
  // sorted by decreasing reduction potential/benefit. For example, things that
  // can knock out entire modules while being cheap should be tried first,
  // before trying to tweak operands of individual arithmetic ops.
  add(std::make_unique<PassReduction>(context, firrtl::createInlinerPass()));
  add(std::make_unique<PassReduction>(context,
                                      createSimpleCanonicalizerPass()));
  add(std::make_unique<PassReduction>(context, firrtl::createLowerCHIRRTLPass(),
                                      true, true));
  add(std::make_unique<PassReduction>(context, firrtl::createInferWidthsPass(),
                                      true, true));
  add(std::make_unique<PassReduction>(context, firrtl::createInferResetsPass(),
                                      true, true));
  add(std::make_unique<PassReduction>(
      context, firrtl::createLowerFIRRTLTypesPass(), true, true));
  add(std::make_unique<PassReduction>(context, firrtl::createExpandWhensPass(),
                                      true, true));
  add(std::make_unique<InstanceStubber>());
  add(std::make_unique<MemoryStubber>());
  add(std::make_unique<ModuleExternalizer>());
  add(std::make_unique<PassReduction>(context, createCSEPass()));
  add(std::make_unique<Constantifier>());
  add(std::make_unique<ConnectInvalidator>());
  add(std::make_unique<OperationPruner>());
  add(std::make_unique<RootPortPruner>());
  add(std::make_unique<ExtmoduleInstanceRemover>());
}
