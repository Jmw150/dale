#include "FunctionProcessor.h"

#include "../Generator/Generator.h"
#include "../ContextSavePoint/ContextSavePoint.h"
#include "../Form/Proc/Inst/Inst.h"
#include "../Operation/Coerce/Coerce.h"
#include "../Operation/Cast/Cast.h"
#include "../Operation/Destruct/Destruct.h"
#include "../Operation/Copy/Copy.h"

#define IMPLICIT 1

namespace dale
{
FunctionProcessor::FunctionProcessor(Generator *gen)
{
    this->gen = gen;
}

FunctionProcessor::~FunctionProcessor()
{
}

bool FunctionProcessor::parseFuncallInternal(
    Function *dfn,
    Node *n,
    bool getAddress,
    ParseResult *fn_ptr,
    int skip,
    std::vector<llvm::Value*> *extra_call_args,
    ParseResult *pr
) {
    assert(n->list && "must receive a list!");

    llvm::BasicBlock *block = fn_ptr->block;
    llvm::Value *fn = fn_ptr->value;
    symlist *lst = n->list;

    std::vector<Node *>::iterator symlist_iter;
    symlist_iter = lst->begin();
    int count = lst->size() - skip;
    if (extra_call_args) {
        count += extra_call_args->size();
    }

    int num_required_args =
        fn_ptr->type->points_to->numberOfRequiredArgs();

    if (fn_ptr->type->points_to->isVarArgs()) {
        if (count < num_required_args) {
            char buf1[100];
            char buf2[100];
            sprintf(buf1, "%d", num_required_args);
            sprintf(buf2, "%d", count);

            Error *e = new Error(
                ErrorInst::Generator::IncorrectMinimumNumberOfArgs,
                n,
                "function pointer call", buf1, buf2
            );
            gen->units->top()->ctx->er->addError(e);
            return false;
        }
    } else {
        if (count != num_required_args) {
            char buf1[100];
            char buf2[100];
            sprintf(buf1, "%d", num_required_args);
            sprintf(buf2, "%d", count);

            Error *e = new Error(
                ErrorInst::Generator::IncorrectNumberOfArgs,
                n,
                "function pointer call", buf1, buf2
            );
            gen->units->top()->ctx->er->addError(e);
            return false;
        }
    }

    std::vector<llvm::Value *> call_args;
    std::vector<ParseResult> call_arg_prs;
    std::vector<Node *> call_arg_nodes;
    if (extra_call_args) {
        for (std::vector<llvm::Value*>::iterator b =
                    extra_call_args->begin(), e = extra_call_args->end(); b !=
                e; ++b) {
            call_args.push_back((*b));
        }
    }
    std::vector<Type *>::iterator param_iter;

    while (skip--) {
        ++symlist_iter;
    }

    param_iter = fn_ptr->type->points_to->parameter_types.begin();
    bool args_cast = false;
    int arg_count = 1;
    int size = 0;
    if (extra_call_args) {
       size = (int) extra_call_args->size();
    }
    while (size--) {
        ++param_iter;
    }
    while (symlist_iter != lst->end()) {
        ParseResult p;
        bool res = FormProcInstParse(gen, 
            dfn, block, (*symlist_iter), getAddress, false, NULL, &p,
            true
        );
        if (!res) {
            return false;
        }

        call_arg_prs.push_back(p);
        call_arg_nodes.push_back(*symlist_iter);
        block = p.block;

        if ((param_iter != fn_ptr->type->points_to->parameter_types.end())
                && (!(p.type->isEqualTo((*param_iter), 1)))
                && ((*param_iter)->base_type != BaseType::VarArgs)) {
            ParseResult coerce;
            bool coerce_result = Operation::Coerce(gen->units->top()->ctx, block,
                                               p.getValue(gen->units->top()->ctx),
                                               p.type,
                                               (*param_iter),
                                               &coerce);
            llvm::Value *new_val = coerce.value;

            if (!coerce_result) {
                std::string twant;
                std::string tgot;
                (*param_iter)->toString(&twant);
                p.type->toString(&tgot);
                char buf[100];
                sprintf(buf, "%d", arg_count);

                Error *e = new Error(
                    ErrorInst::Generator::IncorrectArgType,
                    (*symlist_iter),
                    "function pointer call",
                    twant.c_str(), buf, tgot.c_str()
                );
                gen->units->top()->ctx->er->addError(e);
                return false;
            } else {
                args_cast = true;
                call_args.push_back(new_val);
            }
        } else {
            call_args.push_back(p.getValue(gen->units->top()->ctx));
        }

        ++symlist_iter;

        if (param_iter != fn_ptr->type->points_to->parameter_types.end()) {
            ++param_iter;
            // Skip the varargs type.
            if (param_iter !=
                    fn_ptr->type->points_to->parameter_types.end()) {
                if ((*param_iter)->base_type == BaseType::VarArgs) {
                    ++param_iter;
                }
            }
        }
    }

    llvm::IRBuilder<> builder(block);

    /* Iterate over the types of the found function. For the reference
     * types, replace the call argument with its address. todo: same
     * code as in parseFunctionCall, move into a separate function. */

    std::vector<llvm::Value *> call_args_final = call_args;
    int caps = call_arg_prs.size();
    int pts  = fn_ptr->type->points_to->parameter_types.size();
    int limit = (caps > pts ? pts : caps);
    ParseResult refpr;
    int start = (extra_call_args ? extra_call_args->size() : 0);
    for (int i = start; i < limit; i++) {
        Type *pt = 
            fn_ptr->type->points_to->parameter_types.at(i);
        ParseResult *arg_refpr = &(call_arg_prs.at(i));
        if (pt->is_reference) {
            if (!pt->is_const && !arg_refpr->value_is_lvalue) {
                Error *e = new Error(
                    ErrorInst::Generator::CannotTakeAddressOfNonLvalue,
                    call_arg_nodes.at(i)
                );
                gen->units->top()->ctx->er->addError(e);
                return false;
            }
            bool res = arg_refpr->getAddressOfValue(gen->units->top()->ctx, &refpr);
            if (!res) {
                return false;
            }
            call_args_final[i] = refpr.getValue(gen->units->top()->ctx);
        } else {
            /* If arguments had to be cast, then skip the copies,
             * here. (todo: do the casting after this part, instead.)
             * */
            if (!args_cast) {
                bool res = Operation::Copy(gen->units->top()->ctx, dfn, arg_refpr, arg_refpr);
                if (!res) {
                    return false;
                }
                call_args_final[i] = arg_refpr->getValue(gen->units->top()->ctx);
            }
        }
    }

    processRetval(fn_ptr->type->points_to->return_type,
                  block, pr, &call_args_final);

    llvm::Value *call_res =
        builder.CreateCall(fn, llvm::ArrayRef<llvm::Value*>(call_args_final));

    pr->set(block, fn_ptr->type->points_to->return_type, call_res);

    fn_ptr->block = pr->block;
    ParseResult temp;
    bool res = Operation::Destruct(gen->units->top()->ctx, fn_ptr, &temp);
    if (!res) {
        return false;
    }
    pr->block = temp.block;

    return true;
}

bool FunctionProcessor::parseFunctionCall(Function *dfn,
        llvm::BasicBlock *block,
        Node *n,
        const char *name,
        bool getAddress,
        bool prefixed_with_core,
        Function **macro_to_call,
        ParseResult *pr)
{
    assert(n->list && "must receive a list!");

    if (getAddress) {
        Error *e = new Error(
            ErrorInst::Generator::CannotTakeAddressOfNonLvalue,
            n
        );
        gen->units->top()->ctx->er->addError(e);
        return false;
    }

    symlist *lst = n->list;

    Node *nfn_name = (*lst)[0];

    if (!nfn_name->is_token) {
        Error *e = new Error(
            ErrorInst::Generator::FirstListElementMustBeAtom,
            nfn_name
        );
        gen->units->top()->ctx->er->addError(e);
        return false;
    }

    Token *t = nfn_name->token;

    if (t->type != TokenType::String) {
        Error *e = new Error(
            ErrorInst::Generator::FirstListElementMustBeSymbol,
            nfn_name
        );
        gen->units->top()->ctx->er->addError(e);
        return false;
    }

    /* Put all of the arguments into a list. */

    std::vector<Node *>::iterator symlist_iter;

    std::vector<llvm::Value *> call_args;
    std::vector<Node *> call_arg_nodes;
    std::vector<ParseResult> call_arg_prs;
    std::vector<Type *> call_arg_types;

    std::vector<llvm::Value *> call_args_newer;
    std::vector<Type *> call_arg_types_newer;

    if (!strcmp(name, "setf")) {
        /* Add a bool argument and type to the front of the
         * function call. */
        call_arg_types.push_back(gen->units->top()->ctx->tr->type_bool);
        call_args.push_back(gen->units->top()->ctx->nt->getLLVMFalse());
    }

    symlist_iter = lst->begin();
    /* Skip the function name. */
    ++symlist_iter;

    /* The processing below is only required when the function/macro
     * name is overloaded. For now, short-circuit for macros that are
     * not overloaded, because that will give the greatest benefits.
     * */

    if (!gen->units->top()->ctx->isOverloadedFunction(t->str_value.c_str())) {
        std::map<std::string, std::vector<Function *> *>::iterator
            iter;
        Function *fn = NULL;
        for (std::vector<NSNode *>::reverse_iterator
                rb = gen->units->top()->ctx->used_ns_nodes.rbegin(),
                re = gen->units->top()->ctx->used_ns_nodes.rend();
                rb != re;
                ++rb) {
            iter = (*rb)->ns->functions.find(name);
            if (iter != (*rb)->ns->functions.end()) {
                fn = iter->second->at(0);
                break;
            }
        }
        if (fn && fn->is_macro) {
            /* If the third argument is either non-existent, or a (p
             * DNode) (because typed arguments must appear before the
             * first (p DNode) argument), then short-circuit, so long
             * as the argument count is ok. */
            std::vector<Variable*>::iterator
                b = (fn->parameter_types.begin() + 1);
            if ((b == fn->parameter_types.end())
                    || (*b)->type->isEqualTo(gen->units->top()->ctx->tr->type_pdnode)) {
                bool use = false;
                int size = lst->size();
                if (fn->isVarArgs()) {
                    use = ((fn->numberOfRequiredArgs() - 1)
                            <= (size - 1));
                } else {
                    use = ((fn->numberOfRequiredArgs() - 1)
                            == (size - 1));
                }
                if (use) {
                    *macro_to_call = fn;
                    return false;
                }
            }
        }
    }

    std::vector<Error*> errors;

    /* Record the number of blocks and the instruction index in the
     * current block. If the underlying Function to call
     * is a function, then there's no problem with using the
     * modifications caused by the repeated PFBI calls below. If
     * it's a macro, however, anything that occurred needs to be
     * 'rolled back'. Have to do the same thing for the context. */

    int current_block_count = dfn->llvm_function->size();
    int current_instr_index = block->size();
    int current_dgcount = dfn->deferred_gotos.size();
    std::map<std::string, Label *> labels = dfn->labels;
    llvm::BasicBlock *original_block = block;
    ContextSavePoint *csp = new ContextSavePoint(gen->units->top()->ctx);

    while (symlist_iter != lst->end()) {
        call_arg_nodes.push_back(*symlist_iter);
        int error_count =
            gen->units->top()->ctx->er->getErrorTypeCount(ErrorType::Error);

        ParseResult p;
        bool res = 
            FormProcInstParse(gen, dfn, block, (*symlist_iter),
                                    false, false, NULL,
                                    &p, true);

        int diff = gen->units->top()->ctx->er->getErrorTypeCount(ErrorType::Error)
                   - error_count;

        if (!res || diff) {
            /* May be a macro call (could be an unparseable
             * argument). Pop and store errors for the time being
             * and treat this argument as a (p DNode). */

            if (diff) {
                errors.insert(errors.end(),
                              gen->units->top()->ctx->er->errors.begin() + error_count,
                              gen->units->top()->ctx->er->errors.end());
                gen->units->top()->ctx->er->errors.erase(gen->units->top()->ctx->er->errors.begin() + error_count,
                                    gen->units->top()->ctx->er->errors.end());
            }

            call_args.push_back(NULL);
            call_arg_types.push_back(gen->units->top()->ctx->tr->type_pdnode);
            ++symlist_iter;
            continue;
        }

        block = p.block;
        if (p.type->is_array) {
            p = ParseResult(block, p.type_of_address_of_value,
                            p.address_of_value);
        }
        call_args.push_back(p.getValue(gen->units->top()->ctx));
        call_arg_types.push_back(p.type);
        call_arg_prs.push_back(p);

        ++symlist_iter;
    }

    /* Now have all the argument types. Get the function out of
     * the context. */

    Function *closest_fn = NULL;

    Function *fn =
        gen->units->top()->ctx->getFunction(t->str_value.c_str(),
                         &call_arg_types,
                         &closest_fn,
                         0);

    /* If the function is a macro, set macro_to_call and return
     * NULL. (It's the caller's responsibility to handle
     * processing of macros.) */

    if (fn && fn->is_macro) {
        /* Remove any basic blocks that have been added by way of
         * the parsing of the macro arguments, and remove any
         * extra instructions added to the current block. Restore
         * the context save point. */

        int block_pop_back =
            dfn->llvm_function->size() - current_block_count;
        while (block_pop_back--) {
            llvm::Function::iterator
            bi = dfn->llvm_function->begin(),
            be = dfn->llvm_function->end(),
            bl;

            while (bi != be) {
                bl = bi;
                ++bi;
            }
            bl->eraseFromParent();
        }

        int to_pop_back = original_block->size() - current_instr_index;
        while (to_pop_back--) {
            llvm::BasicBlock::iterator
            bi = original_block->begin(),
            be = original_block->end(), bl;

            while (bi != be) {
                bl = bi;
                ++bi;
            }
            bl->eraseFromParent();
        }

        int dg_to_pop_back = dfn->deferred_gotos.size() - current_dgcount;
        while (dg_to_pop_back--) {
            dfn->deferred_gotos.pop_back();
        }
        dfn->labels = labels;

        csp->restore();
        delete csp;

        *macro_to_call = fn;
        return false;
    }
    delete csp;

    /* If the function is not a macro, and errors were encountered
     * during argument processing, then this function has been
     * loaded in error (it will be a normal function taking a (p
     * DNode) argument, but the argument is not a real (p DNode)
     * value). Replace all the errors and return NULL. */

    if (errors.size() && fn && !fn->is_macro) {
        for (std::vector<Error*>::reverse_iterator b = errors.rbegin(),
                e = errors.rend();
                b != e;
                ++b) {
            gen->units->top()->ctx->er->addError(*b);
        }
        return false;
    }

    bool args_cast = false;

    if (!fn) {
        /* If no function was found, and there are errors related
         * to argument parsing, then push those errors back onto
         * the reporter and return. (May change this later to be a
         * bit more friendly - probably if there are any macros or
         * functions with the same name, this should show the
         * overload failure, rather than the parsing failure
         * errors). */
        if (errors.size()) {
            for (std::vector<Error*>::reverse_iterator b = errors.rbegin(),
                    e = errors.rend();
                    b != e;
                    ++b) {
                gen->units->top()->ctx->er->addError(*b);
            }
            return false;
        }

        if (gen->units->top()->ctx->existsExternCFunction(t->str_value.c_str())) {
            /* The function name is not overloaded. */
            /* Get this single function, try to cast each integral
             * call_arg to the expected type. If that succeeds
             * without error, then keep going. */

            fn = gen->units->top()->ctx->getFunction(t->str_value.c_str(),
                                  NULL, NULL, 0);

            std::vector<Variable *> myarg_types =
                fn->parameter_types;
            std::vector<Variable *>::iterator miter =
                myarg_types.begin();

            std::vector<llvm::Value *>::iterator citer =
                call_args.begin();
            std::vector<Type *>::iterator caiter =
                call_arg_types.begin();

            /* Create strings describing the types, for use in a
             * possible error message. */

            std::string expected_args;
            std::string provided_args;
            while (miter != myarg_types.end()) {
                (*miter)->type->toString(&expected_args);
                expected_args.append(" ");
                ++miter;
            }
            if (expected_args.size() == 0) {
                expected_args.append("void");
            } else {
                expected_args.erase(expected_args.size() - 1, 1);
            }
            while (caiter != call_arg_types.end()) {
                (*caiter)->toString(&provided_args);
                provided_args.append(" ");
                ++caiter;
            }
            if (provided_args.size() == 0) {
                provided_args.append("void");
            } else {
                provided_args.erase(provided_args.size() - 1, 1);
            }
            miter = myarg_types.begin();
            caiter = call_arg_types.begin();
            int size = call_args.size();

            if (size < fn->numberOfRequiredArgs()) {
                Error *e = new Error(
                    ErrorInst::Generator::FunctionNotInScope,
                    n,
                    t->str_value.c_str(),
                    provided_args.c_str(),
                    expected_args.c_str()
                );
                gen->units->top()->ctx->er->addError(e);
                return false;
            }
            if (!fn->isVarArgs()
                    && size != fn->numberOfRequiredArgs()) {
                Error *e = new Error(
                    ErrorInst::Generator::FunctionNotInScope,
                    n,
                    t->str_value.c_str(),
                    provided_args.c_str(),
                    expected_args.c_str()
                );
                gen->units->top()->ctx->er->addError(e);
                return false;
            }

            while (miter != myarg_types.end()
                    && citer != call_args.end()
                    && caiter != call_arg_types.end()) {
                if ((*caiter)->isEqualTo((*miter)->type, 1)) {
                    call_args_newer.push_back((*citer));
                    call_arg_types_newer.push_back((*caiter));
                    ++miter;
                    ++citer;
                    ++caiter;
                    continue;
                }
                if (!(*miter)->type->isIntegerType()
                        and (*miter)->type->base_type != BaseType::Bool) {
                    Error *e = new Error(
                        ErrorInst::Generator::FunctionNotInScope,
                        n,
                        t->str_value.c_str(),
                        provided_args.c_str(),
                        expected_args.c_str()
                    );
                    gen->units->top()->ctx->er->addError(e);
                    return false;
                }
                if (!(*caiter)->isIntegerType()
                        and (*caiter)->base_type != BaseType::Bool) {
                    Error *e = new Error(
                        ErrorInst::Generator::FunctionNotInScope,
                        n,
                        t->str_value.c_str(),
                        provided_args.c_str(),
                        expected_args.c_str()
                    );
                    gen->units->top()->ctx->er->addError(e);
                    return false;
                }

                ParseResult mytemp;
                bool res = Operation::Cast(gen->units->top()->ctx, block,
                           (*citer),
                           (*caiter),
                           (*miter)->type,
                           n,
                           IMPLICIT,
                           &mytemp);
                if (!res) {
                    Error *e = new Error(
                        ErrorInst::Generator::FunctionNotInScope,
                        n,
                        t->str_value.c_str(),
                        provided_args.c_str(),
                        expected_args.c_str()
                    );
                    gen->units->top()->ctx->er->addError(e);
                    return false;
                }
                block = mytemp.block;
                call_args_newer.push_back(mytemp.getValue(gen->units->top()->ctx));
                call_arg_types_newer.push_back(mytemp.type);

                ++miter;
                ++citer;
                ++caiter;
            }

            call_args = call_args_newer;
            call_arg_types = call_arg_types_newer;
            args_cast = true;
        } else if (gen->units->top()->ctx->existsNonExternCFunction(t->str_value.c_str())) {
            /* Return a no-op ParseResult if the function name is
             * 'destroy', because it's tedious to have to check in
             * generic code whether a particular value can be
             * destroyed or not. */
            if (!t->str_value.compare("destroy")) {
                pr->set(block, gen->units->top()->ctx->tr->type_void, NULL);
                return true;
            }

            std::vector<Type *>::iterator titer =
                call_arg_types.begin();

            std::string args;
            while (titer != call_arg_types.end()) {
                (*titer)->toString(&args);
                ++titer;
                if (titer != call_arg_types.end()) {
                    args.append(" ");
                }
            }

            if (closest_fn) {
                std::string expected;
                std::vector<Variable *>::iterator viter;
                viter = closest_fn->parameter_types.begin();
                if (closest_fn->is_macro) {
                    ++viter;
                }
                while (viter != closest_fn->parameter_types.end()) {
                    (*viter)->type->toString(&expected);
                    expected.append(" ");
                    ++viter;
                }
                if (expected.size() > 0) {
                    expected.erase(expected.size() - 1, 1);
                }
                Error *e = new Error(
                    ErrorInst::Generator::OverloadedFunctionOrMacroNotInScopeWithClosest,
                    n,
                    t->str_value.c_str(), args.c_str(),
                    expected.c_str()
                );
                gen->units->top()->ctx->er->addError(e);
                return false;
            } else {
                Error *e = new Error(
                    ErrorInst::Generator::OverloadedFunctionOrMacroNotInScope,
                    n,
                    t->str_value.c_str(), args.c_str()
                );
                gen->units->top()->ctx->er->addError(e);
                return false;
            }
        } else {
            Error *e = new Error(
                ErrorInst::Generator::NotInScope,
                n,
                t->str_value.c_str()
            );
            gen->units->top()->ctx->er->addError(e);
            return false;
        }
    }

    llvm::IRBuilder<> builder(block);

    /* If this function is varargs, find the point at which the
     * varargs begin, and then promote any call_args floats to
     * doubles, and any integer types smaller than the native
     * integer size to native integer size. */

    if (fn->isVarArgs()) {
        args_cast = true;
        int n = fn->numberOfRequiredArgs();

        std::vector<llvm::Value *>::iterator call_args_iter
        = call_args.begin();
        std::vector<Type *>::iterator call_arg_types_iter
        = call_arg_types.begin();

        while (n--) {
            ++call_args_iter;
            ++call_arg_types_iter;
        }
        while (call_args_iter != call_args.end()) {
            if ((*call_arg_types_iter)->base_type == BaseType::Float) {
                (*call_args_iter) =
                    builder.CreateFPExt(
                        (*call_args_iter),
                        llvm::Type::getDoubleTy(llvm::getGlobalContext())
                    );
                (*call_arg_types_iter) =
                    gen->units->top()->ctx->tr->type_double;
            } else if ((*call_arg_types_iter)->isIntegerType()) {
                int real_size =
                    gen->units->top()->ctx->nt->internalSizeToRealSize(
                        (*call_arg_types_iter)->getIntegerSize()
                    );

                if (real_size < gen->units->top()->ctx->nt->getNativeIntSize()) {
                    if ((*call_arg_types_iter)->isSignedIntegerType()) {
                        /* Target integer is signed - use sext. */
                        (*call_args_iter) =
                            builder.CreateSExt((*call_args_iter),
                                               gen->units->top()->ctx->toLLVMType(
                                                    gen->units->top()->ctx->tr->type_int,
                                                              NULL, false));
                        (*call_arg_types_iter) = gen->units->top()->ctx->tr->type_int;
                    } else {
                        /* Target integer is not signed - use zext. */
                        (*call_args_iter) =
                            builder.CreateZExt((*call_args_iter),
                                               gen->units->top()->ctx->toLLVMType(
                                                    gen->units->top()->ctx->tr->type_uint,
                                                              NULL, false));
                        (*call_arg_types_iter) = gen->units->top()->ctx->tr->type_uint;
                    }
                }
            }
            ++call_args_iter;
            ++call_arg_types_iter;
        }
    }

    /* Iterate over the types of the found function. For the reference
     * types, replace the call argument with its address. */
    
    std::vector<llvm::Value *> call_args_final = call_args;
    int caps = call_arg_prs.size();
    int pts  = fn->parameter_types.size();
    int limit = (caps > pts ? pts : caps);
    ParseResult refpr;
    for (int i = 0; i < limit; i++) {
        Type *pt = fn->parameter_types.at(i)->type;
        ParseResult *arg_refpr = &(call_arg_prs.at(i));
        if (pt->is_reference) {
            if (!pt->is_const && !arg_refpr->value_is_lvalue) {
                Error *e = new Error(
                    ErrorInst::Generator::CannotTakeAddressOfNonLvalue,
                    call_arg_nodes.at(i)
                );
                gen->units->top()->ctx->er->addError(e);
                return false;
            }
            bool res = arg_refpr->getAddressOfValue(gen->units->top()->ctx, &refpr);
            if (!res) {
                return false;
            }
            call_args_final[i] = refpr.getValue(gen->units->top()->ctx);
        } else {
            /* If arguments had to be cast, then skip the copies,
             * here. (todo: do the casting after this part, instead.)
             * */
            if (!args_cast) {
                bool res = Operation::Copy(gen->units->top()->ctx, dfn, arg_refpr, arg_refpr);
                if (!res) {
                    return false;
                }
                call_args_final[i] = arg_refpr->getValue(gen->units->top()->ctx);
            }
        }
    }
   
    processRetval(fn->return_type, block, pr, &call_args_final);

    llvm::Value *call_res = builder.CreateCall(
                                fn->llvm_function,
                                llvm::ArrayRef<llvm::Value*>(call_args_final));

    pr->set(block, fn->return_type, call_res);

    /* If the return type of the function is one that should be
     * copied with an overridden setf, that will occur in the
     * function, so prevent the value from being re-copied here
     * (because no corresponding destructor call will occur). */

    pr->do_not_copy_with_setf = 1;

    return true;
}

void FunctionProcessor::processRetval(Type *return_type,
                              llvm::BasicBlock *block,
                              ParseResult *pr,
                              std::vector<llvm::Value*> *call_args)
{
    if (return_type->is_retval) {
        pr->do_not_destruct = 1;
        pr->do_not_copy_with_setf = 1;
        /* todo: may turn out to be unnecessary. */
        pr->retval_used = true;
        if (!pr->retval) {
            llvm::IRBuilder<> builder(block);
            llvm::Type *et =
                gen->units->top()->ctx->toLLVMType(return_type, NULL, false,
                                false);
            if (!et) {
                return;
            }
            llvm::Value *new_ptr =
                llvm::cast<llvm::Value>(
                    builder.CreateAlloca(et)
                );
            call_args->push_back(new_ptr);
            pr->retval = new_ptr;
            pr->retval_type = gen->units->top()->ctx->tr->getPointerType(return_type);
        } else {
            call_args->push_back(pr->retval);
        }
    }

    return;
}
}