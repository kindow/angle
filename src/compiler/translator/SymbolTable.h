//
// Copyright (c) 2002-2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#ifndef COMPILER_TRANSLATOR_SYMBOLTABLE_H_
#define COMPILER_TRANSLATOR_SYMBOLTABLE_H_

//
// Symbol table for parsing.  Has these design characteristics:
//
// * Same symbol table can be used to compile many shaders, to preserve
//   effort of creating and loading with the large numbers of built-in
//   symbols.
//
// * Name mangling will be used to give each function a unique name
//   so that symbol table lookups are never ambiguous.  This allows
//   a simpler symbol table structure.
//
// * Pushing and popping of scope, so symbol table will really be a stack
//   of symbol tables.  Searched from the top, with new inserts going into
//   the top.
//
// * Constants:  Compile time constant symbols will keep their values
//   in the symbol table.  The parser can substitute constants at parse
//   time, including doing constant folding and constant propagation.
//
// * No temporaries:  Temporaries made from operations (+, --, .xy, etc.)
//   are tracked in the intermediate representation, not the symbol table.
//

#include <array>
#include <assert.h>
#include <set>

#include "common/angleutils.h"
#include "compiler/translator/ExtensionBehavior.h"
#include "compiler/translator/InfoSink.h"
#include "compiler/translator/IntermNode.h"
#include "compiler/translator/Symbol.h"

namespace sh
{

class TSymbolTableLevel
{
  public:
    typedef TUnorderedMap<TString, TSymbol *> tLevel;
    typedef tLevel::const_iterator const_iterator;
    typedef const tLevel::value_type tLevelPair;
    typedef std::pair<tLevel::iterator, bool> tInsertResult;

    TSymbolTableLevel() : mGlobalInvariant(false) {}
    ~TSymbolTableLevel();

    bool insert(TSymbol *symbol);

    // Insert a function using its unmangled name as the key.
    bool insertUnmangled(TFunction *function);

    TSymbol *find(const TString &name) const;

    void addInvariantVarying(const std::string &name) { mInvariantVaryings.insert(name); }

    bool isVaryingInvariant(const std::string &name)
    {
        return (mGlobalInvariant || mInvariantVaryings.count(name) > 0);
    }

    void setGlobalInvariant(bool invariant) { mGlobalInvariant = invariant; }

    void insertUnmangledBuiltInName(const std::string &name)
    {
        mUnmangledBuiltInNames.insert(name);
    }

    bool hasUnmangledBuiltIn(const std::string &name)
    {
        return mUnmangledBuiltInNames.count(name) > 0;
    }

  protected:
    tLevel level;
    std::set<std::string> mInvariantVaryings;
    bool mGlobalInvariant;

  private:
    std::set<std::string> mUnmangledBuiltInNames;
};

// Define ESymbolLevel as int rather than an enum since level can go
// above GLOBAL_LEVEL and cause atBuiltInLevel() to fail if the
// compiler optimizes the >= of the last element to ==.
typedef int ESymbolLevel;
const int COMMON_BUILTINS    = 0;
const int ESSL1_BUILTINS     = 1;
const int ESSL3_BUILTINS     = 2;
const int ESSL3_1_BUILTINS   = 3;
// GLSL_BUILTINS are desktop GLSL builtins that don't exist in ESSL but are used to implement
// features in ANGLE's GLSL backend. They're not visible to the parser.
const int GLSL_BUILTINS      = 4;
const int LAST_BUILTIN_LEVEL = GLSL_BUILTINS;
const int GLOBAL_LEVEL       = 5;

class TSymbolTable : angle::NonCopyable
{
  public:
    TSymbolTable() : mUniqueIdCounter(0), mUserDefinedUniqueIdsStart(-1)
    {
        // The symbol table cannot be used until push() is called, but
        // the lack of an initial call to push() can be used to detect
        // that the symbol table has not been preloaded with built-ins.
    }

    ~TSymbolTable();

    // When the symbol table is initialized with the built-ins, there should
    // 'push' calls, so that built-ins are at level 0 and the shader
    // globals are at level 1.
    bool isEmpty() const { return table.empty(); }
    bool atBuiltInLevel() const { return currentLevel() <= LAST_BUILTIN_LEVEL; }
    bool atGlobalLevel() const { return currentLevel() == GLOBAL_LEVEL; }
    void push()
    {
        table.push_back(new TSymbolTableLevel);
        precisionStack.push_back(new PrecisionStackLevel);
    }

    void pop()
    {
        delete table.back();
        table.pop_back();

        delete precisionStack.back();
        precisionStack.pop_back();
    }

    // The declare* entry points are used when parsing and declare symbols at the current scope.
    // They return the created symbol / true in case the declaration was successful, and nullptr /
    // false if the declaration failed due to redefinition.
    bool declareVariable(TVariable *variable);
    bool declareStructType(TStructure *str);
    bool declareInterfaceBlock(TInterfaceBlock *interfaceBlock);

    // The insert* entry points are used when initializing the symbol table with built-ins.
    // They return the created symbol / true in case the declaration was successful, and nullptr /
    // false if the declaration failed due to redefinition.
    TVariable *insertVariable(ESymbolLevel level, const char *name, const TType &type);
    TVariable *insertVariableExt(ESymbolLevel level,
                                 TExtension ext,
                                 const char *name,
                                 const TType &type);
    bool insertVariable(ESymbolLevel level, TVariable *variable);
    bool insertStructType(ESymbolLevel level, TStructure *str);
    bool insertInterfaceBlock(ESymbolLevel level, TInterfaceBlock *interfaceBlock);

    bool insertConstInt(ESymbolLevel level, const char *name, int value, TPrecision precision)
    {
        TVariable *constant = new TVariable(
            this, NewPoolTString(name), TType(EbtInt, precision, EvqConst, 1), SymbolType::BuiltIn);
        TConstantUnion *unionArray = new TConstantUnion[1];
        unionArray[0].setIConst(value);
        constant->shareConstPointer(unionArray);
        return insert(level, constant);
    }

    bool insertConstIntExt(ESymbolLevel level,
                           TExtension ext,
                           const char *name,
                           int value,
                           TPrecision precision)
    {
        TVariable *constant =
            new TVariable(this, NewPoolTString(name), TType(EbtInt, precision, EvqConst, 1),
                          SymbolType::BuiltIn, ext);
        TConstantUnion *unionArray = new TConstantUnion[1];
        unionArray[0].setIConst(value);
        constant->shareConstPointer(unionArray);
        return insert(level, constant);
    }

    bool insertConstIvec3(ESymbolLevel level,
                          const char *name,
                          const std::array<int, 3> &values,
                          TPrecision precision)
    {
        TVariable *constantIvec3 = new TVariable(
            this, NewPoolTString(name), TType(EbtInt, precision, EvqConst, 3), SymbolType::BuiltIn);

        TConstantUnion *unionArray = new TConstantUnion[3];
        for (size_t index = 0u; index < 3u; ++index)
        {
            unionArray[index].setIConst(values[index]);
        }
        constantIvec3->shareConstPointer(unionArray);

        return insert(level, constantIvec3);
    }

    void insertBuiltIn(ESymbolLevel level,
                       TOperator op,
                       TExtension ext,
                       const TType *rvalue,
                       const char *name,
                       const TType *ptype1,
                       const TType *ptype2 = 0,
                       const TType *ptype3 = 0,
                       const TType *ptype4 = 0,
                       const TType *ptype5 = 0);

    void insertBuiltIn(ESymbolLevel level,
                       const TType *rvalue,
                       const char *name,
                       const TType *ptype1,
                       const TType *ptype2 = 0,
                       const TType *ptype3 = 0,
                       const TType *ptype4 = 0,
                       const TType *ptype5 = 0)
    {
        insertUnmangledBuiltInName(name, level);
        insertBuiltIn(level, EOpNull, TExtension::UNDEFINED, rvalue, name, ptype1, ptype2, ptype3,
                      ptype4, ptype5);
    }

    void insertBuiltIn(ESymbolLevel level,
                       TExtension ext,
                       const TType *rvalue,
                       const char *name,
                       const TType *ptype1,
                       const TType *ptype2 = 0,
                       const TType *ptype3 = 0,
                       const TType *ptype4 = 0,
                       const TType *ptype5 = 0)
    {
        insertUnmangledBuiltInName(name, level);
        insertBuiltIn(level, EOpNull, ext, rvalue, name, ptype1, ptype2, ptype3, ptype4, ptype5);
    }

    void insertBuiltInOp(ESymbolLevel level,
                         TOperator op,
                         const TType *rvalue,
                         const TType *ptype1,
                         const TType *ptype2 = 0,
                         const TType *ptype3 = 0,
                         const TType *ptype4 = 0,
                         const TType *ptype5 = 0);

    void insertBuiltInOp(ESymbolLevel level,
                         TOperator op,
                         TExtension ext,
                         const TType *rvalue,
                         const TType *ptype1,
                         const TType *ptype2 = 0,
                         const TType *ptype3 = 0,
                         const TType *ptype4 = 0,
                         const TType *ptype5 = 0);

    void insertBuiltInFunctionNoParameters(ESymbolLevel level,
                                           TOperator op,
                                           const TType *rvalue,
                                           const char *name);

    void insertBuiltInFunctionNoParametersExt(ESymbolLevel level,
                                              TExtension ext,
                                              TOperator op,
                                              const TType *rvalue,
                                              const char *name);

    TSymbol *find(const TString &name,
                  int shaderVersion,
                  bool *builtIn   = nullptr,
                  bool *sameScope = nullptr) const;

    TSymbol *findGlobal(const TString &name) const;

    TSymbol *findBuiltIn(const TString &name, int shaderVersion) const;

    TSymbol *findBuiltIn(const TString &name, int shaderVersion, bool includeGLSLBuiltins) const;

    TSymbolTableLevel *getOuterLevel()
    {
        assert(currentLevel() >= 1);
        return table[currentLevel() - 1];
    }

    void setDefaultPrecision(TBasicType type, TPrecision prec)
    {
        int indexOfLastElement = static_cast<int>(precisionStack.size()) - 1;
        // Uses map operator [], overwrites the current value
        (*precisionStack[indexOfLastElement])[type] = prec;
    }

    // Searches down the precisionStack for a precision qualifier
    // for the specified TBasicType
    TPrecision getDefaultPrecision(TBasicType type) const;

    // This records invariant varyings declared through
    // "invariant varying_name;".
    void addInvariantVarying(const std::string &originalName)
    {
        ASSERT(atGlobalLevel());
        table[currentLevel()]->addInvariantVarying(originalName);
    }
    // If this returns false, the varying could still be invariant
    // if it is set as invariant during the varying variable
    // declaration - this piece of information is stored in the
    // variable's type, not here.
    bool isVaryingInvariant(const std::string &originalName) const
    {
        ASSERT(atGlobalLevel());
        return table[currentLevel()]->isVaryingInvariant(originalName);
    }

    void setGlobalInvariant(bool invariant)
    {
        ASSERT(atGlobalLevel());
        table[currentLevel()]->setGlobalInvariant(invariant);
    }

    const TSymbolUniqueId nextUniqueId() { return TSymbolUniqueId(this); }

    // Checks whether there is a built-in accessible by a shader with the specified version.
    bool hasUnmangledBuiltInForShaderVersion(const char *name, int shaderVersion);

    void markBuiltInInitializationFinished();
    void clearCompilationResults();

  private:
    friend class TSymbolUniqueId;
    int nextUniqueIdValue();

    ESymbolLevel currentLevel() const { return static_cast<ESymbolLevel>(table.size() - 1); }

    TVariable *insertVariable(ESymbolLevel level,
                              const TString *name,
                              const TType &type,
                              SymbolType symbolType);

    bool insert(ESymbolLevel level, TSymbol *symbol)
    {
        ASSERT(level > LAST_BUILTIN_LEVEL || mUserDefinedUniqueIdsStart == -1);
        return table[level]->insert(symbol);
    }

    // Used to insert unmangled functions to check redeclaration of built-ins in ESSL 3.00 and
    // above.
    void insertUnmangledBuiltInName(const char *name, ESymbolLevel level);

    bool hasUnmangledBuiltInAtLevel(const char *name, ESymbolLevel level);

    std::vector<TSymbolTableLevel *> table;
    typedef TMap<TBasicType, TPrecision> PrecisionStackLevel;
    std::vector<PrecisionStackLevel *> precisionStack;

    int mUniqueIdCounter;

    // -1 before built-in init has finished, one past the last built-in id afterwards.
    // TODO(oetuaho): Make this a compile-time constant once the symbol table is initialized at
    // compile time. http://anglebug.com/1432
    int mUserDefinedUniqueIdsStart;
};

}  // namespace sh

#endif  // COMPILER_TRANSLATOR_SYMBOLTABLE_H_
