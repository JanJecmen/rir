#include <assert.h>
#include <alloca.h>

#include "interp.h"
#include "interp_context.h"
#include "runtime.h"



#define NOT_IMPLEMENTED assert(false)

// TODO we are using the RInternals, but soud not when the code moves to GNU-R
// #include "RIntlns.h"

#undef eval

extern SEXP R_TrueValue;
extern SEXP R_FalseValue;
extern SEXP Rf_NewEnvironment(SEXP, SEXP, SEXP);
extern Rboolean R_Visible;

typedef SEXP (*CCODE)(SEXP, SEXP, SEXP, SEXP);

/* Information for Deparsing Expressions */
typedef enum {
    PP_INVALID = 0,
    PP_ASSIGN = 1,
    PP_ASSIGN2 = 2,
    PP_BINARY = 3,
    PP_BINARY2 = 4,
    PP_BREAK = 5,
    PP_CURLY = 6,
    PP_FOR = 7,
    PP_FUNCALL = 8,
    PP_FUNCTION = 9,
    PP_IF = 10,
    PP_NEXT = 11,
    PP_PAREN = 12,
    PP_RETURN = 13,
    PP_SUBASS = 14,
    PP_SUBSET = 15,
    PP_WHILE = 16,
    PP_UNARY = 17,
    PP_DOLLAR = 18,
    PP_FOREIGN = 19,
    PP_REPEAT = 20
} PPkind;

typedef enum {
    PREC_FN = 0,
    PREC_LEFT = 1,
    PREC_EQ = 2,
    PREC_RIGHT = 3,
    PREC_TILDE = 4,
    PREC_OR = 5,
    PREC_AND = 6,
    PREC_NOT = 7,
    PREC_COMPARE = 8,
    PREC_SUM = 9,
    PREC_PROD = 10,
    PREC_PERCENT = 11,
    PREC_COLON = 12,
    PREC_SIGN = 13,
    PREC_POWER = 14,
    PREC_DOLLAR = 15,
    PREC_NS = 16,
    PREC_SUBSET = 17
} PPprec;

typedef struct {
    PPkind kind;             /* deparse kind */
    PPprec precedence;       /* operator precedence */
    unsigned int rightassoc; /* right associative? */
} PPinfo;

/* The type definitions for the table of built-in functions. */
/* This table can be found in ../main/names.c */
typedef struct {
    char* name;  /* print name */
    CCODE cfun;  /* c-code address */
    int code;    /* offset within c-code */
    int eval;    /* evaluate args? */
    int arity;   /* function arity */
    PPinfo gram; /* pretty-print info */
} FUNTAB;

extern FUNTAB R_FunTab[];

#include <setjmp.h>
#include <signal.h>
#define JMP_BUF jmp_buf
#define SETJMP(x) setjmp(x)
#define LONGJMP(x, i) longjmp(x, i)

/* Evaluation Context Structure */
typedef struct RCNTXT {
    struct RCNTXT* nextcontext; /* The next context up the chain */
    int callflag;               /* The context "type" */
    JMP_BUF cjmpbuf;            /* C stack and register information */
    int cstacktop;              /* Top of the pointer protection stack */
    int evaldepth;              /* evaluation depth at inception */
    SEXP promargs;              /* Promises supplied to closure */
    SEXP callfun;               /* The closure called */
    SEXP sysparent;             /* environment the closure was called from */
    SEXP call;                  /* The call that effected this context*/
    SEXP cloenv;                /* The environment */
    SEXP conexit;               /* Interpreted "on.exit" code */
    void (*cend)(void*);        /* C "on.exit" thunk */
    void* cenddata;             /* data for C "on.exit" thunk */
    void* vmax;                 /* top of R_alloc stack */
    int intsusp;                /* interrupts are suspended */
    SEXP handlerstack;          /* condition handler stack */
    SEXP restartstack;          /* stack of available restarts */
    struct RPRSTACK* prstack;   /* stack of pending promises */
    void* nodestack;
    // Since we don't know if the R we are linked against has an INT stack, we
    // have to be conservative from here on....
    void* dontuse1;
    SEXP dontuse2; /* The source line in effect */
    int dontuse3;  /* should browser finish this context without stopping */
    SEXP dontuse4; /* only set during on.exit calls */
} RCNTXT, *context;

/* The Various Context Types.

 * In general the type is a bitwise OR of the values below.
 * Note that CTXT_LOOP is already the or of CTXT_NEXT and CTXT_BREAK.
 * Only functions should have the third bit turned on;
 * this allows us to move up the context stack easily
 * with either RETURN's or GENERIC's or RESTART's.
 * If you add a new context type for functions make sure
 *   CTXT_NEWTYPE & CTXT_FUNCTION > 0
 */
enum {
    CTXT_TOPLEVEL = 0,
    CTXT_NEXT = 1,
    CTXT_BREAK = 2,
    CTXT_LOOP = 3, /* break OR next target */
    CTXT_FUNCTION = 4,
    CTXT_CCODE = 8,
    CTXT_RETURN = 12,
    CTXT_BROWSER = 16,
    CTXT_GENERIC = 20,
    CTXT_RESTART = 32,
    CTXT_BUILTIN = 64 /* used in profiling */
};

typedef struct sxpinfo_struct_rjit {
    unsigned int type : 5; /* ==> (FUNSXP == 99) %% 2^5 == 3 == CLOSXP
                        * -> warning: `type' is narrower than values
                        *              of its type
                        * when SEXPTYPE was an enum */
    unsigned int obj : 1;
    unsigned int named : 2;
    unsigned int gp : 16;
    unsigned int mark : 1;
    unsigned int debug : 1;
    unsigned int trace : 1; /* functions and memory tracing */
    unsigned int spare : 1; /* currently unused */
    unsigned int gcgen : 1; /* old generation number */
    unsigned int gccls : 3; /* node class */
} sxpifo_struct_rjit;       /*		    Tot: 32 */

typedef struct cons_rjit {
    SEXP car;
    SEXP cdr;
    SEXP tag;
} cons_rjit;

typedef struct sexprec_rjit {
    struct sxpinfo_struct_rjit sxpinfo;
    SEXP attrib;
    SEXP gengc_next_node, gengc_prev_node;
    union {
        struct cons_rjit cons;
        int i;
    } u;
} sexprec_rjit;

// helpers

/** Moves the pc to next instruction, based on the current instruction length
 */
OpcodeT* advancePc(OpcodeT* pc) {
    switch (*pc++) {
#define DEF_INSTR(name, imm, ...)                                              \
    case name:                                                                 \
        pc += sizeof(ArgT) * imm;                                              \
        break;
#include "ir/insns.h"
    default:
        assert(false && "Unknown instruction");
    }
    return pc;
}

#define PC_BOUNDSCHECK(pc)                                                     \
    SLOWASSERT((pc) >= code(c) && (pc) < code(c) + c->codeSize);

// bytecode accesses

INLINE Opcode readOpcode(OpcodeT** pc) {
    Opcode result = *(OpcodeT*)(*pc);
    *pc += sizeof(OpcodeT);
    return result;
}

INLINE unsigned readImmediate(OpcodeT** pc) {
    unsigned result = *(Immediate*)*pc;
    *pc += sizeof(Immediate);
    return result;
}

INLINE int readSignedImmediate(OpcodeT** pc) {
    int result = *(SignedImmediate*)*pc;
    *pc += sizeof(SignedImmediate);
    return result;
}

INLINE SEXP readConst(Context* ctx, OpcodeT** pc) {
    return cp_pool_at(ctx, readImmediate(pc));
}

INLINE int readJumpOffset(OpcodeT** pc) {
    int result = *(JumpOffset*)(*pc);
    *pc += sizeof(JumpOffset);
    return result;
}

INLINE SEXP getSrcAt(Code* c, OpcodeT* pc, Context* ctx) {
    // we need to determine index of the current instruction
    OpcodeT* x = code(c);
    // find the pc of the current instructions, it is ok to be slow
    unsigned insIdx = 0;
    while ((x = advancePc(x)) != pc)
        ++insIdx;
    unsigned sidx = src(c)[insIdx];
    // return the ast for the instruction, or if not defined, the ast of the
    // function
    return src_pool_at(ctx, sidx == 0 ? c->src : sidx);
}

INLINE SEXP getSrcForCall(Code* c, OpcodeT* pc, Context* ctx) {
    // we need to determine index of the current instruction
    OpcodeT* x = code(c);
    // find the pc of the current instructions, it is ok to be slow
    unsigned insIdx = 0;
    while ((x = advancePc(x)) != pc)
        ++insIdx;
    unsigned sidx = src(c)[insIdx];
    // return the ast for the instruction, or if not defined, the ast of the
    // function
    assert(sidx);
    return src_pool_at(ctx, sidx);
}


/** Creates a promise from given code object and environment.

 */
INLINE SEXP createPromise(Code* code, SEXP env) {
#if RIR_AS_PACKAGE == 1
    return mkPROMISE(rir_createWrapperPromise(code), env);
#else
    SEXP p = mkPROMISE((SEXP)code, env);
    PROTECT(p);
    // TODO: This is a bit of a hack to make sure the promise keeps its function
    // reachable from the GC pov.
    SEXP a = CONS_NR(functionStore(function(code)), R_NilValue);
    SET_ATTRIB(p, a);
    UNPROTECT(1);
    return p;
#endif
}

INLINE SEXP promiseValue(SEXP promise, Context * ctx) {
    // if already evaluated, return the value
    if (PRVALUE(promise) && PRVALUE(promise) != R_UnboundValue) {
        promise = PRVALUE(promise);
        assert(TYPEOF(promise) != PROMSXP);
        SET_NAMED(promise, 2);
        return promise;
    } else {
        return forcePromise(promise);
    }
}

INLINE SEXP escape(SEXP val) {
    // FIXME : as long as our code objects can leak to various places
    // outside our control, we need to make sure to convert them back
    if (isValidCodeObject(val))
        val = rirExpr(val);

    assert(!TYPEOF(val) != 31);
    return val;
}

extern RCNTXT* R_GlobalContext;
extern void Rf_begincontext(void*, int, SEXP, SEXP, SEXP, SEXP, SEXP);
extern void Rf_endcontext(RCNTXT*);

// =============================================================================
// TODO: This should be put into the normal RCNTXT!
typedef struct {
    size_t stackPointer;
    OpcodeT* pc;
} RirContext;

#define createRirClosureContext(cntxt, continuation, call, env, parent, args,  \
                                op, pc, ctx)                                   \
    do {                                                                       \
        SEXP cntxt_store =                                                     \
            Rf_allocVector(RAWSXP, sizeof(RCNTXT) + sizeof(RirContext));       \
        ostack_push(ctx, cntxt_store);                                         \
                                                                               \
        cntxt = (RCNTXT*)RAW(cntxt_store);                                     \
        continuation = (RirContext*)(cntxt + 1);                               \
                                                                               \
        if (R_GlobalContext->callflag == CTXT_GENERIC)                         \
            Rf_begincontext(cntxt, CTXT_RETURN, call, env,                     \
                            R_GlobalContext->sysparent, args, op);             \
        else                                                                   \
            Rf_begincontext(cntxt, CTXT_RETURN, call, env, parent, args, op);  \
                                                                               \
        continuation->stackPointer = ostack_length(ctx);                       \
        continuation->pc = NULL;                                               \
    } while (false)

#define restoreCont(continuation)                                              \
    do {                                                                       \
        SLOWASSERT(ostack_length(ctx) >= (continuation)->stackPointer);        \
        rl_setLength(&ctx->ostack, (continuation)->stackPointer);              \
    } while (false)

INLINE void endRirContext(Context* ctx, SEXP value) {
    SEXP cntxt_store = ostack_top(ctx);
    assert(TYPEOF(cntxt_store) == RAWSXP);
    RCNTXT* cntxt = (RCNTXT*)RAW(cntxt_store);
    if (value) {
// TODO we need to set the returnvalue in the context, but we cannot
// access it since we do not know if we have INTSTACK or not (which
// changes RCNTXT layout. therefore we rely on the endClosure here,
// which will not work in the package version.... Not sure what is a
// good solution here.
#if RIR_AS_PACKAGE == 0
        endClosureContext(cntxt, value);
#else
        assert(false);
#endif
    } else {
        Rf_endcontext(cntxt);
    }
    ostack_pop(ctx); // Context
}

// TODO remove numArgs and bp -- this is only needed for the on stack argument
// handling
#define INSTRUCTION(name)                                                      \
    INLINE void ins_##name(Code* c, SEXP env, OpcodeT** pc, Context* ctx,      \
                           unsigned numArgs)

INSTRUCTION(push_) {
    SEXP x = readConst(ctx, pc);
    R_Visible = TRUE;
    ostack_push(ctx, x);
}

static void jit(SEXP cls, Context* ctx) {
    assert(TYPEOF(cls) == CLOSXP);
    if (TYPEOF(BODY(cls)) == INTSXP)
        return;
    SEXP body = BODY(cls);
    if (TYPEOF(body) == BCODESXP)
        body = VECTOR_ELT(CDR(body), 0);
    SET_BODY(cls, ctx->compiler(body));
}

INSTRUCTION(ldfun_) {
    SEXP sym = readConst(ctx, pc);
    SEXP val = findFun(sym, env);

    // TODO something should happen here
    if (val == R_UnboundValue)
        assert(false && "Unbound var");
    else if (val == R_MissingArg)
        assert(false && "Missing argument");

    switch (TYPEOF(val)) {
    case CLOSXP:
        /** If compile on demand is active, check that the function to be called
         * is compiled already, and compile if not.
         */
        if (COMPILE_ON_DEMAND) {
            jit(val, ctx);
        }
        break;
    case SPECIALSXP:
    case BUILTINSXP:
        // special and builtin functions are ok
        break;
    default:
	error("attempt to apply non-function");
    }
    ostack_push(ctx, val);
}

INSTRUCTION(ldddvar_) {
    SEXP sym = readConst(ctx, pc);
    SEXP val = Rf_ddfindVar(sym, env);
    R_Visible = TRUE;

    // TODO better errors
    if (val == R_UnboundValue) {
        Rf_error("object not found");
    } else if (val == R_MissingArg) {
        error("argument is missing, with no default");
    }

    // if promise, evaluate & return
    if (TYPEOF(val) == PROMSXP)
        val = promiseValue(val, ctx);

    // WTF? is this just defensive programming or what?
    if (NAMED(val) == 0 && val != R_NilValue)
        SET_NAMED(val, 1);

    ostack_push(ctx, val);
}


INSTRUCTION(ldvar_) {
    SEXP sym = readConst(ctx, pc);
    SEXP val = findVar(sym, env);
    R_Visible = TRUE;

    // TODO better errors
    if (val == R_UnboundValue) {
        Rf_error("object not found");
    } else if (val == R_MissingArg) {
        Rf_error("argument \"%s\" is missing, with no default", CHAR(PRINTNAME(sym)));
    }

    // if promise, evaluate & return
    if (TYPEOF(val) == PROMSXP)
        val = promiseValue(val, ctx);

    // WTF? is this just defensive programming or what?
    if (NAMED(val) == 0 && val != R_NilValue)
        SET_NAMED(val, 1);

    ostack_push(ctx, val);
}

/** Given argument code offsets, creates the argslist from their promises.
 */
// TODO unnamed only at this point
int __listAppend(SEXP* front, SEXP* last, SEXP value, SEXP name) {
    int p = 0;

    assert(TYPEOF(*front) == LISTSXP || TYPEOF(*front) == NILSXP);
    assert(TYPEOF(*last) == LISTSXP || TYPEOF(*last) == NILSXP);

    SEXP app = CONS_NR(value, R_NilValue);
    
    SET_TAG(app, name);

    if (*front == R_NilValue) {
        *front = app;
        PROTECT(*front);
        p++;
    }

    if (*last != R_NilValue)
        SETCDR(*last, app);
    *last = app;

    return p;
}

SEXP createArgsListStack(Code* c, size_t nargs, SEXP names, SEXP env, SEXP call,
                         Context* ctx, bool eager) {
    SEXP result = R_NilValue;
    SEXP pos = result;
    int p = 0;

    SEXP* argbase = ostack_at(ctx, nargs - 1);

    for (size_t i = 0; i < nargs; ++i) {
        SEXP name = names != R_NilValue ? VECTOR_ELT(names, i) : R_NilValue;

        SEXP arg = argbase[i];

        // if the argument is an ellipsis, then retrieve it from the environment and 
        // flatten the ellipsis
        if (arg == R_DotsSymbol) {
            SEXP ellipsis = findVar(R_DotsSymbol, env);
            if (TYPEOF(ellipsis) == DOTSXP) {
                while (ellipsis != R_NilValue) {
                    name = TAG(ellipsis);
                    if (eager) {
                        SEXP arg = rirEval(CAR(ellipsis), env);
                        assert(TYPEOF(arg) != PROMSXP);
                        p += __listAppend(&result, &pos, arg, name);
                    } else {
                        SEXP promise = mkPROMISE(CAR(ellipsis), env);
                        p += __listAppend(&result, &pos, promise, name);
                    }
                    ellipsis = CDR(ellipsis);
                }
            }
        } else if (arg == R_MissingArg) {
            if (eager)
                Rf_errorcall(call, "argument %d is empty", i + 1);
            p += __listAppend(&result, &pos, R_MissingArg, R_NilValue);
        } else {
            if (eager && TYPEOF(arg) == PROMSXP) {
                arg = rirEval(arg, env);
            }
            arg = escape(arg);

            p += __listAppend(&result, &pos, arg, name);
        }
    }

    UNPROTECT(p);
    return result;
}

SEXP createArgsList(Code* c, FunctionIndex* args, SEXP call, size_t nargs,
                    SEXP names, SEXP env, Context* ctx, bool eager) {
    SEXP result = R_NilValue;
    SEXP pos = result;
    int p = 0;

    // loop through the arguments and create a promise, unless it is a missing
    // argument
    for (size_t i = 0; i < nargs; ++i) {
        unsigned offset = args[i];
        SEXP name = names != R_NilValue ? VECTOR_ELT(names, i) : R_NilValue;

        // if the argument is an ellipsis, then retrieve it from the environment and 
        // flatten the ellipsis
        if (args[i] == DOTS_ARG_IDX) {
            SEXP ellipsis = findVar(R_DotsSymbol, env);
            if (TYPEOF(ellipsis) == DOTSXP) {
                while (ellipsis != R_NilValue) {
                    name = TAG(ellipsis);
                    if (eager) {
                        SEXP arg = rirEval(CAR(ellipsis), env);
                        assert(TYPEOF(arg) != PROMSXP);
                        p += __listAppend(&result, &pos, arg, name);
                    } else {
                        SEXP promise = mkPROMISE(CAR(ellipsis), env);
                        p += __listAppend(&result, &pos, promise, name);
                    }
                    ellipsis = CDR(ellipsis);
                }
            }
        } else if (args[i] == MISSING_ARG_IDX) {
            if (eager)
                Rf_errorcall(call, "argument %d is empty", i + 1);
            p += __listAppend(&result, &pos, R_MissingArg, R_NilValue);
        } else {
            if (eager) {
                SEXP arg =
                    evalRirCode(codeAt(function(c), offset), ctx, env, 0);
                arg = escape(arg);
                assert(TYPEOF(arg) != PROMSXP);
                p += __listAppend(&result, &pos, arg, name);
            } else {
                Code* arg = codeAt(function(c), offset);
                SEXP promise = createPromise(arg, env);
                p += __listAppend(&result, &pos, promise, name);
            }
        }
    }

    UNPROTECT(p);
    return result;
}

/** Returns us the CCODE object from R_FunTab based on name.

  TODO This exists in gnu-r (names.c), when integrated inside, we want to make
  use of it.
 */
CCODE getBuiltin(SEXP f) {
    int i = ((sexprec_rjit*)f)->u.i;
    return R_FunTab[i].cfun;
}
int getFlag(SEXP f) {
    int i = ((sexprec_rjit*)f)->u.i;
    return (((R_FunTab[i].eval)/100)%10);
}

#if RIR_AS_PACKAGE == 0
void closureDebug(SEXP call, SEXP op, SEXP rho, SEXP newrho, RCNTXT* cntxt);
void endClosureDebug(SEXP op, SEXP call, SEXP rho);
SEXP closureArgumentAdaptor(SEXP call, SEXP op, SEXP arglist, SEXP rho, SEXP suppliedvars);
extern SEXP R_ReturnedValue;

SEXP rirCallTrampoline(RCNTXT* cntxt, RirContext* continuation, Code* code,
                       SEXP env, unsigned nargs, Context* ctx) {
    if ((SETJMP(cntxt->cjmpbuf))) {
        restoreCont(continuation);

        if (R_ReturnedValue == R_RestartToken) {
            cntxt->callflag = CTXT_RETURN; /* turn restart off */
            R_ReturnedValue = R_NilValue;  /* remove restart token */
            return evalRirCode(code, ctx, env, nargs);
        } else {
            return R_ReturnedValue;
        }
    }
    return evalRirCode(code, ctx, env, nargs);
}

INLINE SEXP rirCallClosure(SEXP call, SEXP env, SEXP callee, SEXP actuals,
                           unsigned nargs, OpcodeT** pc, Context* ctx) {
    // match formal arguments and create the env of this new activation record
    SEXP newEnv =
        closureArgumentAdaptor(call, callee, actuals, env, R_NilValue);

    ostack_push(ctx, newEnv);

    // Create a new R context
    RCNTXT* cntxt;
    RirContext* continuation;
    createRirClosureContext(cntxt, continuation, call, newEnv, env, actuals,
                            callee, pc, ctx);

    // Exec the closure
    closureDebug(call, callee, env, newEnv, cntxt);
    SEXP body = BODY(callee);
    Code* code = functionCode((Function*)INTEGER(body));

    unsigned stackguard_size_before = ostack_length(ctx);
    SEXP stackguard_before = ostack_top(ctx);
    SEXP result =
        rirCallTrampoline(cntxt, continuation, code, newEnv, nargs, ctx);
    unsigned stackguard_size_after = ostack_length(ctx);
    SEXP stackguard_after = ostack_top(ctx);
    assert(stackguard_before == stackguard_after &&
           stackguard_size_before == stackguard_size_after);
    endClosureDebug(callee, call, env);

    endRirContext(ctx, result);

    ostack_pop(ctx); // newEnv
    return result;
}
#define USE_RIR_CONTEXT_SETUP true
#endif

void warnSpecial(SEXP callee, SEXP call) {
    return;

    if (((sexprec_rjit*)callee)->u.i == 26) {
        printf("warning: calling special: .Internal(%s\n",
                CHAR(PRINTNAME(CAR(CADR(call)))));
    } else {
        printf("warning: calling special: %s\n",
                R_FunTab[((sexprec_rjit*)callee)->u.i].name);
    }
}

/** Performs the call.

  TODO this is currently super simple.

 */
INLINE SEXP doCall(Code* caller, SEXP call, SEXP callee, unsigned* args,
                   size_t nargs, SEXP names, SEXP env, OpcodeT** pc,
                   Context* ctx) {

    SEXP result = R_NilValue;
    switch (TYPEOF(callee)) {
    case SPECIALSXP: {
        // get the ccode
        CCODE f = getBuiltin(callee);
        int flag = getFlag(callee);
        R_Visible = flag != 1;
        warnSpecial(callee, call);

        // Store and restore stack status in case we get back here through
        // non-local return
        RirContext c = {ostack_length(ctx), NULL};
        // call it with the AST only
        result = f(call, callee, CDR(call), env);
        restoreCont(&c);

        if (flag < 2) R_Visible = flag != 1;
        break;
    }
    case BUILTINSXP: {
        // get the ccode
        CCODE f = getBuiltin(callee);
        int flag = getFlag(callee);
        // create the argslist
        SEXP argslist =
            createArgsList(caller, args, call, nargs, names, env, ctx, true);
        // callit
        PROTECT(argslist);
        if (flag < 2) R_Visible = flag != 1;

        // Store and restore stack status in case we get back here through
        // non-local return
        RirContext c = {ostack_length(ctx), NULL};
        result = f(call, callee, argslist, env);
        restoreCont(&c);

        if (flag < 2) R_Visible = flag != 1;
        UNPROTECT(1);
        break;
    }
    case CLOSXP: {
        SEXP actuals;
        actuals =
            createArgsList(caller, args, call, nargs, names, env, ctx, false);
        PROTECT(actuals);
#if RIR_AS_PACKAGE == 0
        // if body is INTSXP, it is rir serialized code, execute it directly
        SEXP body = BODY(callee);
        assert(TYPEOF(body) == INTSXP || !COMPILE_ON_DEMAND);
        if (USE_RIR_CONTEXT_SETUP && TYPEOF(body) == INTSXP) {
            assert(isValidFunctionSEXP(body));
            result = rirCallClosure(call, env, callee, actuals, nargs, pc, ctx);
            UNPROTECT(1);
            break;
        }
#endif
        Function * f = isValidClosureSEXP(callee);

        // Store and restore stack status in case we get back here through
        // non-local return
        RirContext c = {ostack_length(ctx), NULL};
        result = applyClosure(call, callee, actuals, env, R_NilValue);
        restoreCont(&c);

        UNPROTECT(1); // argslist
        break;
    }
    default:
        assert(false && "Don't know how to run other stuff");
    }
    return result;
}

// TODO: unify with the above doCall
INLINE SEXP doCallStack(Code* caller, SEXP call, size_t nargs, SEXP names,
                        SEXP env, OpcodeT** pc, Context* ctx) {

    SEXP res = R_NilValue;

    SEXP callee = *ostack_at(ctx, nargs);

    int p = 0;

    // This is a hack to support complex assignment's rewritten asts for
    // getters and setters.
    // The rewritten ast has target (and value for setters) marked as
    // placeholders, which we need to fill in here.
    // TODO: in the case of closures we should not do it eagerly
    if ((TYPEOF(callee) == SPECIALSXP || TYPEOF(callee) == CLOSXP) &&
        (CADR(call) == getterPlaceholderSym ||
         CADR(call) == setterPlaceholderSym)) {
        int setter = CADR(call) == setterPlaceholderSym;
        call = Rf_shallow_duplicate(call);
        PROTECT(call);
        p++;

        SEXP a = CDR(call);

        SEXP target = *ostack_at(ctx, nargs - 1);

        target = escape(target);

        // If the target is an ast, we need to wrap it in a quote, since
        // otherwise its not a value anymore, but will be evaluated.
        if (TYPEOF(target) == LANGSXP || TYPEOF(target) == SYMSXP) {
            target = LCONS(quoteSym, CONS_NR(target, R_NilValue));
        }

        SETCAR(a, target);

        if (setter) {
            SEXP prev = call;
            while (CDR(a) != R_NilValue) {
                prev = a;
                a = CDR(a);
            }

            assert(CAR(a) == setterPlaceholderSym);
            SEXP val = ostack_top(ctx);

            val = escape(val);

            // If the value is an ast, we need to wrap it in a quote, since
            // otherwise its not a value anymore, but will be evaluated.
            if (TYPEOF(val) == LANGSXP || TYPEOF(val) == SYMSXP) {
                val = LCONS(quoteSym, CONS_NR(val, R_NilValue));
            }

            INCREMENT_NAMED(val);
            SEXP v = CONS_NR(val, R_NilValue);
            SET_TAG(v, R_valueSym);
            SETCDR(prev, v);
        }
    }

    switch (TYPEOF(callee)) {
    case SPECIALSXP: {
        assert(call != R_NilValue);
        for (size_t i = 0; i < nargs; ++i)
            ostack_pop(ctx);
        ostack_pop(ctx); // callee
        // get the ccode
        CCODE f = getBuiltin(callee);
        int flag = getFlag(callee);
        R_Visible = flag != 1;
        warnSpecial(callee, call);

        // Store and restore stack status in case we get back here through
        // non-local return
        RirContext c = {ostack_length(ctx), NULL};
        // call it with the AST only
        res = f(call, callee, CDR(call), env);
        restoreCont(&c);

        if (flag < 2)
            R_Visible = flag != 1;
        break;
    }
    case BUILTINSXP: {
        SEXP argslist =
            createArgsListStack(caller, nargs, names, env, call, ctx, true);
        PROTECT(argslist);
        p++;
        for (size_t i = 0; i < nargs; ++i)
            ostack_pop(ctx);
        ostack_pop(ctx); // callee
        // get the ccode
        CCODE f = getBuiltin(callee);
        int flag = getFlag(callee);
        // create the argslist
        // callit
        if (flag < 2)
            R_Visible = flag != 1;

        // Store and restore stack status in case we get back here through
        // non-local return
        RirContext c = {ostack_length(ctx), NULL};
        res = f(call, callee, argslist, env);
        restoreCont(&c);

        if (flag < 2)
            R_Visible = flag != 1;
        break;
    }
    case CLOSXP: {
        SEXP argslist =
            createArgsListStack(caller, nargs, names, env, call, ctx, false);
        PROTECT(argslist);
        p++;
        for (size_t i = 0; i < nargs; ++i)
            ostack_pop(ctx);
        ostack_pop(ctx); // callee
#if RIR_AS_PACKAGE == 0
        // if body is INTSXP, it is rir serialized code, execute it directly
        SEXP body = BODY(callee);
        assert(TYPEOF(body) == INTSXP || !COMPILE_ON_DEMAND);
        if (USE_RIR_CONTEXT_SETUP && TYPEOF(body) == INTSXP) {
            assert(isValidFunctionSEXP(body));
            res = rirCallClosure(call, env, callee, argslist, nargs, pc, ctx);
            break;
        }
#endif
        Function* f = isValidClosureSEXP(callee);

        // Store and restore stack status in case we get back here through
        // non-local return
        RirContext c = {ostack_length(ctx), NULL};
        res = applyClosure(call, callee, argslist, env, R_NilValue);
        restoreCont(&c);
        break;
    }
    default:
        assert(false && "Don't know how to run other stuff");
    }
    UNPROTECT(p);
    return res;
}

// Imports from GNUR for method dispatch
SEXP R_possible_dispatch(SEXP call, SEXP op, SEXP args, SEXP rho,
                         Rboolean promisedArgs);
Rboolean R_has_methods(SEXP selector);
int Rf_usemethod(const char* generic, SEXP obj, SEXP call, SEXP args, SEXP rho,
                 SEXP callrho, SEXP defrho, SEXP* ans);

SEXP doDispatch(Code* caller, SEXP call, SEXP selector, SEXP obj,
                unsigned* args, size_t nargs, SEXP names, SEXP env,
                OpcodeT** pc, Context* ctx) {

#if RIR_AS_PACKAGE == 1
    // TODO
    assert(false);
#endif

    assert(isObject(obj));
    SEXP actuals =
        createArgsList(caller, args, call, nargs, names, env, ctx, false);
    protect(actuals);
    SEXP res = NULL;

    // Patch the already evaluated object into the first entry of the promise
    // args list
    SET_PRVALUE(CAR(actuals), obj);

    // Store and restore stack status in case we get back here through
    // non-local return
    RirContext c = {ostack_length(ctx), NULL};

    do {
        // ===============================================
        // First try S4
        if (IS_S4_OBJECT(obj) && R_has_methods(selector)) {
            res = R_possible_dispatch(call, selector, actuals, env, TRUE);
            if (res) {
                restoreCont(&c);
                break;
            }
        }

        // ===============================================
        // Then try S3
        const char* generic = CHAR(PRINTNAME(selector));
        char cntxt[400];
        SEXP rho1;
        PROTECT(rho1 = Rf_NewEnvironment(R_NilValue, R_NilValue, env));
        initClosureContext(&cntxt, call, rho1, env, actuals, selector);
        bool success = Rf_usemethod(generic, obj, call, actuals, rho1, env,
                                    R_BaseEnv, &res);
        UNPROTECT(1);
        endClosureContext(&cntxt, success ? res : R_NilValue);
        if (success) {
            restoreCont(&c);
            break;
        }

        // ===============================================
        // Now normal dispatch (mostly a copy from doCall)
        SEXP callee = findFun(selector, env);

        // TODO something should happen here
        if (callee == R_UnboundValue)
            assert(false && "Unbound var");
        else if (callee == R_MissingArg)
            assert(false && "Missing argument");

        switch (TYPEOF(callee)) {
        case SPECIALSXP: {
            // get the ccode
            CCODE f = getBuiltin(callee);
            int flag = getFlag(callee);
            R_Visible = flag != 1;

            // call it with the AST only
            res = f(call, callee, CDR(call), env);
            restoreCont(&c);

            if (flag < 2)
                R_Visible = flag != 1;
            break;
        }
        case BUILTINSXP: {
            // get the ccode
            CCODE f = getBuiltin(callee);
            int flag = getFlag(callee);
            // force all promises in the args list
            for (SEXP a = actuals; a != R_NilValue; a = CDR(a))
                SETCAR(a, rirEval(CAR(a), env));
            if (flag < 2)
                R_Visible = flag != 1;

            // Store and restore stack status in case we get back here through
            // non-local return
            RirContext c = {ostack_length(ctx), NULL};
            res = f(call, callee, actuals, env);
            restoreCont(&c);

            if (flag < 2)
                R_Visible = flag != 1;
            break;
        }
        case CLOSXP: {
#if RIR_AS_PACKAGE == 0
            // if body is INTSXP, it is rir serialized code, execute it directly
            SEXP body = BODY(callee);
            assert(TYPEOF(body) == INTSXP || !COMPILE_ON_DEMAND);
            if (USE_RIR_CONTEXT_SETUP && TYPEOF(body) == INTSXP) {
                assert(isValidFunctionSEXP(body));
                res = rirCallClosure(call, env, callee, actuals, nargs, pc, ctx);
                break;
            }
#endif
            // Store and restore stack status in case we get back here through
            // non-local return
            RirContext c = {ostack_length(ctx), NULL};
            res = applyClosure(call, callee, actuals, env, R_NilValue);
            restoreCont(&c);
            break;
        }
        default:
            assert(false && "Don't know how to run other stuff");
        }
    } while (false);

    UNPROTECT(1);
    assert(res);
    return res;
}

INSTRUCTION(call_stack_) {
    unsigned nargs = readImmediate(pc);
    // get the names of the arguments (or R_NilValue) if none
    SEXP names = readConst(ctx, pc);
    // get the call
    SEXP call = getSrcForCall(c, *pc, ctx);

    ostack_push(ctx, doCallStack(c, call, nargs, names, env, pc, ctx));
}

INSTRUCTION(call_) {
    // get the indices of argument promises
    SEXP args_ = readConst(ctx, pc);
    assert(TYPEOF(args_) == INTSXP &&
           "TODO change to INTSXP, not RAWSXP it used to be");
    unsigned nargs = Rf_length(args_) / sizeof(unsigned);
    unsigned* args = (unsigned*)INTEGER(args_);
    // get the names of the arguments (or R_NilValue) if none
    SEXP names = readConst(ctx, pc);
    // get the closure itself
    SEXP cls = ostack_pop(ctx);
    // get the call
    SEXP call = getSrcForCall(c, *pc, ctx);

    PROTECT(cls);
    ostack_push(ctx, doCall(c, call, cls, args, nargs, names, env, pc, ctx));
    UNPROTECT(1);
}

INSTRUCTION(dispatch_) {
    // get the indices of argument promises
    SEXP args_ = readConst(ctx, pc);
    assert(TYPEOF(args_) == INTSXP &&
           "TODO change to INTSXP, not RAWSXP it used to be");
    unsigned nargs = Rf_length(args_) / sizeof(unsigned);
    unsigned* args = (unsigned*)INTEGER(args_);
    // get the names of the arguments (or R_NilValue) if none
    SEXP names = readConst(ctx, pc);

    SEXP selector = readConst(ctx, pc);
    SEXP obj = ostack_pop(ctx);
    SEXP call = getSrcForCall(c, *pc, ctx);

    ostack_push(ctx, doDispatch(c, call, selector, obj, args, nargs, names, env,
                                pc, ctx));
}

INSTRUCTION(promise_) {
    // get the Code * pointer we need
    unsigned codeOffset = readImmediate(pc);
    Code* promiseCode = codeAt(function(c), codeOffset);
    // create the promise and push it on stack
    ostack_push(ctx, createPromise(promiseCode, env));
}

INSTRUCTION(push_code_) {
    // get the Code * pointer we need
    unsigned codeOffset = readImmediate(pc);
    Code* promiseCode = codeAt(function(c), codeOffset);
    // create the promise and push it on stack
    ostack_push(ctx, (SEXP)promiseCode);
}

INSTRUCTION(close_) {
    SEXP body = ostack_pop(ctx);
    SEXP formals = ostack_pop(ctx);
    PROTECT(body);
    PROTECT(formals);
    SEXP result = allocSExp(CLOSXP);
    SET_FORMALS(result, formals);
    SET_BODY(result, body);
    SET_CLOENV(result, env);
    UNPROTECT(2);
    ostack_push(ctx, result);
}

INSTRUCTION(force_) {
    SEXP p = ostack_pop(ctx);
    assert(TYPEOF(p) == PROMSXP);
    // If the promise is already evaluated then push the value inside the
    // promise
    // onto the stack, otherwise push the value from forcing the promise
    ostack_push(ctx, promiseValue(p, ctx));
}

INSTRUCTION(pop_) { ostack_pop(ctx); }


INSTRUCTION(asast_) {
    SEXP p = ostack_pop(ctx);
    assert(TYPEOF(p) == PROMSXP);
    SEXP ast = PRCODE(p);
    // if the code is NILSXP then it is rir Code object, get its ast
    if (TYPEOF(ast) == NILSXP)
        ast = cp_pool_at(ctx, ((Code*)ast)->src);
    // otherwise return whatever we had, make sure we do not see bytecode
    assert(TYPEOF(ast) != BCODESXP);
    ostack_push(ctx, ast);
}

INSTRUCTION(swap_) {
    SEXP a = ostack_pop(ctx);
    SEXP b = ostack_pop(ctx);
    ostack_push(ctx, a);
    ostack_push(ctx, b);
}

INSTRUCTION(put_) {
    uint32_t i = readImmediate(pc);
    SEXP* pos = ostack_at(ctx, 0);
    SEXP val = *pos;
    while (i--) {
        *pos = *(pos - 1);
        pos--;
    }
    *pos = val;
}

INSTRUCTION(pick_) {
    uint32_t i = readImmediate(pc);
    SEXP* pos = ostack_at(ctx, i);
    SEXP val = *pos;
    while (i--) {
        *pos = *(pos + 1);
        pos++;
    }
    *pos = val;
}

INSTRUCTION(is_) {
    SEXP test = ostack_pop(ctx);
    uint32_t i = readImmediate(pc);
    bool res;
    switch (i) {
    case NILSXP:
    case LGLSXP:
    case REALSXP:
        res = TYPEOF(test) == i;
        break;

    case VECSXP:
        res = TYPEOF(test) == VECSXP || TYPEOF(test) == LISTSXP;
        break;

    case LISTSXP:
        res = TYPEOF(test) == LISTSXP || TYPEOF(test) == NILSXP;
        break;

    default:
        assert(false);
        break;
    }
    ostack_push(ctx, res ? R_TrueValue : R_FalseValue);
}

INSTRUCTION(stvar_) {
    SEXP sym = readConst(ctx, pc);
    assert(TYPEOF(sym) == SYMSXP);
    SEXP val = escape(ostack_pop(ctx));
    INCREMENT_NAMED(val);
    defineVar(sym, val, env);
}

INSTRUCTION(aslogical_) {
    SEXP t = ostack_top(ctx);
    int r = asLogical(t);
    SEXP res = ScalarLogical(r);
    ostack_pop(ctx);
    ostack_push(ctx, res);
}

INSTRUCTION(lgl_or_) {
    int x2 = LOGICAL(ostack_pop(ctx))[0];
    int x1 = LOGICAL(ostack_pop(ctx))[0];
    assert(x1 == 1 || x1 == 0 || x1 == NA_LOGICAL);
    assert(x2 == 1 || x2 == 0 || x2 == NA_LOGICAL);
    if (x1 == 1 || x2 == 1)
        ostack_push(ctx, R_TrueValue);
    else if (x1 == 0 && x2 == 0)
        ostack_push(ctx, R_FalseValue);
    else
        ostack_push(ctx, R_LogicalNAValue);
}

INSTRUCTION(lgl_and_) {
    int x2 = LOGICAL(ostack_pop(ctx))[0];
    int x1 = LOGICAL(ostack_pop(ctx))[0];
    assert(x1 == 1 || x1 == 0 || x1 == NA_LOGICAL);
    assert(x2 == 1 || x2 == 0 || x2 == NA_LOGICAL);
    if (x1 == 1 && x2 == 1)
        ostack_push(ctx, R_TrueValue);
    else if (x1 == 0 || x2 == 0)
        ostack_push(ctx, R_FalseValue);
    else
        ostack_push(ctx, R_LogicalNAValue);
}

INSTRUCTION(asbool_) {
    SEXP t = ostack_top(ctx);
    int cond = NA_LOGICAL;
    if (Rf_length(t) > 1)
        warningcall(getSrcAt(c, *pc, ctx),
                    ("the condition has length > 1 and only the first "
                     "element will be used"));

    if (Rf_length(t) > 0) {
        switch (TYPEOF(t)) {
        case LGLSXP:
            cond = LOGICAL(t)[0];
            break;
        case INTSXP:
            cond = INTEGER(t)[0]; // relies on NA_INTEGER == NA_LOGICAL
        default:
            cond = asLogical(t);
        }
    }

    if (cond == NA_LOGICAL) {
        const char* msg =
            Rf_length(t)
                ? (isLogical(t) ? ("missing value where TRUE/FALSE needed")
                                : ("argument is not interpretable as logical"))
                : ("argument is of length zero");
        errorcall(getSrcAt(c, *pc, ctx), msg);
    }

    ostack_pop(ctx);
    ostack_push(ctx, cond ? R_TrueValue : R_FalseValue);
}

INSTRUCTION(brobj_) {
    int offset = readJumpOffset(pc);
    if (isObject(ostack_top(ctx)))
        *pc = *pc + offset;
    PC_BOUNDSCHECK(*pc);
}

INSTRUCTION(endcontext_) {
    SEXP cntxt_store = ostack_top(ctx);
    assert(TYPEOF(cntxt_store) == RAWSXP);
    RCNTXT* cntxt = (RCNTXT*)RAW(cntxt_store);
    Rf_endcontext(cntxt);
    ostack_pop(ctx); // Context
}

INSTRUCTION(brtrue_) {
    int offset = readJumpOffset(pc);
    if (ostack_pop(ctx) == R_TrueValue)
        *pc = *pc + offset;
    PC_BOUNDSCHECK(*pc);
}

INSTRUCTION(brfalse_) {
    int offset = readJumpOffset(pc);
    if (ostack_pop(ctx) == R_FalseValue)
        *pc = *pc + offset;
    PC_BOUNDSCHECK(*pc);
}

INSTRUCTION(br_) {
    int offset = readJumpOffset(pc);
    *pc = *pc + offset;
    PC_BOUNDSCHECK(*pc);
}

#if RIR_AS_PACKAGE == 0
SEXP do_subset2_dflt(SEXP, SEXP, SEXP, SEXP);
SEXP do_subset_dflt(SEXP, SEXP, SEXP, SEXP);
#endif

INSTRUCTION(subset1_) {
    SEXP idx = ostack_pop(ctx);
    SEXP val = ostack_pop(ctx);

    SEXP res;
#if RIR_AS_PACKAGE == 0
    SEXP args;
    PROTECT(val);
    args = CONS_NR(idx, R_NilValue);
    UNPROTECT(1);
    args = CONS_NR(val, args);
    PROTECT(args);
    res = do_subset_dflt(getSrcForCall(c, *pc, ctx), R_SubsetSym, args, env);
    UNPROTECT(1);
#else
    res = Rf_eval(getSrcForCall(c, *pc, ctx), env);
#endif

    R_Visible = 1;
    ostack_push(ctx, res);
}

INSTRUCTION(extract1_) {
    SEXP idx = ostack_pop(ctx);
    SEXP val = ostack_pop(ctx);

    SEXP res;
    unsigned type = TYPEOF(val) << 5 + TYPEOF(idx);

#define SIMPLECASE(vectype, vecaccess, idxtype, idxaccess)                     \
    case vectype << 5 + idxtype: {                                             \
        if (getAttrib(val, R_NamesSymbol) != R_NilValue ||                     \
            !IS_SIMPLE_SCALAR(idx, idxtype))                                   \
            goto fallback;                                                     \
        if (IS_SIMPLE_SCALAR(val, vectype) && !MAYBE_SHARED(val))              \
            res = val;                                                         \
        else                                                                   \
            res = allocVector(vectype, 1);                                     \
        int i = (int)idxaccess(idx)[0] - 1;                                    \
        if (Rf_length(val) <= i)                                               \
            goto fallback;                                                     \
        vecaccess(res)[0] = vecaccess(val)[i];                                 \
        break;                                                                 \
    }

    switch (type) {
        SIMPLECASE(REALSXP, REAL, REALSXP, REAL);
        SIMPLECASE(REALSXP, REAL, INTSXP, INTEGER);
        SIMPLECASE(REALSXP, REAL, LGLSXP, LOGICAL);
        SIMPLECASE(INTSXP, INTEGER, REALSXP, REAL);
        SIMPLECASE(INTSXP, INTEGER, INTSXP, INTEGER);
        SIMPLECASE(INTSXP, INTEGER, LGLSXP, LOGICAL);
        SIMPLECASE(LGLSXP, LOGICAL, REALSXP, REAL);
        SIMPLECASE(LGLSXP, LOGICAL, INTSXP, INTEGER);
        SIMPLECASE(LGLSXP, LOGICAL, LGLSXP, LOGICAL);
#undef SIMPLECASE
    default:
    fallback : {
#if RIR_AS_PACKAGE == 0
        SEXP args;
        PROTECT(val);
        args = CONS_NR(idx, R_NilValue);
        UNPROTECT(1);
        args = CONS_NR(val, args);
        PROTECT(args);
        res = do_subset2_dflt(getSrcForCall(c, *pc, ctx), R_Subset2Sym, args,
                              env);
        UNPROTECT(1);
#else
        res = Rf_eval(getSrcForCall(c, *pc, ctx), env);
#endif
    }
    }

    R_Visible = 1;
    ostack_push(ctx, res);
}

INSTRUCTION(dup_) { ostack_push(ctx, ostack_top(ctx)); }

INSTRUCTION(add_) {
    assert(false);
    SEXP rhs = ostack_pop(ctx);
    SEXP lhs = ostack_pop(ctx);
    if (TYPEOF(lhs) == REALSXP && TYPEOF(rhs) == REALSXP &&
        Rf_length(lhs) == 1 && Rf_length(rhs) == 1) {
        SEXP res = Rf_allocVector(REALSXP, 1);
        SET_NAMED(res, 1);
        REAL(res)[0] = REAL(lhs)[0] + REAL(rhs)[0];
        ostack_push(ctx, res);
    } else {
        NOT_IMPLEMENTED;
        // SEXP op = getPrimitive("+");
        // SEXP primfun = getPrimfun("+");
        // TODO we should change how primitives are called now that we can
        // integrate
        // SEXP res = callPrimitive(primfun, getCurrentCall(c, pc), op, { lhs,
        // rhs });
        // TODO push
    }
}

INSTRUCTION(sub_) {
    assert(false);
    SEXP rhs = ostack_pop(ctx);
    SEXP lhs = ostack_pop(ctx);
    if (TYPEOF(lhs) == REALSXP && TYPEOF(rhs) == REALSXP &&
        Rf_length(lhs) == 1 && Rf_length(rhs) == 1) {
        SEXP res = Rf_allocVector(REALSXP, 1);
        SET_NAMED(res, 1);
        REAL(res)[0] = REAL(lhs)[0] - REAL(rhs)[0];
        ostack_push(ctx, res);
    } else {
        NOT_IMPLEMENTED;
        // SEXP op = getPrimitive("-");
        // SEXP primfun = getPrimfun("-");
        // TODO we should change how primitives are called now that we can
        // integrate
        // SEXP res = callPrimitive(primfun, getCurrentCall(c, pc), op, { lhs,
        // rhs });
        // TODO push
    }
}

INSTRUCTION(lt_) {
    SEXP rhs = ostack_pop(ctx);
    SEXP lhs = ostack_pop(ctx);
    if (TYPEOF(lhs) == REALSXP && TYPEOF(rhs) == REALSXP &&
        Rf_length(lhs) == 1 && Rf_length(rhs) == 1) {
        SEXP res = Rf_allocVector(REALSXP, 1);
        SET_NAMED(res, 1);
        ostack_push(ctx,
                    REAL(lhs)[0] < REAL(rhs)[0] ? R_TrueValue : R_FalseValue);
    } else {
        NOT_IMPLEMENTED;
        // SEXP op = getPrimitive("<");
        // SEXP primfun = getPrimfun("<");
        // TODO we should change how primitives are called now that we can
        // integrate
        // SEXP res = callPrimitive(primfun, getCurrentCall(c, pc), op, { lhs,
        // rhs });
        // TODO push
    }
}

INSTRUCTION(isspecial_) {
    // TODO I do not think this is a proper way - we must check all the way
    // down, not just findVar (vars do not shadow closures)
    SEXP sym = readConst(ctx, pc);
    SEXP val = findVar(sym, env);
    // TODO better check
    assert(TYPEOF(val) == SPECIALSXP || TYPEOF(val) == BUILTINSXP);
}

INSTRUCTION(isfun_) {
    SEXP val = ostack_top(ctx);

    switch (TYPEOF(val)) {
    case CLOSXP:
        jit(val, ctx);
        break;
    case SPECIALSXP:
    case BUILTINSXP:
        // builtins and specials are fine
        // TODO for now - we might be fancier here later
        break;
    default:
	error("attempt to apply non-function");
    }
}

INSTRUCTION(inc_) {
    SEXP n = ostack_top(ctx);
    assert(TYPEOF(n) == INTSXP);
    int i = INTEGER(n)[0];
    if (MAYBE_SHARED(n)) {
        ostack_pop(ctx);
        SEXP nn = Rf_allocVector(INTSXP, 1);
        INTEGER(nn)[0] = i + 1;
        ostack_push(ctx, nn);
    } else {
        INTEGER(n)[0]++;
    }
}

INSTRUCTION(test_bounds_) {
    SEXP vec = *ostack_at(ctx, 1);
    SEXP idx = *ostack_at(ctx, 0);
    int len = Rf_length(vec);
    int i = asInteger(idx);
    ostack_push(ctx, i > 0 && i <= len ? R_TrueValue : R_FalseValue);
}

INSTRUCTION(dup2_) {
    SEXP a = *ostack_at(ctx, 1);
    SEXP b = *ostack_at(ctx, 0);
    ostack_push(ctx, a);
    ostack_push(ctx, b);
}

INSTRUCTION(invisible_) {
    R_Visible = 0;
}

INSTRUCTION(uniq_) {
    SEXP v = ostack_top(ctx);
    if (MAYBE_SHARED(v)) {
        v = shallow_duplicate(v);
        *ostack_at(ctx, 0) = v;
    }
    SET_NAMED(ostack_top(ctx), NAMED(ostack_top(ctx)) + 1);
}

extern void printCode(Code* c);
extern void printFunction(Function* f);

extern SEXP Rf_deparse1(SEXP call, Rboolean abbrev, int opts);

#undef R_CheckStack

SEXP evalRirCode(Code* c, Context* ctx, SEXP env, unsigned numArgs) {

    // printCode(c);

    if (!env)
	error("'rho' cannot be C NULL: detected in C-level eval");
    if (!isEnvironment(env))
	error("'rho' must be an environment not %s: detected in C-level eval",
	      type2char(TYPEOF(env)));

    R_CheckStack();

    // make sure there is enough room on the stack
    // there is some slack of 5 to make sure the call instruction can store
    // some intermediate values on the stack
    ostack_ensureSize(ctx, c->stackLength + 5);

    OpcodeT* pc = code(c);

    R_Visible = TRUE;
    // main loop
    while (true) {
        // printf("%p : %d, s: %d\n", c, **pc, ostack_length(ctx));
        switch (readOpcode(&pc)) {

#define INS(name)                                                              \
    case name:                                                                 \
        ins_##name(c, env, &pc, ctx, numArgs);                                 \
        break

            INS(push_);
            INS(ldfun_);
            INS(ldvar_);
            INS(ldddvar_);
            INS(call_);
            INS(call_stack_);
            INS(promise_);
            INS(push_code_);
            INS(close_);
            INS(force_);
            INS(pop_);
            INS(asast_);
            INS(stvar_);
            INS(asbool_);
            INS(brobj_);
            INS(endcontext_);
            INS(brtrue_);
            INS(brfalse_);
            INS(br_);
            INS(dup_);
            INS(add_);
            INS(sub_);
            INS(lt_);
            INS(swap_);
            INS(put_);
            INS(pick_);
            INS(is_);
            INS(isspecial_);
            INS(isfun_);
            INS(inc_);
            INS(dup2_);
            INS(test_bounds_);
            INS(invisible_);
            INS(extract1_);
            INS(subset1_);
            INS(dispatch_);
            INS(uniq_);
            INS(aslogical_);
            INS(lgl_and_);
            INS(lgl_or_);

        case beginloop_: {
            // Allocate a RCNTXT on the stack
            SEXP cntxt_store =
                Rf_allocVector(RAWSXP, sizeof(RCNTXT) + sizeof(pc));
            ostack_push(ctx, cntxt_store);

            RCNTXT* cntxt = (RCNTXT*)RAW(cntxt_store);

            // (ab)use the same buffe to store the current pc
            OpcodeT** oldPc = (OpcodeT**)(cntxt + 1);
            *oldPc = pc;

            Rf_begincontext(cntxt, CTXT_LOOP, R_NilValue, env, R_BaseEnv,
                            R_NilValue, R_NilValue);
            // (ab)use the unused cenddata field to store sp
            cntxt->cenddata = (void*)ostack_length(ctx);

            readJumpOffset(&pc);

            int s;
            if ((s = SETJMP(cntxt->cjmpbuf))) {
                // incomming non-local break/continue:
                // restore our stack state
                rl_setLength(&ctx->ostack, (size_t)R_GlobalContext->cenddata);

                // get the RCNTXT from the stack
                SEXP cntxt_store = ostack_top(ctx);
                assert(TYPEOF(cntxt_store) == RAWSXP && "stack botched");
                RCNTXT* cntxt = (RCNTXT*)RAW(cntxt_store);
                assert(cntxt == R_GlobalContext && "stack botched");
                OpcodeT** oldPc = (OpcodeT**)(cntxt + 1);
                pc = *oldPc;

                int offset = readJumpOffset(&pc);

                if (s == CTXT_BREAK)
                    pc = pc + offset;
                PC_BOUNDSCHECK(pc);
            }
            break;
        }

        case ret_: {
            // not in its own function so that we can avoid nonlocal returns
            goto __eval_done;
        }
        default:
            assert(false && "wrong or unimplemented opcode");
        }
    }
__eval_done : {
    return ostack_pop(ctx);
}
}


SEXP rirExpr(SEXP f) {
    if (isValidCodeObject(f)) {
        Code* c = (Code*)f;
        return src_pool_at(globalContext(), c->src);
    }
    assert(TYPEOF(f) != 31);
    if (isValidFunctionObject(f)) {
        Function* ff = (Function*)(INTEGER(f));
        return src_pool_at(globalContext(), functionCode(ff)->src);
    }
    return f;
}

SEXP rirEval_f(SEXP f, SEXP env) {
    // TODO we do not really need the arg counts now
    if (isValidCodeObject(f)) {
        //        Rprintf("Evaluating promise:\n");
        Code* c = (Code*)f;
        SEXP x = evalRirCode(c, globalContext(), env, 0);
      //        Rprintf("Promise evaluated, length %u, value %d",
        //        Rf_length(x), REAL(x)[0]);
        return escape(x);
    } else {
        //        Rprintf("=====================================================\n");
        //        Rprintf("Evaluating function\n");
        Function* ff = (Function*)(INTEGER(f));
        return escape(evalRirCode(functionCode(ff), globalContext(), env, 0));
    }
}

SEXP rirEval(SEXP e, SEXP env) {
    static int evalcount = 0;
    if (++evalcount > 1000) { /* was 100 before 2.8.0 */
        R_CheckUserInterrupt();
        evalcount = 0 ;
    }

    R_Visible = TRUE;

    /* handle self-evluating objects with minimal overhead */
    switch (TYPEOF(e)) {
    case INTSXP: {
        if (isValidFunctionSEXP(e)) {
            Function* ff = (Function*)(INTEGER(e));
            return escape(
                evalRirCode(functionCode(ff), globalContext(), env, 0));
        }
        // Fall through
    }
    case NILSXP:
    case LISTSXP:
    case LGLSXP:
    case REALSXP:
    case STRSXP:
    case CPLXSXP:
    case RAWSXP:
    case S4SXP:
    case SPECIALSXP:
    case BUILTINSXP:
    case ENVSXP:
    case CLOSXP:
    case VECSXP:
    case EXTPTRSXP:
    case WEAKREFSXP:
    case EXPRSXP:
	/* Make sure constants in expressions are NAMED before being
	   used as values.  Setting NAMED to 2 makes sure weird calls
	   to replacement functions won't modify constants in
	   expressions.  */
	if (NAMED(e) <= 1) SET_NAMED(e, 2);
	return e;

    case 31: {
        Code* c = (Code*)e;
        assert(isValidCodeObject(e));
        return escape(evalRirCode(c, globalContext(), env, 0));
    }

    case NATIVESXP:
        assert(false);
        break;

    case BCODESXP: {
        SEXP expr = VECTOR_ELT(CDR(e), 0);
        SEXP code = globalContext()->compiler(expr);
        PROTECT(code);
        Function* ff = (Function*)(INTEGER(code));
        SEXP res = evalRirCode(functionCode(ff), globalContext(), env, 0);
        UNPROTECT(1);
        return escape(res);
    }

    case SYMSXP: {
        if (e == R_DotsSymbol)
            error("'...' used in an incorrect context");

        SEXP val;
        
        if (DDVAL(e))
            val = ddfindVar(e, env);
        else
            val = findVar(e, env);

        // TODO better errors
        if (val == R_UnboundValue) {
            Rf_error("object not found");
        } else if (val == R_MissingArg) {
            Rf_error("argument \"%s\" is missing, with no default", CHAR(PRINTNAME(e)));
        }

        // if promise, evaluate & return
        if (TYPEOF(val) == PROMSXP)
            val = promiseValue(val, globalContext());

        // WTF? is this just defensive programming or what?
        if (NAMED(val) == 0 && val != R_NilValue)
            SET_NAMED(val, 1);

        return val;
        break;
    }

    case PROMSXP:
        return promiseValue(e, globalContext());

    case LANGSXP: {
        SEXP code = globalContext()->compiler(e);
        PROTECT(code);
        Function* ff = (Function*)(INTEGER(code));
        SEXP res = evalRirCode(functionCode(ff), globalContext(), env, 0);
        UNPROTECT(1);
        return escape(res);
    }

    case DOTSXP:
	error("'...' used in an incorrect context");
    default:
        assert(false && "UNIMPLEMENTED_TYPE");
    }

    assert(false);
    return R_NilValue;
}
