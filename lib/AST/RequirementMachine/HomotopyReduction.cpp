//===--- HomotopyReduction.cpp - Higher-dimensional term rewriting --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the algorithm for computing a minimal set of rules from
// a confluent rewrite system. A minimal set of rules is:
//
// 1) Large enough that computing the confluent completion produces the original
//    rewrite system;
//
// 2) Small enough that no further rules can be deleted without changing the
//    resulting confluent rewrite system.
//
// Redundant rules that are not part of the minimal set are redundant are
// detected by analyzing the set of rewrite loops computed by the completion
// procedure.
//
// If a rewrite rule appears exactly once in a loop and without context, the
// loop witnesses a redundancy; the rewrite rule is equivalent to traveling
// around the loop "in the other direction". This rewrite rule and the
// corresponding rewrite loop can be deleted.
//
// Any occurrence of the rule in the remaining loops is replaced with the
// alternate definition obtained by splitting the loop that witnessed the
// redundancy.
//
// Iterating this process eventually produces a minimal set of rewrite rules.
//
// For a description of the general algorithm, see "A Homotopical Completion
// Procedure with Applications to Coherence of Monoids",
// https://hal.inria.fr/hal-00818253.
//
// Note that in the world of Swift, rewrite rules for introducing associated
// type symbols are marked 'permanent'; they are always re-added when a new
// rewrite system is built from a minimal generic signature, so instead of
// deleting them it is better to leave them in place in case it allows other
// rules to be deleted instead.
//
// Also, for a conformance rule (V.[P] => V) to be redundant, a stronger
// condition is needed than appearing once in a loop and without context;
// the rule must not be a _generating conformance_. The algorithm for computing
// a minimal set of generating conformances is implemented in
// GeneratingConformances.cpp.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Type.h"
#include "swift/Basic/Range.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include "RewriteSystem.h"

using namespace swift;
using namespace rewriting;

/// A rewrite rule is redundant if it appears exactly once in a loop
/// without context.
llvm::SmallVector<unsigned, 1>
RewriteLoop::findRulesAppearingOnceInEmptyContext(
    const RewriteSystem &system) const {
  // Rules appearing in empty context (possibly more than once).
  llvm::SmallDenseSet<unsigned, 2> rulesInEmptyContext;

  // The number of times each rule appears (with or without context).
  llvm::SmallDenseMap<unsigned, unsigned, 2> ruleMultiplicity;

  RewritePathEvaluator evaluator(Basepoint);

  for (auto step : Path) {
    switch (step.Kind) {
    case RewriteStep::ApplyRewriteRule: {
      if (!step.isInContext() && !evaluator.isInContext())
        rulesInEmptyContext.insert(step.RuleID);

      ++ruleMultiplicity[step.RuleID];
      break;
    }

    case RewriteStep::AdjustConcreteType:
    case RewriteStep::Shift:
    case RewriteStep::Decompose:
    case RewriteStep::ConcreteConformance:
    case RewriteStep::SuperclassConformance:
    case RewriteStep::ConcreteTypeWitness:
    case RewriteStep::SameTypeWitness:
      break;
    }

    evaluator.apply(step, system);
  }

  // Collect all rules that we saw exactly once in empty context.
  SmallVector<unsigned, 1> result;
  for (auto rule : rulesInEmptyContext) {
    auto found = ruleMultiplicity.find(rule);
    assert(found != ruleMultiplicity.end());

    if (found->second == 1)
      result.push_back(rule);
  }

  return result;
}

/// If a rewrite loop contains an explicit rule in empty context, propagate the
/// explicit bit to all other rules appearing in empty context within the same
/// loop.
///
/// When computing generating conformances we prefer to eliminate non-explicit
/// rules, as a heuristic to ensure that minimized conformance requirements
/// remain in the same protocol as originally written, in cases where they can
/// be moved between protocols.
///
/// However, conformance rules can also be written in a non-canonical way.
///
/// Most conformance requirements are non-canonical, since the original
/// requirements use unresolved types. For example, a requirement 'Self.X.Y : Q'
/// inside a protocol P will lower to a rewrite rule
///
///    [P].X.Y.[Q] => [P].X.Y
///
/// Completion will then add a new rule that looks something like this, using
/// associated type symbols:
///
///    [P:X].[P2:Y].[Q] => [P:X].[P2:Y]
///
/// Furthermore, if [P:X].[P2:Y] simplies to some other term, such as [P:Z],
/// there will be yet another rule added by completion:
///
///    [P:Z].[Q] => [P:Z]
///
/// The new rules are related to the original rule via rewrite loops where
/// both rules appear in empty context. This algorithm will propagate the
/// explicit bit from the original rule to the canonical rule.
void RewriteSystem::propagateExplicitBits() {
  for (const auto &loop : Loops) {
    SmallVector<unsigned, 1> rulesInEmptyContext =
      loop.findRulesAppearingOnceInEmptyContext(*this);

    bool sawExplicitRule = false;

    for (unsigned ruleID : rulesInEmptyContext) {
      const auto &rule = getRule(ruleID);
      if (rule.isExplicit())
        sawExplicitRule = true;
    }
    if (sawExplicitRule) {
      for (unsigned ruleID : rulesInEmptyContext) {
        auto &rule = getRule(ruleID);
        if (!rule.isPermanent() && !rule.isExplicit())
          rule.markExplicit();
      }
    }
  }
}

/// Given a rewrite rule which appears exactly once in a loop
/// without context, return a new definition for this rewrite rule.
/// The new definition is the path obtained by deleting the
/// rewrite rule from the loop.
RewritePath RewritePath::splitCycleAtRule(unsigned ruleID) const {
  // A cycle is a path from the basepoint to the basepoint.
  // Somewhere in this path, an application of \p ruleID
  // appears in an empty context.

  // First, we split the cycle into two paths:
  //
  // (1) A path from the basepoint to the rule's
  // left hand side,
  RewritePath basepointToLhs;
  // (2) And a path from the rule's right hand side
  // to the basepoint.
  RewritePath rhsToBasepoint;

  // Because the rule only appears once, we know that basepointToLhs
  // and rhsToBasepoint do not involve the rule itself.

  // If the rule is inverted, we have to invert the whole thing
  // again at the end.
  bool ruleWasInverted = false;

  bool sawRule = false;

  for (auto step : Steps) {
    switch (step.Kind) {
    case RewriteStep::ApplyRewriteRule: {
      if (step.RuleID != ruleID)
        break;

      assert(!sawRule && "Rule appears more than once?");
      assert(!step.isInContext() && "Rule appears in context?");

      ruleWasInverted = step.Inverse;
      sawRule = true;
      continue;
    }
    case RewriteStep::AdjustConcreteType:
    case RewriteStep::Shift:
    case RewriteStep::Decompose:
    case RewriteStep::ConcreteConformance:
    case RewriteStep::SuperclassConformance:
    case RewriteStep::ConcreteTypeWitness:
    case RewriteStep::SameTypeWitness:
      break;
    }

    if (sawRule)
      rhsToBasepoint.add(step);
    else
      basepointToLhs.add(step);
  }

  // Build a path from the rule's lhs to the rule's rhs via the
  // basepoint.
  RewritePath result = rhsToBasepoint;
  result.append(basepointToLhs);

  // We want a path from the lhs to the rhs, so invert it unless
  // the rewrite step was also inverted.
  if (!ruleWasInverted)
    result.invert();

  return result;
}

/// Replace every rewrite step involving the given rewrite rule with
/// either the replacement path (or its inverse, if the step was
/// inverted).
///
/// The replacement path is re-contextualized at each occurrence of a
/// rewrite step involving the given rule.
///
/// Returns true if any rewrite steps were replaced; false means the
/// rule did not appear in this path.
bool RewritePath::replaceRuleWithPath(unsigned ruleID,
                                      const RewritePath &path) {
  bool foundAny = false;

  for (const auto &step : Steps) {
    if (step.Kind == RewriteStep::ApplyRewriteRule &&
        step.RuleID == ruleID) {
      foundAny = true;
      break;
    }
  }

  if (!foundAny)
    return false;

  SmallVector<RewriteStep, 4> newSteps;

  // Keep track of Decompose/Compose pairs. Any rewrite steps in
  // between do not need to be re-contextualized, since they
  // operate on new terms that were pushed on the stack by the
  // Compose operation.
  unsigned decomposeCount = 0;

  for (const auto &step : Steps) {
    switch (step.Kind) {
    case RewriteStep::ApplyRewriteRule: {
      if (step.RuleID != ruleID) {
        newSteps.push_back(step);
        break;
      }

      auto adjustStep = [&](RewriteStep newStep) {
        bool inverse = newStep.Inverse ^ step.Inverse;

        if (newStep.Kind == RewriteStep::Decompose && inverse) {
          assert(decomposeCount > 0);
          --decomposeCount;
        }

        if (decomposeCount == 0) {
          newStep.StartOffset += step.StartOffset;
          newStep.EndOffset += step.EndOffset;
        }

        newStep.Inverse = inverse;
        newSteps.push_back(newStep);

        if (newStep.Kind == RewriteStep::Decompose && !inverse) {
          ++decomposeCount;
        }
      };

      if (step.Inverse) {
        for (auto newStep : llvm::reverse(path))
          adjustStep(newStep);
      } else {
        for (auto newStep : path)
          adjustStep(newStep);
      }

      break;
    }
    case RewriteStep::AdjustConcreteType:
    case RewriteStep::Shift:
    case RewriteStep::Decompose:
    case RewriteStep::ConcreteConformance:
    case RewriteStep::SuperclassConformance:
    case RewriteStep::ConcreteTypeWitness:
    case RewriteStep::SameTypeWitness:
      newSteps.push_back(step);
      break;
    }
  }

  std::swap(newSteps, Steps);
  return true;
}

/// Check if a rewrite rule is a candidate for deletion in this pass of the
/// minimization algorithm.
bool RewriteSystem::
isCandidateForDeletion(unsigned ruleID,
                       const llvm::DenseSet<unsigned> *redundantConformances) const {
  const auto &rule = getRule(ruleID);

  // We should not find a rule that has already been marked redundant
  // here; it should have already been replaced with a rewrite path
  // in all homotopy generators.
  assert(!rule.isRedundant());

  // Associated type introduction rules are 'permanent'. They're
  // not worth eliminating since they are re-added every time; it
  // is better to find other candidates to eliminate in the same
  // loop instead.
  if (rule.isPermanent())
    return false;

  // Other rules involving unresolved name symbols are derived from an
  // associated type introduction rule together with a conformance rule.
  // They are eliminated in the first pass.
  if (rule.getLHS().containsUnresolvedSymbols())
    return true;

  // Protocol conformance rules are eliminated via a different
  // algorithm which computes "generating conformances".
  //
  // The first pass skips protocol conformance rules.
  //
  // The second pass eliminates any protocol conformance rule which is
  // redundant according to both homotopy reduction and the generating
  // conformances algorithm.
  //
  // Later on, we verify that any conformance redundant via generating
  // conformances was also redundant via homotopy reduction. This
  // means that the set of generating conformances is always a superset
  // (or equal to) of the set of minimal protocol conformance
  // requirements that homotopy reduction alone would produce.
  if (rule.isAnyConformanceRule()) {
    if (!redundantConformances)
      return false;

    if (!redundantConformances->count(ruleID))
      return false;
  }

  return true;
}

/// Find a rule to delete by looking through all loops for rewrite rules appearing
/// once in empty context. Returns a redundant rule to delete if one was found,
/// otherwise returns None.
///
/// Minimization performs three passes over the rewrite system.
///
/// 1) First, rules that are not conformance rules are deleted, with
///    \p redundantConformances equal to nullptr.
///
/// 2) Second, generating conformances are computed.
///
/// 3) Finally, redundant conformance rules are deleted, with
/// \p redundantConformances equal to the set of conformance rules that are
///    not generating conformances.
Optional<unsigned> RewriteSystem::
findRuleToDelete(const llvm::DenseSet<unsigned> *redundantConformances,
                 RewritePath &replacementPath) {
  SmallVector<std::pair<unsigned, unsigned>, 2> redundancyCandidates;
  for (unsigned loopID : indices(Loops)) {
    auto &loop = Loops[loopID];
    if (loop.isDeleted())
      continue;

    bool foundAny = false;
    for (unsigned ruleID : loop.findRulesAppearingOnceInEmptyContext(*this)) {
      redundancyCandidates.emplace_back(loopID, ruleID);
      foundAny = true;
    }

    if (!foundAny)
      loop.markDeleted();
  }

  Optional<std::pair<unsigned, unsigned>> found;

  for (const auto &pair : redundancyCandidates) {
    unsigned ruleID = pair.second;
    if (!isCandidateForDeletion(ruleID, redundantConformances))
      continue;

    if (!found) {
      found = pair;
      continue;
    }

    const auto &rule = getRule(ruleID);
    const auto &otherRule = getRule(found->second);

    // Prefer to delete "less canonical" rules.
    if (rule.compare(otherRule, Context) > 0)
      found = pair;
  }

  if (!found)
    return None;

  unsigned loopID = found->first;
  unsigned ruleID = found->second;
  assert(replacementPath.empty());

  auto &loop = Loops[loopID];
  replacementPath = loop.Path.splitCycleAtRule(ruleID);

  loop.markDeleted();

  auto &rule = getRule(ruleID);
  rule.markRedundant();

  return ruleID;
}

/// Delete a rewrite rule that is known to be redundant, replacing all
/// occurrences of the rule in all loops with the replacement path.
void RewriteSystem::deleteRule(unsigned ruleID,
                               const RewritePath &replacementPath) {
  if (Debug.contains(DebugFlags::HomotopyReduction)) {
    const auto &rule = getRule(ruleID);
    llvm::dbgs() << "* Deleting rule ";
    rule.dump(llvm::dbgs());
    llvm::dbgs() << " (#" << ruleID << ")\n";
    llvm::dbgs() << "* Replacement path: ";
    MutableTerm mutTerm(rule.getLHS());
    replacementPath.dump(llvm::dbgs(), mutTerm, *this);
    llvm::dbgs() << "\n";
  }

  // Replace all occurrences of the rule with the replacement path and
  // normalize all loops.
  for (auto &loop : Loops) {
    if (loop.isDeleted())
      continue;

    bool changed = loop.Path.replaceRuleWithPath(ruleID, replacementPath);
    if (!changed)
      continue;

    if (Debug.contains(DebugFlags::HomotopyReduction)) {
      llvm::dbgs() << "** Updated loop: ";
      loop.dump(llvm::dbgs(), *this);
      llvm::dbgs() << "\n";
    }
  }
}

void RewriteSystem::performHomotopyReduction(
    const llvm::DenseSet<unsigned> *redundantConformances) {
  while (true) {
    RewritePath replacementPath;
    auto optRuleID = findRuleToDelete(redundantConformances,
                                      replacementPath);

    // If no redundant rules remain which can be eliminated by this pass, stop.
    if (!optRuleID)
      return;

    deleteRule(*optRuleID, replacementPath);
  }
}

/// Use the loops to delete redundant rewrite rules via a series of Tietze
/// transformations, updating and simplifying existing loops as each rule
/// is deleted.
///
/// Redundant rules are mutated to set their isRedundant() bit.
void RewriteSystem::minimizeRewriteSystem() {
  assert(Complete);
  assert(!Minimized);
  Minimized = 1;

  // Check invariants before homotopy reduction.
  verifyRewriteLoops();

  propagateExplicitBits();

  // First pass: Eliminate all redundant rules that are not conformance rules.
  performHomotopyReduction(/*redundantConformances=*/nullptr);

  // Now find a minimal set of generating conformances.
  //
  // FIXME: For now this just produces a set of redundant conformances, but
  // it should actually output the canonical generating conformance equation
  // for each non-generating conformance. We can then use information to
  // compute conformance access paths, instead of the current "brute force"
  // algorithm used for that purpose.
  llvm::DenseSet<unsigned> redundantConformances;
  computeGeneratingConformances(redundantConformances);

  // Second pass: Eliminate all redundant conformance rules.
  performHomotopyReduction(/*redundantConformances=*/&redundantConformances);

  // Check invariants after homotopy reduction.
  verifyRewriteLoops();
  verifyRedundantConformances(redundantConformances);
  verifyMinimizedRules();
}

/// In a conformance-valid rewrite system, any rule with unresolved symbols on
/// the left or right hand side should have been simplified by another rule.
bool RewriteSystem::hadError() const {
  assert(Complete);
  assert(Minimized);

  for (const auto &rule : Rules) {
    if (rule.isPermanent())
      continue;

    if (rule.isConflicting())
      return true;

    if (!rule.isRedundant() && rule.containsUnresolvedSymbols())
      return true;
  }

  return false;
}

/// Collect all non-permanent, non-redundant rules whose domain is equal to
/// one of the protocols in \p proto. In other words, the first symbol of the
/// left hand side term is either a protocol symbol or associated type symbol
/// whose protocol is in \p proto.
///
/// These rules form the requirement signatures of these protocols.
llvm::DenseMap<const ProtocolDecl *, std::vector<unsigned>>
RewriteSystem::getMinimizedProtocolRules(
    ArrayRef<const ProtocolDecl *> protos) const {
  assert(Minimized);

  llvm::DenseMap<const ProtocolDecl *, std::vector<unsigned>> rules;
  for (unsigned ruleID : indices(Rules)) {
    const auto &rule = getRule(ruleID);

    if (rule.isPermanent() ||
        rule.isRedundant() ||
        rule.isConflicting() ||
        rule.containsUnresolvedSymbols()) {
      continue;
    }

    auto domain = rule.getLHS()[0].getProtocols();
    assert(domain.size() == 1);

    const auto *proto = domain[0];
    if (std::find(protos.begin(), protos.end(), proto) != protos.end())
      rules[proto].push_back(ruleID);
  }

  return rules;
}

/// Collect all non-permanent, non-redundant rules whose left hand side
/// begins with a generic parameter symbol.
///
/// These rules form the top-level generic signature for this rewrite system.
std::vector<unsigned>
RewriteSystem::getMinimizedGenericSignatureRules() const {
  assert(Minimized);

  std::vector<unsigned> rules;
  for (unsigned ruleID : indices(Rules)) {
    const auto &rule = getRule(ruleID);

    if (rule.isPermanent() ||
        rule.isRedundant() ||
        rule.isConflicting() ||
        rule.containsUnresolvedSymbols()) {
      continue;
    }

    if (rule.getLHS()[0].getKind() != Symbol::Kind::GenericParam)
      continue;

    rules.push_back(ruleID);
  }

  return rules;
}

/// Verify that each loop begins and ends at its basepoint.
void RewriteSystem::verifyRewriteLoops() const {
#ifndef NDEBUG
  for (const auto &loop : Loops) {
    RewritePathEvaluator evaluator(loop.Basepoint);

    for (const auto &step : loop.Path) {
      evaluator.apply(step, *this);
    }

    if (evaluator.getCurrentTerm() != loop.Basepoint) {
      llvm::errs() << "Not a loop: ";
      loop.dump(llvm::errs(), *this);
      llvm::errs() << "\n";
      abort();
    }

    if (evaluator.isInContext()) {
      llvm::errs() << "Leftover terms on evaluator stack\n";
      evaluator.dump(llvm::errs());
      abort();
    }
  }
#endif
}

/// Assert if homotopy reduction failed to eliminate a redundant conformance,
/// since this suggests a misunderstanding on my part.
void RewriteSystem::verifyRedundantConformances(
    llvm::DenseSet<unsigned> redundantConformances) const {
#ifndef NDEBUG
  for (unsigned ruleID : redundantConformances) {
    const auto &rule = getRule(ruleID);
    assert(!rule.isPermanent() &&
           "Permanent rule cannot be redundant");
    assert(!rule.isIdentityConformanceRule() &&
           "Identity conformance cannot be redundant");
    assert(rule.isAnyConformanceRule() &&
           "Redundant conformance is not a conformance rule?");

    if (!rule.isRedundant()) {
      llvm::errs() << "Homotopy reduction did not eliminate redundant "
                   << "conformance?\n";
      llvm::errs() << "(#" << ruleID << ") " << rule << "\n\n";
      dump(llvm::errs());
      abort();
    }
  }
#endif
}

// Assert if homotopy reduction failed to eliminate a rewrite rule it was
// supposed to delete.
void RewriteSystem::verifyMinimizedRules() const {
#ifndef NDEBUG
  for (const auto &rule : Rules) {
    // Note that sometimes permanent rules can be simplified, but they can never
    // be redundant.
    if (rule.isPermanent()) {
      if (rule.isRedundant()) {
        llvm::errs() << "Permanent rule is redundant: " << rule << "\n\n";
        dump(llvm::errs());
        abort();
      }

      continue;
    }

    // Simplified rules should be redundant, unless they're protocol conformance
    // rules, which unfortunately might no be redundant, because we try to keep
    // them in the original protocol definition for compatibility with the
    // GenericSignatureBuilder's minimization algorithm.
    if (rule.isSimplified() &&
        !rule.isRedundant() &&
        !rule.isProtocolConformanceRule()) {
      llvm::errs() << "Simplified rule is not redundant: " << rule << "\n\n";
      dump(llvm::errs());
      abort();
    }
  }
#endif
}
