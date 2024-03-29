//=- AnalysisBasedWarnings.cpp - Sema warnings based on libAnalysis -*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines analysis_warnings::[Policy,Executor].
// Together they are used by Sema to issue warnings based on inexpensive
// static analysis algorithms in libAnalysis.
//
//===----------------------------------------------------------------------===//

#include "clang/Sema/AnalysisBasedWarnings.h"
#include "clang/Sema/SemaInternal.h"
#include "clang/Sema/ScopeInfo.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Lexer.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/StmtObjC.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Analysis/AnalysisContext.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/Analyses/ReachableCode.h"
#include "clang/Analysis/Analyses/CFGReachabilityAnalysis.h"
#include "clang/Analysis/Analyses/ThreadSafety.h"
#include "clang/Analysis/CFGStmtMap.h"
#include "clang/Analysis/Analyses/UninitializedValues.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/ImmutableMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include <algorithm>
#include <iterator>
#include <vector>
#include <deque>

using namespace clang;

//===----------------------------------------------------------------------===//
// Unreachable code analysis.
//===----------------------------------------------------------------------===//

namespace {
  class UnreachableCodeHandler : public reachable_code::Callback {
    Sema &S;
  public:
    UnreachableCodeHandler(Sema &s) : S(s) {}

    void HandleUnreachable(SourceLocation L, SourceRange R1, SourceRange R2) {
      S.Diag(L, diag::warn_unreachable) << R1 << R2;
    }
  };
}

/// CheckUnreachable - Check for unreachable code.
static void CheckUnreachable(Sema &S, AnalysisDeclContext &AC) {
  UnreachableCodeHandler UC(S);
  reachable_code::FindUnreachableCode(AC, UC);
}

//===----------------------------------------------------------------------===//
// Check for missing return value.
//===----------------------------------------------------------------------===//

enum ControlFlowKind {
  UnknownFallThrough,
  NeverFallThrough,
  MaybeFallThrough,
  AlwaysFallThrough,
  NeverFallThroughOrReturn
};

/// CheckFallThrough - Check that we don't fall off the end of a
/// Statement that should return a value.
///
/// \returns AlwaysFallThrough iff we always fall off the end of the statement,
/// MaybeFallThrough iff we might or might not fall off the end,
/// NeverFallThroughOrReturn iff we never fall off the end of the statement or
/// return.  We assume NeverFallThrough iff we never fall off the end of the
/// statement but we may return.  We assume that functions not marked noreturn
/// will return.
static ControlFlowKind CheckFallThrough(AnalysisDeclContext &AC) {
  CFG *cfg = AC.getCFG();
  if (cfg == 0) return UnknownFallThrough;

  // The CFG leaves in dead things, and we don't want the dead code paths to
  // confuse us, so we mark all live things first.
  llvm::BitVector live(cfg->getNumBlockIDs());
  unsigned count = reachable_code::ScanReachableFromBlock(&cfg->getEntry(),
                                                          live);

  bool AddEHEdges = AC.getAddEHEdges();
  if (!AddEHEdges && count != cfg->getNumBlockIDs())
    // When there are things remaining dead, and we didn't add EH edges
    // from CallExprs to the catch clauses, we have to go back and
    // mark them as live.
    for (CFG::iterator I = cfg->begin(), E = cfg->end(); I != E; ++I) {
      CFGBlock &b = **I;
      if (!live[b.getBlockID()]) {
        if (b.pred_begin() == b.pred_end()) {
          if (b.getTerminator() && isa<CXXTryStmt>(b.getTerminator()))
            // When not adding EH edges from calls, catch clauses
            // can otherwise seem dead.  Avoid noting them as dead.
            count += reachable_code::ScanReachableFromBlock(&b, live);
          continue;
        }
      }
    }

  // Now we know what is live, we check the live precessors of the exit block
  // and look for fall through paths, being careful to ignore normal returns,
  // and exceptional paths.
  bool HasLiveReturn = false;
  bool HasFakeEdge = false;
  bool HasPlainEdge = false;
  bool HasAbnormalEdge = false;

  // Ignore default cases that aren't likely to be reachable because all
  // enums in a switch(X) have explicit case statements.
  CFGBlock::FilterOptions FO;
  FO.IgnoreDefaultsWithCoveredEnums = 1;

  for (CFGBlock::filtered_pred_iterator
	 I = cfg->getExit().filtered_pred_start_end(FO); I.hasMore(); ++I) {
    const CFGBlock& B = **I;
    if (!live[B.getBlockID()])
      continue;

    // Skip blocks which contain an element marked as no-return. They don't
    // represent actually viable edges into the exit block, so mark them as
    // abnormal.
    if (B.hasNoReturnElement()) {
      HasAbnormalEdge = true;
      continue;
    }

    // Destructors can appear after the 'return' in the CFG.  This is
    // normal.  We need to look pass the destructors for the return
    // statement (if it exists).
    CFGBlock::const_reverse_iterator ri = B.rbegin(), re = B.rend();

    for ( ; ri != re ; ++ri)
      if (isa<CFGStmt>(*ri))
        break;

    // No more CFGElements in the block?
    if (ri == re) {
      if (B.getTerminator() && isa<CXXTryStmt>(B.getTerminator())) {
        HasAbnormalEdge = true;
        continue;
      }
      // A labeled empty statement, or the entry block...
      HasPlainEdge = true;
      continue;
    }

    CFGStmt CS = cast<CFGStmt>(*ri);
    const Stmt *S = CS.getStmt();
    if (isa<ReturnStmt>(S)) {
      HasLiveReturn = true;
      continue;
    }
    if (isa<ObjCAtThrowStmt>(S)) {
      HasFakeEdge = true;
      continue;
    }
    if (isa<CXXThrowExpr>(S)) {
      HasFakeEdge = true;
      continue;
    }
    if (isa<MSAsmStmt>(S)) {
      // TODO: Verify this is correct.
      HasFakeEdge = true;
      HasLiveReturn = true;
      continue;
    }
    if (isa<CXXTryStmt>(S)) {
      HasAbnormalEdge = true;
      continue;
    }
    if (std::find(B.succ_begin(), B.succ_end(), &cfg->getExit())
        == B.succ_end()) {
      HasAbnormalEdge = true;
      continue;
    }

    HasPlainEdge = true;
  }
  if (!HasPlainEdge) {
    if (HasLiveReturn)
      return NeverFallThrough;
    return NeverFallThroughOrReturn;
  }
  if (HasAbnormalEdge || HasFakeEdge || HasLiveReturn)
    return MaybeFallThrough;
  // This says AlwaysFallThrough for calls to functions that are not marked
  // noreturn, that don't return.  If people would like this warning to be more
  // accurate, such functions should be marked as noreturn.
  return AlwaysFallThrough;
}

namespace {

struct CheckFallThroughDiagnostics {
  unsigned diag_MaybeFallThrough_HasNoReturn;
  unsigned diag_MaybeFallThrough_ReturnsNonVoid;
  unsigned diag_AlwaysFallThrough_HasNoReturn;
  unsigned diag_AlwaysFallThrough_ReturnsNonVoid;
  unsigned diag_NeverFallThroughOrReturn;
  enum { Function, Block, Lambda } funMode;
  SourceLocation FuncLoc;

  static CheckFallThroughDiagnostics MakeForFunction(const Decl *Func) {
    CheckFallThroughDiagnostics D;
    D.FuncLoc = Func->getLocation();
    D.diag_MaybeFallThrough_HasNoReturn =
      diag::warn_falloff_noreturn_function;
    D.diag_MaybeFallThrough_ReturnsNonVoid =
      diag::warn_maybe_falloff_nonvoid_function;
    D.diag_AlwaysFallThrough_HasNoReturn =
      diag::warn_falloff_noreturn_function;
    D.diag_AlwaysFallThrough_ReturnsNonVoid =
      diag::warn_falloff_nonvoid_function;

    // Don't suggest that virtual functions be marked "noreturn", since they
    // might be overridden by non-noreturn functions.
    bool isVirtualMethod = false;
    if (const CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(Func))
      isVirtualMethod = Method->isVirtual();
    
    // Don't suggest that template instantiations be marked "noreturn"
    bool isTemplateInstantiation = false;
    if (const FunctionDecl *Function = dyn_cast<FunctionDecl>(Func))
      isTemplateInstantiation = Function->isTemplateInstantiation();
        
    if (!isVirtualMethod && !isTemplateInstantiation)
      D.diag_NeverFallThroughOrReturn =
        diag::warn_suggest_noreturn_function;
    else
      D.diag_NeverFallThroughOrReturn = 0;
    
    D.funMode = Function;
    return D;
  }

  static CheckFallThroughDiagnostics MakeForBlock() {
    CheckFallThroughDiagnostics D;
    D.diag_MaybeFallThrough_HasNoReturn =
      diag::err_noreturn_block_has_return_expr;
    D.diag_MaybeFallThrough_ReturnsNonVoid =
      diag::err_maybe_falloff_nonvoid_block;
    D.diag_AlwaysFallThrough_HasNoReturn =
      diag::err_noreturn_block_has_return_expr;
    D.diag_AlwaysFallThrough_ReturnsNonVoid =
      diag::err_falloff_nonvoid_block;
    D.diag_NeverFallThroughOrReturn =
      diag::warn_suggest_noreturn_block;
    D.funMode = Block;
    return D;
  }

  static CheckFallThroughDiagnostics MakeForLambda() {
    CheckFallThroughDiagnostics D;
    D.diag_MaybeFallThrough_HasNoReturn =
      diag::err_noreturn_lambda_has_return_expr;
    D.diag_MaybeFallThrough_ReturnsNonVoid =
      diag::warn_maybe_falloff_nonvoid_lambda;
    D.diag_AlwaysFallThrough_HasNoReturn =
      diag::err_noreturn_lambda_has_return_expr;
    D.diag_AlwaysFallThrough_ReturnsNonVoid =
      diag::warn_falloff_nonvoid_lambda;
    D.diag_NeverFallThroughOrReturn = 0;
    D.funMode = Lambda;
    return D;
  }

  bool checkDiagnostics(DiagnosticsEngine &D, bool ReturnsVoid,
                        bool HasNoReturn) const {
    if (funMode == Function) {
      return (ReturnsVoid ||
              D.getDiagnosticLevel(diag::warn_maybe_falloff_nonvoid_function,
                                   FuncLoc) == DiagnosticsEngine::Ignored)
        && (!HasNoReturn ||
            D.getDiagnosticLevel(diag::warn_noreturn_function_has_return_expr,
                                 FuncLoc) == DiagnosticsEngine::Ignored)
        && (!ReturnsVoid ||
            D.getDiagnosticLevel(diag::warn_suggest_noreturn_block, FuncLoc)
              == DiagnosticsEngine::Ignored);
    }

    // For blocks / lambdas.
    return ReturnsVoid && !HasNoReturn
            && ((funMode == Lambda) ||
                D.getDiagnosticLevel(diag::warn_suggest_noreturn_block, FuncLoc)
                  == DiagnosticsEngine::Ignored);
  }
};

}

/// CheckFallThroughForFunctionDef - Check that we don't fall off the end of a
/// function that should return a value.  Check that we don't fall off the end
/// of a noreturn function.  We assume that functions and blocks not marked
/// noreturn will return.
static void CheckFallThroughForBody(Sema &S, const Decl *D, const Stmt *Body,
                                    const BlockExpr *blkExpr,
                                    const CheckFallThroughDiagnostics& CD,
                                    AnalysisDeclContext &AC) {

  bool ReturnsVoid = false;
  bool HasNoReturn = false;

  if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
    ReturnsVoid = FD->getResultType()->isVoidType();
    HasNoReturn = FD->hasAttr<NoReturnAttr>() ||
       FD->getType()->getAs<FunctionType>()->getNoReturnAttr();
  }
  else if (const ObjCMethodDecl *MD = dyn_cast<ObjCMethodDecl>(D)) {
    ReturnsVoid = MD->getResultType()->isVoidType();
    HasNoReturn = MD->hasAttr<NoReturnAttr>();
  }
  else if (isa<BlockDecl>(D)) {
    QualType BlockTy = blkExpr->getType();
    if (const FunctionType *FT =
          BlockTy->getPointeeType()->getAs<FunctionType>()) {
      if (FT->getResultType()->isVoidType())
        ReturnsVoid = true;
      if (FT->getNoReturnAttr())
        HasNoReturn = true;
    }
  }

  DiagnosticsEngine &Diags = S.getDiagnostics();

  // Short circuit for compilation speed.
  if (CD.checkDiagnostics(Diags, ReturnsVoid, HasNoReturn))
      return;

  // FIXME: Function try block
  if (const CompoundStmt *Compound = dyn_cast<CompoundStmt>(Body)) {
    switch (CheckFallThrough(AC)) {
      case UnknownFallThrough:
        break;

      case MaybeFallThrough:
        if (HasNoReturn)
          S.Diag(Compound->getRBracLoc(),
                 CD.diag_MaybeFallThrough_HasNoReturn);
        else if (!ReturnsVoid)
          S.Diag(Compound->getRBracLoc(),
                 CD.diag_MaybeFallThrough_ReturnsNonVoid);
        break;
      case AlwaysFallThrough:
        if (HasNoReturn)
          S.Diag(Compound->getRBracLoc(),
                 CD.diag_AlwaysFallThrough_HasNoReturn);
        else if (!ReturnsVoid)
          S.Diag(Compound->getRBracLoc(),
                 CD.diag_AlwaysFallThrough_ReturnsNonVoid);
        break;
      case NeverFallThroughOrReturn:
        if (ReturnsVoid && !HasNoReturn && CD.diag_NeverFallThroughOrReturn) {
          if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
            S.Diag(Compound->getLBracLoc(), CD.diag_NeverFallThroughOrReturn)
              << 0 << FD;
          } else if (const ObjCMethodDecl *MD = dyn_cast<ObjCMethodDecl>(D)) {
            S.Diag(Compound->getLBracLoc(), CD.diag_NeverFallThroughOrReturn)
              << 1 << MD;
          } else {
            S.Diag(Compound->getLBracLoc(), CD.diag_NeverFallThroughOrReturn);
          }
        }
        break;
      case NeverFallThrough:
        break;
    }
  }
}

//===----------------------------------------------------------------------===//
// -Wuninitialized
//===----------------------------------------------------------------------===//

namespace {
/// ContainsReference - A visitor class to search for references to
/// a particular declaration (the needle) within any evaluated component of an
/// expression (recursively).
class ContainsReference : public EvaluatedExprVisitor<ContainsReference> {
  bool FoundReference;
  const DeclRefExpr *Needle;

public:
  ContainsReference(ASTContext &Context, const DeclRefExpr *Needle)
    : EvaluatedExprVisitor<ContainsReference>(Context),
      FoundReference(false), Needle(Needle) {}

  void VisitExpr(Expr *E) {
    // Stop evaluating if we already have a reference.
    if (FoundReference)
      return;

    EvaluatedExprVisitor<ContainsReference>::VisitExpr(E);
  }

  void VisitDeclRefExpr(DeclRefExpr *E) {
    if (E == Needle)
      FoundReference = true;
    else
      EvaluatedExprVisitor<ContainsReference>::VisitDeclRefExpr(E);
  }

  bool doesContainReference() const { return FoundReference; }
};
}

static bool SuggestInitializationFixit(Sema &S, const VarDecl *VD) {
  QualType VariableTy = VD->getType().getCanonicalType();
  if (VariableTy->isBlockPointerType() &&
      !VD->hasAttr<BlocksAttr>()) {
    S.Diag(VD->getLocation(), diag::note_block_var_fixit_add_initialization) << VD->getDeclName()
    << FixItHint::CreateInsertion(VD->getLocation(), "__block ");
    return true;
  }
  
  // Don't issue a fixit if there is already an initializer.
  if (VD->getInit())
    return false;
  
  // Suggest possible initialization (if any).
  std::string Init = S.getFixItZeroInitializerForType(VariableTy);
  if (Init.empty())
    return false;

  // Don't suggest a fixit inside macros.
  if (VD->getLocEnd().isMacroID())
    return false;

  SourceLocation Loc = S.PP.getLocForEndOfToken(VD->getLocEnd());
  
  S.Diag(Loc, diag::note_var_fixit_add_initialization) << VD->getDeclName()
    << FixItHint::CreateInsertion(Loc, Init);
  return true;
}

/// Create a fixit to remove an if-like statement, on the assumption that its
/// condition is CondVal.
static void CreateIfFixit(Sema &S, const Stmt *If, const Stmt *Then,
                          const Stmt *Else, bool CondVal,
                          FixItHint &Fixit1, FixItHint &Fixit2) {
  if (CondVal) {
    // If condition is always true, remove all but the 'then'.
    Fixit1 = FixItHint::CreateRemoval(
        CharSourceRange::getCharRange(If->getLocStart(),
                                      Then->getLocStart()));
    if (Else) {
      SourceLocation ElseKwLoc = Lexer::getLocForEndOfToken(
          Then->getLocEnd(), 0, S.getSourceManager(), S.getLangOpts());
      Fixit2 = FixItHint::CreateRemoval(
          SourceRange(ElseKwLoc, Else->getLocEnd()));
    }
  } else {
    // If condition is always false, remove all but the 'else'.
    if (Else)
      Fixit1 = FixItHint::CreateRemoval(
          CharSourceRange::getCharRange(If->getLocStart(),
                                        Else->getLocStart()));
    else
      Fixit1 = FixItHint::CreateRemoval(If->getSourceRange());
  }
}

/// DiagUninitUse -- Helper function to produce a diagnostic for an
/// uninitialized use of a variable.
static void DiagUninitUse(Sema &S, const VarDecl *VD, const UninitUse &Use,
                          bool IsCapturedByBlock) {
  bool Diagnosed = false;

  // Diagnose each branch which leads to a sometimes-uninitialized use.
  for (UninitUse::branch_iterator I = Use.branch_begin(), E = Use.branch_end();
       I != E; ++I) {
    assert(Use.getKind() == UninitUse::Sometimes);

    const Expr *User = Use.getUser();
    const Stmt *Term = I->Terminator;

    // Information used when building the diagnostic.
    unsigned DiagKind;
    StringRef Str;
    SourceRange Range;

    // FixIts to suppress the diagnosic by removing the dead condition.
    // For all binary terminators, branch 0 is taken if the condition is true,
    // and branch 1 is taken if the condition is false.
    int RemoveDiagKind = -1;
    const char *FixitStr =
        S.getLangOpts().CPlusPlus ? (I->Output ? "true" : "false")
                                  : (I->Output ? "1" : "0");
    FixItHint Fixit1, Fixit2;

    switch (Term->getStmtClass()) {
    default:
      // Don't know how to report this. Just fall back to 'may be used
      // uninitialized'. This happens for range-based for, which the user
      // can't explicitly fix.
      // FIXME: This also happens if the first use of a variable is always
      // uninitialized, eg "for (int n; n < 10; ++n)". We should report that
      // with the 'is uninitialized' diagnostic.
      continue;

    // "condition is true / condition is false".
    case Stmt::IfStmtClass: {
      const IfStmt *IS = cast<IfStmt>(Term);
      DiagKind = 0;
      Str = "if";
      Range = IS->getCond()->getSourceRange();
      RemoveDiagKind = 0;
      CreateIfFixit(S, IS, IS->getThen(), IS->getElse(),
                    I->Output, Fixit1, Fixit2);
      break;
    }
    case Stmt::ConditionalOperatorClass: {
      const ConditionalOperator *CO = cast<ConditionalOperator>(Term);
      DiagKind = 0;
      Str = "?:";
      Range = CO->getCond()->getSourceRange();
      RemoveDiagKind = 0;
      CreateIfFixit(S, CO, CO->getTrueExpr(), CO->getFalseExpr(),
                    I->Output, Fixit1, Fixit2);
      break;
    }
    case Stmt::BinaryOperatorClass: {
      const BinaryOperator *BO = cast<BinaryOperator>(Term);
      if (!BO->isLogicalOp())
        continue;
      DiagKind = 0;
      Str = BO->getOpcodeStr();
      Range = BO->getLHS()->getSourceRange();
      RemoveDiagKind = 0;
      if ((BO->getOpcode() == BO_LAnd && I->Output) ||
          (BO->getOpcode() == BO_LOr && !I->Output))
        // true && y -> y, false || y -> y.
        Fixit1 = FixItHint::CreateRemoval(SourceRange(BO->getLocStart(),
                                                      BO->getOperatorLoc()));
      else
        // false && y -> false, true || y -> true.
        Fixit1 = FixItHint::CreateReplacement(BO->getSourceRange(), FixitStr);
      break;
    }

    // "loop is entered / loop is exited".
    case Stmt::WhileStmtClass:
      DiagKind = 1;
      Str = "while";
      Range = cast<WhileStmt>(Term)->getCond()->getSourceRange();
      RemoveDiagKind = 1;
      Fixit1 = FixItHint::CreateReplacement(Range, FixitStr);
      break;
    case Stmt::ForStmtClass:
      DiagKind = 1;
      Str = "for";
      Range = cast<ForStmt>(Term)->getCond()->getSourceRange();
      RemoveDiagKind = 1;
      if (I->Output)
        Fixit1 = FixItHint::CreateRemoval(Range);
      else
        Fixit1 = FixItHint::CreateReplacement(Range, FixitStr);
      break;

    // "condition is true / loop is exited".
    case Stmt::DoStmtClass:
      DiagKind = 2;
      Str = "do";
      Range = cast<DoStmt>(Term)->getCond()->getSourceRange();
      RemoveDiagKind = 1;
      Fixit1 = FixItHint::CreateReplacement(Range, FixitStr);
      break;

    // "switch case is taken".
    case Stmt::CaseStmtClass:
      DiagKind = 3;
      Str = "case";
      Range = cast<CaseStmt>(Term)->getLHS()->getSourceRange();
      break;
    case Stmt::DefaultStmtClass:
      DiagKind = 3;
      Str = "default";
      Range = cast<DefaultStmt>(Term)->getDefaultLoc();
      break;
    }

    S.Diag(Range.getBegin(), diag::warn_sometimes_uninit_var)
      << VD->getDeclName() << IsCapturedByBlock << DiagKind
      << Str << I->Output << Range;
    S.Diag(User->getLocStart(), diag::note_uninit_var_use)
      << IsCapturedByBlock << User->getSourceRange();
    if (RemoveDiagKind != -1)
      S.Diag(Fixit1.RemoveRange.getBegin(), diag::note_uninit_fixit_remove_cond)
        << RemoveDiagKind << Str << I->Output << Fixit1 << Fixit2;

    Diagnosed = true;
  }

  if (!Diagnosed)
    S.Diag(Use.getUser()->getLocStart(),
           Use.getKind() == UninitUse::Always ? diag::warn_uninit_var
                                              : diag::warn_maybe_uninit_var)
        << VD->getDeclName() << IsCapturedByBlock
        << Use.getUser()->getSourceRange();
}

/// DiagnoseUninitializedUse -- Helper function for diagnosing uses of an
/// uninitialized variable. This manages the different forms of diagnostic
/// emitted for particular types of uses. Returns true if the use was diagnosed
/// as a warning. If a particular use is one we omit warnings for, returns
/// false.
static bool DiagnoseUninitializedUse(Sema &S, const VarDecl *VD,
                                     const UninitUse &Use,
                                     bool alwaysReportSelfInit = false) {

  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Use.getUser())) {
    // Inspect the initializer of the variable declaration which is
    // being referenced prior to its initialization. We emit
    // specialized diagnostics for self-initialization, and we
    // specifically avoid warning about self references which take the
    // form of:
    //
    //   int x = x;
    //
    // This is used to indicate to GCC that 'x' is intentionally left
    // uninitialized. Proven code paths which access 'x' in
    // an uninitialized state after this will still warn.
    if (const Expr *Initializer = VD->getInit()) {
      if (!alwaysReportSelfInit && DRE == Initializer->IgnoreParenImpCasts())
        return false;

      ContainsReference CR(S.Context, DRE);
      CR.Visit(const_cast<Expr*>(Initializer));
      if (CR.doesContainReference()) {
        S.Diag(DRE->getLocStart(),
               diag::warn_uninit_self_reference_in_init)
          << VD->getDeclName() << VD->getLocation() << DRE->getSourceRange();
        return true;
      }
    }

    DiagUninitUse(S, VD, Use, false);
  } else {
    const BlockExpr *BE = cast<BlockExpr>(Use.getUser());
    if (VD->getType()->isBlockPointerType() && !VD->hasAttr<BlocksAttr>())
      S.Diag(BE->getLocStart(),
             diag::warn_uninit_byref_blockvar_captured_by_block)
        << VD->getDeclName();
    else
      DiagUninitUse(S, VD, Use, true);
  }

  // Report where the variable was declared when the use wasn't within
  // the initializer of that declaration & we didn't already suggest
  // an initialization fixit.
  if (!SuggestInitializationFixit(S, VD))
    S.Diag(VD->getLocStart(), diag::note_uninit_var_def)
      << VD->getDeclName();

  return true;
}

namespace {
  class FallthroughMapper : public RecursiveASTVisitor<FallthroughMapper> {
  public:
    FallthroughMapper(Sema &S)
      : FoundSwitchStatements(false),
        S(S) {
    }

    bool foundSwitchStatements() const { return FoundSwitchStatements; }

    void markFallthroughVisited(const AttributedStmt *Stmt) {
      bool Found = FallthroughStmts.erase(Stmt);
      assert(Found);
      (void)Found;
    }

    typedef llvm::SmallPtrSet<const AttributedStmt*, 8> AttrStmts;

    const AttrStmts &getFallthroughStmts() const {
      return FallthroughStmts;
    }

    bool checkFallThroughIntoBlock(const CFGBlock &B, int &AnnotatedCnt) {
      int UnannotatedCnt = 0;
      AnnotatedCnt = 0;

      std::deque<const CFGBlock*> BlockQueue;

      std::copy(B.pred_begin(), B.pred_end(), std::back_inserter(BlockQueue));

      while (!BlockQueue.empty()) {
        const CFGBlock *P = BlockQueue.front();
        BlockQueue.pop_front();

        const Stmt *Term = P->getTerminator();
        if (Term && isa<SwitchStmt>(Term))
          continue; // Switch statement, good.

        const SwitchCase *SW = dyn_cast_or_null<SwitchCase>(P->getLabel());
        if (SW && SW->getSubStmt() == B.getLabel() && P->begin() == P->end())
          continue; // Previous case label has no statements, good.

        if (P->pred_begin() == P->pred_end()) {  // The block is unreachable.
          // This only catches trivially unreachable blocks.
          for (CFGBlock::const_iterator ElIt = P->begin(), ElEnd = P->end();
               ElIt != ElEnd; ++ElIt) {
            if (const CFGStmt *CS = ElIt->getAs<CFGStmt>()){
              if (const AttributedStmt *AS = asFallThroughAttr(CS->getStmt())) {
                S.Diag(AS->getLocStart(),
                       diag::warn_fallthrough_attr_unreachable);
                markFallthroughVisited(AS);
                ++AnnotatedCnt;
              }
              // Don't care about other unreachable statements.
            }
          }
          // If there are no unreachable statements, this may be a special
          // case in CFG:
          // case X: {
          //    A a;  // A has a destructor.
          //    break;
          // }
          // // <<<< This place is represented by a 'hanging' CFG block.
          // case Y:
          continue;
        }

        const Stmt *LastStmt = getLastStmt(*P);
        if (const AttributedStmt *AS = asFallThroughAttr(LastStmt)) {
          markFallthroughVisited(AS);
          ++AnnotatedCnt;
          continue; // Fallthrough annotation, good.
        }

        if (!LastStmt) { // This block contains no executable statements.
          // Traverse its predecessors.
          std::copy(P->pred_begin(), P->pred_end(),
                    std::back_inserter(BlockQueue));
          continue;
        }

        ++UnannotatedCnt;
      }
      return !!UnannotatedCnt;
    }

    // RecursiveASTVisitor setup.
    bool shouldWalkTypesOfTypeLocs() const { return false; }

    bool VisitAttributedStmt(AttributedStmt *S) {
      if (asFallThroughAttr(S))
        FallthroughStmts.insert(S);
      return true;
    }

    bool VisitSwitchStmt(SwitchStmt *S) {
      FoundSwitchStatements = true;
      return true;
    }

  private:

    static const AttributedStmt *asFallThroughAttr(const Stmt *S) {
      if (const AttributedStmt *AS = dyn_cast_or_null<AttributedStmt>(S)) {
        if (hasSpecificAttr<FallThroughAttr>(AS->getAttrs()))
          return AS;
      }
      return 0;
    }

    static const Stmt *getLastStmt(const CFGBlock &B) {
      if (const Stmt *Term = B.getTerminator())
        return Term;
      for (CFGBlock::const_reverse_iterator ElemIt = B.rbegin(),
                                            ElemEnd = B.rend();
                                            ElemIt != ElemEnd; ++ElemIt) {
        if (const CFGStmt *CS = ElemIt->getAs<CFGStmt>())
          return CS->getStmt();
      }
      // Workaround to detect a statement thrown out by CFGBuilder:
      //   case X: {} case Y:
      //   case X: ; case Y:
      if (const SwitchCase *SW = dyn_cast_or_null<SwitchCase>(B.getLabel()))
        if (!isa<SwitchCase>(SW->getSubStmt()))
          return SW->getSubStmt();

      return 0;
    }

    bool FoundSwitchStatements;
    AttrStmts FallthroughStmts;
    Sema &S;
  };
}

static void DiagnoseSwitchLabelsFallthrough(Sema &S, AnalysisDeclContext &AC,
                                            bool PerFunction) {
  FallthroughMapper FM(S);
  FM.TraverseStmt(AC.getBody());

  if (!FM.foundSwitchStatements())
    return;

  if (PerFunction && FM.getFallthroughStmts().empty())
    return;

  CFG *Cfg = AC.getCFG();

  if (!Cfg)
    return;

  int AnnotatedCnt;

  for (CFG::reverse_iterator I = Cfg->rbegin(), E = Cfg->rend(); I != E; ++I) {
    const CFGBlock &B = **I;
    const Stmt *Label = B.getLabel();

    if (!Label || !isa<SwitchCase>(Label))
      continue;

    if (!FM.checkFallThroughIntoBlock(B, AnnotatedCnt))
      continue;

    S.Diag(Label->getLocStart(),
        PerFunction ? diag::warn_unannotated_fallthrough_per_function
                    : diag::warn_unannotated_fallthrough);

    if (!AnnotatedCnt) {
      SourceLocation L = Label->getLocStart();
      if (L.isMacroID())
        continue;
      if (S.getLangOpts().CPlusPlus0x) {
        const Stmt *Term = B.getTerminator();
        if (!(B.empty() && Term && isa<BreakStmt>(Term))) {
          Preprocessor &PP = S.getPreprocessor();
          TokenValue Tokens[] = {
            tok::l_square, tok::l_square, PP.getIdentifierInfo("clang"),
            tok::coloncolon, PP.getIdentifierInfo("fallthrough"),
            tok::r_square, tok::r_square
          };
          StringRef AnnotationSpelling = "[[clang::fallthrough]]";
          StringRef MacroName = PP.getLastMacroWithSpelling(L, Tokens);
          if (!MacroName.empty())
            AnnotationSpelling = MacroName;
          SmallString<64> TextToInsert(AnnotationSpelling);
          TextToInsert += "; ";
          S.Diag(L, diag::note_insert_fallthrough_fixit) <<
              AnnotationSpelling <<
              FixItHint::CreateInsertion(L, TextToInsert);
        }
      }
      S.Diag(L, diag::note_insert_break_fixit) <<
        FixItHint::CreateInsertion(L, "break; ");
    }
  }

  const FallthroughMapper::AttrStmts &Fallthroughs = FM.getFallthroughStmts();
  for (FallthroughMapper::AttrStmts::const_iterator I = Fallthroughs.begin(),
                                                    E = Fallthroughs.end();
                                                    I != E; ++I) {
    S.Diag((*I)->getLocStart(), diag::warn_fallthrough_attr_invalid_placement);
  }

}

namespace {
typedef std::pair<const Stmt *,
                  sema::FunctionScopeInfo::WeakObjectUseMap::const_iterator>
        StmtUsesPair;

class StmtUseSorter {
  const SourceManager &SM;

public:
  explicit StmtUseSorter(const SourceManager &SM) : SM(SM) { }

  bool operator()(const StmtUsesPair &LHS, const StmtUsesPair &RHS) {
    return SM.isBeforeInTranslationUnit(LHS.first->getLocStart(),
                                        RHS.first->getLocStart());
  }
};
}

static bool isInLoop(const ParentMap &PM, const Stmt *S) {
  assert(S);

  do {
    switch (S->getStmtClass()) {
    case Stmt::DoStmtClass:
    case Stmt::ForStmtClass:
    case Stmt::WhileStmtClass:
    case Stmt::CXXForRangeStmtClass:
    case Stmt::ObjCForCollectionStmtClass:
      return true;
    default:
      break;
    }
  } while ((S = PM.getParent(S)));

  return false;
}


static void diagnoseRepeatedUseOfWeak(Sema &S,
                                      const sema::FunctionScopeInfo *CurFn,
                                      const Decl *D,
                                      const ParentMap &PM) {
  typedef sema::FunctionScopeInfo::WeakObjectProfileTy WeakObjectProfileTy;
  typedef sema::FunctionScopeInfo::WeakObjectUseMap WeakObjectUseMap;
  typedef sema::FunctionScopeInfo::WeakUseVector WeakUseVector;

  const WeakObjectUseMap &WeakMap = CurFn->getWeakObjectUses();

  // Extract all weak objects that are referenced more than once.
  SmallVector<StmtUsesPair, 8> UsesByStmt;
  for (WeakObjectUseMap::const_iterator I = WeakMap.begin(), E = WeakMap.end();
       I != E; ++I) {
    const WeakUseVector &Uses = I->second;

    // Find the first read of the weak object.
    WeakUseVector::const_iterator UI = Uses.begin(), UE = Uses.end();
    for ( ; UI != UE; ++UI) {
      if (UI->isUnsafe())
        break;
    }

    // If there were only writes to this object, don't warn.
    if (UI == UE)
      continue;

    // If there was only one read, followed by any number of writes, and the
    // read is not within a loop, don't warn.
    if (UI == Uses.begin()) {
      WeakUseVector::const_iterator UI2 = UI;
      for (++UI2; UI2 != UE; ++UI2)
        if (UI2->isUnsafe())
          break;

      if (UI2 == UE)
        if (!isInLoop(PM, UI->getUseExpr()))
          continue;
    }

    UsesByStmt.push_back(StmtUsesPair(UI->getUseExpr(), I));
  }

  if (UsesByStmt.empty())
    return;

  // Sort by first use so that we emit the warnings in a deterministic order.
  std::sort(UsesByStmt.begin(), UsesByStmt.end(),
            StmtUseSorter(S.getSourceManager()));

  // Classify the current code body for better warning text.
  // This enum should stay in sync with the cases in
  // warn_arc_repeated_use_of_weak and warn_arc_possible_repeated_use_of_weak.
  // FIXME: Should we use a common classification enum and the same set of
  // possibilities all throughout Sema?
  enum {
    Function,
    Method,
    Block,
    Lambda
  } FunctionKind;

  if (isa<sema::BlockScopeInfo>(CurFn))
    FunctionKind = Block;
  else if (isa<sema::LambdaScopeInfo>(CurFn))
    FunctionKind = Lambda;
  else if (isa<ObjCMethodDecl>(D))
    FunctionKind = Method;
  else
    FunctionKind = Function;

  // Iterate through the sorted problems and emit warnings for each.
  for (SmallVectorImpl<StmtUsesPair>::const_iterator I = UsesByStmt.begin(),
                                                     E = UsesByStmt.end();
       I != E; ++I) {
    const Stmt *FirstRead = I->first;
    const WeakObjectProfileTy &Key = I->second->first;
    const WeakUseVector &Uses = I->second->second;

    // For complicated expressions like 'a.b.c' and 'x.b.c', WeakObjectProfileTy
    // may not contain enough information to determine that these are different
    // properties. We can only be 100% sure of a repeated use in certain cases,
    // and we adjust the diagnostic kind accordingly so that the less certain
    // case can be turned off if it is too noisy.
    unsigned DiagKind;
    if (Key.isExactProfile())
      DiagKind = diag::warn_arc_repeated_use_of_weak;
    else
      DiagKind = diag::warn_arc_possible_repeated_use_of_weak;

    // Classify the weak object being accessed for better warning text.
    // This enum should stay in sync with the cases in
    // warn_arc_repeated_use_of_weak and warn_arc_possible_repeated_use_of_weak.
    enum {
      Variable,
      Property,
      ImplicitProperty,
      Ivar
    } ObjectKind;

    const NamedDecl *D = Key.getProperty();
    if (isa<VarDecl>(D))
      ObjectKind = Variable;
    else if (isa<ObjCPropertyDecl>(D))
      ObjectKind = Property;
    else if (isa<ObjCMethodDecl>(D))
      ObjectKind = ImplicitProperty;
    else if (isa<ObjCIvarDecl>(D))
      ObjectKind = Ivar;
    else
      llvm_unreachable("Unexpected weak object kind!");

    // Show the first time the object was read.
    S.Diag(FirstRead->getLocStart(), DiagKind)
      << ObjectKind << D << FunctionKind
      << FirstRead->getSourceRange();

    // Print all the other accesses as notes.
    for (WeakUseVector::const_iterator UI = Uses.begin(), UE = Uses.end();
         UI != UE; ++UI) {
      if (UI->getUseExpr() == FirstRead)
        continue;
      S.Diag(UI->getUseExpr()->getLocStart(),
             diag::note_arc_weak_also_accessed_here)
        << UI->getUseExpr()->getSourceRange();
    }
  }
}


namespace {
struct SLocSort {
  bool operator()(const UninitUse &a, const UninitUse &b) {
    // Prefer a more confident report over a less confident one.
    if (a.getKind() != b.getKind())
      return a.getKind() > b.getKind();
    SourceLocation aLoc = a.getUser()->getLocStart();
    SourceLocation bLoc = b.getUser()->getLocStart();
    return aLoc.getRawEncoding() < bLoc.getRawEncoding();
  }
};

class UninitValsDiagReporter : public UninitVariablesHandler {
  Sema &S;
  typedef SmallVector<UninitUse, 2> UsesVec;
  typedef llvm::DenseMap<const VarDecl *, std::pair<UsesVec*, bool> > UsesMap;
  UsesMap *uses;
  
public:
  UninitValsDiagReporter(Sema &S) : S(S), uses(0) {}
  ~UninitValsDiagReporter() { 
    flushDiagnostics();
  }

  std::pair<UsesVec*, bool> &getUses(const VarDecl *vd) {
    if (!uses)
      uses = new UsesMap();

    UsesMap::mapped_type &V = (*uses)[vd];
    UsesVec *&vec = V.first;
    if (!vec)
      vec = new UsesVec();
    
    return V;
  }
  
  void handleUseOfUninitVariable(const VarDecl *vd, const UninitUse &use) {
    getUses(vd).first->push_back(use);
  }
  
  void handleSelfInit(const VarDecl *vd) {
    getUses(vd).second = true;    
  }
  
  void flushDiagnostics() {
    if (!uses)
      return;
    
    // FIXME: This iteration order, and thus the resulting diagnostic order,
    //        is nondeterministic.
    for (UsesMap::iterator i = uses->begin(), e = uses->end(); i != e; ++i) {
      const VarDecl *vd = i->first;
      const UsesMap::mapped_type &V = i->second;

      UsesVec *vec = V.first;
      bool hasSelfInit = V.second;

      // Specially handle the case where we have uses of an uninitialized 
      // variable, but the root cause is an idiomatic self-init.  We want
      // to report the diagnostic at the self-init since that is the root cause.
      if (!vec->empty() && hasSelfInit && hasAlwaysUninitializedUse(vec))
        DiagnoseUninitializedUse(S, vd,
                                 UninitUse(vd->getInit()->IgnoreParenCasts(),
                                           /* isAlwaysUninit */ true),
                                 /* alwaysReportSelfInit */ true);
      else {
        // Sort the uses by their SourceLocations.  While not strictly
        // guaranteed to produce them in line/column order, this will provide
        // a stable ordering.
        std::sort(vec->begin(), vec->end(), SLocSort());
        
        for (UsesVec::iterator vi = vec->begin(), ve = vec->end(); vi != ve;
             ++vi) {
          // If we have self-init, downgrade all uses to 'may be uninitialized'.
          UninitUse Use = hasSelfInit ? UninitUse(vi->getUser(), false) : *vi;

          if (DiagnoseUninitializedUse(S, vd, Use))
            // Skip further diagnostics for this variable. We try to warn only
            // on the first point at which a variable is used uninitialized.
            break;
        }
      }
      
      // Release the uses vector.
      delete vec;
    }
    delete uses;
  }

private:
  static bool hasAlwaysUninitializedUse(const UsesVec* vec) {
  for (UsesVec::const_iterator i = vec->begin(), e = vec->end(); i != e; ++i) {
    if (i->getKind() == UninitUse::Always) {
      return true;
    }
  }
  return false;
}
};
}


//===----------------------------------------------------------------------===//
// -Wthread-safety
//===----------------------------------------------------------------------===//
namespace clang {
namespace thread_safety {
typedef llvm::SmallVector<PartialDiagnosticAt, 1> OptionalNotes;
typedef std::pair<PartialDiagnosticAt, OptionalNotes> DelayedDiag;
typedef std::list<DelayedDiag> DiagList;

struct SortDiagBySourceLocation {
  SourceManager &SM;
  SortDiagBySourceLocation(SourceManager &SM) : SM(SM) {}

  bool operator()(const DelayedDiag &left, const DelayedDiag &right) {
    // Although this call will be slow, this is only called when outputting
    // multiple warnings.
    return SM.isBeforeInTranslationUnit(left.first.first, right.first.first);
  }
};

namespace {
class ThreadSafetyReporter : public clang::thread_safety::ThreadSafetyHandler {
  Sema &S;
  DiagList Warnings;
  SourceLocation FunLocation, FunEndLocation;

  // Helper functions
  void warnLockMismatch(unsigned DiagID, Name LockName, SourceLocation Loc) {
    // Gracefully handle rare cases when the analysis can't get a more
    // precise source location.
    if (!Loc.isValid())
      Loc = FunLocation;
    PartialDiagnosticAt Warning(Loc, S.PDiag(DiagID) << LockName);
    Warnings.push_back(DelayedDiag(Warning, OptionalNotes()));
  }

 public:
  ThreadSafetyReporter(Sema &S, SourceLocation FL, SourceLocation FEL)
    : S(S), FunLocation(FL), FunEndLocation(FEL) {}

  /// \brief Emit all buffered diagnostics in order of sourcelocation.
  /// We need to output diagnostics produced while iterating through
  /// the lockset in deterministic order, so this function orders diagnostics
  /// and outputs them.
  void emitDiagnostics() {
    Warnings.sort(SortDiagBySourceLocation(S.getSourceManager()));
    for (DiagList::iterator I = Warnings.begin(), E = Warnings.end();
         I != E; ++I) {
      S.Diag(I->first.first, I->first.second);
      const OptionalNotes &Notes = I->second;
      for (unsigned NoteI = 0, NoteN = Notes.size(); NoteI != NoteN; ++NoteI)
        S.Diag(Notes[NoteI].first, Notes[NoteI].second);
    }
  }

  void handleInvalidLockExp(SourceLocation Loc) {
    PartialDiagnosticAt Warning(Loc,
                                S.PDiag(diag::warn_cannot_resolve_lock) << Loc);
    Warnings.push_back(DelayedDiag(Warning, OptionalNotes()));
  }
  void handleUnmatchedUnlock(Name LockName, SourceLocation Loc) {
    warnLockMismatch(diag::warn_unlock_but_no_lock, LockName, Loc);
  }

  void handleDoubleLock(Name LockName, SourceLocation Loc) {
    warnLockMismatch(diag::warn_double_lock, LockName, Loc);
  }

  void handleMutexHeldEndOfScope(Name LockName, SourceLocation LocLocked,
                                 SourceLocation LocEndOfScope,
                                 LockErrorKind LEK){
    unsigned DiagID = 0;
    switch (LEK) {
      case LEK_LockedSomePredecessors:
        DiagID = diag::warn_lock_some_predecessors;
        break;
      case LEK_LockedSomeLoopIterations:
        DiagID = diag::warn_expecting_lock_held_on_loop;
        break;
      case LEK_LockedAtEndOfFunction:
        DiagID = diag::warn_no_unlock;
        break;
      case LEK_NotLockedAtEndOfFunction:
        DiagID = diag::warn_expecting_locked;
        break;
    }
    if (LocEndOfScope.isInvalid())
      LocEndOfScope = FunEndLocation;

    PartialDiagnosticAt Warning(LocEndOfScope, S.PDiag(DiagID) << LockName);
    PartialDiagnosticAt Note(LocLocked, S.PDiag(diag::note_locked_here));
    Warnings.push_back(DelayedDiag(Warning, OptionalNotes(1, Note)));
  }


  void handleExclusiveAndShared(Name LockName, SourceLocation Loc1,
                                SourceLocation Loc2) {
    PartialDiagnosticAt Warning(
      Loc1, S.PDiag(diag::warn_lock_exclusive_and_shared) << LockName);
    PartialDiagnosticAt Note(
      Loc2, S.PDiag(diag::note_lock_exclusive_and_shared) << LockName);
    Warnings.push_back(DelayedDiag(Warning, OptionalNotes(1, Note)));
  }

  void handleNoMutexHeld(const NamedDecl *D, ProtectedOperationKind POK,
                         AccessKind AK, SourceLocation Loc) {
    assert((POK == POK_VarAccess || POK == POK_VarDereference)
             && "Only works for variables");
    unsigned DiagID = POK == POK_VarAccess?
                        diag::warn_variable_requires_any_lock:
                        diag::warn_var_deref_requires_any_lock;
    PartialDiagnosticAt Warning(Loc, S.PDiag(DiagID)
      << D->getNameAsString() << getLockKindFromAccessKind(AK));
    Warnings.push_back(DelayedDiag(Warning, OptionalNotes()));
  }

  void handleMutexNotHeld(const NamedDecl *D, ProtectedOperationKind POK,
                          Name LockName, LockKind LK, SourceLocation Loc,
                          Name *PossibleMatch) {
    unsigned DiagID = 0;
    if (PossibleMatch) {
      switch (POK) {
        case POK_VarAccess:
          DiagID = diag::warn_variable_requires_lock_precise;
          break;
        case POK_VarDereference:
          DiagID = diag::warn_var_deref_requires_lock_precise;
          break;
        case POK_FunctionCall:
          DiagID = diag::warn_fun_requires_lock_precise;
          break;
      }
      PartialDiagnosticAt Warning(Loc, S.PDiag(DiagID)
        << D->getNameAsString() << LockName << LK);
      PartialDiagnosticAt Note(Loc, S.PDiag(diag::note_found_mutex_near_match)
                               << *PossibleMatch);
      Warnings.push_back(DelayedDiag(Warning, OptionalNotes(1, Note)));
    } else {
      switch (POK) {
        case POK_VarAccess:
          DiagID = diag::warn_variable_requires_lock;
          break;
        case POK_VarDereference:
          DiagID = diag::warn_var_deref_requires_lock;
          break;
        case POK_FunctionCall:
          DiagID = diag::warn_fun_requires_lock;
          break;
      }
      PartialDiagnosticAt Warning(Loc, S.PDiag(DiagID)
        << D->getNameAsString() << LockName << LK);
      Warnings.push_back(DelayedDiag(Warning, OptionalNotes()));
    }
  }

  void handleFunExcludesLock(Name FunName, Name LockName, SourceLocation Loc) {
    PartialDiagnosticAt Warning(Loc,
      S.PDiag(diag::warn_fun_excludes_mutex) << FunName << LockName);
    Warnings.push_back(DelayedDiag(Warning, OptionalNotes()));
  }
};
}
}
}

//===----------------------------------------------------------------------===//
// AnalysisBasedWarnings - Worker object used by Sema to execute analysis-based
//  warnings on a function, method, or block.
//===----------------------------------------------------------------------===//

clang::sema::AnalysisBasedWarnings::Policy::Policy() {
  enableCheckFallThrough = 1;
  enableCheckUnreachable = 0;
  enableThreadSafetyAnalysis = 0;
}

clang::sema::AnalysisBasedWarnings::AnalysisBasedWarnings(Sema &s)
  : S(s),
    NumFunctionsAnalyzed(0),
    NumFunctionsWithBadCFGs(0),
    NumCFGBlocks(0),
    MaxCFGBlocksPerFunction(0),
    NumUninitAnalysisFunctions(0),
    NumUninitAnalysisVariables(0),
    MaxUninitAnalysisVariablesPerFunction(0),
    NumUninitAnalysisBlockVisits(0),
    MaxUninitAnalysisBlockVisitsPerFunction(0) {
  DiagnosticsEngine &D = S.getDiagnostics();
  DefaultPolicy.enableCheckUnreachable = (unsigned)
    (D.getDiagnosticLevel(diag::warn_unreachable, SourceLocation()) !=
        DiagnosticsEngine::Ignored);
  DefaultPolicy.enableThreadSafetyAnalysis = (unsigned)
    (D.getDiagnosticLevel(diag::warn_double_lock, SourceLocation()) !=
     DiagnosticsEngine::Ignored);

}

static void flushDiagnostics(Sema &S, sema::FunctionScopeInfo *fscope) {
  for (SmallVectorImpl<sema::PossiblyUnreachableDiag>::iterator
       i = fscope->PossiblyUnreachableDiags.begin(),
       e = fscope->PossiblyUnreachableDiags.end();
       i != e; ++i) {
    const sema::PossiblyUnreachableDiag &D = *i;
    S.Diag(D.Loc, D.PD);
  }
}

void clang::sema::
AnalysisBasedWarnings::IssueWarnings(sema::AnalysisBasedWarnings::Policy P,
                                     sema::FunctionScopeInfo *fscope,
                                     const Decl *D, const BlockExpr *blkExpr) {

  // We avoid doing analysis-based warnings when there are errors for
  // two reasons:
  // (1) The CFGs often can't be constructed (if the body is invalid), so
  //     don't bother trying.
  // (2) The code already has problems; running the analysis just takes more
  //     time.
  DiagnosticsEngine &Diags = S.getDiagnostics();

  // Do not do any analysis for declarations in system headers if we are
  // going to just ignore them.
  if (Diags.getSuppressSystemWarnings() &&
      S.SourceMgr.isInSystemHeader(D->getLocation()))
    return;

  // For code in dependent contexts, we'll do this at instantiation time.
  if (cast<DeclContext>(D)->isDependentContext())
    return;

  if (Diags.hasErrorOccurred() || Diags.hasFatalErrorOccurred()) {
    // Flush out any possibly unreachable diagnostics.
    flushDiagnostics(S, fscope);
    return;
  }
  
  const Stmt *Body = D->getBody();
  assert(Body);

  AnalysisDeclContext AC(/* AnalysisDeclContextManager */ 0, D);

  // Don't generate EH edges for CallExprs as we'd like to avoid the n^2
  // explosion for destrutors that can result and the compile time hit.
  AC.getCFGBuildOptions().PruneTriviallyFalseEdges = true;
  AC.getCFGBuildOptions().AddEHEdges = false;
  AC.getCFGBuildOptions().AddInitializers = true;
  AC.getCFGBuildOptions().AddImplicitDtors = true;
  AC.getCFGBuildOptions().AddTemporaryDtors = true;

  // Force that certain expressions appear as CFGElements in the CFG.  This
  // is used to speed up various analyses.
  // FIXME: This isn't the right factoring.  This is here for initial
  // prototyping, but we need a way for analyses to say what expressions they
  // expect to always be CFGElements and then fill in the BuildOptions
  // appropriately.  This is essentially a layering violation.
  if (P.enableCheckUnreachable || P.enableThreadSafetyAnalysis) {
    // Unreachable code analysis and thread safety require a linearized CFG.
    AC.getCFGBuildOptions().setAllAlwaysAdd();
  }
  else {
    AC.getCFGBuildOptions()
      .setAlwaysAdd(Stmt::BinaryOperatorClass)
      .setAlwaysAdd(Stmt::CompoundAssignOperatorClass)
      .setAlwaysAdd(Stmt::BlockExprClass)
      .setAlwaysAdd(Stmt::CStyleCastExprClass)
      .setAlwaysAdd(Stmt::DeclRefExprClass)
      .setAlwaysAdd(Stmt::ImplicitCastExprClass)
      .setAlwaysAdd(Stmt::UnaryOperatorClass)
      .setAlwaysAdd(Stmt::AttributedStmtClass);
  }

  // Construct the analysis context with the specified CFG build options.
  
  // Emit delayed diagnostics.
  if (!fscope->PossiblyUnreachableDiags.empty()) {
    bool analyzed = false;

    // Register the expressions with the CFGBuilder.
    for (SmallVectorImpl<sema::PossiblyUnreachableDiag>::iterator
         i = fscope->PossiblyUnreachableDiags.begin(),
         e = fscope->PossiblyUnreachableDiags.end();
         i != e; ++i) {
      if (const Stmt *stmt = i->stmt)
        AC.registerForcedBlockExpression(stmt);
    }

    if (AC.getCFG()) {
      analyzed = true;
      for (SmallVectorImpl<sema::PossiblyUnreachableDiag>::iterator
            i = fscope->PossiblyUnreachableDiags.begin(),
            e = fscope->PossiblyUnreachableDiags.end();
            i != e; ++i)
      {
        const sema::PossiblyUnreachableDiag &D = *i;
        bool processed = false;
        if (const Stmt *stmt = i->stmt) {
          const CFGBlock *block = AC.getBlockForRegisteredExpression(stmt);
          CFGReverseBlockReachabilityAnalysis *cra =
              AC.getCFGReachablityAnalysis();
          // FIXME: We should be able to assert that block is non-null, but
          // the CFG analysis can skip potentially-evaluated expressions in
          // edge cases; see test/Sema/vla-2.c.
          if (block && cra) {
            // Can this block be reached from the entrance?
            if (cra->isReachable(&AC.getCFG()->getEntry(), block))
              S.Diag(D.Loc, D.PD);
            processed = true;
          }
        }
        if (!processed) {
          // Emit the warning anyway if we cannot map to a basic block.
          S.Diag(D.Loc, D.PD);
        }
      }
    }

    if (!analyzed)
      flushDiagnostics(S, fscope);
  }
  
  
  // Warning: check missing 'return'
  if (P.enableCheckFallThrough) {
    const CheckFallThroughDiagnostics &CD =
      (isa<BlockDecl>(D) ? CheckFallThroughDiagnostics::MakeForBlock()
       : (isa<CXXMethodDecl>(D) &&
          cast<CXXMethodDecl>(D)->getOverloadedOperator() == OO_Call &&
          cast<CXXMethodDecl>(D)->getParent()->isLambda())
            ? CheckFallThroughDiagnostics::MakeForLambda()
            : CheckFallThroughDiagnostics::MakeForFunction(D));
    CheckFallThroughForBody(S, D, Body, blkExpr, CD, AC);
  }

  // Warning: check for unreachable code
  if (P.enableCheckUnreachable) {
    // Only check for unreachable code on non-template instantiations.
    // Different template instantiations can effectively change the control-flow
    // and it is very difficult to prove that a snippet of code in a template
    // is unreachable for all instantiations.
    bool isTemplateInstantiation = false;
    if (const FunctionDecl *Function = dyn_cast<FunctionDecl>(D))
      isTemplateInstantiation = Function->isTemplateInstantiation();
    if (!isTemplateInstantiation)
      CheckUnreachable(S, AC);
  }

  // Check for thread safety violations
  if (P.enableThreadSafetyAnalysis) {
    SourceLocation FL = AC.getDecl()->getLocation();
    SourceLocation FEL = AC.getDecl()->getLocEnd();
    thread_safety::ThreadSafetyReporter Reporter(S, FL, FEL);
    thread_safety::runThreadSafetyAnalysis(AC, Reporter);
    Reporter.emitDiagnostics();
  }

  if (Diags.getDiagnosticLevel(diag::warn_uninit_var, D->getLocStart())
      != DiagnosticsEngine::Ignored ||
      Diags.getDiagnosticLevel(diag::warn_sometimes_uninit_var,D->getLocStart())
      != DiagnosticsEngine::Ignored ||
      Diags.getDiagnosticLevel(diag::warn_maybe_uninit_var, D->getLocStart())
      != DiagnosticsEngine::Ignored) {
    if (CFG *cfg = AC.getCFG()) {
      UninitValsDiagReporter reporter(S);
      UninitVariablesAnalysisStats stats;
      std::memset(&stats, 0, sizeof(UninitVariablesAnalysisStats));
      runUninitializedVariablesAnalysis(*cast<DeclContext>(D), *cfg, AC,
                                        reporter, stats);

      if (S.CollectStats && stats.NumVariablesAnalyzed > 0) {
        ++NumUninitAnalysisFunctions;
        NumUninitAnalysisVariables += stats.NumVariablesAnalyzed;
        NumUninitAnalysisBlockVisits += stats.NumBlockVisits;
        MaxUninitAnalysisVariablesPerFunction =
            std::max(MaxUninitAnalysisVariablesPerFunction,
                     stats.NumVariablesAnalyzed);
        MaxUninitAnalysisBlockVisitsPerFunction =
            std::max(MaxUninitAnalysisBlockVisitsPerFunction,
                     stats.NumBlockVisits);
      }
    }
  }

  bool FallThroughDiagFull =
      Diags.getDiagnosticLevel(diag::warn_unannotated_fallthrough,
                               D->getLocStart()) != DiagnosticsEngine::Ignored;
  bool FallThroughDiagPerFunction =
      Diags.getDiagnosticLevel(diag::warn_unannotated_fallthrough_per_function,
                               D->getLocStart()) != DiagnosticsEngine::Ignored;
  if (FallThroughDiagFull || FallThroughDiagPerFunction) {
    DiagnoseSwitchLabelsFallthrough(S, AC, !FallThroughDiagFull);
  }

  if (S.getLangOpts().ObjCARCWeak &&
      Diags.getDiagnosticLevel(diag::warn_arc_repeated_use_of_weak,
                               D->getLocStart()) != DiagnosticsEngine::Ignored)
    diagnoseRepeatedUseOfWeak(S, fscope, D, AC.getParentMap());

  // Collect statistics about the CFG if it was built.
  if (S.CollectStats && AC.isCFGBuilt()) {
    ++NumFunctionsAnalyzed;
    if (CFG *cfg = AC.getCFG()) {
      // If we successfully built a CFG for this context, record some more
      // detail information about it.
      NumCFGBlocks += cfg->getNumBlockIDs();
      MaxCFGBlocksPerFunction = std::max(MaxCFGBlocksPerFunction,
                                         cfg->getNumBlockIDs());
    } else {
      ++NumFunctionsWithBadCFGs;
    }
  }
}

void clang::sema::AnalysisBasedWarnings::PrintStats() const {
  llvm::errs() << "\n*** Analysis Based Warnings Stats:\n";

  unsigned NumCFGsBuilt = NumFunctionsAnalyzed - NumFunctionsWithBadCFGs;
  unsigned AvgCFGBlocksPerFunction =
      !NumCFGsBuilt ? 0 : NumCFGBlocks/NumCFGsBuilt;
  llvm::errs() << NumFunctionsAnalyzed << " functions analyzed ("
               << NumFunctionsWithBadCFGs << " w/o CFGs).\n"
               << "  " << NumCFGBlocks << " CFG blocks built.\n"
               << "  " << AvgCFGBlocksPerFunction
               << " average CFG blocks per function.\n"
               << "  " << MaxCFGBlocksPerFunction
               << " max CFG blocks per function.\n";

  unsigned AvgUninitVariablesPerFunction = !NumUninitAnalysisFunctions ? 0
      : NumUninitAnalysisVariables/NumUninitAnalysisFunctions;
  unsigned AvgUninitBlockVisitsPerFunction = !NumUninitAnalysisFunctions ? 0
      : NumUninitAnalysisBlockVisits/NumUninitAnalysisFunctions;
  llvm::errs() << NumUninitAnalysisFunctions
               << " functions analyzed for uninitialiazed variables\n"
               << "  " << NumUninitAnalysisVariables << " variables analyzed.\n"
               << "  " << AvgUninitVariablesPerFunction
               << " average variables per function.\n"
               << "  " << MaxUninitAnalysisVariablesPerFunction
               << " max variables per function.\n"
               << "  " << NumUninitAnalysisBlockVisits << " block visits.\n"
               << "  " << AvgUninitBlockVisitsPerFunction
               << " average block visits per function.\n"
               << "  " << MaxUninitAnalysisBlockVisitsPerFunction
               << " max block visits per function.\n";
}
