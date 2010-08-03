//===--- SemaExpr.cpp - Swift Expression Semantic Analysis ----------------===//
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
//
//  This file implements semantic analysis for Swift expressions.
//
//===----------------------------------------------------------------------===//

#include "swift/Sema/SemaExpr.h"
#include "swift/Sema/Sema.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExprVisitor.h"
#include "swift/AST/Type.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SMLoc.h"
using namespace swift;
using llvm::NullablePtr;

//===----------------------------------------------------------------------===//
// Expression Semantic Analysis Routines
//===----------------------------------------------------------------------===//

// Each expression node has an implementation here, which takes the properties
// of the expression, validates it and computes a result type.  This is shared
// logic between the Sema Action implementations and type inferencing code.
//
// These each produce diagnostics and return true on error.  On success, they
// may mutate the input values, then return false.


static bool SemaIntegerLiteral(Type *&ResultTy, SemaExpr &SE) {
  ResultTy = SE.S.Context.IntType;
  return false;
}

static bool SemaDeclRefExpr(NamedDecl *D, llvm::SMLoc Loc, Type *&ResultTy,
                            SemaExpr &SE) {
  if (D == 0) {
    SE.Error(Loc, "use of undeclared identifier");
    return true;
  }
  
  // TODO: QOI: If the decl had an "invalid" bit set, then return the error
  // object to improve error recovery.
  
  ResultTy = D->Ty;
  return false;
}

static bool SemaBraceExpr(llvm::SMLoc LBLoc, 
                          llvm::PointerUnion<Expr*, NamedDecl*> *Elements,
                          unsigned NumElements,
                          bool HasMissingSemi, llvm::SMLoc RBLoc, 
                          Type *&ResultTy, SemaExpr &SE) {
  // If any of the elements of the braces has a function type (which indicates
  // that a function didn't get called), then produce an error.  We don't do
  // this for the last element in the 'missing semi' case, because the brace
  // expr as a whole has the function result.
  // TODO: What about tuples which contain functions by-value that are dead?
  for (unsigned i = 0; i != NumElements-(HasMissingSemi ? 1 : 0); ++i)
    if (Elements[i].is<Expr*>() &&
        llvm::isa<FunctionType>(Elements[i].get<Expr*>()->Ty))
      // TODO: QOI: Add source range.
      SE.Error(Elements[i].get<Expr*>()->getLocStart(),
               "expression resolves to an unevaluated function");
  
  if (HasMissingSemi)
    ResultTy = Elements[NumElements-1].get<Expr*>()->Ty;
  else
    ResultTy = SE.S.Context.VoidType;
  
  return false;
}


static bool SemaTupleExpr(llvm::SMLoc LPLoc, Expr **SubExprs,
                          unsigned NumSubExprs, llvm::SMLoc RPLoc,
                          Type *&ResultTy, SemaExpr &SE) {
  // A tuple expr with a single subexpression is just a grouping paren.
  if (NumSubExprs == 1) {
    ResultTy = SubExprs[0]->Ty;
    return false;
  }
  
  // Compute the result type.
  llvm::SmallVector<TupleType::TypeOrDecl, 8> ResultTyElts;
  
  for (unsigned i = 0, e = NumSubExprs; i != e; ++i)
    ResultTyElts.push_back(SubExprs[i]->Ty);
  
  ResultTy = SE.S.Context.getTupleType(ResultTyElts.data(), NumSubExprs);
  return false;
}

static bool SemaApplyExpr(Expr *&E1, Expr *&E2, Type *&ResultTy, SemaExpr &SE) {
  if (E1->Ty->Dependent) {
    ResultTy = SE.S.Context.DependentType;
    return false;
  }
  
  // If this expression in the sequence is just a floating value that isn't
  // a function, then we have a discarded value, such as "4 5".  Just return
  // true so that it gets properly sequenced.
  FunctionType *FT = llvm::dyn_cast<FunctionType>(E1->Ty);
  if (FT == 0) {
    SE.Error(E1->getLocStart(),
             "function application requires value of function type");
    return true;
  }
  
  // Otherwise, we do have a function application.  Check that the argument
  // type matches the expected type of the function.
  E2 = SE.ConvertToType(E2, FT->Input, false, SemaExpr::CR_FuncApply);
  if (E2 == 0)
    return true;
  
  ResultTy = FT->Result;
  return false;
}

/// SemaSequenceExpr - Perform semantic analysis for sequence expressions.
static bool SemaSequenceExpr(Expr **Elements, unsigned NumElements,
                             Type *&ResultTy, SemaExpr &SE) {
  ResultTy = Elements[NumElements-1]->Ty;
  return false;
}

/// SemaBinaryExpr - Perform semantic analysis of binary expressions.
static bool SemaBinaryExpr(Expr *&LHS, NamedDecl *OpFn,
                           llvm::SMLoc OpLoc, Expr *&RHS, Type *&ResultTy,
                           SemaExpr &SE) {
  if (LHS->Ty->Dependent || RHS->Ty->Dependent) {
    ResultTy = SE.S.Context.DependentType;
    return false; 
  }
  
  // Parser verified that OpFn has an Infix Precedence.  Sema verified that OpFn
  // only has InfixPrecedence if it takes a 2 element tuple as input.
  assert(OpFn->Attrs.InfixPrecedence != -1 &&
         "Sema and parser should verify that only binary predicates are used"); 
  FunctionType *FnTy = llvm::cast<FunctionType>(OpFn->Ty);
  TupleType *Input = llvm::cast<TupleType>(FnTy->Input);
  assert(Input->NumFields == 2 && "Sema error validating infix fn type");
  
  // Verify that the LHS/RHS have the right type and do conversions as needed.
  LHS = SE.ConvertToType(LHS, Input->getElementType(0), false,
                         SemaExpr::CR_BinOpLHS);
  if (LHS == 0) return true;
  
  RHS = SE.ConvertToType(RHS, Input->getElementType(1), false,
                         SemaExpr::CR_BinOpRHS);
  if (RHS == 0) return true;
  
  ResultTy = FnTy->Result;
  return false;
}

//===----------------------------------------------------------------------===//
// Action Implementations
//===----------------------------------------------------------------------===//

NullablePtr<Expr> SemaExpr::ActOnNumericConstant(llvm::StringRef Text,
                                                 llvm::SMLoc Loc) {
  Type *ResultTy = 0;
  if (SemaIntegerLiteral(ResultTy, *this)) return 0;
  return new (S.Context) IntegerLiteral(Text, Loc, ResultTy);
}

NullablePtr<Expr> 
SemaExpr::ActOnIdentifierExpr(llvm::StringRef Text, llvm::SMLoc Loc) {
  NamedDecl *D = S.decl.LookupName(S.Context.getIdentifier(Text));
  
  // If this identifier is _0 -> _9, then it is a use of an implicit anonymous
  // closure argument.
  if (D == 0 && Text.size() == 2 &&
      Text[0] == '_' && Text[1] >= '0' && Text[1] <= '9')
    D = S.decl.GetAnonDecl(Text, Loc);
  
  Type *ResultTy = 0;
  if (SemaDeclRefExpr(D, Loc, ResultTy, *this)) return 0;
  
  return new (S.Context) DeclRefExpr(D, Loc, D->Ty);
}

NullablePtr<Expr> 
SemaExpr::ActOnBraceExpr(llvm::SMLoc LBLoc,
                         const llvm::PointerUnion<Expr*, NamedDecl*> *Elements,
                         unsigned NumElements, bool HasMissingSemi,
                         llvm::SMLoc RBLoc) {
  llvm::PointerUnion<Expr*, NamedDecl*> *NewElements = 
  (llvm::PointerUnion<Expr*, NamedDecl*> *)
  S.Context.Allocate(sizeof(*Elements)*NumElements, 8);
  memcpy(NewElements, Elements, sizeof(*Elements)*NumElements);
  
  Type *ResultTy = 0;
  if (SemaBraceExpr(LBLoc, NewElements, NumElements, HasMissingSemi, RBLoc,
                    ResultTy, *this))
    return 0;
  
  return new (S.Context) BraceExpr(LBLoc, NewElements, NumElements,
                                   HasMissingSemi, RBLoc, ResultTy);
}

NullablePtr<Expr> 
SemaExpr::ActOnTupleExpr(llvm::SMLoc LPLoc, Expr **SubExprs,
                         unsigned NumSubExprs, llvm::SMLoc RPLoc) {
  
  Expr **NewSubExprs = (Expr**)S.Context.Allocate(sizeof(Expr*)*NumSubExprs, 8);
  memcpy(NewSubExprs, SubExprs, sizeof(Expr*)*NumSubExprs);
  
  Type *ResultTy = 0;
  if (SemaTupleExpr(LPLoc, NewSubExprs, NumSubExprs, RPLoc, ResultTy, *this))
    return 0;
  
  return new (S.Context) TupleExpr(LPLoc, NewSubExprs, NumSubExprs,
                                   RPLoc, ResultTy);
}

/// ActOnJuxtaposition - This is invoked whenever the parser sees a
/// juxtaposition operation.  If the two expressions can be applied, this
/// returns the new expression (which can be null) and false, otherwise it
/// return null and true to indicate that they are sequenced.
llvm::PointerIntPair<Expr*, 1, bool>
SemaExpr::ActOnJuxtaposition(Expr *E1, Expr *E2) {
  // If this expression in the sequence is just a floating value that isn't
  // a function, then we have a discarded value, such as "4 5".  Just return
  // true so that it gets properly sequenced.  If the input is a dependent type,
  // we assume that it is a function so that (_0 1 2 3) binds correctly.  If it
  // is not a function, parens or ; can be used to disambiguate.
  if (!llvm::isa<FunctionType>(E1->Ty) && E1->Ty->Kind != BuiltinDependentKind)
    return llvm::PointerIntPair<Expr*, 1, bool>(0, true);
  
  // Okay, we have a function application, analyze it.
  Type *ResultTy = 0;
  if (SemaApplyExpr(E1, E2, ResultTy, *this))
    return llvm::PointerIntPair<Expr*, 1, bool>(0, false);
  
  E1 = new (S.Context) ApplyExpr(E1, E2, ResultTy);
  return llvm::PointerIntPair<Expr*, 1, bool>(E1, false);
}


llvm::NullablePtr<Expr>
SemaExpr::ActOnSequence(Expr **Elements, unsigned NumElements) {
  assert(NumElements != 0 && "Empty sequence isn't possible");
  
  Expr **NewElements =(Expr**)
  S.Context.Allocate(sizeof(*NewElements)*NumElements, 8);
  memcpy(NewElements, Elements, sizeof(*NewElements)*NumElements);
  
  Type *ResultTy = 0;
  if (SemaSequenceExpr(NewElements, NumElements, ResultTy, *this)) return 0;
  
  return new (S.Context) SequenceExpr(NewElements, NumElements, ResultTy);
}

NullablePtr<Expr> 
SemaExpr::ActOnBinaryExpr(Expr *LHS, NamedDecl *OpFn,
                          llvm::SMLoc OpLoc, Expr *RHS) {
  Type *ResultTy = 0;
  if (SemaBinaryExpr(LHS, OpFn, OpLoc, RHS, ResultTy, *this)) return 0;
  
  return new (S.Context) BinaryExpr(LHS, OpFn, OpLoc, RHS, ResultTy);
}

//===----------------------------------------------------------------------===//
// Expression Reanalysis - SemaExpressionTree
//===----------------------------------------------------------------------===//


namespace {
  /// SemaExpressionTree - This class implements top-down (aka "leaf to root",
  /// analyzing 1 and 4 before the + in "1+4") semantic analysis of an
  /// already-existing expression tree.  This is performed when a closure is
  /// formed and anonymous decls like "_4" get a concrete type associated with
  /// them.  During the initial parse, these decls get a 'dependent' type, which
  /// disables most semantic analysis associated with them.
  ///
  /// When the expression tree is bound to a context, the anonymous decls get a
  /// concrete type and we have to rescan the tree to assign types to
  /// intermediate nodes, introduce type coercion etc.  This visitor does this
  /// job.  Each visit method reanalyzes the children of a node, then reanalyzes
  /// the node, and returns true on error.
  class SemaExpressionTree : public ExprVisitor<SemaExpressionTree, bool> {
    friend class ExprVisitor<SemaExpressionTree, bool>;
    SemaExpr &SE;
    
    bool VisitIntegerLiteral(IntegerLiteral *E) {
      return SemaIntegerLiteral(E->Ty, SE);
    }
    bool VisitDeclRefExpr(DeclRefExpr *E) {
      return SemaDeclRefExpr(E->D, E->Loc, E->Ty, SE);
    }
    bool VisitTupleExpr(TupleExpr *E) {
      for (unsigned i = 0, e = E->NumSubExprs; i != e; ++i)
        if (Visit(E->SubExprs[i]))
          return true;
      
      return SemaTupleExpr(E->LParenLoc, E->SubExprs, E->NumSubExprs,
                           E->RParenLoc, E->Ty, SE);
    }
    bool VisitTupleConvertExpr(TupleConvertExpr *E) {
      // The type of the tuple wouldn't be dependent to get this node.
      assert(!E->Ty->Dependent);
      return Visit(E->SubExpr);
    }
    bool VisitApplyExpr(ApplyExpr *E) {
      if (Visit(E->Fn) || Visit(E->Arg))
        return true;
      
      return SemaApplyExpr(E->Fn, E->Arg, E->Ty, SE);
    }
    bool VisitSequenceExpr(SequenceExpr *E) {
      for (unsigned i = 0, e = E->NumElements; i != e; ++i)
        if (Visit(E->Elements[i]))
          return true;
      
      return SemaSequenceExpr(E->Elements, E->NumElements, E->Ty, SE);
    }
    bool VisitBraceExpr(BraceExpr *E) {
      for (unsigned i = 0, e = E->NumElements; i != e; ++i)
        if (Expr *SubExpr = E->Elements[i].dyn_cast<Expr*>()) {
          if (Visit(SubExpr)) return true;
        } else if (Expr *Init = E->Elements[i].get<NamedDecl*>()->Init) {
          if (Visit(Init)) return true;
          E->Elements[i].get<NamedDecl*>()->Ty = Init->Ty;
        }
      
      return SemaBraceExpr(E->LBLoc, E->Elements, E->NumElements,
                           E->MissingSemi, E->RBLoc, E->Ty, SE);
    }
    bool VisitClosureExpr(ClosureExpr *E) {
      // Anon-decls within a nested closure will have already been resolved, so
      // we don't need to recurse into it.  This also prevents N^2 re-sema
      // activity with lots of nested closures.
      return false;      
    }
    bool VisitBinaryExpr(BinaryExpr *E) {
      if (Visit(E->LHS) || Visit(E->RHS))
        return true;
      
      return SemaBinaryExpr(E->LHS, E->Fn, E->OpLoc, E->RHS, E->Ty, SE);
    }
    
  public:
    SemaExpressionTree(SemaExpr &se) : SE(se) {}
    bool doIt(Expr *E) {
      return Visit(E);
    }
  };
} // end anonymous namespace.


//===----------------------------------------------------------------------===//
// Expression Type Coercing - CoerceDependentResultToType
//===----------------------------------------------------------------------===//

// Don't need bottom-up type propagation until we get generics or start caring
// about diagnostics QoI.
#if 0

/// CoerceDependentResultToType - This visitor is used when an expression with
/// dependent type is used in a context which forces the type to a known Type.
/// Do this and return a new expression.  On error, diagnose the problem
/// and return null. 
namespace {
  
class CoerceDependentResultToType
    : public ExprVisitor<CoerceDependentResultToType, Expr*> {
  friend class ExprVisitor<CoerceDependentResultToType, Expr*>;
  Type *DestTy;
  SemaExpr::ConversionReason Reason;
  SemaExpr &SE;
  
  Expr *VisitIntegerLiteral(IntegerLiteral *E) {
    assert(0 && "Integer literal shouldn't have dependent type");
    return E;
  }
  Expr *VisitDeclRefExpr(DeclRefExpr *E) {
    assert(llvm::isa<AnonDecl>(E->D) &&
           "Only anondecls can have dependent type");
    AnonDecl *AD = llvm::cast<AnonDecl>(E->D);

    // FIXME: Really want a 'merge types' routine, that can handle paritally
    // specified types if DestTy can be dependent. (i32, ?)  

    // If the AnonDecl has Dependent type, give it and the DeclRefExpr the
    // inferred type!
    if (AD->Ty->Kind == BuiltinDependentKind) {
      E->Ty = AD->Ty = DestTy;
      return E;
    }
    
    // If we already inferred a type for the AnonDecl and it matches the
    // inferred type, just update the DeclRefExpr to match.
    if (AD->Ty == DestTy) {
      E->Ty = DestTy;
      return E;
    }
    
    // The same AnonDecl may be used in multiple places in an expression tree,
    // and those uses may cause it to be inferred to have different types.  If
    // this happens, diagnose the problem.
    SE.Error(E->Loc,
             "anonymous declaration inferred to have two different types");
    return E;
  }
  Expr *VisitTupleExpr(TupleExpr *E) {
    // Handle the grouping paren case.
    if (E->NumSubExprs == 1) {
      Expr *SubExpr = doIt(E->SubExprs[0]);
      if (SubExpr == 0) return 0;
      E->SubExprs[0] = SubExpr;
      E->Ty = SubExpr->Ty;
      return E;
    }
    
    assert(0 && "FIXME: Handle tuple with dependent type");
    return E;
  }
  Expr *VisitApplyExpr(ApplyExpr *E) {
    assert(0 && "FIXME: Handle apply with dependent type");
    return E;
  }
  Expr *VisitSequenceExpr(SequenceExpr *E) {
    Expr *SubExpr = doIt(E->Elements[E->NumElements-1]);
    if (SubExpr == 0) return 0;
    E->Elements[E->NumElements-1] = SubExpr;
    E->Ty = SubExpr->Ty;
    return E;
  }
  Expr *VisitBraceExpr(BraceExpr *E) {
    assert(E->MissingSemi && "Expression should have void type!");
    Expr *SubExpr = doIt(E->Elements[E->NumElements-1].get<Expr*>());
    if (SubExpr == 0) return 0;
    E->Elements[E->NumElements-1] = SubExpr;
    E->Ty = SubExpr->Ty;
    return E;
  }
  Expr *VisitClosureExpr(ClosureExpr *E) {
    assert(0 && "FIXME: Handle closure with dependent type");
    return E;
  }
  Expr *VisitBinaryExpr(BinaryExpr *E) {
    assert(0 && "FIXME: Handle binaryexpr with dependent type");
    return E;
  }

public:
  CoerceDependentResultToType(Type *destty, SemaExpr::ConversionReason reason,
                              SemaExpr &se)
    : DestTy(destty), Reason(reason), SE(se) {
  }
  Expr *doIt(Expr *E) {
    assert(E->Ty->Dependent && "Expr doesn't have dependent type");
    return Visit(E);
  }
};
  
} // end anonymous namespace.

#endif



//===----------------------------------------------------------------------===//
// Utility Functions
//===----------------------------------------------------------------------===//


/// BindAndValidateClosureArgs - The specified list of anonymous closure
/// arguments was bound to a closure function with the specified input
/// arguments.  Validate the argument list and, if valid, allocate and return
/// a pointer to the argument to be used for the ClosureExpr.
static llvm::NullablePtr<AnonDecl> *
BindAndValidateClosureArgs(Expr *Body, Type *FuncInput, SemaDecl &SD) {
  const llvm::NullablePtr<AnonDecl> *AnonArgs = SD.AnonClosureArgs.data();
  unsigned NumAnonArgs = SD.AnonClosureArgs.size();
  
  // If the input to the function is a non-tuple, only _0 is valid, if it is a
  // tuple, then _0.._N are valid depending on the number of inputs to the
  // tuple.
  unsigned NumInputArgs = 1;
  if (TupleType *TT = llvm::dyn_cast<TupleType>(FuncInput))
    NumInputArgs = TT->NumFields;
  
  // Verify that the code didn't use too many anonymous arguments, e.g. using _4
  // when the bound function only has 2 inputs.
  if (NumAnonArgs > NumInputArgs) {
    for (unsigned i = NumInputArgs; i != NumAnonArgs; ++i) {
      // Ignore elements not used.
      if (AnonArgs[i].isNull()) continue;
      
      SD.Error(AnonArgs[i].get()->UseLoc,
               "use of invalid anonymous argument, with number higher than"
               " # arguments to bound function");
    }
    NumAnonArgs = NumInputArgs;
  }
  
  // Return the right number of inputs.
  llvm::NullablePtr<AnonDecl> *NewInputs =(llvm::NullablePtr<AnonDecl>*)
    SD.S.Context.Allocate(sizeof(*NewInputs)*NumInputArgs, 8);
  for (unsigned i = 0, e = NumInputArgs; i != e; ++i)
    if (i < NumAnonArgs)
      NewInputs[i] = AnonArgs[i];
    else
      NewInputs[i] = 0;
  
  // Assign the AnonDecls their actual concrete types now that we know the
  // context they are being used in.
  if (TupleType *TT = llvm::dyn_cast<TupleType>(FuncInput)) {
    for (unsigned i = 0, e = NumAnonArgs; i != e; ++i) {
      if (NewInputs[i].isNull()) continue;
      NewInputs[i].get()->Ty = TT->getElementType(i);
    }
  } else if (NumInputArgs) {
    assert(NumInputArgs == 1 && NewInputs[0].isNonNull() &&
           "Must have unary case");
    assert(NewInputs[0].get()->Ty->Kind == BuiltinDependentKind &&
           "AnonDecl doesn't have dependent type?");
    NewInputs[0].get()->Ty = FuncInput;
  }
  
  // Now that the AnonDecls have a type applied to them, redo semantic analysis
  // from the leaves of the expression tree up.
  if (SemaExpressionTree(SD.S.expr).doIt(Body))
    return 0;
  
  // We used/consumed the anonymous closure arguments.
  SD.AnonClosureArgs.clear();
  return NewInputs;
}

/// HandleConversionToType - This is the recursive implementation of
/// ConvertToType.  It does not produce diagnostics, it just returns null on
/// failure.
static Expr *HandleConversionToType(Expr *E, Type *DestTy, bool IgnoreAnonDecls,
                                    SemaExpr &SE) {
  Type *ETy = E->Ty;
  
  // If we have an exact match, we're done.
  if (ETy == DestTy) return E;
  
  assert(!DestTy->Dependent && "Result of conversion can't be dependent");
  
  if (TupleType *TT = llvm::dyn_cast<TupleType>(DestTy)) {
    // If the destination is a tuple type with a single element, see if the
    // expression's type is convertable to the element type.
    if (TT->NumFields == 1) {
      if (Expr *ERes = HandleConversionToType(E, TT->getElementType(0),
                                              false, SE)) {
        // Must allocate space for the AST node.
        Expr **NewSE = (Expr**)SE.S.Context.Allocate(sizeof(Expr*), 8);
        NewSE[0] = ERes;
        return new (SE.S.Context) TupleExpr(llvm::SMLoc(), NewSE, 1,
                                            llvm::SMLoc(), DestTy);
      }
    }

    // If the input is a tuple and the output is a tuple with the same number of
    // elements, see if we can convert each element.
    // FIXME: Do this for "funcdecl4(funcdecl3(), 12);"
    // FIXME: Do this for dependent types, to resolve: foo(_0, 4);
    
    // Handle reswizzling elements based on names, handle default values.
    if (llvm::isa<TupleType>(ETy) &&
        TT->NumFields == llvm::cast<TupleType>(ETy)->NumFields) {
      // FIXME: Handle default args etc.
      return new (SE.S.Context) TupleConvertExpr(E, DestTy);
    }
  }
  
  // Otherwise, check to see if this is an auto-closure case.  This case happens
  // when we convert an expression E to a function type whose result is E's
  // type.
  if (FunctionType *FT = llvm::dyn_cast<FunctionType>(DestTy)) {
    // If there are any live anonymous closure arguments, this level will use
    // them and remove them.  When binding something like _0+_1 to
    // (int,int)->(int,int)->() the arguments bind to the first level, not the
    // inner level.  To handle this, we ignore anonymous decls in the recursive
    // case here.
    if (Expr *ERes = HandleConversionToType(E, FT->Result, true, SE)) {
      // If we bound any anonymous closure arguments, validate them and resolve
      // their types.
      llvm::NullablePtr<AnonDecl> *ActualArgList = 0;
      if (!IgnoreAnonDecls && !SE.S.decl.AnonClosureArgs.empty()) {
        ActualArgList = BindAndValidateClosureArgs(ERes, FT->Input, SE.S.decl);
        if (ActualArgList == 0)
          return 0;
      }
      return new (SE.S.Context) ClosureExpr(ERes, ActualArgList, DestTy);
    }
  }
  
  // When we want to do early bottom-up inference, this can be enabled
  // and fleshed out.
#if 0
  // If E has dependent type, then this resolves the type: propagate the type
  // information into the subtree.
  if (E->Ty->Dependent)
    return CoerceDependentResultToType(OrigDestTy, Reason, *this).doIt(E);
#endif
  
  // If this has a dependent type, just return the expression, the nested
  // subexpression arguments will be bound later and the expression will be
  // re-sema'd.
  if (E->Ty->Dependent)
    return E;
  
  // Could not do the conversion.
  return 0;
}



Expr *SemaExpr::ConvertToType(Expr *E, Type *DestTy, bool IgnoreAnonDecls,
                              ConversionReason Reason) {
  if (Expr *ERes = HandleConversionToType(E, DestTy, IgnoreAnonDecls, *this))
    return ERes;
  
  // FIXME: We only have this because diagnostics are terrible right now.
  E->dump();
  DestTy->dump();
  
  // TODO: QOI: Source ranges + print the type.
  switch (Reason) {
  case CR_BinOpLHS:
    Error(E->getLocStart(), "left hand side of binary operator has wrong type");
    break;
  case CR_BinOpRHS: 
    Error(E->getLocStart(),"right hand side of binary operator has wrong type");
    break;
  case CR_FuncApply:
    Error(E->getLocStart(), "argument to function invocation has wrong type");
    break;
  case CR_VarInit:
    Error(E->getLocStart(),
          "explicitly specified type doesn't match initializer expression");
    break;
  case CR_FuncBody:
    Error(E->getLocStart(),
          "type of func body doesn't match specified prototype");
    break;
  }
  
  return 0;
}

