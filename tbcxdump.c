/*
 * tbcxdump.c — Disassembler and human-readable dumper for TBCX (Tcl 9.1) files.
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>

#include "tbcx.h"

#define TCL_INSTRUCTION_ENTRY(name, stack)                                                                                                                                                             \
    {                                                                                                                                                                                                  \
        name, 1, stack, 0, {                                                                                                                                                                           \
            OPERAND_NONE, OPERAND_NONE                                                                                                                                                                 \
        }                                                                                                                                                                                              \
    }
#define TCL_INSTRUCTION_ENTRY1(name, size, stack, type1)                                                                                                                                               \
    {                                                                                                                                                                                                  \
        name, size, stack, 1, {                                                                                                                                                                        \
            type1, OPERAND_NONE                                                                                                                                                                        \
        }                                                                                                                                                                                              \
    }
#define TCL_INSTRUCTION_ENTRY2(name, size, stack, type1, type2)                                                                                                                                        \
    {                                                                                                                                                                                                  \
        name, size, stack, 2, {                                                                                                                                                                        \
            type1, type2                                                                                                                                                                               \
        }                                                                                                                                                                                              \
    }
#define DEPRECATED_INSTRUCTION_ENTRY(name, stack)                                                                                                                                                      \
    {                                                                                                                                                                                                  \
        NULL, 1, 0, 0, {                                                                                                                                                                               \
            OPERAND_NONE, OPERAND_NONE                                                                                                                                                                 \
        }                                                                                                                                                                                              \
    }
#define DEPRECATED_INSTRUCTION_ENTRY1(name, size, stack, type1)                                                                                                                                        \
    {                                                                                                                                                                                                  \
        NULL, size, 0, 0, {                                                                                                                                                                            \
            OPERAND_NONE, OPERAND_NONE                                                                                                                                                                 \
        }                                                                                                                                                                                              \
    }
#define DEPRECATED_INSTRUCTION_ENTRY2(name, size, stack, type1, type2)                                                                                                                                 \
    {                                                                                                                                                                                                  \
        NULL, size, 0, 0, {                                                                                                                                                                            \
            OPERAND_NONE, OPERAND_NONE                                                                                                                                                                 \
        }                                                                                                                                                                                              \
    }

static const InstructionDesc tbcxInstructionTable[] = {
    /*  Name	      Bytes stackEffect	  Operand types */
    TCL_INSTRUCTION_ENTRY("done", -1),
    /* Finish ByteCode execution and return stktop (top stack item) */
    DEPRECATED_INSTRUCTION_ENTRY1("push1", 2, +1, OPERAND_LIT1),
    /* Push object at ByteCode objArray[op1] */
    TCL_INSTRUCTION_ENTRY1("push", 5, +1, OPERAND_LIT4),
    /* Push object at ByteCode objArray[op4] */
    TCL_INSTRUCTION_ENTRY("pop", -1),
    /* Pop the topmost stack object */
    TCL_INSTRUCTION_ENTRY("dup", +1),
    /* Duplicate the topmost stack object and push the result */
    TCL_INSTRUCTION_ENTRY1("strcat", 2, INT_MIN, OPERAND_UINT1),
    /* Concatenate the top op1 items and push result */
    DEPRECATED_INSTRUCTION_ENTRY1("invokeStk1", 2, INT_MIN, OPERAND_UINT1),
    /* Invoke command named objv[0]; <objc,objv> = <op1,top op1> */
    TCL_INSTRUCTION_ENTRY1("invokeStk", 5, INT_MIN, OPERAND_UINT4),
    /* Invoke command named objv[0]; <objc,objv> = <op4,top op4> */
    TCL_INSTRUCTION_ENTRY("evalStk", 0),
    /* Evaluate command in stktop using Tcl_EvalObj. */
    TCL_INSTRUCTION_ENTRY("exprStk", 0),
    /* Execute expression in stktop using Tcl_ExprStringObj. */

    DEPRECATED_INSTRUCTION_ENTRY1("loadScalar1", 2, 1, OPERAND_LVT1),
    /* Load scalar variable at index op1 <= 255 in call frame */
    TCL_INSTRUCTION_ENTRY1("loadScalar", 5, 1, OPERAND_LVT4),
    /* Load scalar variable at index op1 >= 256 in call frame */
    TCL_INSTRUCTION_ENTRY("loadScalarStk", 0),
    /* Load scalar variable; scalar's name is stktop */
    DEPRECATED_INSTRUCTION_ENTRY1("loadArray1", 2, 0, OPERAND_LVT1),
    /* Load array element; array at slot op1<=255, element is stktop */
    TCL_INSTRUCTION_ENTRY1("loadArray", 5, 0, OPERAND_LVT4),
    /* Load array element; array at slot op1 > 255, element is stktop */
    TCL_INSTRUCTION_ENTRY("loadArrayStk", -1),
    /* Load array element; element is stktop, array name is stknext */
    TCL_INSTRUCTION_ENTRY("loadStk", 0),
    /* Load general variable; unparsed variable name is stktop */
    DEPRECATED_INSTRUCTION_ENTRY1("storeScalar1", 2, 0, OPERAND_LVT1),
    /* Store scalar variable at op1<=255 in frame; value is stktop */
    TCL_INSTRUCTION_ENTRY1("storeScalar", 5, 0, OPERAND_LVT4),
    /* Store scalar variable at op1 > 255 in frame; value is stktop */
    TCL_INSTRUCTION_ENTRY("storeScalarStk", -1),
    /* Store scalar; value is stktop, scalar name is stknext */
    DEPRECATED_INSTRUCTION_ENTRY1("storeArray1", 2, -1, OPERAND_LVT1),
    /* Store array element; array at op1<=255, value is top then elem */
    TCL_INSTRUCTION_ENTRY1("storeArray", 5, -1, OPERAND_LVT4),
    /* Store array element; array at op1>=256, value is top then elem */
    TCL_INSTRUCTION_ENTRY("storeArrayStk", -2),
    /* Store array element; value is stktop, then elem, array names */
    TCL_INSTRUCTION_ENTRY("storeStk", -1),
    /* Store general variable; value is stktop, then unparsed name */

    DEPRECATED_INSTRUCTION_ENTRY1("incrScalar1", 2, 0, OPERAND_LVT1),
    /* Incr scalar at index op1<=255 in frame; incr amount is stktop */
    TCL_INSTRUCTION_ENTRY("incrScalarStk", -1),
    /* Incr scalar; incr amount is stktop, scalar's name is stknext */
    DEPRECATED_INSTRUCTION_ENTRY1("incrArray1", 2, -1, OPERAND_LVT1),
    /* Incr array elem; arr at slot op1<=255, amount is top then elem */
    TCL_INSTRUCTION_ENTRY("incrArrayStk", -2),
    /* Incr array element; amount is top then elem then array names */
    TCL_INSTRUCTION_ENTRY("incrStk", -1),
    /* Incr general variable; amount is stktop then unparsed var name */
    DEPRECATED_INSTRUCTION_ENTRY2("incrScalar1Imm", 3, +1, OPERAND_LVT1, OPERAND_INT1),
    /* Incr scalar at slot op1 <= 255; amount is 2nd operand byte */
    TCL_INSTRUCTION_ENTRY1("incrScalarStkImm", 2, 0, OPERAND_INT1),
    /* Incr scalar; scalar name is stktop; incr amount is op1 */
    DEPRECATED_INSTRUCTION_ENTRY2("incrArray1Imm", 3, 0, OPERAND_LVT1, OPERAND_INT1),
    /* Incr array elem; array at slot op1 <= 255, elem is stktop,
     * amount is 2nd operand byte */
    TCL_INSTRUCTION_ENTRY1("incrArrayStkImm", 2, -1, OPERAND_INT1),
    /* Incr array element; elem is top then array name, amount is op1 */
    TCL_INSTRUCTION_ENTRY1("incrStkImm", 2, 0, OPERAND_INT1),
    /* Incr general variable; unparsed name is top, amount is op1 */

    DEPRECATED_INSTRUCTION_ENTRY1("jump1", 2, 0, OPERAND_OFFSET1),
    /* Jump relative to (pc + op1) */
    TCL_INSTRUCTION_ENTRY1("jump", 5, 0, OPERAND_OFFSET4),
    /* Jump relative to (pc + op4) */
    DEPRECATED_INSTRUCTION_ENTRY1("jumpTrue1", 2, -1, OPERAND_OFFSET1),
    /* Jump relative to (pc + op1) if stktop expr object is true */
    TCL_INSTRUCTION_ENTRY1("jumpTrue", 5, -1, OPERAND_OFFSET4),
    /* Jump relative to (pc + op4) if stktop expr object is true */
    DEPRECATED_INSTRUCTION_ENTRY1("jumpFalse1", 2, -1, OPERAND_OFFSET1),
    /* Jump relative to (pc + op1) if stktop expr object is false */
    TCL_INSTRUCTION_ENTRY1("jumpFalse", 5, -1, OPERAND_OFFSET4),
    /* Jump relative to (pc + op4) if stktop expr object is false */

    TCL_INSTRUCTION_ENTRY("bitor", -1),
    /* Bitwise or:	push (stknext | stktop) */
    TCL_INSTRUCTION_ENTRY("bitxor", -1),
    /* Bitwise xor	push (stknext ^ stktop) */
    TCL_INSTRUCTION_ENTRY("bitand", -1),
    /* Bitwise and:	push (stknext & stktop) */
    TCL_INSTRUCTION_ENTRY("eq", -1),
    /* Equal:	push (stknext == stktop) */
    TCL_INSTRUCTION_ENTRY("neq", -1),
    /* Not equal:	push (stknext != stktop) */
    TCL_INSTRUCTION_ENTRY("lt", -1),
    /* Less:	push (stknext < stktop) */
    TCL_INSTRUCTION_ENTRY("gt", -1),
    /* Greater:	push (stknext > stktop) */
    TCL_INSTRUCTION_ENTRY("le", -1),
    /* Less or equal: push (stknext <= stktop) */
    TCL_INSTRUCTION_ENTRY("ge", -1),
    /* Greater or equal: push (stknext >= stktop) */
    TCL_INSTRUCTION_ENTRY("lshift", -1),
    /* Left shift:	push (stknext << stktop) */
    TCL_INSTRUCTION_ENTRY("rshift", -1),
    /* Right shift:	push (stknext >> stktop) */
    TCL_INSTRUCTION_ENTRY("add", -1),
    /* Add:		push (stknext + stktop) */
    TCL_INSTRUCTION_ENTRY("sub", -1),
    /* Sub:		push (stkext - stktop) */
    TCL_INSTRUCTION_ENTRY("mult", -1),
    /* Multiply:	push (stknext * stktop) */
    TCL_INSTRUCTION_ENTRY("div", -1),
    /* Divide:	push (stknext / stktop) */
    TCL_INSTRUCTION_ENTRY("mod", -1),
    /* Mod:		push (stknext % stktop) */
    TCL_INSTRUCTION_ENTRY("uplus", 0),
    /* Unary plus:	push +stktop */
    TCL_INSTRUCTION_ENTRY("uminus", 0),
    /* Unary minus:	push -stktop */
    TCL_INSTRUCTION_ENTRY("bitnot", 0),
    /* Bitwise not:	push ~stktop */
    TCL_INSTRUCTION_ENTRY("not", 0),
    /* Logical not:	push !stktop */
    TCL_INSTRUCTION_ENTRY("tryCvtToNumeric", 0),
    /* Try converting stktop to first int then double if possible. */

    TCL_INSTRUCTION_ENTRY("break", 0),
    /* Abort closest enclosing loop; if none, return TCL_BREAK code. */
    TCL_INSTRUCTION_ENTRY("continue", 0),
    /* Skip to next iteration of closest enclosing loop; if none, return
     * TCL_CONTINUE code. */

    TCL_INSTRUCTION_ENTRY1("beginCatch", 5, 0, OPERAND_UINT4),
    /* Record start of catch with the operand's exception index. Push the
     * current stack depth onto a special catch stack. */
    TCL_INSTRUCTION_ENTRY("endCatch", 0),
    /* End of last catch. Pop the bytecode interpreter's catch stack. */
    TCL_INSTRUCTION_ENTRY("pushResult", +1),
    /* Push the interpreter's object result onto the stack. */
    TCL_INSTRUCTION_ENTRY("pushReturnCode", +1),
    /* Push interpreter's return code (e.g. TCL_OK or TCL_ERROR) as a new
     * object onto the stack. */

    TCL_INSTRUCTION_ENTRY("streq", -1),
    /* Str Equal:	push (stknext eq stktop) */
    TCL_INSTRUCTION_ENTRY("strneq", -1),
    /* Str !Equal:	push (stknext neq stktop) */
    TCL_INSTRUCTION_ENTRY("strcmp", -1),
    /* Str Compare:	push (stknext cmp stktop) */
    TCL_INSTRUCTION_ENTRY("strlen", 0),
    /* Str Length:	push (strlen stktop) */
    TCL_INSTRUCTION_ENTRY("strindex", -1),
    /* Str Index:	push (strindex stknext stktop) */
    TCL_INSTRUCTION_ENTRY1("strmatch", 2, -1, OPERAND_INT1),
    /* Str Match:	push (strmatch stknext stktop) opnd == nocase */

    TCL_INSTRUCTION_ENTRY1("list", 5, INT_MIN, OPERAND_UINT4),
    /* List:	push (stk1 stk2 ... stktop) */
    TCL_INSTRUCTION_ENTRY("listIndex", -1),
    /* List Index:	push (listindex stknext stktop) */
    TCL_INSTRUCTION_ENTRY("listLength", 0),
    /* List Len:	push (listlength stktop) */

    DEPRECATED_INSTRUCTION_ENTRY1("appendScalar1", 2, 0, OPERAND_LVT1),
    /* Append scalar variable at op1<=255 in frame; value is stktop */
    TCL_INSTRUCTION_ENTRY1("appendScalar", 5, 0, OPERAND_LVT4),
    /* Append scalar variable at op1 > 255 in frame; value is stktop */
    DEPRECATED_INSTRUCTION_ENTRY1("appendArray1", 2, -1, OPERAND_LVT1),
    /* Append array element; array at op1<=255, value is top then elem */
    TCL_INSTRUCTION_ENTRY1("appendArray", 5, -1, OPERAND_LVT4),
    /* Append array element; array at op1>=256, value is top then elem */
    TCL_INSTRUCTION_ENTRY("appendArrayStk", -2),
    /* Append array element; value is stktop, then elem, array names */
    TCL_INSTRUCTION_ENTRY("appendStk", -1),
    /* Append general variable; value is stktop, then unparsed name */
    DEPRECATED_INSTRUCTION_ENTRY1("lappendScalar1", 2, 0, OPERAND_LVT1),
    /* Lappend scalar variable at op1<=255 in frame; value is stktop */
    TCL_INSTRUCTION_ENTRY1("lappendScalar", 5, 0, OPERAND_LVT4),
    /* Lappend scalar variable at op1 > 255 in frame; value is stktop */
    DEPRECATED_INSTRUCTION_ENTRY1("lappendArray1", 2, -1, OPERAND_LVT1),
    /* Lappend array element; array at op1<=255, value is top then elem */
    TCL_INSTRUCTION_ENTRY1("lappendArray", 5, -1, OPERAND_LVT4),
    /* Lappend array element; array at op1>=256, value is top then elem */
    TCL_INSTRUCTION_ENTRY("lappendArrayStk", -2),
    /* Lappend array element; value is stktop, then elem, array names */
    TCL_INSTRUCTION_ENTRY("lappendStk", -1),
    /* Lappend general variable; value is stktop, then unparsed name */

    TCL_INSTRUCTION_ENTRY1("lindexMulti", 5, INT_MIN, OPERAND_UINT4),
    /* Lindex with generalized args, operand is number of stacked objs
     * used: (operand-1) entries from stktop are the indices; then list to
     * process. */
    TCL_INSTRUCTION_ENTRY1("over", 5, +1, OPERAND_UINT4),
    /* Duplicate the arg-th element from top of stack (TOS=0) */
    TCL_INSTRUCTION_ENTRY("lsetList", -2),
    /* Four-arg version of 'lset'. stktop is old value; next is new
     * element value, next is the index list; pushes new value */
    TCL_INSTRUCTION_ENTRY1("lsetFlat", 5, INT_MIN, OPERAND_UINT4),
    /* Three- or >=5-arg version of 'lset', operand is number of stacked
     * objs: stktop is old value, next is new element value, next come
     * (operand-2) indices; pushes the new value. */

    TCL_INSTRUCTION_ENTRY2("returnImm", 9, -1, OPERAND_INT4, OPERAND_UINT4),
    /* Compiled [return], code, level are operands; options and result
     * are on the stack. */
    TCL_INSTRUCTION_ENTRY("expon", -1),
    /* Binary exponentiation operator: push (stknext ** stktop) */

    /*
     * NOTE: the stack effects of expandStkTop and invokeExpanded are wrong -
     * but it cannot be done right at compile time, the stack effect is only
     * known at run time. The value for invokeExpanded is estimated better at
     * compile time.
     * See the comments further down in this file, where INST_INVOKE_EXPANDED
     * is emitted.
     */
    TCL_INSTRUCTION_ENTRY("expandStart", 0),
    /* Start of command with {*} (expanded) arguments */
    TCL_INSTRUCTION_ENTRY1("expandStkTop", 5, 0, OPERAND_UINT4),
    /* Expand the list at stacktop: push its elements on the stack */
    TCL_INSTRUCTION_ENTRY("invokeExpanded", 0),
    /* Invoke the command marked by the last 'expandStart' */

    TCL_INSTRUCTION_ENTRY1("listIndexImm", 5, 0, OPERAND_IDX4),
    /* List Index:	push (lindex stktop op4) */
    TCL_INSTRUCTION_ENTRY2("listRangeImm", 9, 0, OPERAND_IDX4, OPERAND_IDX4),
    /* List Range:	push (lrange stktop op4 op4) */
    TCL_INSTRUCTION_ENTRY2("startCommand", 9, 0, OPERAND_OFFSET4, OPERAND_UINT4),
    /* Start of bytecoded command: op is the length of the cmd's code, op2
     * is number of commands here */

    TCL_INSTRUCTION_ENTRY("listIn", -1),
    /* List containment: push [lsearch stktop stknext]>=0 */
    TCL_INSTRUCTION_ENTRY("listNotIn", -1),
    /* List negated containment: push [lsearch stktop stknext]<0 */

    TCL_INSTRUCTION_ENTRY("pushReturnOpts", +1),
    /* Push the interpreter's return option dictionary as an object on the
     * stack. */
    TCL_INSTRUCTION_ENTRY("returnStk", -1),
    /* Compiled [return]; options and result are on the stack, code and
     * level are in the options. */

    TCL_INSTRUCTION_ENTRY1("dictGet", 5, INT_MIN, OPERAND_UINT4),
    /* The top op4 words (min 1) are a key path into the dictionary just
     * below the keys on the stack, and all those values are replaced by
     * the value read out of that key-path (like [dict get]).
     * Stack:  ... dict key1 ... keyN => ... value */
    TCL_INSTRUCTION_ENTRY2("dictSet", 9, INT_MIN, OPERAND_UINT4, OPERAND_LVT4),
    /* Update a dictionary value such that the keys are a path pointing to
     * the value. op4#1 = numKeys, op4#2 = LVTindex
     * Stack:  ... key1 ... keyN value => ... newDict */
    TCL_INSTRUCTION_ENTRY2("dictUnset", 9, INT_MIN, OPERAND_UINT4, OPERAND_LVT4),
    /* Update a dictionary value such that the keys are not a path pointing
     * to any value. op4#1 = numKeys, op4#2 = LVTindex
     * Stack:  ... key1 ... keyN => ... newDict */
    TCL_INSTRUCTION_ENTRY2("dictIncrImm", 9, 0, OPERAND_INT4, OPERAND_LVT4),
    /* Update a dictionary value such that the value pointed to by key is
     * incremented by some value (or set to it if the key isn't in the
     * dictionary at all). op4#1 = incrAmount, op4#2 = LVTindex
     * Stack:  ... key => ... newDict */
    TCL_INSTRUCTION_ENTRY1("dictAppend", 5, -1, OPERAND_LVT4),
    /* Update a dictionary value such that the value pointed to by key has
     * some value string-concatenated onto it. op4 = LVTindex
     * Stack:  ... key valueToAppend => ... newDict */
    TCL_INSTRUCTION_ENTRY1("dictLappend", 5, -1, OPERAND_LVT4),
    /* Update a dictionary value such that the value pointed to by key has
     * some value list-appended onto it. op4 = LVTindex
     * Stack:  ... key valueToAppend => ... newDict */
    TCL_INSTRUCTION_ENTRY1("dictFirst", 5, +2, OPERAND_LVT4),
    /* Begin iterating over the dictionary, using the local scalar
     * indicated by op4 to hold the iterator state. The local scalar
     * should not refer to a named variable as the value is not wholly
     * managed correctly.
     * Stack:  ... dict => ... value key doneBool */
    TCL_INSTRUCTION_ENTRY1("dictNext", 5, +3, OPERAND_LVT4),
    /* Get the next iteration from the iterator in op4's local scalar.
     * Stack:  ... => ... value key doneBool */
    TCL_INSTRUCTION_ENTRY2("dictUpdateStart", 9, 0, OPERAND_LVT4, OPERAND_AUX4),
    /* Create the variables (described in the aux data referred to by the
     * second immediate argument) to mirror the state of the dictionary in
     * the variable referred to by the first immediate argument. The list
     * of keys (top of the stack, not popped) must be the same length as
     * the list of variables.
     * Stack:  ... keyList => ... keyList */
    TCL_INSTRUCTION_ENTRY2("dictUpdateEnd", 9, -1, OPERAND_LVT4, OPERAND_AUX4),
    /* Reflect the state of local variables (described in the aux data
     * referred to by the second immediate argument) back to the state of
     * the dictionary in the variable referred to by the first immediate
     * argument. The list of keys (popped from the stack) must be the same
     * length as the list of variables.
     * Stack:  ... keyList => ... */
    TCL_INSTRUCTION_ENTRY1("jumpTable", 5, -1, OPERAND_AUX4),
    /* Jump according to the jump-table (in AuxData as indicated by the
     * operand) and the argument popped from the list. Always executes the
     * next instruction if no match against the table's entries was found.
     * Keys are strings.
     * Stack:  ... value => ...
     * Note that the jump table contains offsets relative to the PC when
     * it points to this instruction; the code is relocatable. */
    TCL_INSTRUCTION_ENTRY1("upvar", 5, -1, OPERAND_LVT4),
    /* finds level and otherName in stack, links to local variable at
     * index op1. Leaves the level on stack. */
    TCL_INSTRUCTION_ENTRY1("nsupvar", 5, -1, OPERAND_LVT4),
    /* finds namespace and otherName in stack, links to local variable at
     * index op1. Leaves the namespace on stack. */
    TCL_INSTRUCTION_ENTRY1("variable", 5, -1, OPERAND_LVT4),
    /* finds namespace and otherName in stack, links to local variable at
     * index op1. Leaves the namespace on stack. */
    TCL_INSTRUCTION_ENTRY2("syntax", 9, -1, OPERAND_INT4, OPERAND_UINT4),
    /* Compiled bytecodes to signal syntax error. Equivalent to returnImm
     * except for the ERR_ALREADY_LOGGED flag in the interpreter. */
    TCL_INSTRUCTION_ENTRY1("reverse", 5, 0, OPERAND_UINT4),
    /* Reverse the order of the arg elements at the top of stack */

    TCL_INSTRUCTION_ENTRY1("regexp", 2, -1, OPERAND_INT1),
    /* Regexp:	push (regexp stknext stktop) opnd == nocase */

    TCL_INSTRUCTION_ENTRY1("existScalar", 5, 1, OPERAND_LVT4),
    /* Test if scalar variable at index op1 in call frame exists */
    TCL_INSTRUCTION_ENTRY1("existArray", 5, 0, OPERAND_LVT4),
    /* Test if array element exists; array at slot op1, element is
     * stktop */
    TCL_INSTRUCTION_ENTRY("existArrayStk", -1),
    /* Test if array element exists; element is stktop, array name is
     * stknext */
    TCL_INSTRUCTION_ENTRY("existStk", 0),
    /* Test if general variable exists; unparsed variable name is stktop*/

    TCL_INSTRUCTION_ENTRY("nop", 0),
    /* Do nothing */
    DEPRECATED_INSTRUCTION_ENTRY("returnCodeBranch1", -1),
    /* Jump to next instruction based on the return code on top of stack
     * ERROR: +1;	RETURN: +3;	BREAK: +5;	CONTINUE: +7;
     * Other non-OK: +9 */

    TCL_INSTRUCTION_ENTRY2("unsetScalar", 6, 0, OPERAND_UNSF1, OPERAND_LVT4),
    /* Make scalar variable at index op2 in call frame cease to exist;
     * op1 is 1 for errors on problems, 0 otherwise */
    TCL_INSTRUCTION_ENTRY2("unsetArray", 6, -1, OPERAND_UNSF1, OPERAND_LVT4),
    /* Make array element cease to exist; array at slot op2, element is
     * stktop; op1 is 1 for errors on problems, 0 otherwise */
    TCL_INSTRUCTION_ENTRY1("unsetArrayStk", 2, -2, OPERAND_UNSF1),
    /* Make array element cease to exist; element is stktop, array name is
     * stknext; op1 is 1 for errors on problems, 0 otherwise */
    TCL_INSTRUCTION_ENTRY1("unsetStk", 2, -1, OPERAND_UNSF1),
    /* Make general variable cease to exist; unparsed variable name is
     * stktop; op1 is 1 for errors on problems, 0 otherwise */

    TCL_INSTRUCTION_ENTRY("dictExpand", -1),
    /* Probe into a dict and extract it (or a subdict of it) into
     * variables with matched names. Produces list of keys bound as
     * result. Part of [dict with].
     * Stack:  ... dict path => ... keyList */
    TCL_INSTRUCTION_ENTRY("dictRecombineStk", -3),
    /* Map variable contents back into a dictionary in a variable. Part of
     * [dict with].
     * Stack:  ... dictVarName path keyList => ... */
    TCL_INSTRUCTION_ENTRY1("dictRecombineImm", 5, -2, OPERAND_LVT4),
    /* Map variable contents back into a dictionary in the local variable
     * indicated by the LVT index. Part of [dict with].
     * Stack:  ... path keyList => ... */
    TCL_INSTRUCTION_ENTRY1("dictExists", 5, INT_MIN, OPERAND_UINT4),
    /* The top op4 words (min 1) are a key path into the dictionary just
     * below the keys on the stack, and all those values are replaced by a
     * boolean indicating whether it is possible to read out a value from
     * that key-path (like [dict exists]).
     * Stack:  ... dict key1 ... keyN => ... boolean */
    TCL_INSTRUCTION_ENTRY("verifyDict", -1),
    /* Verifies that the word on the top of the stack is a dictionary,
     * popping it if it is and throwing an error if it is not.
     * Stack:  ... value => ... */

    TCL_INSTRUCTION_ENTRY("strmap", -2),
    /* Simplified version of [string map] that only applies one change
     * string, and only case-sensitively.
     * Stack:  ... from to string => ... changedString */
    TCL_INSTRUCTION_ENTRY("strfind", -1),
    /* Find the first index of a needle string in a haystack string,
     * producing the index (integer) or -1 if nothing found.
     * Stack:  ... needle haystack => ... index */
    TCL_INSTRUCTION_ENTRY("strrfind", -1),
    /* Find the last index of a needle string in a haystack string,
     * producing the index (integer) or -1 if nothing found.
     * Stack:  ... needle haystack => ... index */
    TCL_INSTRUCTION_ENTRY2("strrangeImm", 9, 0, OPERAND_IDX4, OPERAND_IDX4),
    /* String Range: push (string range stktop op4 op4) */
    TCL_INSTRUCTION_ENTRY("strrange", -2),
    /* String Range with non-constant arguments.
     * Stack:  ... string idxA idxB => ... substring */

    TCL_INSTRUCTION_ENTRY("yield", 0),
    /* Makes the current coroutine yield the value at the top of the
     * stack, and places the response back on top of the stack when it
     * resumes.
     * Stack:  ... valueToYield => ... resumeValue */
    TCL_INSTRUCTION_ENTRY("coroName", +1),
    /* Push the name of the interpreter's current coroutine as an object
     * on the stack. */
    DEPRECATED_INSTRUCTION_ENTRY1("tailcall", 2, INT_MIN, OPERAND_UINT1),
    /* Do a tailcall with the opnd items on the stack as the thing to
     * tailcall to; opnd must be greater than 0 for the semantics to work
     * right. */

    TCL_INSTRUCTION_ENTRY("currentNamespace", +1),
    /* Push the name of the interpreter's current namespace as an object
     * on the stack. */
    TCL_INSTRUCTION_ENTRY("infoLevelNumber", +1),
    /* Push the stack depth (i.e., [info level]) of the interpreter as an
     * object on the stack. */
    TCL_INSTRUCTION_ENTRY("infoLevelArgs", 0),
    /* Push the argument words to a stack depth (i.e., [info level <n>])
     * of the interpreter as an object on the stack.
     * Stack:  ... depth => ... argList */
    TCL_INSTRUCTION_ENTRY("resolveCmd", 0),
    /* Resolves the command named on the top of the stack to its fully
     * qualified version, or produces the empty string if no such command
     * exists. Never generates errors.
     * Stack:  ... cmdName => ... fullCmdName */

    TCL_INSTRUCTION_ENTRY("tclooSelf", +1),
    /* Push the identity of the current TclOO object (i.e., the name of
     * its current public access command) on the stack. */
    TCL_INSTRUCTION_ENTRY("tclooClass", 0),
    /* Push the class of the TclOO object named at the top of the stack
     * onto the stack.
     * Stack:  ... object => ... class */
    TCL_INSTRUCTION_ENTRY("tclooNamespace", 0),
    /* Push the namespace of the TclOO object named at the top of the
     * stack onto the stack.
     * Stack:  ... object => ... namespace */
    TCL_INSTRUCTION_ENTRY("tclooIsObject", 0),
    /* Push whether the value named at the top of the stack is a TclOO
     * object (i.e., a boolean). Can corrupt the interpreter result
     * despite not throwing, so not safe for use in a post-exception
     * context.
     * Stack:  ... value => ... boolean */

    TCL_INSTRUCTION_ENTRY("arrayExistsStk", 0),
    /* Looks up the element on the top of the stack and tests whether it
     * is an array. Pushes a boolean describing whether this is the
     * case. Also runs the whole-array trace on the named variable, so can
     * throw anything.
     * Stack:  ... varName => ... boolean */
    TCL_INSTRUCTION_ENTRY1("arrayExistsImm", 5, +1, OPERAND_LVT4),
    /* Looks up the variable indexed by opnd and tests whether it is an
     * array. Pushes a boolean describing whether this is the case. Also
     * runs the whole-array trace on the named variable, so can throw
     * anything.
     * Stack:  ... => ... boolean */
    TCL_INSTRUCTION_ENTRY("arrayMakeStk", -1),
    /* Forces the element on the top of the stack to be the name of an
     * array.
     * Stack:  ... varName => ... */
    TCL_INSTRUCTION_ENTRY1("arrayMakeImm", 5, 0, OPERAND_LVT4),
    /* Forces the variable indexed by opnd to be an array. Does not touch
     * the stack. */

    TCL_INSTRUCTION_ENTRY2("invokeReplace", 6, INT_MIN, OPERAND_UINT4, OPERAND_UINT1),
    /* Invoke command named objv[0], replacing the first two words with
     * the op1 words at the top of the stack;
     * <objc,objv> = <op4,top op4 after popping 1> */

    TCL_INSTRUCTION_ENTRY("listConcat", -1),
    /* Concatenates the two lists at the top of the stack into a single
     * list and pushes that resulting list onto the stack.
     * Stack: ... list1 list2 => ... [lconcat list1 list2] */

    TCL_INSTRUCTION_ENTRY("expandDrop", 0),
    /* Drops an element from the auxiliary stack, popping stack elements
     * until the matching stack depth is reached. */

    /* New foreach implementation */
    TCL_INSTRUCTION_ENTRY1("foreach_start", 5, +2, OPERAND_AUX4),
    /* Initialize execution of a foreach loop. Operand is aux data index
     * of the ForeachInfo structure for the foreach command. It pushes 2
     * elements which hold runtime params for foreach_step, they are later
     * dropped by foreach_end together with the value lists. NOTE that the
     * iterator-tracker and info reference must not be passed to bytecodes
     * that handle normal Tcl values. NOTE that this instruction jumps to
     * the foreach_step instruction paired with it; the stack info below
     * is only nominal.
     * Stack: ... listObjs... => ... listObjs... iterTracker info */
    TCL_INSTRUCTION_ENTRY("foreach_step", 0),
    /* "Step" or begin next iteration of foreach loop. Assigns to foreach
     * iteration variables. May jump to straight after the foreach_start
     * that pushed the iterTracker and info values. MUST be followed
     * immediately by a foreach_end.
     * Stack: ... listObjs... iterTracker info =>
     *				... listObjs... iterTracker info */
    TCL_INSTRUCTION_ENTRY("foreach_end", 0),
    /* Clean up a foreach loop by dropping the info value, the tracker
     * value and the lists that were being iterated over.
     * Stack: ... listObjs... iterTracker info => ... */
    TCL_INSTRUCTION_ENTRY("lmap_collect", -1),
    /* Appends the value at the top of the stack to the list located on
     * the stack the "other side" of the foreach-related values.
     * Stack: ... collector listObjs... iterTracker info value =>
     *			... collector listObjs... iterTracker info */

    TCL_INSTRUCTION_ENTRY("strtrim", -1),
    /* [string trim] core: removes the characters (designated by the value
     * at the top of the stack) from both ends of the string and pushes
     * the resulting string.
     * Stack: ... string charset => ... trimmedString */
    TCL_INSTRUCTION_ENTRY("strtrimLeft", -1),
    /* [string trimleft] core: removes the characters (designated by the
     * value at the top of the stack) from the left of the string and
     * pushes the resulting string.
     * Stack: ... string charset => ... trimmedString */
    TCL_INSTRUCTION_ENTRY("strtrimRight", -1),
    /* [string trimright] core: removes the characters (designated by the
     * value at the top of the stack) from the right of the string and
     * pushes the resulting string.
     * Stack: ... string charset => ... trimmedString */

    TCL_INSTRUCTION_ENTRY1("concatStk", 5, INT_MIN, OPERAND_UINT4),
    /* Wrapper round Tcl_ConcatObj(), used for [concat] and [eval]. opnd
     * is number of values to concatenate.
     * Operation:	push concat(stk1 stk2 ... stktop) */

    TCL_INSTRUCTION_ENTRY("strcaseUpper", 0),
    /* [string toupper] core: converts whole string to upper case using
     * the default (extended "C" locale) rules.
     * Stack: ... string => ... newString */
    TCL_INSTRUCTION_ENTRY("strcaseLower", 0),
    /* [string tolower] core: converts whole string to upper case using
     * the default (extended "C" locale) rules.
     * Stack: ... string => ... newString */
    TCL_INSTRUCTION_ENTRY("strcaseTitle", 0),
    /* [string totitle] core: converts whole string to upper case using
     * the default (extended "C" locale) rules.
     * Stack: ... string => ... newString */
    TCL_INSTRUCTION_ENTRY("strreplace", -3),
    /* [string replace] core: replaces a non-empty range of one string
     * with the contents of another.
     * Stack: ... string fromIdx toIdx replacement => ... newString */

    TCL_INSTRUCTION_ENTRY("originCmd", 0),
    /* Reports which command was the origin (via namespace import chain)
     * of the command named on the top of the stack.
     * Stack:  ... cmdName => ... fullOriginalCmdName */

    DEPRECATED_INSTRUCTION_ENTRY1("tclooNext", 2, INT_MIN, OPERAND_UINT1),
    /* Call the next item on the TclOO call chain, passing opnd arguments
     * (min 1, max 255, *includes* "next").  The result of the invoked
     * method implementation will be pushed on the stack in place of the
     * arguments (similar to invokeStk).
     * Stack:  ... "next" arg2 arg3 -- argN => ... result */
    DEPRECATED_INSTRUCTION_ENTRY1("tclooNextClass", 2, INT_MIN, OPERAND_UINT1),
    /* Call the following item on the TclOO call chain defined by class
     * className, passing opnd arguments (min 2, max 255, *includes*
     * "nextto" and the class name). The result of the invoked method
     * implementation will be pushed on the stack in place of the
     * arguments (similar to invokeStk).
     * Stack:  ... "nextto" className arg3 arg4 -- argN => ... result */

    TCL_INSTRUCTION_ENTRY("yieldToInvoke", 0),
    /* Makes the current coroutine yield the value at the top of the
     * stack, invoking the given command/args with resolution in the given
     * namespace (all packed into a list), and places the list of values
     * that are the response back on top of the stack when it resumes.
     * Stack:  ... [list ns cmd arg1 ... argN] => ... resumeList */

    TCL_INSTRUCTION_ENTRY("numericType", 0),
    /* Pushes the numeric type code of the word at the top of the stack.
     * Stack:  ... value => ... typeCode */
    TCL_INSTRUCTION_ENTRY("tryCvtToBoolean", +1),
    /* Try converting stktop to boolean if possible. No errors.
     * Stack:  ... value => ... value isStrictBool */
    TCL_INSTRUCTION_ENTRY1("strclass", 2, 0, OPERAND_SCLS1),
    /* See if all the characters of the given string are a member of the
     * specified (by opnd) character class. Note that an empty string will
     * satisfy the class check (standard definition of "all").
     * Stack:  ... stringValue => ... boolean */

    TCL_INSTRUCTION_ENTRY1("lappendList", 5, 0, OPERAND_LVT4),
    /* Lappend list to scalar variable at op4 in frame.
     * Stack:  ... list => ... listVarContents */
    TCL_INSTRUCTION_ENTRY1("lappendListArray", 5, -1, OPERAND_LVT4),
    /* Lappend list to array element; array at op4.
     * Stack:  ... elem list => ... listVarContents */
    TCL_INSTRUCTION_ENTRY("lappendListArrayStk", -2),
    /* Lappend list to array element.
     * Stack:  ... arrayName elem list => ... listVarContents */
    TCL_INSTRUCTION_ENTRY("lappendListStk", -1),
    /* Lappend list to general variable.
     * Stack:  ... varName list => ... listVarContents */

    TCL_INSTRUCTION_ENTRY1("clockRead", 2, +1, OPERAND_CLK1),
    /* Read clock out to the stack. Operand is which clock to read
     * 0=clicks, 1=microseconds, 2=milliseconds, 3=seconds.
     * Stack: ... => ... time */

    TCL_INSTRUCTION_ENTRY1("dictGetDef", 5, INT_MIN, OPERAND_UINT4),
    /* The top word is the default, the next op4 words (min 1) are a key
     * path into the dictionary just below the keys on the stack, and all
     * those values are replaced by the value read out of that key-path
     * (like [dict get]) except if there is no such key, when instead the
     * default is pushed instead.
     * Stack:  ... dict key1 ... keyN default => ... value */

    TCL_INSTRUCTION_ENTRY("strlt", -1),
    /* String Less:			push (stknext < stktop) */
    TCL_INSTRUCTION_ENTRY("strgt", -1),
    /* String Greater:		push (stknext > stktop) */
    TCL_INSTRUCTION_ENTRY("strle", -1),
    /* String Less or equal:	push (stknext <= stktop) */
    TCL_INSTRUCTION_ENTRY("strge", -1),
    /* String Greater or equal:	push (stknext >= stktop) */
    TCL_INSTRUCTION_ENTRY2("lreplace", 6, INT_MIN, OPERAND_UINT4, OPERAND_LRPL1),
    /* Operands: number of arguments, flags
     * flags: Combination of TCL_LREPLACE_* flags
     * Stack: ... listobj index1 ?index2? new1 ... newN => ... newlistobj
     * where index2 is present only if TCL_LREPLACE_SINGLE_INDEX is not
     * set in flags. */

    TCL_INSTRUCTION_ENTRY1("constImm", 5, -1, OPERAND_LVT4),
    /* Create constant. Index into LVT is immediate, value is on stack.
     * Stack: ... value => ... */
    TCL_INSTRUCTION_ENTRY("constStk", -2),
    /* Create constant. Variable name and value on stack.
     * Stack: ... varName value => ... */

    TCL_INSTRUCTION_ENTRY1("incrScalar", 5, 0, OPERAND_LVT4),
    /* Incr scalar at index op1 in frame; incr amount is stktop */
    TCL_INSTRUCTION_ENTRY1("incrArray", 5, -1, OPERAND_LVT4),
    /* Incr array elem; arr at slot op1, amount is top then elem */
    TCL_INSTRUCTION_ENTRY2("incrScalarImm", 6, +1, OPERAND_LVT4, OPERAND_INT1),
    /* Incr scalar at slot op1; amount is 2nd operand byte */
    TCL_INSTRUCTION_ENTRY2("incrArrayImm", 6, 0, OPERAND_LVT4, OPERAND_INT1),
    /* Incr array elem; array at slot op1, elem is stktop,
     * amount is 2nd operand byte */
    TCL_INSTRUCTION_ENTRY1("tailcall", 5, INT_MIN, OPERAND_UINT4),
    /* Do a tailcall with the opnd items on the stack as the thing to
     * tailcall to; opnd must be greater than 0 for the semantics to work
     * right. */
    TCL_INSTRUCTION_ENTRY1("tclooNext", 5, INT_MIN, OPERAND_UINT4),
    /* Call the next item on the TclOO call chain, passing opnd arguments
     * (min 1, *includes* "next").  The result of the invoked
     * method implementation will be pushed on the stack in place of the
     * arguments (similar to invokeStk).
     * Stack:  ... "next" arg2 arg3 -- argN => ... result */
    TCL_INSTRUCTION_ENTRY1("tclooNextClass", 5, INT_MIN, OPERAND_UINT4),
    /* Call the following item on the TclOO call chain defined by class
     * className, passing opnd arguments (min 2, *includes*
     * "nextto" and the class name). The result of the invoked method
     * implementation will be pushed on the stack in place of the
     * arguments (similar to invokeStk).
     * Stack:  ... "nextto" className arg3 arg4 -- argN => ... result */

    TCL_INSTRUCTION_ENTRY("swap", 0),
    /* Exchanges the top two items on the stack.
     * Stack:  ... val1 val2 => ... val2 val1 */
    TCL_INSTRUCTION_ENTRY1("errorPrefixEq", 5, -1, OPERAND_UINT4),
    /* Compare the two lists at stack top for equality in the first opnd
     * words. The words are themselves compared using string equality.
     * As: [string equal [lrange list1 0 opnd] [lrange list2 0 opnd]]
     * Stack:  ... list1 list2 => isEqual */
    TCL_INSTRUCTION_ENTRY("tclooId", 0),
    /* Push the global ID of the TclOO object named at the top of the
     * stack onto the stack.
     * Stack:  ... object => ... id */
    TCL_INSTRUCTION_ENTRY("dictPut", -2),
    /* Modify the dict by replacing/creating the key/value pair given,
     * pushing the result on the stack.
     * Stack:  ... dict key value => ... updatedDict */
    TCL_INSTRUCTION_ENTRY("dictRemove", -1),
    /* Modify the dict by removing the key/value pair for the given key,
     * pushing the result on the stack.
     * Stack:  ... dict key => ... updatedDict */
    TCL_INSTRUCTION_ENTRY("isEmpty", 0),
    /* Test if the value at the top of the stack is empty (via a call to
     * Tcl_IsEmpty).
     * Stack:  ... value => ... boolean */
    TCL_INSTRUCTION_ENTRY1("jumpTableNum", 5, -1, OPERAND_AUX4),
    /* Jump according to the jump-table (in AuxData as indicated by the
     * operand) and the argument popped from the list. Always executes the
     * next instruction if no match against the table's entries was found.
     * Keys are Tcl_WideInt.
     * Stack:  ... value => ...
     * Note that the jump table contains offsets relative to the PC when
     * it points to this instruction; the code is relocatable. */
    TCL_INSTRUCTION_ENTRY("tailcallList", 0),
    /* Do a tailcall with the words from the argument as the thing to
     * tailcall to, and currNs is the namespace scope.
     * Stack: ... {currNs words...} => ...[NOT REACHED] */
    TCL_INSTRUCTION_ENTRY("tclooNextList", 0),
    /* Call the next item on the TclOO call chain, passing the arguments
     * from argumentList (min 1, *includes* "next"). The result of the
     * invoked method implementation will be pushed on the stack after the
     * target returns.
     * Stack:  ... argumentList => ... result */
    TCL_INSTRUCTION_ENTRY("tclooNextClassList", 0),
    /* Call the following item on the TclOO call chain defined by class
     * className, passing the arguments from argumentList (min 2,
     * *includes* "nextto" and the class name). The result of the invoked
     * method implementation will be pushed on the stack after the target
     * returns.
     * Stack:  ... argumentList => ... result */
    TCL_INSTRUCTION_ENTRY1("arithSeries", 2, -3, OPERAND_UINT1),
    /* Push a new arithSeries object on the stack. The opnd is a bit mask
     * stating which values are valid; bit 0 -> from, bit 1 -> to,
     * bit 2 -> step, bit 3 -> count. Invalid values are passed to
     * TclNewArithSeriesObj() as NULL (and the corresponding values on the
     * stack simply are ignored).
     * Stack:  ... from to step count => ... series */
    TCL_INSTRUCTION_ENTRY("uplevel", -1),
    /* Call the script in the given stack level, and stack the result.
     * Stack:  ... level script => ... result */

    {NULL, 0, 0, 0, {OPERAND_NONE}}};
/* === End copied helpers from Tcl's compiler === */

#if defined(LAST_INST_OPCODE)
_Static_assert((sizeof(tbcxInstructionTable) / sizeof(tbcxInstructionTable[0])) == (size_t)LAST_INST_OPCODE, "tbcx: opcode table size mismatch vs LAST_INST_OPCODE");
#endif

static void AppendPrintf(Tcl_Obj *dst, const char *fmt, ...) {
    char    stackbuf[1024];
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);
    if (need < (int)sizeof(stackbuf)) {
        Tcl_AppendToObj(dst, stackbuf, need);
        return;
    }
    char   *buf = (char *)Tcl_Alloc(need + 1);
    va_list ap2;
    va_start(ap2, fmt);
    vsnprintf(buf, need + 1, fmt, ap2);
    va_end(ap2);
    Tcl_AppendToObj(dst, buf, need);
    Tcl_Free(buf);
}

static void AppendEscaped(Tcl_Obj *dst, const char *buf, Tcl_Size n) {
    Tcl_DString ds;
    Tcl_DStringInit(&ds);
    for (Tcl_Size i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x20 || c == 0x7F || c == '\\' || c == '"') {
            char tmp[5];
            snprintf(tmp, sizeof(tmp), "\\x%02X", (unsigned)c);
            Tcl_DStringAppend(&ds, tmp, -1);
        } else {
            Tcl_DStringAppend(&ds, (const char *)&buf[i], 1);
        }
    }
    Tcl_AppendToObj(dst, Tcl_DStringValue(&ds), Tcl_DStringLength(&ds));
    Tcl_DStringFree(&ds);
}

static const char *ObjTypeName(const Tcl_Obj *o) {
    if (!o || !o->typePtr)
        return "string";
    return o->typePtr->name ? o->typePtr->name : "string";
}

static int ReadAll(Tcl_Channel ch, unsigned char *dst, Tcl_Size need) {
    while (need) {
        Tcl_Size n = Tcl_ReadRaw(ch, (char *)dst, need);
        if (n <= 0)
            return 0;
        dst += n;
        need -= n;
    }
    return 1;
}

static int ReadOneLiteral(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Obj **outObj);

typedef struct DumpAuxSummary {
    unsigned char kind;
    Tcl_Obj      *summary;
} DumpAuxSummary;

typedef struct DumpExcept {
    unsigned char type;
    uint32_t      nesting;
    uint32_t      codeFrom, codeTo;
    uint32_t      cont, brk, cat;
} DumpExcept;

typedef struct DumpBC {
    unsigned char  *code;
    size_t          codeLen;
    Tcl_Obj       **lits;
    uint32_t        numLits;
    DumpAuxSummary *aux;
    uint32_t        numAux;
    DumpExcept     *xr;
    uint32_t        numEx;
    uint32_t        maxStack;
    uint32_t        numLocals;
} DumpBC;

static void FreeDumpBC(DumpBC *bc) {
    if (!bc)
        return;
    if (bc->code)
        Tcl_Free((char *)bc->code);
    if (bc->lits) {
        for (uint32_t i = 0; i < bc->numLits; i++)
            if (bc->lits[i])
                Tcl_DecrRefCount(bc->lits[i]);
        Tcl_Free((char *)bc->lits);
    }
    if (bc->aux) {
        for (uint32_t i = 0; i < bc->numAux; i++)
            if (bc->aux[i].summary)
                Tcl_DecrRefCount(bc->aux[i].summary);
        Tcl_Free((char *)bc->aux);
    }
    if (bc->xr)
        Tcl_Free((char *)bc->xr);
    memset(bc, 0, sizeof(*bc));
}

static int ReadHeader(Tcl_Interp *interp, Tcl_Channel ch, TbcxHeader *H) {
    unsigned char b[sizeof(TbcxHeader)];
    if (!ReadAll(ch, b, (Tcl_Size)sizeof b)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: header", -1));
        return TCL_ERROR;
    }
    H->magic         = le32(b + 0);
    H->format        = le32(b + 4);
    H->flags         = le32(b + 8);
    H->codeLen       = le64(b + 12);
    H->numCmds       = le32(b + 20);
    H->numExcept     = le32(b + 24);
    H->numLiterals   = le32(b + 28);
    H->numAux        = le32(b + 32);
    H->numLocals     = le32(b + 36);
    H->maxStackDepth = le32(b + 40);

    if (H->magic != TBCX_MAGIC || H->format != TBCX_FORMAT || H->flags != 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("not a TBCX file", -1));
        return TCL_ERROR;
    }
    return TCL_OK;
}

static int ReadOneAuxSummary(Tcl_Interp *interp, Tcl_Channel ch, DumpAuxSummary *out) {
    unsigned char k;
    if (!ReadAll(ch, &k, 1)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: aux kind", -1));
        return TCL_ERROR;
    }
    out->kind    = k;
    out->summary = Tcl_NewObj();
    Tcl_IncrRefCount(out->summary);

    if (k == TBCX_AUX_JT_STR) {
        unsigned char cb[4];
        if (!ReadAll(ch, cb, 4))
            goto short_aux;
        uint32_t count = le32(cb);
        AppendPrintf(out->summary, "JumpTable[str] entries=%u: ", count);
        for (uint32_t i = 0; i < count; i++) {
            unsigned char lb[4];
            if (!ReadAll(ch, lb, 4))
                goto short_aux;
            uint32_t    L = le32(lb);
            Tcl_DString ds;
            Tcl_DStringInit(&ds);
            if (L) {
                char *tmp = (char *)Tcl_Alloc(L + 1);
                if (!ReadAll(ch, (unsigned char *)tmp, (Tcl_Size)L)) {
                    Tcl_Free(tmp);
                    goto short_aux;
                }
                tmp[L] = 0;
                AppendEscaped(out->summary, tmp, (Tcl_Size)L);
                Tcl_Free(tmp);
            }
            unsigned char ob[4];
            if (!ReadAll(ch, ob, 4))
                goto short_aux;
            uint32_t off = le32(ob);
            AppendPrintf(out->summary, " -> +%u; ", off);
        }
        return TCL_OK;
    } else if (k == TBCX_AUX_JT_NUM) {
        unsigned char cb[4];
        if (!ReadAll(ch, cb, 4))
            goto short_aux;
        uint32_t count = le32(cb);
        AppendPrintf(out->summary, "JumpTable[num] entries=%u: ", count);
        for (uint32_t i = 0; i < count; i++) {
            unsigned char kb[8];
            if (!ReadAll(ch, kb, 8))
                goto short_aux;
            unsigned char ob[4];
            if (!ReadAll(ch, ob, 4))
                goto short_aux;
            unsigned long long key = (unsigned long long)le64(kb);
            uint32_t           off = le32(ob);
            AppendPrintf(out->summary, "%llu -> +%u; ", key, off);
        }
        return TCL_OK;
    } else if (k == TBCX_AUX_DICTUPD) {
        unsigned char nb[4];
        if (!ReadAll(ch, nb, 4))
            goto short_aux;
        uint32_t n = le32(nb);
        AppendPrintf(out->summary, "DictUpdate varIndices[");
        for (uint32_t i = 0; i < n; i++) {
            unsigned char vb[4];
            if (!ReadAll(ch, vb, 4))
                goto short_aux;
            AppendPrintf(out->summary, "%u%s", le32(vb), (i + 1 < n ? "," : ""));
        }
        AppendPrintf(out->summary, "]");
        return TCL_OK;
    } else if (k == TBCX_AUX_NEWFORE || k == TBCX_AUX_FOREACH) {
        unsigned char lb[4];
        if (!ReadAll(ch, lb, 4))
            goto short_aux;
        uint32_t      lists = le32(lb);
        unsigned char tb1[4], tb2[4];
        if (!ReadAll(ch, tb1, 4) || !ReadAll(ch, tb2, 4))
            goto short_aux;
        AppendPrintf(out->summary, "Foreach lists=%u firstTemp=%u loopCtTemp=%u [", lists, le32(tb1), le32(tb2));
        for (uint32_t j = 0; j < lists; j++) {
            unsigned char cb2[4];
            if (!ReadAll(ch, cb2, 4))
                goto short_aux;
            uint32_t nv = le32(cb2);
            AppendPrintf(out->summary, (j ? "; " : ""));
            AppendPrintf(out->summary, "vars=");
            for (uint32_t k2 = 0; k2 < nv; k2++) {
                unsigned char vb[4];
                if (!ReadAll(ch, vb, 4))
                    goto short_aux;
                AppendPrintf(out->summary, "%u%s", le32(vb), (k2 + 1 < nv ? "," : ""));
            }
        }
        AppendPrintf(out->summary, "]");
        return TCL_OK;
    } else {
        AppendPrintf(out->summary, "UNKNOWN-AUX(kind=%u)", k);
        return TCL_OK;
    }

short_aux:
    Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: aux payload", -1));
    return TCL_ERROR;
}

static int ReadDumpBC_Block(Tcl_Interp *interp, Tcl_Channel ch, int packedHasCounts, const TbcxHeader *topHdr, DumpBC *out, uint32_t *outNumLocals) {
    memset(out, 0, sizeof(*out));
    size_t codeLen = packedHasCounts ? 0 : (size_t)topHdr->codeLen;
    if (packedHasCounts) {
        unsigned char lb[4];
        if (!ReadAll(ch, lb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: code len", -1));
            return TCL_ERROR;
        }
        codeLen = (size_t)le32(lb);
    }
    out->code    = (unsigned char *)Tcl_Alloc(codeLen ? codeLen : 1);
    out->codeLen = codeLen;
    if (codeLen && !ReadAll(ch, out->code, (Tcl_Size)codeLen)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: code bytes", -1));
        FreeDumpBC(out);
        return TCL_ERROR;
    }

    uint32_t numLits = packedHasCounts ? 0 : topHdr->numLiterals;
    if (packedHasCounts) {
        unsigned char b4[4];
        if (!ReadAll(ch, b4, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: lit count", -1));
            FreeDumpBC(out);
            return TCL_ERROR;
        }
        numLits = le32(b4);
    }
    out->numLits = numLits;
    if (numLits) {
        out->lits = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * numLits);
        memset(out->lits, 0, sizeof(Tcl_Obj *) * numLits);
        for (uint32_t i = 0; i < numLits; i++) {
            if (ReadOneLiteral(interp, ch, &out->lits[i]) != TCL_OK) {
                FreeDumpBC(out);
                return TCL_ERROR;
            }
            Tcl_IncrRefCount(out->lits[i]);
        }
    }

    uint32_t numAux = packedHasCounts ? 0 : topHdr->numAux;
    if (packedHasCounts) {
        unsigned char b4[4];
        if (!ReadAll(ch, b4, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: aux count", -1));
            FreeDumpBC(out);
            return TCL_ERROR;
        }
        numAux = le32(b4);
    }
    out->numAux = numAux;
    if (numAux) {
        out->aux = (DumpAuxSummary *)Tcl_Alloc(sizeof(DumpAuxSummary) * numAux);
        for (uint32_t i = 0; i < numAux; i++) {
            if (ReadOneAuxSummary(interp, ch, &out->aux[i]) != TCL_OK) {
                FreeDumpBC(out);
                return TCL_ERROR;
            }
        }
    }

    uint32_t numEx = packedHasCounts ? 0 : topHdr->numExcept;
    if (packedHasCounts) {
        unsigned char b4[4];
        if (!ReadAll(ch, b4, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: except count", -1));
            FreeDumpBC(out);
            return TCL_ERROR;
        }
        numEx = le32(b4);
    }
    out->numEx = numEx;
    if (numEx) {
        out->xr = (DumpExcept *)Tcl_Alloc(sizeof(DumpExcept) * numEx);
        for (uint32_t i = 0; i < numEx; i++) {
            unsigned char tb, nb[4], sb[4], eb[4], cb[4], bb[4], hb[4];
            if (!ReadAll(ch, &tb, 1)) {
                FreeDumpBC(out);
                Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: except type", -1));
                return TCL_ERROR;
            }
            if (!ReadAll(ch, nb, 4) || !ReadAll(ch, sb, 4) || !ReadAll(ch, eb, 4) || !ReadAll(ch, cb, 4) || !ReadAll(ch, bb, 4) || !ReadAll(ch, hb, 4)) {
                FreeDumpBC(out);
                Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: except body", -1));
                return TCL_ERROR;
            }
            out->xr[i].type     = tb;
            out->xr[i].nesting  = le32(nb);
            out->xr[i].codeFrom = le32(sb);
            out->xr[i].codeTo   = le32(eb);
            out->xr[i].cont     = le32(cb);
            out->xr[i].brk      = le32(bb);
            out->xr[i].cat      = le32(hb);
        }
    }

    if (packedHasCounts) {
        unsigned char bA[4], bB[4], bC[4];
        if (!ReadAll(ch, bA, 4) || !ReadAll(ch, bB, 4) || !ReadAll(ch, bC, 4)) {
            FreeDumpBC(out);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: proc tail", -1));
            return TCL_ERROR;
        }
        out->maxStack  = le32(bA);
        out->numLocals = le32(bC);
        if (outNumLocals)
            *outNumLocals = out->numLocals;
    } else {
        out->maxStack  = topHdr->maxStackDepth;
        out->numLocals = topHdr->numLocals;
        if (outNumLocals)
            *outNumLocals = topHdr->numLocals;
    }

    return TCL_OK;
}

static int ReadOneLiteral(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Obj **outObj) {
    unsigned char tag;
    if (!ReadAll(ch, &tag, 1)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: literal tag", -1));
        return TCL_ERROR;
    }
    switch (tag) {
    case TBCX_LIT_STRING: {
        unsigned char lb[4];
        if (!ReadAll(ch, lb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: string len", -1));
            return TCL_ERROR;
        }
        uint32_t L = le32(lb);
        if (L == 0) {
            *outObj = Tcl_NewObj();
            return TCL_OK;
        }
        char *buf = (char *)Tcl_Alloc(L);
        if (L && !ReadAll(ch, (unsigned char *)buf, L)) {
            Tcl_Free(buf);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: string bytes", -1));
            return TCL_ERROR;
        }
        *outObj = Tcl_NewStringObj(buf, (Tcl_Size)L);
        Tcl_Free(buf);
        return TCL_OK;
    }
    case TBCX_LIT_WIDEINT: {
        unsigned char ib[8];
        if (!ReadAll(ch, ib, 8)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: wideint", -1));
            return TCL_ERROR;
        }
        *outObj = Tcl_NewWideIntObj((Tcl_WideInt)(int64_t)le64(ib));
        return TCL_OK;
    }
    case TBCX_LIT_DOUBLE: {
        unsigned char db[8];
        if (!ReadAll(ch, db, 8)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: double", -1));
            return TCL_ERROR;
        }
        uint64_t bits = le64(db);
        double   d;
        memcpy(&d, &bits, 8);
        *outObj = Tcl_NewDoubleObj(d);
        return TCL_OK;
    }
    case TBCX_LIT_BIGNUM: {
        unsigned char sign, lb[4];
        if (!ReadAll(ch, &sign, 1) || !ReadAll(ch, lb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: bignum header", -1));
            return TCL_ERROR;
        }
        uint32_t       L   = le32(lb);
        unsigned char *mag = (unsigned char *)Tcl_Alloc(L ? L : 1);
        if (L && !ReadAll(ch, mag, L)) {
            Tcl_Free((char *)mag);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: bignum mag", -1));
            return TCL_ERROR;
        }
        mp_int big;
        if (mp_init(&big) != MP_OKAY) {
            Tcl_Free((char *)mag);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("tommath: mp_init failed", -1));
            return TCL_ERROR;
        }
        if (L) {
            mp_err rc = mp_unpack(&big, (size_t)L, MP_MSB_FIRST, 1, MP_BIG_ENDIAN, 0, mag);
            if (rc != MP_OKAY) {
                mp_clear(&big);
                Tcl_Free((char *)mag);
                Tcl_SetObjResult(interp, Tcl_NewStringObj("tommath: mp_unpack failed", -1));
                return TCL_ERROR;
            }
        }
        if (sign) {
            if (mp_neg(&big, &big) != MP_OKAY) {
                mp_clear(&big);
                Tcl_Free((char *)mag);
                Tcl_SetObjResult(interp, Tcl_NewStringObj("tommath: mp_neg failed", -1));
                return TCL_ERROR;
            }
        }
        Tcl_Free((char *)mag);
        *outObj = Tcl_NewBignumObj(&big);
        return TCL_OK;
    }
    case TBCX_LIT_WIDEUINT: {
        unsigned char ub[8];
        if (!ReadAll(ch, ub, 8)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: wideuint", -1));
            return TCL_ERROR;
        }
        uint64_t u = le64(ub);
        if (u <= (uint64_t)INT64_MAX)
            *outObj = Tcl_NewWideIntObj((Tcl_WideInt)(int64_t)u);
        else {
            mp_int big;
            if (mp_init(&big) != MP_OKAY) {
                Tcl_SetObjResult(interp, Tcl_NewStringObj("tommath: mp_init failed", -1));
                return TCL_ERROR;
            }
            mp_set_u64(&big, u);
            *outObj = Tcl_NewBignumObj(&big);
        }
        return TCL_OK;
    }
    case TBCX_LIT_BOOLEAN: {
        unsigned char bb;
        if (!ReadAll(ch, &bb, 1)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: boolean", -1));
            return TCL_ERROR;
        }
        *outObj = Tcl_NewBooleanObj(bb ? 1 : 0);
        return TCL_OK;
    }
    case TBCX_LIT_BYTEARR: {
        unsigned char lb[4];
        if (!ReadAll(ch, lb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: bytearray len", -1));
            return TCL_ERROR;
        }
        uint32_t       L   = le32(lb);
        unsigned char *buf = (unsigned char *)Tcl_Alloc(L ? L : 1);
        if (L && !ReadAll(ch, buf, L)) {
            Tcl_Free(buf);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: bytearray bytes", -1));
            return TCL_ERROR;
        }
        *outObj = Tcl_NewByteArrayObj(buf, (Tcl_Size)L);
        Tcl_Free(buf);
        return TCL_OK;
    }
    case TBCX_LIT_LIST: {
        unsigned char nb[4];
        if (!ReadAll(ch, nb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: list count", -1));
            return TCL_ERROR;
        }
        uint32_t  n     = le32(nb);
        Tcl_Obj **elems = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * n);
        for (uint32_t i = 0; i < n; i++) {
            Tcl_Obj *e = NULL;
            if (ReadOneLiteral(interp, ch, &e) != TCL_OK) {
                while (i--)
                    Tcl_DecrRefCount(elems[i]);
                Tcl_Free((char *)elems);
                return TCL_ERROR;
            }
            Tcl_IncrRefCount(e);
            elems[i] = e;
        }
        *outObj = Tcl_NewListObj((Tcl_Size)n, elems);
        for (uint32_t i = 0; i < n; i++)
            Tcl_DecrRefCount(elems[i]);
        Tcl_Free((char *)elems);
        return TCL_OK;
    }
    case TBCX_LIT_DICT: {
        unsigned char nb[4];
        if (!ReadAll(ch, nb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: dict pairs", -1));
            return TCL_ERROR;
        }
        uint32_t pairs = le32(nb);
        Tcl_Obj *d     = Tcl_NewDictObj();
        for (uint32_t i = 0; i < pairs; i++) {
            Tcl_Obj *k = NULL, *v = NULL;
            if (ReadOneLiteral(interp, ch, &k) != TCL_OK) {
                Tcl_DecrRefCount(d);
                return TCL_ERROR;
            }
            if (ReadOneLiteral(interp, ch, &v) != TCL_OK) {
                Tcl_DecrRefCount(k);
                Tcl_DecrRefCount(d);
                return TCL_ERROR;
            }
            Tcl_DictObjPut(NULL, d, k, v);
        }
        *outObj = d;
        return TCL_OK;
    }
    default:
        Tcl_SetObjResult(interp, Tcl_NewStringObj("unknown literal tag", -1));
        return TCL_ERROR;
    }
}

static void MarkLabel(Tcl_HashTable *labels, size_t target) {
    int isNew;
    Tcl_CreateHashEntry(labels, (const char *)(uintptr_t)target, &isNew);
}

static int HasLabel(Tcl_HashTable *labels, size_t target) {
    Tcl_HashEntry *h = Tcl_FindHashEntry(labels, (const char *)(uintptr_t)target);
    return h != NULL;
}

static void AppendOperand(Tcl_Obj *out, InstOperandType tp, const unsigned char *pc, size_t *cursor, Tcl_Obj **lits, uint32_t numLits) {
    switch (tp) {
    case OPERAND_INT1: {
        int v = TclGetInt1AtPtr(pc + *cursor);
        *cursor += 1;
        AppendPrintf(out, "%d", v);
        break;
    }
    case OPERAND_INT4: {
        int v = TclGetInt4AtPtr(pc + *cursor);
        *cursor += 4;
        AppendPrintf(out, "%d", v);
        break;
    }
    case OPERAND_UINT1: {
        unsigned v = TclGetUInt1AtPtr(pc + *cursor);
        *cursor += 1;
        AppendPrintf(out, "%u", v);
        break;
    }
    case OPERAND_UINT4: {
        unsigned v = TclGetUInt4AtPtr(pc + *cursor);
        *cursor += 4;
        AppendPrintf(out, "%u", v);
        break;
    }
    case OPERAND_IDX4: {
        int v = TclGetInt4AtPtr(pc + *cursor);
        *cursor += 4;
        AppendPrintf(out, "#%d", v);
        break;
    }
    case OPERAND_LVT1: {
        unsigned v = TclGetUInt1AtPtr(pc + *cursor);
        *cursor += 1;
        AppendPrintf(out, "lvt:%u", v);
        break;
    }
    case OPERAND_LVT4: {
        unsigned v = TclGetUInt4AtPtr(pc + *cursor);
        *cursor += 4;
        AppendPrintf(out, "lvt:%u", v);
        break;
    }
    case OPERAND_AUX4: {
        unsigned v = TclGetUInt4AtPtr(pc + *cursor);
        *cursor += 4;
        AppendPrintf(out, "aux:%u", v);
        break;
    }
    case OPERAND_OFFSET1: {
        int off = TclGetInt1AtPtr(pc + *cursor);
        *cursor += 1;
        AppendPrintf(out, "ofs:%+d", off);
        break;
    }
    case OPERAND_OFFSET4: {
        int off = TclGetInt4AtPtr(pc + *cursor);
        *cursor += 4;
        AppendPrintf(out, "ofs:%+d", off);
        break;
    }
    case OPERAND_LIT1: {
        unsigned idx = TclGetUInt1AtPtr(pc + *cursor);
        *cursor += 1;
        if (idx < numLits && lits[idx]) {
            Tcl_Size    ln = 0;
            const char *ls = Tcl_GetStringFromObj(lits[idx], &ln);
            AppendPrintf(out, "lit:%u \"", idx);
            AppendEscaped(out, ls, ln);
            Tcl_AppendToObj(out, "\"", 1);
        } else
            AppendPrintf(out, "lit:%u", idx);
        break;
    }
    case OPERAND_LIT4: {
        unsigned idx = TclGetUInt4AtPtr(pc + *cursor);
        *cursor += 4;
        if (idx < numLits && lits[idx]) {
            Tcl_Size    ln = 0;
            const char *ls = Tcl_GetStringFromObj(lits[idx], &ln);
            AppendPrintf(out, "lit:%u \"", idx);
            AppendEscaped(out, ls, ln);
            Tcl_AppendToObj(out, "\"", 1);
        } else
            AppendPrintf(out, "lit:%u", idx);
        break;
    }
    case OPERAND_SCLS1: {
        unsigned v = TclGetUInt1AtPtr(pc + *cursor);
        *cursor += 1;
        static const char *names[] = {"alnum", "alpha", "ascii", "control", "digit", "graph", "lower", "print", "punct", "space", "upper", "word", "xdigit"};
        const char        *nm      = (v < (sizeof(names) / sizeof(names[0]))) ? names[v] : "?";
        AppendPrintf(out, "class:%s(%u)", nm, v);
        break;
    }
    case OPERAND_UNSF1: {
        unsigned v = TclGetUInt1AtPtr(pc + *cursor);
        *cursor += 1;
        AppendPrintf(out, "unsetFlags:%u", v);
        break;
    }
    case OPERAND_CLK1: {
        unsigned v = TclGetUInt1AtPtr(pc + *cursor);
        *cursor += 1;
        AppendPrintf(out, "clock:%u", v);
        break;
    }
    case OPERAND_LRPL1: {
        unsigned v = TclGetUInt1AtPtr(pc + *cursor);
        *cursor += 1;
        AppendPrintf(out, "lreplaceFlags:%u", v);
        break;
    }
    default:
        break;
    }
}

static void ScanLabels(Tcl_HashTable *labels, const unsigned char *code, size_t codeLen, Tcl_Obj **lits, uint32_t numLits) {
    Tcl_InitHashTable(labels, TCL_ONE_WORD_KEYS);
    size_t pc = 0;
    while (pc < codeLen) {
        unsigned               op     = code[pc];
        const InstructionDesc *d      = &tbcxInstructionTable[op];
        size_t                 len    = (size_t)d->numBytes;
        size_t                 cursor = pc + 1;
        for (int i = 0; i < d->numOperands; i++) {
            InstOperandType tp = d->opTypes[i];
            if (tp == OPERAND_OFFSET1) {
                int off = TclGetInt1AtPtr(code + cursor);
                cursor += 1;
                size_t tgt = pc + len + off;
                MarkLabel(labels, tgt);
            } else if (tp == OPERAND_OFFSET4) {
                int off = TclGetInt4AtPtr(code + cursor);
                cursor += 4;
                size_t tgt = pc + len + off;
                MarkLabel(labels, tgt);
            } else {
                if (tp == OPERAND_INT1 || tp == OPERAND_UINT1 || tp == OPERAND_LVT1 || tp == OPERAND_LIT1 || tp == OPERAND_SCLS1 || tp == OPERAND_UNSF1 || tp == OPERAND_CLK1 || tp == OPERAND_LRPL1)
                    cursor += 1;
                else if (tp == OPERAND_INT4 || tp == OPERAND_UINT4 || tp == OPERAND_LVT4 || tp == OPERAND_IDX4 || tp == OPERAND_LIT4 || tp == OPERAND_AUX4 || tp == OPERAND_OFFSET4)
                    cursor += 4;
            }
        }
        pc += len ? len : 1;
    }
}

static inline int SafeHave(const unsigned char *base, size_t codeLen, size_t needFromPc) {
    return needFromPc <= codeLen;
}

#define ENSURE_ROOM_OR_BREAK(_pc, _need)                                                                                                                                                               \
    do {                                                                                                                                                                                               \
        if (!SafeHave(bc->code, bc->codeLen, (_pc) + (_need))) {                                                                                                                                       \
            AppendPrintf(out, "    %04zu: [decoder error: need %zu bytes, have %zu]\n", (size_t)(_pc), (size_t)(_need), (size_t)(bc->codeLen - (_pc)));                                                \
            pc = bc->codeLen;                                                                                                                                                                          \
            goto done_dis;                                                                                                                                                                             \
        }                                                                                                                                                                                              \
    } while (0)

static inline void SkipOperand(InstOperandType tp, size_t *cursor) {
    *cursor +=
        (tp == OPERAND_INT1 || tp == OPERAND_UINT1 || tp == OPERAND_LVT1 || tp == OPERAND_LIT1 || tp == OPERAND_SCLS1 || tp == OPERAND_UNSF1 || tp == OPERAND_CLK1 || tp == OPERAND_LRPL1) ? 1 : 4;
}

static void DisassembleBC(Tcl_Obj *out, const char *title, const DumpBC *bc) {
    AppendPrintf(out, "%s:\n", title);
    AppendPrintf(out, "  codeLen=%zu  maxStack=%u  numLocals=%u  literals=%u  aux=%u  except=%u\n", bc->codeLen, bc->maxStack, bc->numLocals, bc->numLits, bc->numAux, bc->numEx);

    if (bc->numLits) {
        AppendPrintf(out, "  Literals:\n");
        for (uint32_t i = 0; i < bc->numLits; i++) {
            Tcl_Size    ln = 0;
            const char *ls = Tcl_GetStringFromObj(bc->lits[i], &ln);
            AppendPrintf(out, "    [%4u] type=%s value=\"", i, ObjTypeName(bc->lits[i]));
            AppendEscaped(out, ls, ln);
            Tcl_AppendToObj(out, "\"\n", 2);
        }
    }

    if (bc->numAux) {
        AppendPrintf(out, "  AuxData:\n");
        for (uint32_t i = 0; i < bc->numAux; i++) {
            AppendPrintf(out, "    #%u kind=%u => %s\n", i, bc->aux[i].kind, Tcl_GetString(bc->aux[i].summary));
        }
    }

    if (bc->numEx) {
        AppendPrintf(out, "  ExceptionRanges:\n");
        for (uint32_t i = 0; i < bc->numEx; i++) {
            const DumpExcept *x = &bc->xr[i];
            AppendPrintf(out, "    [%u] type=%s nest=%u code=[%u..%u) continue=%u break=%u catch=%u\n", i, (x->type ? "CATCH" : "LOOP"), x->nesting, x->codeFrom, x->codeTo, x->cont, x->brk, x->cat);
        }
    }

    AppendPrintf(out, "  Disassembly:\n");
    Tcl_HashTable labels;
    ScanLabels(&labels, bc->code, bc->codeLen, bc->lits, bc->numLits);

    size_t pc = 0;
    while (pc < bc->codeLen) {
        if (HasLabel(&labels, pc))
            AppendPrintf(out, "  L%04zu:\n", pc);
        ENSURE_ROOM_OR_BREAK(pc, 1);
        unsigned               op  = bc->code[pc];
        const InstructionDesc *d   = &tbcxInstructionTable[op];
        size_t                 len = (size_t)d->numBytes;
        if (len == 0)
            len = 1;
        ENSURE_ROOM_OR_BREAK(pc, len);
        AppendPrintf(out, "    %04zu: %-24s", pc, d->name ? d->name : "???");
        size_t cursor = pc + 1;
        if (d->numOperands > 0) {
            Tcl_AppendToObj(out, "  ", 2);
            for (int i = 0; i < d->numOperands; i++) {
                size_t need = (d->opTypes[i] == OPERAND_INT1 || d->opTypes[i] == OPERAND_UINT1 || d->opTypes[i] == OPERAND_LVT1 || d->opTypes[i] == OPERAND_LIT1 || d->opTypes[i] == OPERAND_SCLS1 ||
                               d->opTypes[i] == OPERAND_UNSF1 || d->opTypes[i] == OPERAND_CLK1 || d->opTypes[i] == OPERAND_LRPL1)
                                  ? 1
                                  : 4;
                if ((cursor - pc) + need > len) {
                    AppendPrintf(out, " [bad operand #%d]", i + 1);
                    break;
                }
                if (i)
                    Tcl_AppendToObj(out, ", ", 2);
                AppendOperand(out, d->opTypes[i], bc->code, &cursor, bc->lits, bc->numLits);
            }
        }
        for (int i = 0; i < d->numOperands; i++) {
            InstOperandType tp = d->opTypes[i];
            if (tp == OPERAND_OFFSET1 || tp == OPERAND_OFFSET4) {
                size_t c2 = pc + 1;
                for (int j = 0; j < i; j++) {
                    InstOperandType t2 = d->opTypes[j];
                    SkipOperand(t2, &c2);
                }
                if ((tp == OPERAND_OFFSET1 && (c2 + 1) > pc + len) || (tp == OPERAND_OFFSET4 && (c2 + 4) > pc + len)) {
                    AppendPrintf(out, "  ; [bad jump operand]");
                    continue;
                }
                int    off = (tp == OPERAND_OFFSET1) ? TclGetInt1AtPtr(bc->code + c2) : TclGetInt4AtPtr(bc->code + c2);
                size_t tgt = pc + len + off;
                AppendPrintf(out, "  ; -> L%04zu", tgt);
            }
        }
        if (d->stackEffect == INT_MIN && d->numOperands > 0) {
            int             op1 = 0;
            InstOperandType t0  = d->opTypes[0];
            if (t0 == OPERAND_INT1 || t0 == OPERAND_UINT1) {
                ENSURE_ROOM_OR_BREAK(pc, 2);
                op1 = (t0 == OPERAND_INT1) ? TclGetInt1AtPtr(bc->code + pc + 1) : (int)TclGetUInt1AtPtr(bc->code + pc + 1);
            } else if (t0 == OPERAND_INT4 || t0 == OPERAND_UINT4) {
                ENSURE_ROOM_OR_BREAK(pc, 5);
                op1 = (t0 == OPERAND_INT4) ? TclGetInt4AtPtr(bc->code + pc + 1) : (int)TclGetUInt4AtPtr(bc->code + pc + 1);
            } else {
                goto after_stack_note;
            }
            AppendPrintf(out, "  ; stack=(1-op1)=%d", 1 - op1);
        } else if (d->stackEffect != 0) {
            AppendPrintf(out, "  ; stack=%+d", d->stackEffect);
        }
    after_stack_note:
        Tcl_AppendToObj(out, "\n", 1);
        pc += len ? len : 1;
    }
done_dis:
    Tcl_DeleteHashTable(&labels);
}

static const char *MethKindName(unsigned char k) {
    switch (k) {
    case TBCX_METH_INST:
        return "method";
    case TBCX_METH_CTOR:
        return "constructor";
    case TBCX_METH_DTOR:
        return "destructor";
    default:
        return "method?";
    }
}

static int DumpFromChannel(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Obj **outObj) {
    Tcl_SetChannelOption(interp, ch, "-translation", "binary");
    Tcl_SetChannelOption(interp, ch, "-eofchar", "");

    TbcxHeader H;
    if (ReadHeader(interp, ch, &H) != TCL_OK)
        return TCL_ERROR;

    Tcl_Obj *out = Tcl_NewObj();
    Tcl_IncrRefCount(out);
    AppendPrintf(out,
                 "TBCX Header: format=%u flags=0x%x codeLen=%llu numCmds=%u numExcept=%u numLiterals=%u numAux=%u numLocals=%u "
                 "maxStack=%u\n",
                 H.format, H.flags, (unsigned long long)H.codeLen, H.numCmds, H.numExcept, H.numLiterals, H.numAux, H.numLocals, H.maxStackDepth);

    DumpBC top = {0};
    if (ReadDumpBC_Block(interp, ch, 0, &H, &top, NULL) != TCL_OK) {
        Tcl_DecrRefCount(out);
        return TCL_ERROR;
    }

    unsigned char nb[4];
    uint32_t      numProcs = 0;
    Tcl_Size      got      = Tcl_ReadRaw(ch, (char *)nb, (Tcl_Size)4);
    if (got < 0) {
        FreeDumpBC(&top);
        Tcl_DecrRefCount(out);
        Tcl_SetObjResult(interp, Tcl_NewStringObj("I/O error reading proc count", -1));
        return TCL_ERROR;
    }
    if (got > 0) {
        if (got < (Tcl_Size)4) {
            if (!ReadAll(ch, nb + got, (Tcl_Size)(4 - got))) {
                FreeDumpBC(&top);
                Tcl_DecrRefCount(out);
                Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: proc count", -1));
                return TCL_ERROR;
            }
        }
        numProcs = le32(nb);
    }

    DisassembleBC(out, "TopLevel", &top);
    FreeDumpBC(&top);

    for (uint32_t i = 0; i < numProcs; i++) {
        if (!ReadAll(ch, nb, 4)) {
            Tcl_DecrRefCount(out);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: proc name len", -1));
            return TCL_ERROR;
        }
        uint32_t nL   = le32(nb);
        char    *name = (char *)Tcl_Alloc(nL + 1);
        if (nL && !ReadAll(ch, (unsigned char *)name, nL)) {
            Tcl_Free(name);
            Tcl_DecrRefCount(out);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: proc name", -1));
            return TCL_ERROR;
        }
        name[nL] = 0;
        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(name);
            Tcl_DecrRefCount(out);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: proc ns len", -1));
            return TCL_ERROR;
        }
        uint32_t nsL = le32(nb);
        char    *ns  = (char *)Tcl_Alloc(nsL + 1);
        if (nsL && !ReadAll(ch, (unsigned char *)ns, nsL)) {
            Tcl_Free(name);
            Tcl_Free(ns);
            Tcl_DecrRefCount(out);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: proc ns", -1));
            return TCL_ERROR;
        }
        ns[nsL] = 0;
        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(name);
            Tcl_Free(ns);
            Tcl_DecrRefCount(out);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: proc args len", -1));
            return TCL_ERROR;
        }
        uint32_t aL   = le32(nb);
        char    *args = (char *)Tcl_Alloc(aL + 1);
        if (aL && !ReadAll(ch, (unsigned char *)args, aL)) {
            Tcl_Free(name);
            Tcl_Free(ns);
            Tcl_Free(args);
            Tcl_DecrRefCount(out);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: proc args", -1));
            return TCL_ERROR;
        }
        args[aL]           = 0;

        DumpBC   pbc       = {0};
        uint32_t numLocals = 0;
        if (ReadDumpBC_Block(interp, ch, 1, &H, &pbc, &numLocals) != TCL_OK) {
            Tcl_Free(name);
            Tcl_Free(ns);
            Tcl_Free(args);
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        AppendPrintf(out, "\nProc: %s\n  ns=%s\n  args=%s\n", name, ns, args);
        DisassembleBC(out, "  Body", &pbc);
        FreeDumpBC(&pbc);

        Tcl_Free(name);
        Tcl_Free(ns);
        Tcl_Free(args);
    }
    if (!ReadAll(ch, nb, 4)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: class count", -1));
        Tcl_DecrRefCount(out);
        return TCL_ERROR;
    }
    uint32_t nClasses = le32(nb);
    AppendPrintf(out, "\nClasses: %u\n", nClasses);
    for (uint32_t i = 0; i < nClasses; i++) {
        if (!ReadAll(ch, nb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: class name len", -1));
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        uint32_t cnL = le32(nb);
        char    *cls = (char *)Tcl_Alloc(cnL + 1);
        if (cnL && !ReadAll(ch, (unsigned char *)cls, cnL)) {
            Tcl_Free(cls);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: class name", -1));
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        cls[cnL] = 0;
        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(cls);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: superclass count", -1));
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        uint32_t nSup = le32(nb);
        AppendPrintf(out, "Class: %s\n", cls);
        if (nSup) {
            AppendPrintf(out, "  supers: ");
            for (uint32_t s = 0; s < nSup; s++) {
                if (!ReadAll(ch, nb, 4)) {
                    Tcl_Free(cls);
                    Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: superclass name len", -1));
                    Tcl_DecrRefCount(out);
                    return TCL_ERROR;
                }
                uint32_t sl  = le32(nb);
                char    *sup = (char *)Tcl_Alloc(sl + 1);
                if (sl && !ReadAll(ch, (unsigned char *)sup, sl)) {
                    Tcl_Free(sup);
                    Tcl_Free(cls);
                    Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: superclass name", -1));
                    Tcl_DecrRefCount(out);
                    return TCL_ERROR;
                }
                sup[sl] = 0;
                AppendPrintf(out, "%s%s", sup, (s + 1 < nSup ? ", " : ""));
                Tcl_Free(sup);
            }
            Tcl_AppendToObj(out, "\n", 1);
        }
        Tcl_Free(cls);
    }

    if (!ReadAll(ch, nb, 4)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: method count", -1));
        Tcl_DecrRefCount(out);
        return TCL_ERROR;
    }
    uint32_t nMeth = le32(nb);
    AppendPrintf(out, "\nMethods: %u\n", nMeth);
    for (uint32_t i = 0; i < nMeth; i++) {
        if (!ReadAll(ch, nb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: method class len", -1));
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        uint32_t cL  = le32(nb);
        char    *cls = (char *)Tcl_Alloc(cL + 1);
        if (cL && !ReadAll(ch, (unsigned char *)cls, cL)) {
            Tcl_Free(cls);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: method class", -1));
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        cls[cL] = 0;
        unsigned char kind;
        if (!ReadAll(ch, &kind, 1)) {
            Tcl_Free(cls);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: method kind", -1));
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(cls);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: method name len", -1));
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        uint32_t nL   = le32(nb);
        char    *name = (char *)Tcl_Alloc(nL + 1);
        if (nL && !ReadAll(ch, (unsigned char *)name, nL)) {
            Tcl_Free(name);
            Tcl_Free(cls);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: method name", -1));
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        name[nL] = 0;
        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(name);
            Tcl_Free(cls);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: method args len", -1));
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        uint32_t aL   = le32(nb);
        char    *args = (char *)Tcl_Alloc(aL + 1);
        if (aL && !ReadAll(ch, (unsigned char *)args, aL)) {
            Tcl_Free(args);
            Tcl_Free(name);
            Tcl_Free(cls);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: method args", -1));
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        args[aL] = 0;
        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(args);
            Tcl_Free(name);
            Tcl_Free(cls);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: method body len", -1));
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        uint32_t bL = le32(nb);
        if (bL) {
            char *skip = (char *)Tcl_Alloc(bL);
            if (!ReadAll(ch, (unsigned char *)skip, bL)) {
                Tcl_Free(skip);
                Tcl_Free(args);
                Tcl_Free(name);
                Tcl_Free(cls);
                Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: method body", -1));
                Tcl_DecrRefCount(out);
                return TCL_ERROR;
            }
            Tcl_Free(skip);
        }

        AppendPrintf(out, "\nMethod: [%s] %s::%s\n  args=%s  bodyTextLen=%u\n", MethKindName(kind), cls, (nL ? name : (kind == TBCX_METH_CTOR ? "<ctor>" : "<dtor>")), args, bL);

        DumpBC   mbc       = {0};
        uint32_t numLocals = 0;
        if (ReadDumpBC_Block(interp, ch, 1, &H, &mbc, &numLocals) != TCL_OK) {
            Tcl_Free(args);
            Tcl_Free(name);
            Tcl_Free(cls);
            Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        DisassembleBC(out, "  Compiled", &mbc);
        FreeDumpBC(&mbc);

        Tcl_Free(args);
        Tcl_Free(name);
        Tcl_Free(cls);
    }
    *outObj = out;
    return TCL_OK;
}

int Tbcx_DumpFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "path");
        return TCL_ERROR;
    }
    Tcl_Channel ch = Tcl_OpenFileChannel(interp, Tcl_GetString(objv[1]), "r", 0);
    if (!ch)
        return TCL_ERROR;
    Tcl_Obj *out = NULL;
    int      rc  = DumpFromChannel(interp, ch, &out);
    Tcl_Close(NULL, ch);
    if (rc != TCL_OK)
        return rc;
    Tcl_SetObjResult(interp, out);
    Tcl_DecrRefCount(out);
    return TCL_OK;
}
