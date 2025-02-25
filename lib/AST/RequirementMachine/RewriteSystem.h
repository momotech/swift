//===--- RewriteSystem.h - Generics with term rewriting ---------*- C++ -*-===//
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

#ifndef SWIFT_REWRITESYSTEM_H
#define SWIFT_REWRITESYSTEM_H

#include "llvm/ADT/DenseSet.h"

#include "Debug.h"
#include "RewriteLoop.h"
#include "Symbol.h"
#include "Term.h"
#include "Trie.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {

namespace rewriting {

class PropertyMap;
class RewriteContext;
class RewriteSystem;

/// A rewrite rule that replaces occurrences of LHS with RHS.
///
/// LHS must be greater than RHS in the linear order over terms.
///
/// Out-of-line methods are documented in RewriteSystem.cpp.
class Rule final {
  Term LHS;
  Term RHS;

  /// A 'permanent' rule cannot be deleted by homotopy reduction. These
  /// do not correspond to generic requirements and are re-added when the
  /// rewrite system is built.
  unsigned Permanent : 1;

  /// An 'explicit' rule is a generic requirement written by the user.
  unsigned Explicit : 1;

  /// A 'simplified' rule was eliminated by simplifyRewriteSystem() if one of two
  /// things happen:
  /// - The rule's left hand side can be reduced via some other rule, in which
  ///   case completion will have filled in the missing edge if necessary.
  /// - The rule's right hand side can be reduced, in which case the reduced
  ///   rule is added when simplifying the rewrite system.
  ///
  /// Simplified rules do not participate in term rewriting, because other rules
  /// can be used to derive an equivalent rewrite path.
  unsigned Simplified : 1;

  /// A 'redundant' rule was eliminated by homotopy reduction. Redundant rules
  /// still participate in term rewriting, but they are not part of the minimal
  /// set of requirements in a generic signature.
  unsigned Redundant : 1;

  /// A 'conflicting' rule is a property rule which cannot be satisfied by any
  /// concrete type because it is mutually exclusive with some other rule.
  /// An example would be a pair of concrete type rules:
  ///
  ///    T.[concrete: Int] => T
  ///    T.[concrete: String] => T
  ///
  /// Conflicting rules are detected in property map construction, and are
  /// dropped from the minimal set of requirements.
  unsigned Conflicting : 1;

public:
  Rule(Term lhs, Term rhs)
      : LHS(lhs), RHS(rhs) {
    Permanent = false;
    Explicit = false;
    Simplified = false;
    Redundant = false;
    Conflicting = false;
  }

  const Term &getLHS() const { return LHS; }
  const Term &getRHS() const { return RHS; }

  Optional<Symbol> isPropertyRule() const;

  const ProtocolDecl *isProtocolConformanceRule() const;

  const ProtocolDecl *isAnyConformanceRule() const;

  bool isIdentityConformanceRule() const;

  bool isProtocolRefinementRule() const;

  /// See above for an explanation of these predicates.
  bool isPermanent() const {
    return Permanent;
  }

  bool isExplicit() const {
    return Explicit;
  }

  bool isSimplified() const {
    return Simplified;
  }

  bool isRedundant() const {
    return Redundant;
  }

  bool isConflicting() const {
    return Conflicting;
  }

  bool containsUnresolvedSymbols() const {
    return (LHS.containsUnresolvedSymbols() ||
            RHS.containsUnresolvedSymbols());
  }

  void markSimplified() {
    assert(!Simplified);
    Simplified = true;
  }

  void markPermanent() {
    assert(!Explicit && !Permanent &&
           "Permanent and explicit are mutually exclusive");
    Permanent = true;
  }

  void markExplicit() {
    assert(!Explicit && !Permanent &&
           "Permanent and explicit are mutually exclusive");
    Explicit = true;
  }

  void markRedundant() {
    assert(!Redundant);
    Redundant = true;
  }

  void markConflicting() {
    // It's okay to mark a rule as conflicting multiple times, but it must not
    // be a permanent rule.
    assert(!Permanent && "Permanent rule should not conflict with anything");
    Conflicting = true;
  }

  unsigned getDepth() const;

  int compare(const Rule &other, RewriteContext &ctx) const;

  void dump(llvm::raw_ostream &out) const;

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                       const Rule &rule) {
    rule.dump(out);
    return out;
  }
};

/// Result type for RewriteSystem::computeConfluentCompletion() and
/// PropertyMap::buildPropertyMap().
enum class CompletionResult {
  /// Confluent completion was computed successfully.
  Success,

  /// Maximum number of iterations reached.
  MaxIterations,

  /// Completion produced a rewrite rule whose left hand side has a length
  /// exceeding the limit.
  MaxDepth
};

/// A term rewrite system for working with types in a generic signature.
///
/// Out-of-line methods are documented in RewriteSystem.cpp.
class RewriteSystem final {
  /// Rewrite context for memory allocation.
  RewriteContext &Context;

  /// The rules added so far, including rules from our client, as well
  /// as rules introduced by the completion procedure.
  std::vector<Rule> Rules;

  /// A prefix trie of rule left hand sides to optimize lookup. The value
  /// type is an index into the Rules array defined above.
  Trie<unsigned, MatchKind::Shortest> Trie;

  DebugOptions Debug;

  /// Whether we've initialized the rewrite system with a call to initialize().
  unsigned Initialized : 1;

  /// Whether we've computed the confluent completion at least once.
  ///
  /// It might be computed multiple times if the property map's concrete type
  /// unification procedure adds new rewrite rules.
  unsigned Complete : 1;

  /// Whether we've minimized the rewrite system.
  unsigned Minimized : 1;

  /// If set, the completion procedure records rewrite loops describing the
  /// identities among rewrite rules discovered while resolving critical pairs.
  unsigned RecordLoops : 1;

public:
  explicit RewriteSystem(RewriteContext &ctx);
  ~RewriteSystem();

  RewriteSystem(const RewriteSystem &) = delete;
  RewriteSystem(RewriteSystem &&) = delete;
  RewriteSystem &operator=(const RewriteSystem &) = delete;
  RewriteSystem &operator=(RewriteSystem &&) = delete;

  /// Return the rewrite context used for allocating memory.
  RewriteContext &getRewriteContext() const { return Context; }

  DebugOptions getDebugOptions() const { return Debug; }

  void initialize(bool recordLoops,
                  std::vector<std::pair<MutableTerm, MutableTerm>> &&permanentRules,
                  std::vector<std::pair<MutableTerm, MutableTerm>> &&requirementRules);

  unsigned getRuleID(const Rule &rule) const {
    assert((unsigned)(&rule - &*Rules.begin()) < Rules.size());
    return (unsigned)(&rule - &*Rules.begin());
  }

  ArrayRef<Rule> getRules() const {
    return Rules;
  }

  Rule &getRule(unsigned ruleID) {
    return Rules[ruleID];
  }

  const Rule &getRule(unsigned ruleID) const {
    return Rules[ruleID];
  }

  bool addRule(MutableTerm lhs, MutableTerm rhs,
               const RewritePath *path=nullptr);

  bool addPermanentRule(MutableTerm lhs, MutableTerm rhs);

  bool addExplicitRule(MutableTerm lhs, MutableTerm rhs);

  bool simplify(MutableTerm &term, RewritePath *path=nullptr) const;

  bool simplifySubstitutions(Symbol &symbol, RewritePath *path=nullptr) const;

  //////////////////////////////////////////////////////////////////////////////
  ///
  /// Completion
  ///
  //////////////////////////////////////////////////////////////////////////////

  /// Pairs of rules which have already been checked for overlap.
  llvm::DenseSet<std::pair<unsigned, unsigned>> CheckedOverlaps;

  std::pair<CompletionResult, unsigned>
  computeConfluentCompletion(unsigned maxIterations,
                             unsigned maxDepth);

  void simplifyLeftHandSides();

  void simplifyRightHandSidesAndSubstitutions();

  enum ValidityPolicy {
    AllowInvalidRequirements,
    DisallowInvalidRequirements
  };

  void verifyRewriteRules(ValidityPolicy policy) const;

private:
  bool
  computeCriticalPair(
      ArrayRef<Symbol>::const_iterator from,
      const Rule &lhs, const Rule &rhs,
      std::vector<std::pair<MutableTerm, MutableTerm>> &pairs,
      std::vector<RewritePath> &paths,
      std::vector<RewriteLoop> &loops) const;

  /// Constructed from a rule of the form X.[P2:T] => X.[P1:T] by
  /// checkMergedAssociatedType().
  struct MergedAssociatedType {
    /// The *right* hand side of the original rule, X.[P1:T].
    Term rhs;

    /// The associated type symbol appearing at the end of the *left*
    /// hand side of the original rule, [P2:T].
    Symbol lhsSymbol;

    /// The merged associated type symbol, [P1&P2:T].
    Symbol mergedSymbol;
  };

  /// A list of pending terms for the associated type merging completion
  /// heuristic. Entries are added by checkMergedAssociatedType(), and
  /// consumed in processMergedAssociatedTypes().
  std::vector<MergedAssociatedType> MergedAssociatedTypes;

  void processMergedAssociatedTypes();

  void checkMergedAssociatedType(Term lhs, Term rhs);

  //////////////////////////////////////////////////////////////////////////////
  ///
  /// "Pseudo-rules" for the property map
  ///
  //////////////////////////////////////////////////////////////////////////////

public:
  struct ConcreteTypeWitness {
    Symbol ConcreteConformance;
    Symbol AssocType;
    Symbol ConcreteType;

    ConcreteTypeWitness(Symbol concreteConformance,
                        Symbol assocType,
                        Symbol concreteType);

    friend bool operator==(const ConcreteTypeWitness &lhs,
                           const ConcreteTypeWitness &rhs);
  };

private:
  /// Cache for concrete type witnesses. The value in the map is an index
  /// into the vector.
  llvm::DenseMap<std::pair<Symbol, Symbol>, unsigned> ConcreteTypeWitnessMap;
  std::vector<ConcreteTypeWitness> ConcreteTypeWitnesses;

public:
  unsigned recordConcreteTypeWitness(ConcreteTypeWitness witness);
  const ConcreteTypeWitness &getConcreteTypeWitness(unsigned index) const;

  //////////////////////////////////////////////////////////////////////////////
  ///
  /// Homotopy reduction
  ///
  //////////////////////////////////////////////////////////////////////////////

  /// Homotopy generators for this rewrite system. These are the
  /// rewrite loops which rewrite a term back to itself.
  ///
  /// In the category theory interpretation, a rewrite rule is a generating
  /// 2-cell, and a rewrite path is a 2-cell made from a composition of
  /// generating 2-cells.
  ///
  /// Homotopy generators, in turn, are 3-cells. The special case of a
  /// 3-cell discovered during completion can be viewed as two parallel
  /// 2-cells; this is actually represented as a single 2-cell forming a
  /// loop around a base point.
  ///
  /// This data is used by the homotopy reduction and generating conformances
  /// algorithms.
  std::vector<RewriteLoop> Loops;

  void recordRewriteLoop(RewriteLoop loop) {
    if (!RecordLoops)
      return;

    Loops.push_back(loop);
  }

  void recordRewriteLoop(MutableTerm basepoint,
                         RewritePath path) {
    if (!RecordLoops)
      return;

    Loops.emplace_back(basepoint, path);
  }

  void propagateExplicitBits();

  bool
  isCandidateForDeletion(unsigned ruleID,
                         const llvm::DenseSet<unsigned> *redundantConformances) const;

  Optional<unsigned>
  findRuleToDelete(const llvm::DenseSet<unsigned> *redundantConformances,
                   RewritePath &replacementPath);

  void deleteRule(unsigned ruleID, const RewritePath &replacementPath);

  void performHomotopyReduction(
      const llvm::DenseSet<unsigned> *redundantConformances);

  void computeGeneratingConformances(
      llvm::DenseSet<unsigned> &redundantConformances);

public:
  ArrayRef<RewriteLoop> getLoops() const {
    return Loops;
  }

  void minimizeRewriteSystem();

  bool hadError() const;

  llvm::DenseMap<const ProtocolDecl *, std::vector<unsigned>>
  getMinimizedProtocolRules(ArrayRef<const ProtocolDecl *> protos) const;

  std::vector<unsigned> getMinimizedGenericSignatureRules() const;

private:
  void verifyRewriteLoops() const;

  void verifyRedundantConformances(
      llvm::DenseSet<unsigned> redundantConformances) const;

  void verifyMinimizedRules() const;

public:
  void dump(llvm::raw_ostream &out) const;
};

} // end namespace rewriting

} // end namespace swift

#endif
