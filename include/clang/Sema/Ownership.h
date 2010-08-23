//===--- Ownership.h - Parser ownership helpers -----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file contains classes for managing ownership of Stmt and Expr nodes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SEMA_OWNERSHIP_H
#define LLVM_CLANG_SEMA_OWNERSHIP_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/PointerIntPair.h"

//===----------------------------------------------------------------------===//
// OpaquePtr
//===----------------------------------------------------------------------===//

namespace clang {
  class ActionBase;
  class Attr;
  class CXXBaseOrMemberInitializer;
  class CXXBaseSpecifier;
  class Decl;
  class DeclGroupRef;
  class Expr;
  class NestedNameSpecifier;
  class Stmt;
  class TemplateName;
  class TemplateParameterList;

  /// OpaquePtr - This is a very simple POD type that wraps a pointer that the
  /// Parser doesn't know about but that Sema or another client does.  The UID
  /// template argument is used to make sure that "Decl" pointers are not
  /// compatible with "Type" pointers for example.
  template <class PtrTy>
  class OpaquePtr {
    void *Ptr;
    explicit OpaquePtr(void *Ptr) : Ptr(Ptr) {}

  public:
    OpaquePtr() : Ptr(0) {}

    static OpaquePtr make(PtrTy P) {
      OpaquePtr OP; OP.set(P); return OP;
    }

    template <typename T> T* getAs() const {
      return get();
    }

    template <typename T> T getAsVal() const {
      return get();
    }

    PtrTy get() const {
      return llvm::PointerLikeTypeTraits<PtrTy>::getFromVoidPointer(Ptr);
    }

    void set(PtrTy P) {
      Ptr = llvm::PointerLikeTypeTraits<PtrTy>::getAsVoidPointer(P);
    }

    operator bool() const { return Ptr != 0; }

    void *getAsOpaquePtr() const { return Ptr; }
    static OpaquePtr getFromOpaquePtr(void *P) { return OpaquePtr(P); }
  };
}

namespace llvm {
  template <class T>
  class PointerLikeTypeTraits<clang::OpaquePtr<T> > {
  public:
    static inline void *getAsVoidPointer(clang::OpaquePtr<T> P) {
      // FIXME: Doesn't work? return P.getAs< void >();
      return P.getAsOpaquePtr();
    }
    static inline clang::OpaquePtr<T> getFromVoidPointer(void *P) {
      return clang::OpaquePtr<T>::getFromOpaquePtr(P);
    }
    enum { NumLowBitsAvailable = 0 };
  };
}



// -------------------------- About Move Emulation -------------------------- //
// The smart pointer classes in this file attempt to emulate move semantics
// as they appear in C++0x with rvalue references. Since C++03 doesn't have
// rvalue references, some tricks are needed to get similar results.
// Move semantics in C++0x have the following properties:
// 1) "Moving" means transferring the value of an object to another object,
//    similar to copying, but without caring what happens to the old object.
//    In particular, this means that the new object can steal the old object's
//    resources instead of creating a copy.
// 2) Since moving can modify the source object, it must either be explicitly
//    requested by the user, or the modifications must be unnoticeable.
// 3) As such, C++0x moving is only allowed in three contexts:
//    * By explicitly using std::move() to request it.
//    * From a temporary object, since that object cannot be accessed
//      afterwards anyway, thus making the state unobservable.
//    * On function return, since the object is not observable afterwards.
//
// To sum up: moving from a named object should only be possible with an
// explicit std::move(), or on function return. Moving from a temporary should
// be implicitly done. Moving from a const object is forbidden.
//
// The emulation is not perfect, and has the following shortcomings:
// * move() is not in namespace std.
// * move() is required on function return.
// * There are difficulties with implicit conversions.
// * Microsoft's compiler must be given the /Za switch to successfully compile.
//
// -------------------------- Implementation -------------------------------- //
// The move emulation relies on the peculiar reference binding semantics of
// C++03: as a rule, a non-const reference may not bind to a temporary object,
// except for the implicit object parameter in a member function call, which
// can refer to a temporary even when not being const.
// The moveable object has five important functions to facilitate moving:
// * A private, unimplemented constructor taking a non-const reference to its
//   own class. This constructor serves a two-fold purpose.
//   - It prevents the creation of a copy constructor that takes a const
//     reference. Temporaries would be able to bind to the argument of such a
//     constructor, and that would be bad.
//   - Named objects will bind to the non-const reference, but since it's
//     private, this will fail to compile. This prevents implicit moving from
//     named objects.
//   There's also a copy assignment operator for the same purpose.
// * An implicit, non-const conversion operator to a special mover type. This
//   type represents the rvalue reference of C++0x. Being a non-const member,
//   its implicit this parameter can bind to temporaries.
// * A constructor that takes an object of this mover type. This constructor
//   performs the actual move operation. There is an equivalent assignment
//   operator.
// There is also a free move() function that takes a non-const reference to
// an object and returns a temporary. Internally, this function uses explicit
// constructor calls to move the value from the referenced object to the return
// value.
//
// There are now three possible scenarios of use.
// * Copying from a const object. Constructor overload resolution will find the
//   non-const copy constructor, and the move constructor. The first is not
//   viable because the const object cannot be bound to the non-const reference.
//   The second fails because the conversion to the mover object is non-const.
//   Moving from a const object fails as intended.
// * Copying from a named object. Constructor overload resolution will select
//   the non-const copy constructor, but fail as intended, because this
//   constructor is private.
// * Copying from a temporary. Constructor overload resolution cannot select
//   the non-const copy constructor, because the temporary cannot be bound to
//   the non-const reference. It thus selects the move constructor. The
//   temporary can be bound to the implicit this parameter of the conversion
//   operator, because of the special binding rule. Construction succeeds.
//   Note that the Microsoft compiler, as an extension, allows binding
//   temporaries against non-const references. The compiler thus selects the
//   non-const copy constructor and fails, because the constructor is private.
//   Passing /Za (disable extensions) disables this behaviour.
// The free move() function is used to move from a named object.
//
// Note that when passing an object of a different type (the classes below
// have OwningResult and OwningPtr, which should be mixable), you get a problem.
// Argument passing and function return use copy initialization rules. The
// effect of this is that, when the source object is not already of the target
// type, the compiler will first seek a way to convert the source object to the
// target type, and only then attempt to copy the resulting object. This means
// that when passing an OwningResult where an OwningPtr is expected, the
// compiler will first seek a conversion from OwningResult to OwningPtr, then
// copy the OwningPtr. The resulting conversion sequence is:
// OwningResult object -> ResultMover -> OwningResult argument to
// OwningPtr(OwningResult) -> OwningPtr -> PtrMover -> final OwningPtr
// This conversion sequence is too complex to be allowed. Thus the special
// move_* functions, which help the compiler out with some explicit
// conversions.

namespace llvm {
  template<>
  class PointerLikeTypeTraits<clang::ActionBase*> {
    typedef clang::ActionBase* PT;
  public:
    static inline void *getAsVoidPointer(PT P) { return P; }
    static inline PT getFromVoidPointer(void *P) {
      return static_cast<PT>(P);
    }
    enum { NumLowBitsAvailable = 2 };
  };
}

namespace clang {
  // Basic
  class DiagnosticBuilder;

  // Determines whether the low bit of the result pointer for the
  // given UID is always zero. If so, ActionResult will use that bit
  // for it's "invalid" flag.
  template<class Ptr>
  struct IsResultPtrLowBitFree {
    static const bool value = false;
  };

  /// ActionBase - A small part split from Action because of the horrible
  /// definition order dependencies between Action and the smart pointers.
  class ActionBase {
  public:
    /// Out-of-line virtual destructor to provide home for this class.
    virtual ~ActionBase();

    // Types - Though these don't actually enforce strong typing, they document
    // what types are required to be identical for the actions.
    typedef OpaquePtr<DeclGroupRef> DeclGroupPtrTy;
    typedef OpaquePtr<TemplateName> TemplateTy;
    typedef Attr AttrTy;
    typedef CXXBaseSpecifier BaseTy;
    typedef CXXBaseOrMemberInitializer MemInitTy;
    typedef Expr ExprTy;
    typedef Stmt StmtTy;
    typedef TemplateParameterList TemplateParamsTy;
    typedef NestedNameSpecifier CXXScopeTy;
    typedef void TypeTy;  // FIXME: Change TypeTy to use OpaquePtr<N>.

    /// ActionResult - This structure is used while parsing/acting on
    /// expressions, stmts, etc.  It encapsulates both the object returned by
    /// the action, plus a sense of whether or not it is valid.
    /// When CompressInvalid is true, the "invalid" flag will be
    /// stored in the low bit of the Val pointer.
    template<class PtrTy,
             bool CompressInvalid = IsResultPtrLowBitFree<PtrTy>::value>
    class ActionResult {
      PtrTy Val;
      bool Invalid;

    public:
      ActionResult(bool Invalid = false) : Val(PtrTy()), Invalid(Invalid) {}
      ActionResult(PtrTy val) : Val(val), Invalid(false) {}
      ActionResult(const DiagnosticBuilder &) : Val(PtrTy()), Invalid(true) {}

      // These two overloads prevent void* -> bool conversions.
      ActionResult(const void *);
      ActionResult(volatile void *);

      PtrTy get() const { return Val; }
      void set(PtrTy V) { Val = V; }
      bool isInvalid() const { return Invalid; }

      const ActionResult &operator=(PtrTy RHS) {
        Val = RHS;
        Invalid = false;
        return *this;
      }
    };

    // This ActionResult partial specialization places the "invalid"
    // flag into the low bit of the pointer.
    template<typename PtrTy>
    class ActionResult<PtrTy, true> {
      // A pointer whose low bit is 1 if this result is invalid, 0
      // otherwise.
      uintptr_t PtrWithInvalid;
      typedef llvm::PointerLikeTypeTraits<PtrTy> PtrTraits;
    public:
      ActionResult(bool Invalid = false)
        : PtrWithInvalid(static_cast<uintptr_t>(Invalid)) { }

      ActionResult(PtrTy V) {
        void *VP = PtrTraits::getAsVoidPointer(V);
        PtrWithInvalid = reinterpret_cast<uintptr_t>(VP);
        assert((PtrWithInvalid & 0x01) == 0 && "Badly aligned pointer");
      }

      // These two overloads prevent void* -> bool conversions.
      ActionResult(const void *);
      ActionResult(volatile void *);

      ActionResult(const DiagnosticBuilder &) : PtrWithInvalid(0x01) { }

      PtrTy get() const {
        void *VP = reinterpret_cast<void *>(PtrWithInvalid & ~0x01);
        return PtrTraits::getFromVoidPointer(VP);
      }

      void set(PtrTy V) {
        void *VP = PtrTraits::getAsVoidPointer(V);
        PtrWithInvalid = reinterpret_cast<uintptr_t>(VP);
        assert((PtrWithInvalid & 0x01) == 0 && "Badly aligned pointer");
      }

      bool isInvalid() const { return PtrWithInvalid & 0x01; }

      const ActionResult &operator=(PtrTy RHS) {
        void *VP = PtrTraits::getAsVoidPointer(RHS);
        PtrWithInvalid = reinterpret_cast<uintptr_t>(VP);
        assert((PtrWithInvalid & 0x01) == 0 && "Badly aligned pointer");
        return *this;
      }
    };

    /// Deletion callbacks - Since the parser doesn't know the concrete types of
    /// the AST nodes being generated, it must do callbacks to delete objects
    /// when recovering from errors. These are in ActionBase because the smart
    /// pointers need access to them.
    virtual void DeleteExpr(ExprTy *E) {}
    virtual void DeleteStmt(StmtTy *S) {}
    virtual void DeleteTemplateParams(TemplateParamsTy *P) {}
  };

  /// ASTOwningResult - A moveable smart pointer for AST nodes that also
  /// has an extra flag to indicate an additional success status.
  template <typename PtrTy> class ASTOwningResult;

  /// ASTMultiPtr - A moveable smart pointer to multiple AST nodes. Only owns
  /// the individual pointers, not the array holding them.
  template <typename PtrTy> class ASTMultiPtr;

  template <class PtrTy> class ASTOwningResult {
  public:
    typedef ActionBase::ActionResult<PtrTy> DumbResult;

  private:
    DumbResult Result;

  public:
    explicit ASTOwningResult(bool invalid = false)
      : Result(invalid) { }
    ASTOwningResult(PtrTy node) : Result(node) { }
    ASTOwningResult(const DumbResult &res) : Result(res) { }
    // Normal copying semantics are defined implicitly.

    // These two overloads prevent void* -> bool conversions.
    explicit ASTOwningResult(const void *);
    explicit ASTOwningResult(volatile void *);

    /// Assignment from a raw pointer.
    ASTOwningResult &operator=(PtrTy raw) {
      Result = raw;
      return *this;
    }

    /// Assignment from an ActionResult.
    ASTOwningResult &operator=(const DumbResult &res) {
      Result = res;
      return *this;
    }

    bool isInvalid() const { return Result.isInvalid(); }

    /// Does this point to a usable AST node? To be usable, the node
    /// must be valid and non-null.
    bool isUsable() const { return !Result.isInvalid() && get(); }

    /// It is forbidden to call either of these methods on an invalid
    /// pointer.  We should assert that, but we're not going to,
    /// because it's likely to trigger it unpredictable ways on
    /// invalid code.
    PtrTy get() const { return Result.get(); }
    PtrTy take() const { return get(); }

    /// Take outside ownership of the raw pointer and cast it down.
    template<typename T>
    T *takeAs() { return static_cast<T*>(get()); }

    /// Alias for interface familiarity with unique_ptr.
    PtrTy release() { return take(); }

    /// Pass ownership to a classical ActionResult.
    DumbResult result() { return Result; }
  };

  template <class PtrTy>
  class ASTMultiPtr {
    PtrTy *Nodes;
    unsigned Count;

  public:
    // Normal copying implicitly defined
    explicit ASTMultiPtr(ActionBase &) : Nodes(0), Count(0) {}
    ASTMultiPtr(ActionBase &, PtrTy *nodes, unsigned count)
      : Nodes(nodes), Count(count) {}
    // Fake mover in Parse/AstGuard.h needs this:
    ASTMultiPtr(PtrTy *nodes, unsigned count) : Nodes(nodes), Count(count) {}

    /// Access to the raw pointers.
    PtrTy *get() const { return Nodes; }

    /// Access to the count.
    unsigned size() const { return Count; }

    PtrTy *release() {
      return Nodes;
    }
  };

  class ParsedTemplateArgument;
    
  class ASTTemplateArgsPtr {
    ParsedTemplateArgument *Args;
    mutable unsigned Count;

  public:
    ASTTemplateArgsPtr(ActionBase &actions, ParsedTemplateArgument *args,
                       unsigned count) :
      Args(args), Count(count) { }

    // FIXME: Lame, not-fully-type-safe emulation of 'move semantics'.
    ASTTemplateArgsPtr(ASTTemplateArgsPtr &Other) :
      Args(Other.Args), Count(Other.Count) {
    }

    // FIXME: Lame, not-fully-type-safe emulation of 'move semantics'.
    ASTTemplateArgsPtr& operator=(ASTTemplateArgsPtr &Other)  {
      Args = Other.Args;
      Count = Other.Count;
      return *this;
    }

    ParsedTemplateArgument *getArgs() const { return Args; }
    unsigned size() const { return Count; }

    void reset(ParsedTemplateArgument *args, unsigned count) {
      Args = args;
      Count = count;
    }

    const ParsedTemplateArgument &operator[](unsigned Arg) const;

    ParsedTemplateArgument *release() const {
      return Args;
    }
  };

  /// \brief A small vector that owns a set of AST nodes.
  template <class PtrTy, unsigned N = 8>
  class ASTOwningVector : public llvm::SmallVector<PtrTy, N> {
    ASTOwningVector(ASTOwningVector &); // do not implement
    ASTOwningVector &operator=(ASTOwningVector &); // do not implement

  public:
    explicit ASTOwningVector(ActionBase &Actions)
    { }

    PtrTy *take() {
      return &this->front();
    }

    template<typename T> T **takeAs() { return reinterpret_cast<T**>(take()); }
  };

  /// A SmallVector of statements, with stack size 32 (as that is the only one
  /// used.)
  typedef ASTOwningVector<Stmt*, 32> StmtVector;
  /// A SmallVector of expressions, with stack size 12 (the maximum used.)
  typedef ASTOwningVector<Expr*, 12> ExprVector;

  template <class T, unsigned N> inline
  ASTMultiPtr<T> move_arg(ASTOwningVector<T, N> &vec) {
    return ASTMultiPtr<T>(vec.take(), vec.size());
  }

  // These versions are hopefully no-ops.
  template <class T> inline
  ASTOwningResult<T> move(ASTOwningResult<T> &ptr) {
    return ptr;
  }

  template <class T> inline
  ASTMultiPtr<T>& move(ASTMultiPtr<T> &ptr) {
    return ptr;
  }

  // We can re-use the low bit of expression, statement, base, and
  // member-initializer pointers for the "invalid" flag of
  // ActionResult.
  template<> struct IsResultPtrLowBitFree<Expr*> {
    static const bool value = true;
  };
  template<> struct IsResultPtrLowBitFree<Stmt*> {
    static const bool value = true;
  };
  template<> struct IsResultPtrLowBitFree<CXXBaseSpecifier*> {
    static const bool value = true;
  };
  template<> struct IsResultPtrLowBitFree<CXXBaseOrMemberInitializer*> {
    static const bool value = true;
  };

  typedef ActionBase::ActionResult<Expr*> ExprResult;
  typedef ActionBase::ActionResult<Stmt*> StmtResult;
  typedef ActionBase::ActionResult<void*> TypeResult;
  typedef ActionBase::ActionResult<CXXBaseSpecifier*> BaseResult;
  typedef ActionBase::ActionResult<CXXBaseOrMemberInitializer*> MemInitResult;

  typedef ActionBase::ActionResult<Decl*> DeclResult;
  typedef OpaquePtr<TemplateName> ParsedTemplateTy;

  typedef ASTOwningResult<Expr*> OwningExprResult;
  typedef ASTOwningResult<Stmt*> OwningStmtResult;

  inline Expr *move(Expr *E) { return E; }
  inline Stmt *move(Stmt *S) { return S; }

  typedef ASTMultiPtr<Expr*> MultiExprArg;
  typedef ASTMultiPtr<TemplateParameterList*> MultiTemplateParamsArg;

  inline Expr *AssertSuccess(OwningExprResult R) {
    assert(!R.isInvalid() && "operation was asserted to never fail!");
    return R.get();
  }

  inline Stmt *AssertSuccess(OwningStmtResult R) {
    assert(!R.isInvalid() && "operation was asserted to never fail!");
    return R.get();
  }
}

#endif
