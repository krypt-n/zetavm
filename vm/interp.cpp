#include <cassert>
#include <iostream>
#include <unordered_map>
#include "runtime.h"
#include "parser.h"
#include "interp.h"
#include "packages.h"
#include <math.h>

/// Opcode enumeration
enum Opcode : uint16_t
{
    // Local variable access
    GET_LOCAL,
    SET_LOCAL,

    // Stack manipulation
    PUSH,
    POP,
    DUP,
    SWAP,

    // 32-bit integer operations
    ADD_I32,
    SUB_I32,
    MUL_I32,
    DIV_I32,
    MOD_I32,
    SHL_I32,
    SHR_I32,
    USHR_I32,
    AND_I32,
    OR_I32,
    XOR_I32,
    NOT_I32,
    LT_I32,
    LE_I32,
    GT_I32,
    GE_I32,
    EQ_I32,

    // Floating-point operations
    ADD_F32,
    SUB_F32,
    MUL_F32,
    DIV_F32,
    LT_F32,
    LE_F32,
    GT_F32,
    GE_F32,
    EQ_F32,
    SIN_F32,
    COS_F32,
    SQRT_F32,

    // Conversion operations
    I32_TO_F32,
    I32_TO_STR,
    F32_TO_I32,
    F32_TO_STR,
    STR_TO_F32,

    // Miscellaneous
    EQ_BOOL,
    HAS_TAG,
    GET_TAG,

    // String operations
    STR_LEN,
    GET_CHAR,
    GET_CHAR_CODE,
    CHAR_TO_STR,
    STR_CAT,
    EQ_STR,

    // Object operations
    NEW_OBJECT,
    HAS_FIELD,
    SET_FIELD,
    GET_FIELD,
    GET_FIELD_LIST,
    EQ_OBJ,

    // Array operations
    NEW_ARRAY,
    ARRAY_LEN,
    ARRAY_PUSH,
    GET_ELEM,
    SET_ELEM,

    // Branch instructions
    JUMP,
    JUMP_STUB,
    IF_TRUE,
    CALL,
    RET,
    THROW,

    IMPORT,
    ABORT,
    MAX_OPCODE
};

/// Inline cache to speed up property lookups
class ICache
{
private:

    // Cached slot index
    size_t slotIdx = 0;

    // Field name to look up
    String fieldName;

public:

    ICache(std::string fieldName)
    : fieldName(fieldName)
    {
    }

    Value getField(Object obj)
    {
        Value val;

        if (!obj.getField(fieldName, val, slotIdx))
        {
            throw RunError("missing field \"" + (std::string)fieldName + "\"");
        }

        return val;
    }

    int32_t getInt32(Object obj)
    {
        auto val = getField(obj);
        assert (val.isInt32());
        return (int32_t)val;
    }

    String getStr(Object obj)
    {
        auto val = getField(obj);
        assert (val.isString());
        return String(val);
    }

    Object getObj(Object obj)
    {
        auto val = getField(obj);
        assert (val.isObject());
        return Object(val);
    }

    Array getArr(Object obj)
    {
        auto val = getField(obj);
        assert (val.isArray());
        return Array(val);
    }
};

class CodeFragment
{
public:

    /// Start index in the executable heap
    uint8_t* startPtr = nullptr;

    /// End index in the executable heap
    uint8_t* endPtr = nullptr;

    /// Get the length of the code fragment
    size_t length()
    {
        assert (startPtr);
        assert (endPtr);
        return endPtr - startPtr;
    }
};

class BlockVersion : public CodeFragment
{
public:

    /// Associated function
    Object fun;

    /// Associated block
    Object block;

    /// Code generation context at block entry
    //CodeGenCtx ctx;

    BlockVersion(Object fun, Object block)
    : fun(fun),
      block(block)
    {
    }
};

/// Struct to associate information with a return address
struct RetEntry
{
    /// Return block version
    BlockVersion* retVer;

    /// Exception/catch block version (may be null)
    BlockVersion* excVer = nullptr;
};

/// Information stored by call instructions
struct CallInfo
{
    // Block version to return to after the call
    BlockVersion* retVer;

    // Last seen (cached) function
    refptr lastFn = nullptr;

    // Entry version for the cached function
    BlockVersion* entryVer = nullptr;

    // Number of locals for the cached function
    uint16_t numLocals = 0;

    // Number of call site arguments
    uint16_t numArgs;
};

typedef std::vector<BlockVersion*> VersionList;

/// Initial code heap size in bytes
const size_t CODE_HEAP_INIT_SIZE = 1 << 20;

/// Initial stack size in words
const size_t STACK_INIT_SIZE = 1 << 16;

/// Flat array of bytes into which code gets compiled
uint8_t* codeHeap = nullptr;

/// Limit pointer for the code heap
uint8_t* codeHeapLimit = nullptr;

/// Current allocation pointer in the code heap
uint8_t* codeHeapAlloc = nullptr;

/// Map of block objects to lists of versions
std::unordered_map<refptr, VersionList> versionMap;

/// Map of instructions to block versions
/// Note: this isn't defined for all instructions
std::unordered_map<uint8_t*, BlockVersion*> instrMap;

/// Map of return addresses to associated info
std::unordered_map<BlockVersion*, RetEntry> retAddrMap;

/// Lower stack limit (stack pointer must be greater than this)
Value* stackLimit = nullptr;

/// Stack base, initial stack pointer value (end of the stack memory array)
Value* stackBase = nullptr;

/// Stack frame base pointer
Value* framePtr = nullptr;

/// Current temp stack top pointer
Value* stackPtr = nullptr;

// Current instruction pointer
uint8_t* instrPtr = nullptr;

/// Cache of all possible one-character string values
Value charStrings[256];

static void* opcodeLabels[MAX_OPCODE];

Value execCode(bool initLabels = false);


/// Write a value to the code heap
template <typename T> void writeCode(T val)
{
    assert (codeHeapAlloc < codeHeapLimit);
    T* heapPtr = (T*)codeHeapAlloc;
    *heapPtr = val;
    codeHeapAlloc += sizeof(T);
    assert (codeHeapAlloc <= codeHeapLimit);
}

void writeCode(Opcode op)
{
    void** heapPtr = (void**)codeHeapAlloc;
    *heapPtr = opcodeLabels[op];
    codeHeapAlloc += sizeof(void*);
    assert (codeHeapAlloc <= codeHeapLimit);
}

/// Return a pointer to a value to read from the code stream
template <typename T> __attribute__((always_inline)) inline T& readCode()
{
    assert (instrPtr + sizeof(T) <= codeHeapLimit);
    T* valPtr = (T*)instrPtr;
    instrPtr += sizeof(T);
    return *valPtr;
}

/// Push a value on the stack
__attribute__((always_inline)) inline void pushVal(Value val)
{
    assert (stackPtr > stackLimit && "stack overflow");
    stackPtr--;
    stackPtr[0] = val;
}

/// Push a boolean on the stack
__attribute__((always_inline)) inline void pushBool(bool val)
{
    pushVal(val? (Value::TRUE) : (Value::FALSE));
}

__attribute__((always_inline)) inline Value popVal()
{
    assert (stackPtr < stackBase && "stack underflow");
    auto val = stackPtr[0];
    stackPtr++;
    return val;
}

__attribute__((always_inline)) inline bool popBool()
{
    // TODO: throw RunError if wrong type
    auto val = popVal();
    assert (val.isBool());
    return (bool)val;
}

__attribute__((always_inline)) inline int32_t popInt32()
{
    // TODO: throw RunError if wrong type
    auto val = popVal();
    assert (val.isInt32());
    return (int32_t)val;
}

__attribute__((always_inline)) inline float popFloat32()
{
    // TODO: throw RunError if wrong type
    auto val = popVal();
    assert (val.isFloat32());
    return (float)val;
}

__attribute__((always_inline)) inline String popStr()
{
    // TODO: throw RunError if wrong type
    auto val = popVal();
    assert (val.isString());
    return (String)val;
}

__attribute__((always_inline)) inline Object popObj()
{
    // TODO: throw RunError if wrong type
    auto val = popVal();
    assert (val.isObject());
    return (Object)val;
}

size_t codeHeapSize()
{
    return codeHeapAlloc - codeHeap;
}

/// Compute the stack size (number of slots allocated)
__attribute__((always_inline)) inline size_t stackSize()
{
    return stackBase - stackPtr;
}

/// Compute the size of the current frame (number of slots allocated)
__attribute__((always_inline)) inline size_t frameSize()
{
    return framePtr - stackPtr + 1;
}

/// Initialize the interpreter
void initInterp()
{
    // Allocate the code heap
    codeHeap = new uint8_t[CODE_HEAP_INIT_SIZE];
    codeHeapLimit = codeHeap + CODE_HEAP_INIT_SIZE;
    codeHeapAlloc = codeHeap;

    // Allocate the stack
    stackLimit = new Value[STACK_INIT_SIZE];
    stackBase = stackLimit + STACK_INIT_SIZE;
    stackPtr = stackBase;

    execCode(true);
}

/// Get a version of a block. This version will be a stub
/// until compiled
BlockVersion* getBlockVersion(
    Object fun,
    Object block
)
{
    auto blockPtr = (refptr)block;
    auto versionItr = versionMap.find((refptr)block);

    if (versionItr == versionMap.end())
    {
        versionMap[blockPtr] = VersionList();
    }
    else
    {
        auto versions = versionItr->second;
        assert (versions.size() > 0);
        for (auto version : versions)
        {
            if (version->fun == fun)
                return version;
        }
    }

    auto& versionList = versionMap[blockPtr];
    auto newVersion = new BlockVersion(fun, block);
    versionList.push_back(newVersion);
    return newVersion;
}

void compile(BlockVersion* version)
{
    //std::cout << "compiling version" << std::endl;

    auto block = version->block;

    // Get the instructions array
    static ICache instrsIC("instrs");
    Array instrs = instrsIC.getArr(block);

    if (instrs.length() == 0)
    {
        throw RunError("empty basic block");
    }

    // Mark the block start
    version->startPtr = codeHeapAlloc;

    // For each instruction
    for (size_t i = 0; i < instrs.length(); ++i)
    {
        auto instrVal = instrs.getElem(i);
        assert (instrVal.isObject());
        auto instr = (Object)instrVal;

        static ICache opIC("op");
        auto op = (std::string)opIC.getStr(instr);

        // std::cout << "op: " << op << std::endl;

        // Store a pointer to the current instruction
        auto instrPtr = codeHeapAlloc;

        if (op == "push")
        {
            static ICache valIC("val");
            auto val = valIC.getField(instr);
            writeCode(PUSH);
            writeCode(val);
            continue;
        }

        if (op == "pop")
        {
            writeCode(POP);
            continue;
        }

        if (op == "dup")
        {
            static ICache idxIC("idx");
            auto idx = (uint16_t)idxIC.getInt32(instr);
            writeCode(DUP);
            writeCode(idx);
            continue;
        }

        if (op == "swap")
        {
            writeCode(SWAP);
            continue;
        }

        if (op == "get_local")
        {
            static ICache idxIC("idx");
            auto idx = (uint16_t)idxIC.getInt32(instr);
            writeCode(GET_LOCAL);
            writeCode(idx);
            continue;
        }

        if (op == "set_local")
        {
            static ICache idxIC("idx");
            auto idx = (uint16_t)idxIC.getInt32(instr);
            writeCode(SET_LOCAL);
            writeCode(idx);
            continue;
        }

        //
        // Integer operations
        //

        if (op == "add_i32")
        {
            writeCode(ADD_I32);
            continue;
        }

        if (op == "sub_i32")
        {
            writeCode(SUB_I32);
            continue;
        }

        if (op == "mul_i32")
        {
            writeCode(MUL_I32);
            continue;
        }

        if (op == "div_i32")
        {
            writeCode(DIV_I32);
            continue;
        }

        if (op == "mod_i32")
        {
            writeCode(MOD_I32);
            continue;
        }

        if (op == "shl_i32")
        {
            writeCode(SHL_I32);
            continue;
        }

        if (op == "shr_i32")
        {
            writeCode(SHR_I32);
            continue;
        }

        if (op == "ushr_i32")
        {
            writeCode(USHR_I32);
            continue;
        }

        if (op == "and_i32")
        {
            writeCode(AND_I32);
            continue;
        }

        if (op == "or_i32")
        {
            writeCode(OR_I32);
            continue;
        }

        if (op == "xor_i32")
        {
            writeCode(XOR_I32);
            continue;
        }

        if (op == "not_i32")
        {
            writeCode(NOT_I32);
            continue;
        }

        if (op == "lt_i32")
        {
            writeCode(LT_I32);
            continue;
        }

        if (op == "le_i32")
        {
            writeCode(LE_I32);
            continue;
        }

        if (op == "gt_i32")
        {
            writeCode(GT_I32);
            continue;
        }

        if (op == "ge_i32")
        {
            writeCode(GE_I32);
            continue;
        }

        if (op == "eq_i32")
        {
            writeCode(EQ_I32);
            continue;
        }

        //
        // Floating-point ops
        //

        if (op == "add_f32")
        {
            writeCode(ADD_F32);
            continue;
        }

        if (op == "sub_f32")
        {
            writeCode(SUB_F32);
            continue;
        }

        if (op == "mul_f32")
        {
            writeCode(MUL_F32);
            continue;
        }

        if (op == "div_f32")
        {
            writeCode(DIV_F32);
            continue;
        }

        if (op == "lt_f32")
        {
            writeCode(LT_F32);
            continue;
        }

        if (op == "le_f32")
        {
            writeCode(LE_F32);
            continue;
        }

        if (op == "gt_f32")
        {
            writeCode(GT_F32);
            continue;
        }

        if (op == "ge_f32")
        {
            writeCode(GE_F32);
            continue;
        }

        if (op == "eq_f32")
        {
            writeCode(EQ_F32);
            continue;
        }

        if (op == "sin_f32")
        {
            writeCode(SIN_F32);
            continue;
        }

        if (op == "cos_f32")
        {
            writeCode(COS_F32);
            continue;
        }

        if (op == "sqrt_f32")
        {
            writeCode(SQRT_F32);
            continue;
        }

        //
        // Conversion ops
        //

        if (op == "i32_to_f32")
        {
            writeCode(I32_TO_F32);
            continue;
        }

        if (op == "i32_to_str")
        {
            writeCode(I32_TO_STR);
            continue;
        }

        if (op == "f32_to_i32")
        {
            writeCode(F32_TO_I32);
            continue;
        }

        if (op == "f32_to_str")
        {
            writeCode(F32_TO_STR);
            continue;
        }

        if (op == "str_to_f32")
        {
            writeCode(STR_TO_F32);
            continue;
        }

        //
        // Miscellaneous ops
        //

        if (op == "eq_bool")
        {
            writeCode(EQ_BOOL);
            continue;
        }

        if (op == "has_tag")
        {
            static ICache tagIC("tag");
            auto tagStr = (std::string)tagIC.getStr(instr);
            auto tag = strToTag(tagStr);

            writeCode(HAS_TAG);
            writeCode(tag);
            continue;
        }

        if (op == "get_tag")
        {
            writeCode(GET_TAG);
            continue;
        }

        //
        // String operations
        //

        if (op == "str_len")
        {
            writeCode(STR_LEN);
            continue;
        }

        if (op == "get_char")
        {
            writeCode(GET_CHAR);
            continue;
        }

        if (op == "get_char_code")
        {
            writeCode(GET_CHAR_CODE);
            continue;
        }

        if (op == "char_to_str")
        {
            writeCode(CHAR_TO_STR);
            continue;
        }

        if (op == "str_cat")
        {
            writeCode(STR_CAT);
            continue;
        }

        if (op == "eq_str")
        {
            writeCode(EQ_STR);
            continue;
        }

        //
        // Object operations
        //

        if (op == "new_object")
        {
            writeCode(NEW_OBJECT);
            continue;
        }

        if (op == "has_field")
        {
            writeCode(HAS_FIELD);
            continue;
        }

        if (op == "set_field")
        {
            writeCode(SET_FIELD);
            continue;
        }

        if (op == "get_field")
        {
            writeCode(GET_FIELD);

            // Cached property slot index
            writeCode(size_t(0));

            continue;
        }

        if (op == "get_field_list")
        {
            writeCode(GET_FIELD_LIST);
            continue;
        }

        //
        // Array operations
        //

        if (op == "new_array")
        {
            writeCode(NEW_ARRAY);
            continue;
        }

        if (op == "array_len")
        {
            writeCode(ARRAY_LEN);
            continue;
        }

        if (op == "array_push")
        {
            writeCode(ARRAY_PUSH);
            continue;
        }

        if (op == "set_elem")
        {
            writeCode(SET_ELEM);
            continue;
        }

        if (op == "get_elem")
        {
            writeCode(GET_ELEM);
            continue;
        }

        if (op == "eq_obj")
        {
            writeCode(EQ_OBJ);
            continue;
        }

        //
        // Branch instructions
        //

        if (op == "jump")
        {
            static ICache toIC("to");
            auto dstBB = toIC.getObj(instr);
            auto dstVer = getBlockVersion(version->fun, dstBB);

            writeCode(JUMP_STUB);
            writeCode(dstVer);
            continue;
        }

        if (op == "if_true")
        {
            static ICache thenIC("then");
            static ICache elseIC("else");
            auto thenBB = thenIC.getObj(instr);
            auto elseBB = elseIC.getObj(instr);

            auto thenVer = getBlockVersion(version->fun, thenBB);
            auto elseVer = getBlockVersion(version->fun, elseBB);

            writeCode(IF_TRUE);
            writeCode(thenVer);
            writeCode(elseVer);

            continue;
        }

        if (op == "call")
        {
            // Store a mapping of this instruction to the block version
            instrMap[instrPtr] = version;

            static ICache numArgsCache("num_args");
            auto numArgs = (int16_t)numArgsCache.getInt32(instr);

            // Get a version for the call continuation block
            static ICache retToCache("ret_to");
            auto retToBB = retToCache.getObj(instr);
            auto retVer = getBlockVersion(version->fun, retToBB);

            RetEntry retEntry;
            retEntry.retVer = retVer;

            if (instr.hasField("throw_to"))
            {
                // Get a version for the exception catch block
                static ICache throwIC("throw_to");
                auto throwBB = throwIC.getObj(instr);
                auto throwVer = getBlockVersion(version->fun, throwBB);
                retEntry.excVer = throwVer;
            }

            // Create an entry for the return address
            retAddrMap[retVer] = retEntry;

            writeCode(CALL);

            CallInfo callInfo;
            callInfo.numArgs = numArgs;
            callInfo.retVer = retVer;
            writeCode(callInfo);

            continue;
        }

        if (op == "ret")
        {
            writeCode(RET);
            continue;
        }

        if (op == "throw")
        {
            // Store a mapping of this instruction to the block version
            // Needed to retrieve the identity of the current function
            instrMap[instrPtr] = version;

            writeCode(THROW);
            continue;
        }

        if (op == "import")
        {
            writeCode(IMPORT);
            continue;
        }

        if (op == "abort")
        {
            // Store a mapping of this instruction to the block version
            // Needed to retrieve the source code position
            instrMap[instrPtr] = version;

            writeCode(ABORT);
            continue;
        }

        throw RunError("unhandled opcode in basic block \"" + op + "\"");
    }

    // Mark the block end
    version->endPtr = codeHeapAlloc;

    //std::cout << "done compiling version" << std::endl;
    //std::cout << codeHeapSize() << std::endl;
}

/// Get the source position for a given instruction, if available
Value getSrcPos(uint8_t* instrPtr)
{
    auto itr = instrMap.find(instrPtr);
    if (itr == instrMap.end())
    {
        std::cout << "no instr to block mapping" << std::endl;
        return Value::UNDEF;
    }

    auto block = itr->second->block;

    static ICache instrsIC("instrs");
    Array instrs = instrsIC.getArr(block);
    assert (instrs.length() > 0);

    // Traverse the instructions in reverse
    for (int i = (int)instrs.length() - 1; i >= 0; --i)
    {
        auto instrVal = instrs.getElem(i);
        assert (instrVal.isObject());
        auto instr = Object(instrVal);

        if (instr.hasField("src_pos"))
            return instr.getField("src_pos");
    }

    return Value::UNDEF;
}

void checkArgCount(
    uint8_t* instrPtr,
    size_t numParams,
    size_t numArgs
)
{
    if (numArgs != numParams)
    {
        Value srcPos = getSrcPos(instrPtr);

        std::string srcPosStr = (
            srcPos.isObject()?
            (posToString(srcPos) + " - "):
            std::string("")
        );

        throw RunError(
            srcPosStr +
            "incorrect argument count in call, received " +
            std::to_string(numArgs) +
            ", expected " +
            std::to_string(numParams)
        );
    }
}

/// Perform a user function call
__attribute__((always_inline)) inline void funCall(
    uint8_t* callInstr,
    Object fun,
    CallInfo& callInfo
)
{
    size_t numArgs = callInfo.numArgs;

    // If the function does not match the inline cache
    if (callInfo.lastFn != (refptr)fun)
    {
        // Get a version for the function entry block
        static ICache entryIC("entry");
        auto entryBB = entryIC.getObj(fun);
        auto entryVer = getBlockVersion(fun, entryBB);

        if (!entryVer->startPtr)
        {
            //std::cout << "compiling function entry block" << std::endl;
            compile(entryVer);
        }

        static ICache localsIC("num_locals");
        auto nlocals = localsIC.getInt32(fun);
        assert(nlocals >= 0);
        auto numLocals = size_t(nlocals);

        static ICache paramsIC("params");
        auto params = paramsIC.getArr(fun);
        auto numParams = size_t(params.length());

        // Check that the argument count matches
        checkArgCount(callInstr, numParams, numArgs);

        // Note: the hidden function/closure parameter is always present
        if (numLocals < numParams + 1)
        {
            throw RunError(
                "not enough locals to store function parameters"
            );
        }

        // Update the inline cache
        callInfo.lastFn = (refptr)fun;
        callInfo.numLocals = numLocals;
        callInfo.entryVer = entryVer;
    }

    size_t numLocals = callInfo.numLocals;
    BlockVersion* entryVer = callInfo.entryVer;
    BlockVersion* retVer = callInfo.retVer;

    // Compute the stack pointer to restore after the call
    auto prevStackPtr = stackPtr + numArgs;

    // Save the current frame pointer
    auto prevFramePtr = framePtr;

    // Point the frame pointer to the first argument
    assert (stackPtr > stackLimit);
    framePtr = stackPtr + numArgs - 1;

    // Store the function/pointer argument
    framePtr[-numArgs] = fun;

    // Pop the arguments, push the callee locals
    stackPtr -= numLocals - numArgs;

    pushVal(Value((refptr)prevStackPtr, TAG_RAWPTR));
    pushVal(Value((refptr)prevFramePtr, TAG_RAWPTR));
    pushVal(Value((refptr)retVer, TAG_RAWPTR));

    // Jump to the entry block of the function
    instrPtr = entryVer->startPtr;
}

/// Perform a host function call
__attribute__((always_inline)) inline void hostCall(
    uint8_t* callInstr,
    Value fun,
    size_t numArgs,
    BlockVersion* retVer
)
{
    auto hostFn = (HostFn*)fun.getWord().ptr;

    // Pointer to the first argument
    auto args = stackPtr + numArgs - 1;

    Value retVal;

    // Call the host function
    switch (numArgs)
    {
        case 0:
        retVal = hostFn->call0();
        break;

        case 1:
        retVal = hostFn->call1(args[0]);
        break;

        case 2:
        retVal = hostFn->call2(args[0], args[-1]);
        break;

        case 3:
        retVal = hostFn->call3(args[0], args[-1], args[-2]);
        break;

        default:
        assert (false);
    }

    // Pop the arguments from the stack
    stackPtr += numArgs;

    // Push the return value
    pushVal(retVal);

    if (!retVer->startPtr)
        compile(retVer);

    instrPtr = retVer->startPtr;
}

/// Implementation of the throw instruction
void throwExc(
    uint8_t* throwInstr,
    Value excVal
)
{
    // Get the current function
    auto itr = instrMap.find(throwInstr);
    assert (itr != instrMap.end());
    auto curFun = itr->second->fun;

    // Until we are done unwinding the stack
    for (;;)
    {
        //std::cout << "Unwinding frame" << std::endl;

        // Get the number of locals in the function
        static ICache numLocalsIC("num_locals");
        auto numLocals = numLocalsIC.getInt32(curFun);

        //std::cout << "numLocals=" << numLocals << std::endl;

        // Get the saved stack ptr, frame ptr and return address
        auto prevStackPtr = framePtr[-(numLocals + 0)];
        auto prevFramePtr = framePtr[-(numLocals + 1)];
        auto retAddr      = framePtr[-(numLocals + 2)];

        assert (retAddr.getTag() == TAG_RAWPTR);
        auto retVer = (BlockVersion*)retAddr.getWord().ptr;

        // If we are at the top level
        if (retVer == nullptr)
        {
            //std::cout << "Uncaught exception" << std::endl;

            std::string errMsg;

            if (excVal.isObject())
            {
                auto excObj = Object(excVal);

                if (excObj.hasField("src_pos"))
                {
                    auto srcPosVal = excObj.getField("src_pos");
                    errMsg += posToString(srcPosVal) + " - ";
                }

                if (excObj.hasField("msg"))
                {
                    auto errMsgVal = excObj.getField("msg");
                    errMsg += errMsgVal.toString();
                }
                else
                {
                    errMsg += "uncaught user exception object";
                }
            }
            else
            {
                errMsg = excVal.toString();
            }

            throw RunError(errMsg);
        }

        // Find the info associated with the return address
        assert (retAddrMap.find(retVer) != retAddrMap.end());
        auto retEntry = retAddrMap[retVer];

        // Get the function associated with the return address
        curFun = retEntry.retVer->fun;

        // Update the stack and frame pointer
        stackPtr = (Value*)prevStackPtr.getWord().ptr;
        framePtr = (Value*)prevFramePtr.getWord().ptr;

        // If there is an exception handler
        if (retEntry.excVer)
        {
            //std::cout << "Found exception handler" << std::endl;

            // Push the exception value on the stack
            pushVal(excVal);

            // Compile exception handler if needed
            if (!retEntry.excVer->startPtr)
                compile(retEntry.excVer);

            instrPtr = retEntry.excVer->startPtr;

            // Done unwinding the stack
            break;
        }
    }
}


#define THREADED

#ifdef THREADED
#define JOIN(x, y) x##y
#define NEXT() goto *readCode<void*>()
#define LABEL(L) JOIN(label, L)
#define REGISTERLABEL(L) opcodeLabels[L] = &&LABEL(L)
#else
#define NEXT() break
#define LABEL(L) case L
#endif

/// Start/continue execution beginning at a current instruction
__attribute__((__noinline__,__noclone__)) 
Value execCode(bool initLabels)
{
    if (initLabels) {
        REGISTERLABEL(GET_LOCAL);
        REGISTERLABEL(SET_LOCAL);

        REGISTERLABEL(PUSH);
        REGISTERLABEL(POP);
        REGISTERLABEL(DUP);
        REGISTERLABEL(SWAP);

        REGISTERLABEL(ADD_I32);
        REGISTERLABEL(SUB_I32);
        REGISTERLABEL(MUL_I32);
        REGISTERLABEL(DIV_I32);
        REGISTERLABEL(MOD_I32);
        REGISTERLABEL(SHL_I32);
        REGISTERLABEL(SHR_I32);
        REGISTERLABEL(USHR_I32);
        REGISTERLABEL(AND_I32);
        REGISTERLABEL(OR_I32);
        REGISTERLABEL(XOR_I32);
        REGISTERLABEL(NOT_I32);
        REGISTERLABEL(LT_I32);
        REGISTERLABEL(LE_I32);
        REGISTERLABEL(GT_I32);
        REGISTERLABEL(GE_I32);
        REGISTERLABEL(EQ_I32);

        REGISTERLABEL(ADD_F32);
        REGISTERLABEL(SUB_F32);
        REGISTERLABEL(MUL_F32);
        REGISTERLABEL(DIV_F32);
        REGISTERLABEL(LT_F32);
        REGISTERLABEL(LE_F32);
        REGISTERLABEL(GT_F32);
        REGISTERLABEL(GE_F32);
        REGISTERLABEL(EQ_F32);
        REGISTERLABEL(SIN_F32);
        REGISTERLABEL(COS_F32);
        REGISTERLABEL(SQRT_F32);

        REGISTERLABEL(I32_TO_F32);
        REGISTERLABEL(I32_TO_STR);
        REGISTERLABEL(F32_TO_I32);
        REGISTERLABEL(F32_TO_STR);
        REGISTERLABEL(STR_TO_F32);

        REGISTERLABEL(EQ_BOOL);
        REGISTERLABEL(HAS_TAG);
        REGISTERLABEL(GET_TAG);

        REGISTERLABEL(STR_LEN);
        REGISTERLABEL(GET_CHAR);
        REGISTERLABEL(GET_CHAR_CODE);
        REGISTERLABEL(CHAR_TO_STR);
        REGISTERLABEL(STR_CAT);
        REGISTERLABEL(EQ_STR);

        REGISTERLABEL(NEW_OBJECT);
        REGISTERLABEL(HAS_FIELD);
        REGISTERLABEL(SET_FIELD);
        REGISTERLABEL(GET_FIELD);
        REGISTERLABEL(GET_FIELD_LIST);
        REGISTERLABEL(EQ_OBJ);

        REGISTERLABEL(NEW_ARRAY);
        REGISTERLABEL(ARRAY_LEN);
        REGISTERLABEL(ARRAY_PUSH);
        REGISTERLABEL(GET_ELEM);
        REGISTERLABEL(SET_ELEM);

        REGISTERLABEL(JUMP);
        REGISTERLABEL(JUMP_STUB);
        REGISTERLABEL(IF_TRUE);
        REGISTERLABEL(CALL);
        REGISTERLABEL(RET);
        REGISTERLABEL(THROW);

        REGISTERLABEL(IMPORT);
        REGISTERLABEL(ABORT);
        return Value::TRUE;
    }
    assert (instrPtr >= codeHeap);
    assert (instrPtr < codeHeapLimit);

    //void** op = nullptr;
    NEXT();

    // For each instruction to execute
    /* for (;;) */
    /* { */
    /*     auto& op = readCode<Opcode>(); */

    /*     //std::cout << "instr" << std::endl; */
    /*     //std::cout << "op=" << (int)op << std::endl; */
    /*     //std::cout << "  stack space: " << (stackBase - stackPtr) << std::endl; */

    /*     switch (op) */
    /*     { */
            LABEL(PUSH):
            {
                auto val = readCode<Value>();
                pushVal(val);
            }
            NEXT();

            LABEL(POP):
            {
                popVal();
            }
            NEXT();

            LABEL(DUP):
            {
                // Read the index of the value to duplicate
                auto idx = readCode<uint16_t>();
                auto val = stackPtr[idx];
                pushVal(val);
            }
            NEXT();

            // Swap the topmost two stack elements
            LABEL(SWAP):
            {
                auto v0 = popVal();
                auto v1 = popVal();
                pushVal(v0);
                pushVal(v1);
            }
            NEXT();

            // Set a local variable
            LABEL(SET_LOCAL):
            {
                auto localIdx = readCode<uint16_t>();
                //std::cout << "set localIdx=" << localIdx << std::endl;
                assert (stackPtr > stackLimit);
                framePtr[-localIdx] = popVal();
            }
            NEXT();

            LABEL(GET_LOCAL):
            {
                // Read the index of the value to push
                auto localIdx = readCode<uint16_t>();
                //std::cout << "get localIdx=" << localIdx << std::endl;
                assert (stackPtr > stackLimit);
                auto val = framePtr[-localIdx];
                pushVal(val);
            }
            NEXT();

            //
            // Integer operations
            //

            LABEL(ADD_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushVal(Value::int32(arg0 + arg1));
            }
            NEXT();

            LABEL(SUB_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushVal(Value::int32(arg0 - arg1));
            }
            NEXT();

            LABEL(MUL_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushVal(Value::int32(arg0 * arg1));
            }
            NEXT();

            LABEL(DIV_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushVal(Value::int32(arg0 / arg1));
            }
            NEXT();

            LABEL(MOD_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushVal(Value::int32(arg0 % arg1));
            }
            NEXT();

            LABEL(SHL_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushVal(Value::int32(arg0 << arg1));
            }
            NEXT();

            LABEL(SHR_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushVal(Value::int32(arg0 >> arg1));
            }
            NEXT();

            LABEL(USHR_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = (uint32_t)popInt32();
                pushVal(Value::int32((int32_t)(arg0 >> arg1)));
            }
            NEXT();

            LABEL(AND_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushVal(Value::int32(arg0 & arg1));
            }
            NEXT();

            LABEL(OR_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushVal(Value::int32(arg0 | arg1));
            }
            NEXT();

            LABEL(XOR_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushVal(Value::int32(arg0 ^ arg1));
            }
            NEXT();

            LABEL(NOT_I32):
            {
                auto arg0 = popInt32();
                pushVal(Value::int32(~arg0));
            }
            NEXT();

            LABEL(LT_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushBool(arg0 < arg1);
            }
            NEXT();

            LABEL(LE_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushBool(arg0 <= arg1);
            }
            NEXT();

            LABEL(GT_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushBool(arg0 > arg1);
            }
            NEXT();

            LABEL(GE_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushBool(arg0 >= arg1);
            }
            NEXT();

            LABEL(EQ_I32):
            {
                auto arg1 = popInt32();
                auto arg0 = popInt32();
                pushBool(arg0 == arg1);
            }
            NEXT();

            //
            // Floating-point operations
            //

            LABEL(ADD_F32):
            {
                auto arg1 = popFloat32();
                auto arg0 = popFloat32();
                pushVal(Value::float32(arg0 + arg1));
            }
            NEXT();

            LABEL(SUB_F32):
            {
                auto arg1 = popFloat32();
                auto arg0 = popFloat32();
                pushVal(Value::float32(arg0 - arg1));
            }
            NEXT();

            LABEL(MUL_F32):
            {
                auto arg1 = popFloat32();
                auto arg0 = popFloat32();
                pushVal(Value::float32(arg0 * arg1));
            }
            NEXT();

            LABEL(DIV_F32):
            {
                auto arg1 = popFloat32();
                auto arg0 = popFloat32();
                pushVal(Value::float32(arg0 / arg1));
            }
            NEXT();

            LABEL(LT_F32):
            {
                auto arg1 = popFloat32();
                auto arg0 = popFloat32();
                pushBool(arg0 < arg1);
            }
            NEXT();

            LABEL(LE_F32):
            {
                auto arg1 = popFloat32();
                auto arg0 = popFloat32();
                pushBool(arg0 <= arg1);
            }
            NEXT();

            LABEL(GT_F32):
            {
                auto arg1 = popFloat32();
                auto arg0 = popFloat32();
                pushBool(arg0 > arg1);
            }
            NEXT();

            LABEL(GE_F32):
            {
                auto arg1 = popFloat32();
                auto arg0 = popFloat32();
                pushBool(arg0 >= arg1);
            }
            NEXT();

            LABEL(EQ_F32):
            {
                auto arg1 = popFloat32();
                auto arg0 = popFloat32();
                pushBool(arg0 == arg1);
            }
            NEXT();

            LABEL(SIN_F32):
            {
                float arg = popFloat32();
                pushVal(Value::float32(sin(arg)));
            }
            NEXT();

            LABEL(COS_F32):
            {
                float arg = popFloat32();
                pushVal(Value::float32(cos(arg)));
            }
            NEXT();

            LABEL(SQRT_F32):
            {
                float arg = popFloat32();
                pushVal(Value::float32(sqrt(arg)));
            }
            NEXT();

            //
            // Conversion operations
            //

            LABEL(I32_TO_F32):
            {
                auto arg0 = popInt32();
                pushVal(Value::float32(arg0));
            }
            NEXT();

            LABEL(I32_TO_STR):
            {
                auto arg0 = popInt32();
                String str = std::to_string(arg0);
                pushVal(str);
            }
            NEXT();

            LABEL(F32_TO_I32):
            {
                auto arg0 = popFloat32();
                pushVal(Value::int32(arg0));
            }
            NEXT();

            LABEL(F32_TO_STR):
            {
                auto arg0 = popFloat32();
                String str = std::to_string(arg0);
                pushVal(str);
            }
            NEXT();

            LABEL(STR_TO_F32):
            {
                auto arg0 = popStr();

                float val;

                // If the float fails to parse, produce NaN
                try
                {
                    val = std::stof(arg0);
                }
                catch (...)
                {
                    val = 0.0f / 0.0f;
                }

                pushVal(Value::float32(val));
            }
            NEXT();

            //
            // Misc operations
            //

            LABEL(EQ_BOOL):
            {
                auto arg1 = popBool();
                auto arg0 = popBool();
                pushBool(arg0 == arg1);
            }
            NEXT();

            // Test if a value has a given tag
            LABEL(HAS_TAG):
            {
                auto testTag = readCode<Tag>();
                auto valTag = popVal().getTag();
                pushBool(valTag == testTag);
            }
            NEXT();

            // Get the type tag associated with a value.
            // Note: this produces a string
            LABEL(GET_TAG):
            {
                auto valTag = popVal().getTag();
                auto tagStr = tagToStr(valTag);
                pushVal(String(tagStr));
            }
            NEXT();

            //
            // String operations
            //

            LABEL(STR_LEN):
            {
                auto str = popStr();
                pushVal(Value::int32(str.length()));
            }
            NEXT();

            LABEL(GET_CHAR):
            {
                auto idx = (size_t)popInt32();
                auto str = popStr();

                if (idx >= str.length())
                {
                    throw RunError(
                        "get_char, index out of bounds"
                    );
                }

                uint8_t ch = str[idx];
                // Cache single-character strings
                if (charStrings[ch] == Value::UNDEF)
                {
                    char buf[2] = { (char)str[idx], '\0' };
                    charStrings[ch] = String(buf);
                }

                pushVal(charStrings[ch]);
            }
            NEXT();

            LABEL(GET_CHAR_CODE):
            {
                auto idx = (size_t)popInt32();
                auto str = popStr();

                if (idx >= str.length())
                {
                    throw RunError(
                        "get_char_code, index out of bounds"
                    );
                }

                pushVal(Value::int32(str[idx]));
            }
            NEXT();

            LABEL(CHAR_TO_STR):
            {
                auto charCode = (char)popInt32();
                char buf[2] = { (char)charCode, '\0' };
                pushVal(String(buf));
            }
            NEXT();

            LABEL(STR_CAT):
            {
                auto a = popStr();
                auto b = popStr();
                auto c = String::concat(b, a);
                pushVal(c);
            }
            NEXT();

            LABEL(EQ_STR):
            {
                auto arg1 = popStr();
                auto arg0 = popStr();
                pushBool(arg0 == arg1);
            }
            NEXT();

            //
            // Object operations
            //

            LABEL(NEW_OBJECT):
            {
                auto capacity = popInt32();
                auto obj = Object::newObject(capacity);
                pushVal(obj);
            }
            NEXT();

            LABEL(HAS_FIELD):
            {
                auto fieldName = popStr();
                auto obj = popObj();
                pushBool(obj.hasField(fieldName));
            }
            NEXT();

            LABEL(SET_FIELD):
            {
                auto val = popVal();
                auto fieldName = popStr();
                auto obj = popObj();
                obj.setField(fieldName, val);
            }
            NEXT();

            // This instruction will abort execution if trying to
            // access a field that is not present on an object.
            // The running program is responsible for testing that
            // fields exist before attempting to read them.
            LABEL(GET_FIELD):
            {
                auto fieldName = popStr();
                auto obj = popObj();

                // Get the cached slot index
                auto& slotIdx = readCode<size_t>();

                Value val;

                if (!obj.getField(fieldName, val, slotIdx))
                {
                    throw RunError(
                        "get_field failed, missing field \"" +
                        (std::string)fieldName + "\""
                    );
                }

                pushVal(val);
            }
            NEXT();

            LABEL(GET_FIELD_LIST):
            {
                Value arg0 = popVal();
                Array array = Array(0);
                for (auto itr = ObjFieldItr(arg0); itr.valid(); itr.next())
                {
                    auto fieldName = (String)itr.get();
                    array.push(fieldName);
                }
                pushVal(array);
            }
            NEXT();

            LABEL(EQ_OBJ):
            {
                Value arg1 = popVal();
                Value arg0 = popVal();
                pushBool(arg0 == arg1);
            }
            NEXT();

            //
            // Array operations
            //

            LABEL(NEW_ARRAY):
            {
                auto len = popInt32();
                auto array = Array(len);
                pushVal(array);
            }
            NEXT();

            LABEL(ARRAY_LEN):
            {
                auto arr = Array(popVal());
                pushVal(Value::int32(arr.length()));
            }
            NEXT();

            LABEL(ARRAY_PUSH):
            {
                auto val = popVal();
                auto arr = Array(popVal());
                arr.push(val);
            }
            NEXT();

            LABEL(SET_ELEM):
            {
                auto val = popVal();
                auto idx = (size_t)popInt32();
                auto arr = Array(popVal());

                if (idx >= arr.length())
                {
                    throw RunError(
                        "set_elem, index out of bounds"
                    );
                }

                arr.setElem(idx, val);
            }
            NEXT();

            LABEL(GET_ELEM):
            {
                auto idx = (size_t)popInt32();
                auto arr = Array(popVal());

                if (idx >= arr.length())
                {
                    throw RunError(
                        "get_elem, index out of bounds"
                    );
                }

                pushVal(arr.getElem(idx));
            }
            NEXT();

            //
            // Branch instructions
            //

            LABEL(JUMP_STUB):
            {
                auto& dstAddr = readCode<uint8_t*>();

                //std::cout << "Patching jump" << std::endl;

                auto dstVer = (BlockVersion*)dstAddr;

                if (!dstVer->startPtr)
                {
                    // If the heap allocation pointer is right
                    // after the jump instruction
                    if (instrPtr == codeHeapAlloc)
                    {
                        // The jump is redundant, so we will write the
                        // next block over this jump instruction
                        instrPtr = codeHeapAlloc = (uint8_t*)((void**) instrPtr - 2);
                    }

                    compile(dstVer);
                }
                else
                {
                    // Patch the jump
                    *(((void**) instrPtr) - 2) = opcodeLabels[JUMP];
                    //*op = opcodeLabels[JUMP];
                    dstAddr = dstVer->startPtr;

                    // Jump to the target
                    instrPtr = dstVer->startPtr;
                }
            }
            NEXT();

            LABEL(JUMP):
            {
                auto& dstAddr = readCode<uint8_t*>();
                instrPtr = dstAddr;
            }
            NEXT();

            LABEL(IF_TRUE):
            {
                auto& thenAddr = readCode<uint8_t*>();
                auto& elseAddr = readCode<uint8_t*>();

                auto arg0 = popVal();

                if (arg0 == Value::TRUE)
                {
                    if (thenAddr < codeHeap || thenAddr >= codeHeapLimit)
                    {
                        //std::cout << "Patching then target" << std::endl;

                        auto thenVer = (BlockVersion*)thenAddr;
                        if (!thenVer->startPtr)
                           compile(thenVer);

                        // Patch the jump
                        thenAddr = thenVer->startPtr;
                    }

                    instrPtr = thenAddr;
                }
                else
                {
                    if (elseAddr < codeHeap || elseAddr >= codeHeapLimit)
                    {
                       //std::cout << "Patching else target" << std::endl;

                       auto elseVer = (BlockVersion*)elseAddr;
                       if (!elseVer->startPtr)
                           compile(elseVer);

                       // Patch the jump
                       elseAddr = elseVer->startPtr;
                    }

                    instrPtr = elseAddr;
                }
            }
            NEXT();

            // Regular function call
            LABEL(CALL):
            {
                auto& callInfo = readCode<CallInfo>();

                auto callee = popVal();

                if (stackSize() < callInfo.numArgs)
                {
                    throw RunError(
                        "stack underflow at call"
                    );
                }

                if (callee.isObject())
                {
                    funCall(
                        (uint8_t*)instrPtr,
                        callee,
                        callInfo
                    );
                }
                else if (callee.isHostFn())
                {
                    hostCall(
                        (uint8_t*)instrPtr,
                        callee,
                        callInfo.numArgs,
                        callInfo.retVer
                    );
                }
                else
                {
                  throw RunError("invalid callee at call site");
                }
            }
            NEXT();

            LABEL(RET):
            {
                // TODO: figure out callee identity from version,
                // caller identity from return address
                //
                // We want args to have been consumed
                // We pop all our locals (or the caller does)
                //
                // The thing is... The caller can't pop our locals,
                // because the call continuation doesn't know

                // Pop the return value
                auto retVal = popVal();

                // Pop the return address
                auto retVer = (BlockVersion*)popVal().getWord().ptr;

                // Pop the previous frame pointer
                auto prevFramePtr = popVal().getWord().ptr;

                // Pop the previous stack pointer
                auto prevStackPtr = popVal().getWord().ptr;

                // Restore the previous frame pointer
                framePtr = (Value*)prevFramePtr;

                // Restore the stack pointer
                stackPtr = (Value*)prevStackPtr;

                // If this is a top-level return
                if (retVer == nullptr)
                {
                    return retVal;
                }
                else
                {
                    // Push the return value on the stack
                    pushVal(retVal);

                    if (!retVer->startPtr)
                        compile(retVer);

                    instrPtr = retVer->startPtr;
                }
            }
            NEXT();

            // Throw an exception
            LABEL(THROW):
            {
                // Pop the exception value
                auto excVal = popVal();
                throwExc((uint8_t*)(((void**)instrPtr) - 1), excVal);
            }
            NEXT();

            LABEL(IMPORT):
            {
                auto pkgName = (std::string)popVal();
                auto pkg = import(pkgName);
                pushVal(pkg);
            }
            NEXT();

            LABEL(ABORT):
            {
                auto errMsg = (std::string)popStr();

                auto srcPos = getSrcPos((uint8_t*)instrPtr);
                if (srcPos != Value::UNDEF)
                    std::cout << posToString(srcPos) << " - ";

                if (errMsg != "")
                {
                    std::cout << "aborting execution due to error: ";
                    std::cout << errMsg << std::endl;
                }
                else
                {
                    std::cout << "aborting execution due to error" << std::endl;
                }

                exit(-1);
            }
            NEXT();

            /* default: */
            /* assert (false && "unhandled instruction in interpreter loop"); */
        /* } */

    /* } */

    assert (false);
}

/// Begin the execution of a function
/// Note: this may be indirectly called from within a running interpreter
Value callFun(Object fun, ValueVec args)
{
    static ICache paramsIC("params");
    static ICache numLocalsIC("num_locals");
    auto params = paramsIC.getArr(fun);
    auto numParams = size_t(params.length());
    auto nlocals = numLocalsIC.getInt32(fun);
    assert(nlocals >= 0);
    auto numLocals = size_t(nlocals);

    if (args.size() != numParams)
    {
        throw RunError(
            "argument count mismatch in top-level call"
        );
    }

    if (numLocals < numParams + 1)
    {
        throw RunError(
            "not enough locals to store function parameters in top-level call"
        );
    }

    // Store the stack size before the call
    auto preCallSz = stackSize();

    // Save the previous instruction pointer
    pushVal(Value((refptr)instrPtr, TAG_RAWPTR));

    // Save the previous stack and frame pointers
    auto prevStackPtr = stackPtr;
    auto prevFramePtr = framePtr;

    // Initialize the frame pointer (used to access locals)
    framePtr = stackPtr - 1;

    // Push space for the local variables
    stackPtr -= numLocals;
    assert (stackPtr >= stackLimit);

    // Push the previous stack pointer, previous
    // frame pointer and return address
    pushVal(Value((refptr)prevStackPtr, TAG_RAWPTR));
    pushVal(Value((refptr)prevFramePtr, TAG_RAWPTR));
    pushVal(Value(nullptr, TAG_RAWPTR));

    // Copy the arguments into the locals
    for (size_t i = 0; i < args.size(); ++i)
    {
        //std::cout << "  " << args[i].toString() << std::endl;
        framePtr[-i] = args[i];
    }

    // Store the function/closure parameter
    framePtr[-numParams] = fun;

    // Get the function entry block
    static ICache entryIC("entry");
    auto entryBlock = entryIC.getObj(fun);
    auto entryVer = getBlockVersion(fun, entryBlock);

    // Generate code for the entry block version
    compile(entryVer);
    assert (entryVer->length() > 0);

    // Begin execution at the entry block
    instrPtr = entryVer->startPtr;
    auto retVal = execCode();

    // Restore the previous instruction pointer
    instrPtr = (uint8_t*)popVal().getWord().ptr;

    // Check that the stack size matches what it was before the call
    if (stackSize() != preCallSz)
    {
        throw RunError("stack size does not match after call termination");
    }

    return retVal;
}

/// Call a function exported by a package
Value callExportFn(
    Object pkg,
    std::string fnName,
    ValueVec args
)
{
    if (!pkg.hasField(fnName))
    {
        throw RunError(
            "package does not export function \"" + fnName + "\""
        );
    }

    auto fnVal = pkg.getField(fnName);

    if (!fnVal.isObject())
    {
        throw RunError(
            "field \"" + fnName + "\" exported by package is not a function"
        );
    }

    auto funObj = Object(fnVal);

    return callFun(funObj, args);
}

Value testRunImage(std::string fileName)
{
    std::cout << "loading image \"" << fileName << "\"" << std::endl;

    auto pkg = parseFile(fileName);

    std::cout << callExportFn(pkg, "main").toString() << "\n";

    return callExportFn(pkg, "main");
}

void testInterp()
{
    assert (testRunImage("tests/vm/ex_ret_cst.zim") == Value::int32(777));
    assert (testRunImage("tests/vm/ex_loop_cnt.zim") == Value::int32(0));
    assert (testRunImage("tests/vm/ex_image.zim") == Value::int32(10));
    assert (testRunImage("tests/vm/ex_rec_fact.zim") == Value::int32(5040));
    assert (testRunImage("tests/vm/ex_fibonacci.zim") == Value::int32(377));
    assert (testRunImage("tests/vm/float_ops.zim").toString() == "10.500000");
}
