//===--- DynamicCasts.cpp - Utilities for dynamic casts -------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Types.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"

#include "swift/SIL/DynamicCasts.h"

using namespace swift;
using namespace Lowering;

static DynamicCastFeasibility weakenSuccess(DynamicCastFeasibility v) {
  if (v == DynamicCastFeasibility::WillSucceed)
    return DynamicCastFeasibility::MaySucceed;
  return v;
}

static unsigned getAnyMetatypeDepth(CanType type) {
  unsigned depth = 0;
  while (auto metatype = dyn_cast<AnyMetatypeType>(type))
    type = metatype.getInstanceType();
  return depth;
}

/// Try to classify the dynamic-cast relationship between two types.
DynamicCastFeasibility
swift::classifyDynamicCast(Module *M, CanType source, CanType target) {
  if (source == target) return DynamicCastFeasibility::WillSucceed;

  auto sourceObject = source.getAnyOptionalObjectType();
  auto targetObject = target.getAnyOptionalObjectType();

  // A common level of optionality doesn't affect the feasibility.
  if (sourceObject && targetObject) {
    return classifyDynamicCast(M, sourceObject, targetObject);

  // Nor does casting to a more optional type.
  } else if (targetObject) {
    return classifyDynamicCast(M, source, targetObject);

  // Casting to a less-optional type can always fail.
  } else if (sourceObject) {
    return weakenSuccess(classifyDynamicCast(M, sourceObject, target));
  }
  assert(!sourceObject && !targetObject);

  // Assume that casts to or from existential types or involving
  // dependent types can always succeed.  This is over-conservative.
  if (source->hasArchetype() || source.isExistentialType() ||
      target->hasArchetype() || target.isExistentialType())
    return DynamicCastFeasibility::MaySucceed;

  // Metatype casts.
  while (auto sourceMetatype = dyn_cast<AnyMetatypeType>(source)) {
    auto targetMetatype = dyn_cast<AnyMetatypeType>(target);
    if (!targetMetatype) return DynamicCastFeasibility::WillFail;

    source = sourceMetatype.getInstanceType();
    target = targetMetatype.getInstanceType();

    // TODO: prove that some conversions to existential metatype will
    // obviously succeed/fail.
    // TODO: prove that some conversions from class existential metatype
    // to a concrete non-class metatype will obviously fail.
    if (isa<ExistentialMetatypeType>(sourceMetatype) ||
        isa<ExistentialMetatypeType>(targetMetatype))
      return (getAnyMetatypeDepth(source) == getAnyMetatypeDepth(target)
              ? DynamicCastFeasibility::MaySucceed
              : DynamicCastFeasibility::WillFail);
  }

  // Class casts.
  auto sourceClass = source.getClassOrBoundGenericClass();
  auto targetClass = target.getClassOrBoundGenericClass();
  if (sourceClass && targetClass) {
    if (target->isSuperclassOf(source, nullptr))
      return DynamicCastFeasibility::WillSucceed;
    if (source->isSuperclassOf(target, nullptr))
      return DynamicCastFeasibility::MaySucceed;

    // FIXME: bridged types, e.g. CF <-> NS (but not for metatypes).
    return DynamicCastFeasibility::WillFail;
  }

  // FIXME: tuple conversions?

  // FIXME: bridged types, e.g. NSString <-> String (but not for metatypes).
  return DynamicCastFeasibility::WillFail;
}

static unsigned getOptionalDepth(CanType type) {
  unsigned depth = 0;
  while (CanType objectType = type.getAnyOptionalObjectType()) {
    depth++;
    type = objectType;
  }
  return depth;
}

namespace {
  struct Source {
    SILValue Value;
    CanType FormalType;
    CastConsumptionKind Consumption;

    bool isAddress() const { return Value.getType().isAddress(); }
    IsTake_t shouldTake() const {
      return IsTake_t(Consumption != CastConsumptionKind::CopyOnSuccess);
    }

    Source() = default;
    Source(SILValue value, CanType formalType, CastConsumptionKind consumption)
      : Value(value), FormalType(formalType), Consumption(consumption) {}
  };

  struct Target {
    SILValue Address;
    SILType LoweredType;
    CanType FormalType;

    bool isAddress() const { return (bool) Address; }

    Source asAddressSource() const {
      assert(isAddress());
      return { Address, FormalType, CastConsumptionKind::TakeAlways };
    }
    Source asScalarSource(SILValue value) const {
      assert(!isAddress());
      assert(!value.getType().isAddress());
      return { value, FormalType, CastConsumptionKind::TakeAlways };
    }

    Target() = default;
    Target(SILValue address, CanType formalType)
      : Address(address), LoweredType(address.getType()),
        FormalType(formalType) {
      assert(LoweredType.isAddress());
    }
    Target(SILType loweredType, CanType formalType)
      : Address(), LoweredType(loweredType), FormalType(formalType) {
      assert(!loweredType.isAddress());
    }
  };

  class CastEmitter {
    SILBuilder &B;
    SILModule &M;
    ASTContext &Ctx;
    SILLocation Loc;
  public:
    CastEmitter(SILBuilder &B, Module *swiftModule, SILLocation loc)
      : B(B), M(B.getModule()), Ctx(M.getASTContext()), Loc(loc) {}

    Source emitTopLevel(Source source, Target target) {
      unsigned sourceOptDepth = getOptionalDepth(source.FormalType);
      unsigned targetOptDepth = getOptionalDepth(target.FormalType);      

      assert(sourceOptDepth <= targetOptDepth);
      return emitAndInjectIntoOptionals(source, target,
                                        targetOptDepth - sourceOptDepth);
    }

  private:
    const TypeLowering &getTypeLowering(SILType type) {
      return M.Types.getTypeLowering(type);
    }

    SILValue getOwnedScalar(Source source, const TypeLowering &srcTL) {
      assert(!source.isAddress());
      if (!source.shouldTake())
        srcTL.emitRetainValue(B, Loc, source.Value);
      return source.Value;
    }

    Source putOwnedScalar(SILValue scalar, Target target) {
      assert(scalar.getType() == target.LoweredType.getObjectType());
      if (!target.isAddress())
        return target.asScalarSource(scalar);

      auto &targetTL = getTypeLowering(target.LoweredType);
      targetTL.emitStoreOfCopy(B, Loc, scalar, target.Address,
                               IsInitialization);
      return target.asAddressSource();
    }

    Source emitSameType(Source source, Target target) {
      assert(source.FormalType == target.FormalType);

      auto &srcTL = getTypeLowering(source.Value.getType());

      // The destination always wants a +1 value, so make the source
      // +1 if it's a scalar.
      if (!source.isAddress()) {
        source.Value = getOwnedScalar(source, srcTL);
        source.Consumption = CastConsumptionKind::TakeAlways;
      }

      // If we've got a scalar and want a scalar, the source is
      // exactly right.
      if (!target.isAddress() && !source.isAddress())
        return source;

      // If the destination wants a non-address value, load
      if (!target.isAddress()) {
        SILValue value = srcTL.emitLoadOfCopy(B, Loc, source.Value,
                                              source.shouldTake());
        return target.asScalarSource(value);
      }

      if (source.isAddress()) {
        srcTL.emitCopyInto(B, Loc, source.Value, target.Address,
                           source.shouldTake(), IsInitialization);
      } else {
        srcTL.emitStoreOfCopy(B, Loc, source.Value, target.Address,
                              IsInitialization);
      }
      return target.asAddressSource();
    }

    Source emit(Source source, Target target) {
      if (source.FormalType == target.FormalType)
        return emitSameType(source, target);

      // Handle subtype conversions involving optionals.
      OptionalTypeKind sourceOptKind;
      if (auto sourceObjectType =
            source.FormalType.getAnyOptionalObjectType(sourceOptKind)) {
        return emitOptionalToOptional(source, sourceOptKind, sourceObjectType,
                                      target);
      }
      assert(!target.FormalType.getAnyOptionalObjectType());

      // The only other thing we return WillSucceed for currently is
      // an upcast.
      auto &srcTL = getTypeLowering(source.Value.getType());
      SILValue value;
      if (source.isAddress()) {
        value = srcTL.emitLoadOfCopy(B, Loc, source.Value, source.shouldTake());
      } else {
        value = getOwnedScalar(source, srcTL);
      }
      value = B.createUpcast(Loc, value, target.LoweredType.getObjectType());
      return putOwnedScalar(value, target);
    }

    Source emitAndInjectIntoOptionals(Source source, Target target,
                                      unsigned depth) {
      if (depth == 0)
        return emit(source, target);

      // Recurse.
      EmitSomeState state;
      Target objectTarget = prepareForEmitSome(target, state);
      Source objectSource =
        emitAndInjectIntoOptionals(source, objectTarget, depth - 1);
      return emitSome(objectSource, target, state);
    }

    Source emitOptionalToOptional(Source source,
                                  OptionalTypeKind sourceOptKind,
                                  CanType sourceObjectType,
                                  Target target) {
      // Switch on the incoming value.
      SILBasicBlock *contBB = B.splitBlockForFallthrough();
      SILBasicBlock *noneBB = B.splitBlockForFallthrough();
      SILBasicBlock *someBB = B.splitBlockForFallthrough();

      // Emit the switch.
      std::pair<EnumElementDecl*, SILBasicBlock*> cases[] = {
        { Ctx.getOptionalSomeDecl(sourceOptKind), someBB },
        { Ctx.getOptionalNoneDecl(sourceOptKind), noneBB },
      };
      if (source.isAddress()) {
        B.createSwitchEnumAddr(Loc, source.Value, /*default*/ nullptr, cases);
      } else {
        B.createSwitchEnum(Loc, source.Value, /*default*/ nullptr, cases);
      }

      // Create the Some block, which recurses.
      B.setInsertionPoint(someBB);
      {
        auto sourceSomeDecl = Ctx.getOptionalSomeDecl(sourceOptKind);

        SILType loweredSourceObjectType =
          source.Value.getType().getEnumElementType(sourceSomeDecl, M);

        // Form the target for the optional object.
        EmitSomeState state;
        Target objectTarget = prepareForEmitSome(target, state);

        // Form the source value.
        AllocStackInst *sourceTemp = nullptr;
        Source objectSource;
        if (source.isAddress()) {
          // TODO: add an instruction for non-destructively getting a
          // specific element's data.
          SILValue sourceAddr = source.Value;
          if (!source.shouldTake()) {
            sourceTemp = B.createAllocStack(Loc,
                                       sourceAddr.getType().getObjectType());
            sourceAddr = sourceTemp->getAddressResult();
            B.createCopyAddr(Loc, source.Value, sourceAddr, IsNotTake,
                             IsInitialization);
          }
          sourceAddr = B.createUncheckedTakeEnumDataAddr(Loc, sourceAddr,
                                    sourceSomeDecl, loweredSourceObjectType);
          objectSource = Source(sourceAddr, sourceObjectType,
                                CastConsumptionKind::TakeAlways);
        } else {
          SILValue sourceObjectValue =
            new (M) SILArgument(loweredSourceObjectType, someBB);
          objectSource = Source(sourceObjectValue, sourceObjectType,
                                source.Consumption);
        }

        Source resultObject = emit(objectSource, objectTarget);

        // Deallocate the source temporary if we needed one.
        if (sourceTemp) {
          B.createDeallocStack(Loc, sourceTemp->getContainerResult());
        }

        Source result = emitSome(resultObject, target, state);
        assert(result.isAddress() == target.isAddress());
        if (target.isAddress()) {
          B.createBranch(Loc, contBB);
        } else {
          B.createBranch(Loc, contBB, { result.Value });
        }
      }

      // Create the None block.
      B.setInsertionPoint(noneBB);
      {
        Source result = emitNone(target);
        assert(result.isAddress() == target.isAddress());
        if (target.isAddress()) {
          B.createBranch(Loc, contBB);
        } else {
          B.createBranch(Loc, contBB, { result.Value });
        }
      }

      // Continuation block.
      B.setInsertionPoint(contBB);
      if (target.isAddress()) {
        return target.asAddressSource();
      } else {
        SILValue result = new (M) SILArgument(target.LoweredType, contBB);
        return target.asScalarSource(result);
      }
    }

    struct EmitSomeState {
      EnumElementDecl *SomeDecl;
    };

    Target prepareForEmitSome(Target target, EmitSomeState &state) {
      OptionalTypeKind optKind;
      auto objectType = target.FormalType.getAnyOptionalObjectType(optKind);
      assert(objectType && "emitting Some into non-optional type");

      auto someDecl = Ctx.getOptionalSomeDecl(optKind);
      state.SomeDecl = someDecl;

      SILType loweredObjectType =
        target.LoweredType.getEnumElementType(someDecl, M);

      if (target.isAddress()) {
        SILValue objectAddr =
          B.createInitEnumDataAddr(Loc, target.Address, someDecl,
                                   loweredObjectType);
        return { objectAddr, objectType };
      } else {
        return { loweredObjectType, objectType };
      }
    }

    Source emitSome(Source source, Target target, EmitSomeState &state) {
      // If our target is an address, prepareForEmitSome should have set this
      // up so that we emitted directly into 
      if (target.isAddress()) {
        B.createInjectEnumAddr(Loc, target.Address, state.SomeDecl);
        return target.asAddressSource();
      } else {
        auto &srcTL = getTypeLowering(source.Value.getType());
        auto sourceObject = getOwnedScalar(source, srcTL);
        auto source = B.createEnum(Loc, sourceObject, state.SomeDecl,
                                   target.LoweredType);
        return target.asScalarSource(source);
      }
    }

    Source emitNone(Target target) {
      OptionalTypeKind optKind;
      auto objectType = target.FormalType.getAnyOptionalObjectType(optKind);
      assert(objectType && "emitting None into non-optional type");
      (void) objectType;

      auto noneDecl = Ctx.getOptionalNoneDecl(optKind);
      
      if (target.isAddress()) {
        B.createInjectEnumAddr(Loc, target.Address, noneDecl);
        return target.asAddressSource();
      } else {
        SILValue res = B.createEnum(Loc, nullptr, noneDecl, target.LoweredType);
        return target.asScalarSource(res);
      }
    }
  };
}

/// Emit an unconditional scalar cast that's known to succeed.
SILValue
swift::emitSuccessfulScalarUnconditionalCast(SILBuilder &B, Module *M,
                                             SILLocation loc, SILValue value,
                                             SILType loweredTargetType,
                                             CanType sourceType,
                                             CanType targetType) {
  assert(classifyDynamicCast(M, sourceType, targetType)
           == DynamicCastFeasibility::WillSucceed);

  // Fast path changes that don't change the type.
  if (sourceType == targetType)
    return value;

  Source source(value, sourceType, CastConsumptionKind::TakeAlways);
  Target target(loweredTargetType, targetType);
  Source result = CastEmitter(B, M, loc).emitTopLevel(source, target);
  assert(!result.isAddress());
  assert(result.Value.getType() == loweredTargetType);
  assert(result.Consumption == CastConsumptionKind::TakeAlways);
  return result.Value;
}

void swift::emitSuccessfulIndirectUnconditionalCast(SILBuilder &B, Module *M,
                                                    SILLocation loc,
                                               CastConsumptionKind consumption,
                                                    SILValue src,
                                                    CanType sourceType,
                                                    SILValue dest,
                                                    CanType targetType) {
  assert(classifyDynamicCast(M, sourceType, targetType)
           == DynamicCastFeasibility::WillSucceed);

  assert(src.getType().isAddress());
  assert(dest.getType().isAddress());

  Source source(src, sourceType, consumption);
  Target target(dest, targetType);
  Source result = CastEmitter(B, M, loc).emitTopLevel(source, target);
  assert(result.isAddress());
  assert(result.Value == dest);
  assert(result.Consumption == CastConsumptionKind::TakeAlways);
  (void) result;
}
