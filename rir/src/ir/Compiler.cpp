#include "Compiler.h"

#include "BC.h"
#include "CodeStream.h"

#include "R/r.h"
#include "R/RList.h"
#include "R/Sexp.h"
#include "R/Symbols.h"

#include "Optimizer.h"
#include "utils/Pool.h"

#include "CodeVerifier.h"

#include <stack>

namespace rir {

namespace {

class Context {
  public:
    class LoopContext {
      public:
        Label next_;
        Label break_;
        LoopContext(Label next_, Label break_) : next_(next_), break_(break_) {}
    };

    class CodeContext {
      public:
        CodeStream cs;
        std::stack<LoopContext> loops;
        CodeContext(SEXP ast, FunctionHandle& fun) : cs(fun, ast) {}
    };

    std::stack<CodeContext> code;

    CodeStream& cs() { return code.top().cs; }

    FunctionHandle& fun;
    Preserve& preserve;

    Context(FunctionHandle& fun, Preserve& preserve)
        : fun(fun), preserve(preserve) {}

    bool inLoop() { return !code.top().loops.empty(); }

    LoopContext& loop() { return code.top().loops.top(); }

    void pushLoop(Label& next_, Label break_) {
        code.top().loops.emplace(next_, break_);
    }

    void popLoop() { code.top().loops.pop(); }

    void push(SEXP ast) { code.emplace(ast, fun); }

    fun_idx_t pop() {
        auto idx = cs().finalize();
        code.pop();
        return idx;
    }
};

fun_idx_t compilePromise(Context& ctx, SEXP exp);
void compileExpr(Context& ctx, SEXP exp);
void compileCall(Context& ctx, SEXP ast, SEXP fun, SEXP args);

void compileDispatch(Context& ctx, SEXP selector, SEXP ast, SEXP fun,
                     SEXP args) {
    // Process arguments:
    // Arguments can be optionally named
    std::vector<fun_idx_t> callArgs;
    std::vector<SEXP> names;

    // This is the sane as in doCall
    for (auto arg = RList(args).begin(); arg != RList::end(); ++arg) {
        if (*arg == R_DotsSymbol) {
            callArgs.push_back(DOTS_ARG_IDX);
            names.push_back(R_NilValue);
            continue;
        }
        if (*arg == R_MissingArg) {
            callArgs.push_back(MISSING_ARG_IDX);
            names.push_back(R_NilValue);
            continue;
        }

        // (1) Arguments are wrapped as Promises:
        //     create a new Code object for the promise
        size_t prom = compilePromise(ctx, *arg);
        callArgs.push_back(prom);

        // (2) remember if the argument had a name associated
        names.push_back(arg.tag());
    }
    assert(callArgs.size() < MAX_NUM_ARGS);

    ctx.cs() << BC::dispatch(selector, callArgs, names);

    ctx.cs().addAst(ast);
}

// Inline some specials
// TODO: once we have sufficiently powerful analysis this should (maybe?) go
//       away and move to an optimization phase.
bool compileSpecialCall(Context& ctx, SEXP ast, SEXP fun, SEXP args_) {
    RList args(args_);
    CodeStream& cs = ctx.cs();

    if (fun == symbol::And && args.length() == 2) {
        cs << BC::isspecial(fun);

        Label nextBranch = cs.mkLabel();

        compileExpr(ctx, args[0]);

        cs << BC::asLogical();
        cs.addAst(args[0]);
        cs << BC::dup()
           << BC::brfalse(nextBranch);

        compileExpr(ctx, args[1]);

        cs << BC::asLogical();
        cs.addAst(args[1]);
        cs << BC::lglAnd();

        cs << nextBranch;

        return true;
    }

    if (fun == symbol::Or && args.length() == 2) {
        cs << BC::isspecial(fun);

        Label nextBranch = cs.mkLabel();

        compileExpr(ctx, args[0]);

        cs << BC::asLogical();
        cs.addAst(args[0]);
        cs << BC::dup()
           << BC::brtrue(nextBranch);

        compileExpr(ctx, args[1]);

        cs << BC::asLogical();
        cs.addAst(args[1]);
        cs << BC::lglOr();

        cs << nextBranch;

        return true;
    }


    if (fun == symbol::quote && args.length() == 1) {
        auto i = compilePromise(ctx, args[0]);

        cs << BC::isspecial(fun) << BC::push_code(i);
        return true;
    }

    if (fun == symbol::Assign) {
        assert(args.length() == 2);

        auto lhs = args[0];
        auto rhs = args[1];

        // Verify lhs is valid
        SEXP l = lhs;
        while (l) {
            Match(l) {
                Case(LANGSXP, fun, args) {
                    if (TYPEOF(fun) == SYMSXP) {
                        l = CAR(args);
                    } else {
                        // Cant rewrite this statically...
                        return false;
                    }
                }
                Case(SYMSXP) { l = nullptr; }
                Case(STRSXP) { l = nullptr; }
                Else({
                    // Probably broken assignment
                    return false;
                })
            }
        }

        cs << BC::isspecial(fun);

        Match(lhs) {
            Case(SYMSXP) {
                compileExpr(ctx, rhs);
                cs << BC::dup()
                   << BC::stvar(lhs)
                   << BC::invisible();
                return true;
            }
            Else(break)
        }

        compileExpr(ctx, rhs);
        cs << BC::dup();

        // Find all parts of the lhs
        SEXP target = nullptr;
        l = lhs;
        std::vector<SEXP> lhsParts;
        while (!target) {
            Match(l) {
                Case(LANGSXP, fun, args) {
                    assert(TYPEOF(fun) == SYMSXP);
                    lhsParts.push_back(l);
                    l = CAR(args);
                }
                Case(SYMSXP) {
                    lhsParts.push_back(l);
                    target = l;
                }
                Case(STRSXP) {
                    assert(Rf_length(l) == 1);
                    target = Rf_install(CHAR(STRING_ELT(l, 0)));
                    lhsParts.push_back(target);
                }
                Else({
                    errorcall(ast,
                              "invalid (do_set) left-hand side to assignment");
                })
            }
        }

        // Evaluate the getter list and push it to the stack in reverse order
        for (unsigned i = lhsParts.size() - 1; i > 0; --i) {
            auto g = lhsParts[i];

            Match(g) {
                Case(SYMSXP) { cs << BC::ldvar(g); }
                Case(LANGSXP) {
                    SEXP fun = CAR(g);
                    RList args(CDR(g));
                    std::vector<SEXP> names;

                    auto arg = args.begin();
                    // Skip first arg (is already on the stack)
                    ++arg;
                    names.push_back(R_NilValue);

                    // Load function and push it before the first arg
                    cs << BC::ldfun(fun) << BC::swap();

                    for (; arg != RList::end(); ++arg) {
                        if (*arg == R_DotsSymbol || *arg == R_MissingArg) {
                            names.push_back(R_NilValue);
                            cs << BC::push(*arg);
                            continue;
                        }

                        names.push_back(arg.tag());
                        if (TYPEOF(*arg) == LANGSXP || TYPEOF(*arg) == SYMSXP) {
                            auto p = compilePromise(ctx, *arg);
                            cs << BC::promise(p);
                        } else {
                            compileExpr(ctx, *arg);
                        }
                    }

                    cs << BC::call_stack(names.size(), names);
                    SEXP rewrite = Rf_shallow_duplicate(g);
                    ctx.preserve(rewrite);
                    SETCAR(CDR(rewrite), symbol::getterPlaceholder);

                    cs.addAst(rewrite);
                }
                Else(assert(false);)
            }
            if (i > 1)
                cs << BC::dup();

            // The setter internals are allowed to modify the lhs, thus
            // we need to make sure its not shared!
            cs << BC::uniq();

            if (i > 1)
                cs << BC::swap();
        }

        // Get down the initial rhs value
        cs << BC::pick(lhsParts.size() - 1);

        // Run the setters
        for (auto g = lhsParts.begin(); (g + 1) != lhsParts.end(); ++g) {
            SEXP fun = CAR(*g);
            RList args(CDR(*g));
            std::string name(CHAR(PRINTNAME(fun)));
            name.append("<-");
            SEXP setterName = Rf_install(name.c_str());

            std::vector<SEXP> names;

            auto arg = RList(args).begin();

            unsigned nargs = 0;
            // Skip first arg (is already on the stack)
            ++arg;
            names.push_back(R_NilValue);

            // Load function and push it before the first arg and the value
            // from the last setter.
            cs << BC::ldfun(setterName) << BC::put(2);

            for (; arg != RList::end(); ++arg) {
                nargs++;
                if (*arg == R_DotsSymbol || *arg == R_MissingArg) {
                    names.push_back(R_NilValue);
                    cs << BC::push(*arg);
                    continue;
                }

                names.push_back(arg.tag());
                if (TYPEOF(*arg) == LANGSXP || TYPEOF(*arg) == SYMSXP) {
                    auto p = compilePromise(ctx, *arg);
                    cs << BC::promise(p);
                } else {
                    compileExpr(ctx, *arg);
                }
            }

            names.push_back(symbol::value);
            // the rhs (aka "value") needs to come last, if we pushed some args
            // we need to swap the order
            if (nargs > 0)
                cs << BC::pick(nargs);

            cs << BC::call_stack(names.size(), names);

            SEXP rewrite = Rf_shallow_duplicate(*g);
            ctx.preserve(rewrite);
            SETCAR(rewrite, setterName);

            SEXP a = CDR(rewrite);
            SETCAR(a, symbol::setterPlaceholder);
            while (CDR(a) != R_NilValue)
                a = CDR(a);
            SEXP value = CONS_NR(symbol::setterPlaceholder, R_NilValue);
            SET_TAG(value, symbol::value);
            SETCDR(a, value);
            cs.addAst(rewrite);

            cs << BC::uniq();
        }

        cs << BC::stvar(target)
           << BC::invisible();

        return true;
    }

    if (fun == symbol::Internal) {
        // TODO: Needs more thought
        return false;
    }

    if (fun == symbol::isnull && args.length() == 1) {
        cs << BC::isspecial(fun);
        compileExpr(ctx, args[0]);
        cs << BC::is(NILSXP);
        return true;
    }

    if (fun == symbol::islist && args.length() == 1) {
        cs << BC::isspecial(fun);
        compileExpr(ctx, args[0]);
        cs << BC::is(VECSXP);
        return true;
    }

    if (fun == symbol::ispairlist && args.length() == 1) {
        cs << BC::isspecial(fun);
        compileExpr(ctx, args[0]);
        cs << BC::is(LISTSXP);
        return true;
    }

    if (fun == symbol::DoubleBracket || fun == symbol::Bracket) {
        if (args.length() == 2) {
            auto lhs = args[0];
            auto idx = args[1];

            // TODO
            if (idx == R_DotsSymbol || idx == R_MissingArg ||
                TAG(idx) != R_NilValue)
                return false;

            Label objBranch = cs.mkLabel();
            Label nextBranch = cs.mkLabel();

            cs << BC::isspecial(fun);
            compileExpr(ctx, lhs);
            cs << BC::brobj(objBranch);

            compileExpr(ctx, idx);
            if (fun == symbol::DoubleBracket)
                cs << BC::extract1();
            else
                cs << BC::subset1();

            cs.addAst(ast);
            cs << BC::br(nextBranch);

            cs << objBranch;
            compileDispatch(ctx, fun, ast, fun, args_);

            cs << nextBranch;
            return true;
        }
    }

    if (fun == symbol::While) {
        assert(args.length() == 2);

        auto cond = args[0];
        auto body = args[1];

        cs << BC::isspecial(fun);

        Label loopBranch = cs.mkLabel();
        Label nextBranch = cs.mkLabel();

        ctx.pushLoop(loopBranch, nextBranch);

        cs << BC::beginloop(nextBranch);
        cs << loopBranch;

        compileExpr(ctx, cond);
        cs << BC::asbool()
           << BC::brfalse(nextBranch);

        compileExpr(ctx, body);
        cs << BC::pop()
           << BC::br(loopBranch);

        cs << nextBranch
           << BC::endcontext()
           << BC::push(R_NilValue)
           << BC::invisible();

        ctx.popLoop();

        return true;
    }

    if (fun == symbol::Repeat) {
        assert(args.length() == 1);

        auto body = args[0];

        cs << BC::isspecial(fun);

        Label loopBranch = cs.mkLabel();
        Label nextBranch = cs.mkLabel();

        ctx.pushLoop(loopBranch, nextBranch);

        cs << BC::beginloop(nextBranch);
        cs << loopBranch;

        compileExpr(ctx, body);
        cs << BC::pop()
           << BC::br(loopBranch);

        cs << nextBranch
           << BC::endcontext()
           << BC::push(R_NilValue)
           << BC::invisible();

        ctx.popLoop();

        return true;
    }

    // TODO: not quite yet
    if (false && fun == symbol::For) {
        // TODO: if the seq is not a vector, we need to throw an error!
        assert(args.length() == 3);

        auto sym = args[0];
        auto seq = args[1];
        auto body = args[2];

        assert(TYPEOF(sym) == SYMSXP);

        cs << BC::isspecial(fun);

        Label loopBranch = cs.mkLabel();
        Label nextBranch = cs.mkLabel();

        ctx.pushLoop(loopBranch, nextBranch);

        compileExpr(ctx, seq);
        cs << BC::uniq()
           << BC::push((int)0);

        cs << BC::beginloop(nextBranch)
           // TODO: that doesn't work, since it pushes the context to the stack
           // and the inc below will fail. but we cant do stack manipulation
           // here either, since the beginloop is target for non-local
           // continues.

           << loopBranch
           << BC::inc()
           << BC::testBounds()
           << BC::brfalse(nextBranch)
           << BC::dup2()
           << BC::extract1();

        // TODO: we would want a less generic extract here, but we don't have it
        // right now. therefore we need to pass an AST here (which we know won't
        // be used since the sequence has to be a vector);
        cs.addAst(R_NilValue);
        cs << BC::stvar(sym);

        compileExpr(ctx, body);
        cs << BC::pop()
           << BC::br(loopBranch);

        cs << nextBranch
           << BC::endcontext()
           << BC::pop()
           << BC::pop()
           << BC::push(R_NilValue)
           << BC::invisible();

        ctx.popLoop();

        return true;
    }

    if (fun == symbol::Next && ctx.inLoop()) {
        assert(args.length() == 0);

        cs << BC::isspecial(fun) << BC::br(ctx.loop().next_);

        return true;
    }

    if (fun == symbol::Break && ctx.inLoop()) {
        assert(args.length() == 0);
        assert(ctx.inLoop());

        cs << BC::isspecial(fun) << BC::br(ctx.loop().break_);

        return true;
    }

    return false;
}

// function application
void compileCall(Context& ctx, SEXP ast, SEXP fun, SEXP args) {
    CodeStream& cs = ctx.cs();

    // application has the form:
    // LHS ( ARGS )

    // LHS can either be an identifier or an expression
    Match(fun) {
        Case(SYMSXP) {
            if (compileSpecialCall(ctx, ast, fun, args))
                return;

            cs << BC::ldfun(fun);
        }
        Else({
            compileExpr(ctx, fun);
            cs << BC::isfun();
        });
    }

    // Process arguments:
    // Arguments can be optionally named
    std::vector<fun_idx_t> callArgs;
    std::vector<SEXP> names;

    for (auto arg = RList(args).begin(); arg != RList::end(); ++arg) {
        if (*arg == R_DotsSymbol) {
            callArgs.push_back(DOTS_ARG_IDX);
            names.push_back(R_NilValue);
            continue;
        }
        if (*arg == R_MissingArg) {
            callArgs.push_back(MISSING_ARG_IDX);
            names.push_back(R_NilValue);
            continue;
        }

        // (1) Arguments are wrapped as Promises:
        //     create a new Code object for the promise
        size_t prom = compilePromise(ctx, *arg);
        callArgs.push_back(prom);

        // (2) remember if the argument had a name associated
        names.push_back(arg.tag());
    }
    assert(callArgs.size() < MAX_NUM_ARGS);

    cs << BC::call(callArgs, names);

    cs.addAst(ast);
}

// Lookup
void compileGetvar(CodeStream& cs, SEXP name) {
    if (DDVAL(name)) {
        cs << BC::ldddvar(name);
    } else if (name == R_MissingArg) {
        cs << BC::push(R_MissingArg);
    } else {
        cs << BC::ldvar(name);
    }
}

// Constant
void compileConst(CodeStream& cs, SEXP constant) {
    SET_NAMED(constant, 2);
    cs << BC::push(constant);
}

void compileExpr(Context& ctx, SEXP exp) {
    // Dispatch on the current type of AST node
    Match(exp) {
        // Function application
        Case(LANGSXP, fun, args) { compileCall(ctx, exp, fun, args); }
        // Variable lookup
        Case(SYMSXP) { compileGetvar(ctx.cs(), exp); }
        Case(PROMSXP, value, expr) {
            // TODO: honestly I do not know what should be the semantics of
            //       this shit.... For now force it here and see what
            //       breaks...
            //       * One of the callers that does this is eg. print.c:1013
            //       * Another (a bit more sane) producer of this kind of ast
            //         is eval.c::applydefine (see rhsprom). At least there
            //         the prom is already evaluated and only used to attach
            //         the expression to the already evaled value
            SEXP val = forcePromise(exp);
            Protect p(val);
            compileConst(ctx.cs(), val);
            ctx.cs().addAst(expr);
        }
        Case(BCODESXP) {
            assert(false);
        }
        // TODO : some code (eg. serialize.c:2154) puts closures into asts...
        //        not sure how we want to handle it...
        // Case(CLOSXP) {
        //     assert(false);
        // }

        // Constant
        Else(compileConst(ctx.cs(), exp));
    }
}

std::vector<fun_idx_t> compileFormals(Context& ctx, SEXP formals) {
    std::vector<fun_idx_t> res;

    for (auto arg = RList(formals).begin(); arg != RList::end(); ++arg) {
        if (*arg == R_MissingArg)
            res.push_back(MISSING_ARG_IDX);
        else
            res.push_back(compilePromise(ctx, *arg));
    }

    return res;
}

fun_idx_t compilePromise(Context& ctx, SEXP exp) {
    ctx.push(exp);
    compileExpr(ctx, exp);
    ctx.cs() << BC::ret();
    return ctx.pop();
}
}

Compiler::CompilerRes Compiler::finalize() {
    // Rprintf("****************************************************\n");
    // Rprintf("Compiling function\n");
    FunctionHandle function = FunctionHandle::create();
    Context ctx(function, preserve);

    auto formProm = compileFormals(ctx, formals);

    ctx.push(exp);

    compileExpr(ctx, exp);
    ctx.cs() << BC::ret();
    ctx.pop();

    FunctionHandle opt = Optimizer::optimize(function);
    // opt.print();
    CodeVerifier::vefifyFunctionLayout(opt.store, globalContext());

    // Protect p;
    // SEXP formout = R_NilValue;
    // SEXP f = formout;
    // SEXP formin = formals;
    // for (auto prom : formProm) {
    //     SEXP arg = (prom == MISSING_ARG_IDX) ? 
    //         R_MissingArg : (SEXP)opt.codeAtOffset(prom);
    //     SEXP next = CONS_NR(arg, R_NilValue);
    //     SET_TAG(next, TAG(formin));
    //     formin = CDR(formin);
    //     if (formout == R_NilValue) {
    //         formout = f = next;
    //         p(formout);
    //     } else {
    //         SETCDR(f, next);
    //         f = next;
    //     }
    // }

    // TODO compiling the formals is broken, since the optimizer drops the
    // formals code from the function object since they are not referenced!
    // 
    return {opt.store, formals /* formout */ };
}
}
