/*
Copyright (c) 2016 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#pragma once
#include <memory>
#include <vector>
#include "util/debug.h"
#include "util/rc.h"
#include "util/serializer.h"
#include "util/numerics/mpz.h"
#include "kernel/environment.h"

namespace lean {
class vm_obj;
enum class vm_obj_kind { Simple, Constructor, Closure, MPZ, External };

/** \brief Base class for VM objects.

    \remark Simple objects are encoded as tagged pointers. */
class vm_obj_cell {
protected:
    friend class vm_obj;
    unsigned    m_rc;
    vm_obj_kind m_kind;
    void inc_ref() { m_rc++; }
    bool dec_ref_core() {
        lean_assert(m_rc > 0);
        m_rc--;
        return m_rc == 0;
    }
    void dec_ref() { if (dec_ref_core()) { dealloc(); } }
    void dealloc();
    void dec_ref(vm_obj & o, buffer<vm_obj_cell*> & todelete);
    vm_obj_cell(vm_obj_kind k):m_rc(0), m_kind(k) {}
public:
    vm_obj_kind kind() const { return m_kind; }
    unsigned get_rc() const { return m_rc; }
};

#define LEAN_VM_IS_PTR(obj) ((reinterpret_cast<size_t>(obj) & 1) == 0)
#define LEAN_VM_BOX(num)    (reinterpret_cast<vm_obj_cell*>((num << 1) | 1))
#define LEAN_VM_UNBOX(obj)  (reinterpret_cast<size_t>(obj) >> 1)

void display(std::ostream & out, vm_obj const & o, std::function<optional<name>(unsigned)> const & idx2name);
void display(std::ostream & out, vm_obj const & o);

/** \brief VM object */
class vm_obj {
    vm_obj_cell * m_data;
    friend class vm_obj_cell;
    vm_obj_cell * steal_ptr() {
        lean_assert(LEAN_VM_IS_PTR(m_data)); vm_obj_cell * r = m_data; m_data = LEAN_VM_BOX(0); return r;
    }
public:
    vm_obj():m_data(LEAN_VM_BOX(0)) {
        static_assert(sizeof(vm_obj) == sizeof(void *), "unexpected class obj size"); // NOLINT
    }
    explicit vm_obj(unsigned cidx):m_data(LEAN_VM_BOX(cidx)) {}
    explicit vm_obj(vm_obj_cell * c):m_data(c) { m_data->inc_ref(); lean_assert(LEAN_VM_IS_PTR(m_data)); }
    vm_obj(vm_obj const & o):m_data(o.m_data) { if (LEAN_VM_IS_PTR(m_data)) m_data->inc_ref(); }
    vm_obj(vm_obj && o):m_data(o.m_data) { o.m_data = LEAN_VM_BOX(0); }
    ~vm_obj() { if (LEAN_VM_IS_PTR(m_data)) m_data->dec_ref(); }

    friend void swap(vm_obj & a, vm_obj & b) { std::swap(a.m_data, b.m_data); }

    operator vm_obj_cell*() const { return m_data; }

    vm_obj & operator=(vm_obj const & s) {
        if (LEAN_VM_IS_PTR(s.m_data))
            s.m_data->inc_ref();
        vm_obj_cell * new_data = s.m_data;
        if (LEAN_VM_IS_PTR(m_data))
            m_data->dec_ref();
        m_data = new_data;
        return *this;
    }

    vm_obj & operator=(vm_obj && s) {
        if (LEAN_VM_IS_PTR(m_data))
            m_data->dec_ref();
        m_data   = s.m_data;
        s.m_data = LEAN_VM_BOX(0);
        return *this;
    }

    vm_obj_kind kind() const {
        return LEAN_VM_IS_PTR(m_data) ? m_data->kind() : vm_obj_kind::Simple;
    }

    vm_obj_cell * raw() const {
        return m_data;
    }
};

class vm_composite : public vm_obj_cell {
    unsigned   m_idx;
    unsigned   m_size;
    vm_obj * get_field_ptr() {
        return reinterpret_cast<vm_obj *>(reinterpret_cast<char *>(this)+sizeof(vm_composite));
    }
    friend vm_obj_cell;
    void dealloc(buffer<vm_obj_cell*> & todelete);
public:
    vm_composite(vm_obj_kind k, unsigned idx, unsigned sz, vm_obj const * data);
    unsigned size() const { return m_size; }
    unsigned idx() const { return m_idx; }
    vm_obj const * fields() const {
        return reinterpret_cast<vm_obj const *>(reinterpret_cast<char const *>(this)+sizeof(vm_composite));
    }
};

class vm_mpz : public vm_obj_cell {
    mpz      m_value;
    friend vm_obj_cell;
    void dealloc();
public:
    vm_mpz(mpz const & v);
    mpz const & get_value() const { return m_value; }
};

class vm_external : public vm_obj_cell {
public:
    virtual ~vm_external() = 0;
};

// =======================================
// Constructors
vm_obj mk_vm_simple(unsigned cidx);
vm_obj mk_vm_constructor(unsigned cidx, unsigned sz, vm_obj const * args);
vm_obj mk_vm_closure(unsigned fnidx, unsigned sz, vm_obj const * args);
vm_obj mk_vm_mpz(mpz const & n);
vm_obj mk_vm_external(vm_external * cell);
/* helper functions for creating natural numbers */
vm_obj mk_vm_nat(unsigned n);
vm_obj mk_vm_nat(mpz const & n);
inline vm_obj mk_vm_false() { return mk_vm_simple(0); }
inline vm_obj mk_vm_true() { return mk_vm_simple(1); }
inline vm_obj mk_vm_bool(bool b) { return mk_vm_simple(b); }
// =======================================

// =======================================
// Testers
inline vm_obj_kind kind(vm_obj_cell const * o) { return LEAN_VM_IS_PTR(o) ? o->kind() : vm_obj_kind::Simple; }
inline bool is_simple(vm_obj_cell const * o) { return !LEAN_VM_IS_PTR(o); }
inline bool is_constructor(vm_obj_cell const * o) { return kind(o) == vm_obj_kind::Constructor; }
inline bool is_closure(vm_obj_cell const * o) { return kind(o) == vm_obj_kind::Closure; }
inline bool is_composite(vm_obj_cell const * o) { return is_constructor(o) || is_closure(o); }
inline bool is_mpz(vm_obj_cell const * o) { return kind(o) == vm_obj_kind::MPZ; }
inline bool is_external(vm_obj_cell const * o) { return kind(o) == vm_obj_kind::External; }
// =======================================

// =======================================
// Casting
inline vm_composite * to_composite(vm_obj_cell * o) { lean_assert(is_composite(o)); return static_cast<vm_composite*>(o); }
inline vm_mpz * to_mpz_core(vm_obj_cell * o) { lean_assert(is_mpz(o)); return static_cast<vm_mpz*>(o); }
inline mpz const & to_mpz(vm_obj_cell * o) { return to_mpz_core(o)->get_value(); }
inline vm_external * to_external(vm_obj_cell * o) { lean_assert(is_external(o)); return static_cast<vm_external*>(o); }
// =======================================

// =======================================
// Accessors
inline unsigned cidx(vm_obj const & o) {
    lean_assert(is_simple(o) || is_constructor(o));
    return LEAN_VM_IS_PTR(o.raw()) ? to_composite(o.raw())->idx() : static_cast<unsigned>(LEAN_VM_UNBOX(o.raw()));
}
inline unsigned csize(vm_obj const & o) { lean_assert(is_composite(o)); return to_composite(o)->size(); }
inline unsigned cfn_idx(vm_obj const & o) { lean_assert(is_closure(o)); return to_composite(o)->idx(); }
inline vm_obj const * cfields(vm_obj const & o) {
    lean_assert(is_composite(o)); return to_composite(o)->fields();
}
inline vm_obj const & cfield(vm_obj const & o, unsigned i) { lean_assert(i < csize(o)); return cfields(o)[i]; }
// =======================================

#define LEAN_MAX_SMALL_NAT 1u<<31

/** \brief VM instruction opcode */
enum class opcode {
    Push, Ret, Drop, Goto,
    SConstructor, Constructor, Num,
    Destruct, Cases2, CasesN, NatCases, Proj,
    Invoke, InvokeGlobal, InvokeBuiltin, Closure, Unreachable
};

/** \brief VM instructions */
class vm_instr {
    opcode m_op;
    union {
        struct {
            unsigned m_fn_idx;  /* InvokeGlobal, InvokeBuiltin and Closure. */
            unsigned m_nargs;   /* Closure and Invoke */
        };
        /* Push, Proj */
        unsigned m_idx;
        /* Drop */
        unsigned m_num;
        /* Goto, Cases2 and NatCases */
        struct {
            unsigned m_pc[2];
        };
        /* CasesN */
        unsigned * m_npcs;
        /* Constructor, SConstructor */
        struct {
            unsigned m_cidx;
            unsigned m_nfields; /* only used by Constructor */
        };
        /* Num */
        mpz * m_mpz;
    };
    /* Ret, Destruct and Unreachable do not have arguments */
    friend vm_instr mk_push_instr(unsigned idx);
    friend vm_instr mk_drop_instr(unsigned n);
    friend vm_instr mk_proj_instr(unsigned n);
    friend vm_instr mk_goto_instr(unsigned pc);
    friend vm_instr mk_sconstructor_instr(unsigned cidx);
    friend vm_instr mk_constructor_instr(unsigned cidx, unsigned nfields);
    friend vm_instr mk_num_instr(mpz const & v);
    friend vm_instr mk_ret_instr();
    friend vm_instr mk_destruct_instr();
    friend vm_instr mk_unreachable_instr();
    friend vm_instr mk_nat_cases_instr(unsigned pc1, unsigned pc2);
    friend vm_instr mk_cases2_instr(unsigned pc1, unsigned pc2);
    friend vm_instr mk_casesn_instr(unsigned num_pc, unsigned const * pcs);
    friend vm_instr mk_invoke_instr(unsigned n);
    friend vm_instr mk_invoke_global_instr(unsigned fn_idx);
    friend vm_instr mk_invoke_builtin_instr(unsigned fn_idx);
    friend vm_instr mk_closure_instr(unsigned fn_idx, unsigned n);

    void copy_args(vm_instr const & i);
public:
    vm_instr():m_op(opcode::Ret) {}
    vm_instr(opcode op):m_op(op) {}
    vm_instr(vm_instr const & i);
    vm_instr(vm_instr && i);
    ~vm_instr();

    vm_instr & operator=(vm_instr const & s);
    vm_instr & operator=(vm_instr && s);

    opcode op() const { return m_op; }

    unsigned get_fn_idx() const {
        lean_assert(m_op == opcode::InvokeGlobal || m_op == opcode::InvokeBuiltin || m_op == opcode::Closure);
        return m_fn_idx;
    }

    unsigned get_nargs() const {
        lean_assert(m_op == opcode::Closure || m_op == opcode::Invoke);
        return m_nargs;
    }

    unsigned get_idx() const {
        lean_assert(m_op == opcode::Push || m_op == opcode::Proj);
        return m_idx;
    }

    unsigned get_num() const {
        lean_assert(m_op == opcode::Drop);
        return m_num;
    }

    unsigned get_goto_pc() const {
        lean_assert(m_op == opcode::Goto);
        return m_pc[0];
    }

    void set_goto_pc(unsigned pc) {
        lean_assert(m_op == opcode::Goto);
        m_pc[0] = pc;
    }

    unsigned get_cases2_pc(unsigned i) const {
        lean_assert(m_op == opcode::Cases2 || m_op == opcode::NatCases);
        lean_assert(i < 2);
        return m_pc[i];
    }

    void set_cases2_pc(unsigned i, unsigned pc) {
        lean_assert(m_op == opcode::Cases2 || m_op == opcode::NatCases);
        lean_assert(i < 2);
        m_pc[i] = pc;
    }

    unsigned get_casesn_size() const {
        lean_assert(m_op == opcode::CasesN);
        return m_npcs[0];
    }

    unsigned get_casesn_pc(unsigned i) const {
        lean_assert(m_op == opcode::CasesN);
        lean_assert(i < get_casesn_size());
        return m_npcs[i+1];
    }

    void set_casesn_pc(unsigned i, unsigned pc) const {
        lean_assert(m_op == opcode::CasesN);
        lean_assert(i < get_casesn_size());
        m_npcs[i+1] = pc;
    }

    unsigned get_cidx() const {
        lean_assert(m_op == opcode::Constructor || m_op == opcode::SConstructor);
        return m_cidx;
    }

    unsigned get_nfields() const {
        lean_assert(m_op == opcode::Constructor);
        return m_nfields;
    }

    mpz const & get_mpz() const {
        lean_assert(m_op == opcode::Num);
        return *m_mpz;
    }

    unsigned get_num_pcs() const;
    unsigned get_pc(unsigned i) const;
    void set_pc(unsigned i, unsigned pc);

    void display(std::ostream & out, std::function<optional<name>(unsigned)> const & idx2name) const;
    void display(std::ostream & out) const;

    void serialize(serializer & s, std::function<name(unsigned)> const & idx2name) const;
};

vm_instr mk_push_instr(unsigned idx);
vm_instr mk_drop_instr(unsigned n);
vm_instr mk_proj_instr(unsigned n);
vm_instr mk_goto_instr(unsigned pc);
vm_instr mk_sconstructor_instr(unsigned cidx);
vm_instr mk_constructor_instr(unsigned cidx, unsigned nfields);
vm_instr mk_num_instr(mpz const & v);
vm_instr mk_ret_instr();
vm_instr mk_destruct_instr();
vm_instr mk_unreachable_instr();
vm_instr mk_nat_cases_instr(unsigned pc1, unsigned pc2);
vm_instr mk_cases2_instr(unsigned pc1, unsigned pc2);
vm_instr mk_casesn_instr(unsigned num_pc, unsigned const * pcs);
vm_instr mk_invoke_instr(unsigned n);
vm_instr mk_invoke_global_instr(unsigned fn_idx);
vm_instr mk_invoke_builtin_instr(unsigned fn_idx);
vm_instr mk_closure_instr(unsigned fn_idx, unsigned n);

class vm_state;
class vm_instr;

typedef void (*vm_function)(vm_state & s);

/** \brief VM function/constant declaration cell */
struct vm_decl_cell {
    MK_LEAN_RC();
    bool       m_builtin;
    name       m_name;
    unsigned   m_idx;
    expr       m_expr;
    unsigned   m_arity;
    union {
        struct {
            unsigned   m_code_size;
            vm_instr * m_code;
        };
        vm_function    m_fn;
    };

    vm_decl_cell(name const & n, unsigned idx, unsigned arity, vm_function fn);
    vm_decl_cell(name const & n, unsigned idx, expr const & e, unsigned code_sz, vm_instr const * code);
    ~vm_decl_cell();
    void dealloc();
};

/** \brief VM function/constant declaration smart pointer. */
class vm_decl {
    vm_decl_cell * m_ptr;
    explicit vm_decl(vm_decl_cell * ptr):m_ptr(ptr) { if (m_ptr) m_ptr->inc_ref(); }
public:
    vm_decl():m_ptr(nullptr) {}
    vm_decl(name const & n, unsigned idx, unsigned arity, vm_function fn):
        vm_decl(new vm_decl_cell(n, idx, arity, fn)) {}
    vm_decl(name const & n, unsigned idx, expr const & e, unsigned code_sz, vm_instr const * code):
        vm_decl(new vm_decl_cell(n, idx, e, code_sz, code)) {}
    vm_decl(vm_decl const & s):m_ptr(s.m_ptr) { if (m_ptr) m_ptr->inc_ref(); }
    vm_decl(vm_decl && s):m_ptr(s.m_ptr) { s.m_ptr = nullptr; }
    ~vm_decl() { if (m_ptr) m_ptr->dec_ref(); }

    friend void swap(vm_decl & a, vm_decl & b) { std::swap(a.m_ptr, b.m_ptr); }

    vm_decl & operator=(vm_decl const & s) { LEAN_COPY_REF(s); }
    vm_decl & operator=(vm_decl && s) { LEAN_MOVE_REF(s); }

    bool is_builtin() const { lean_assert(m_ptr); return m_ptr->m_builtin; }
    unsigned get_idx() const { lean_assert(m_ptr); return m_ptr->m_idx; }
    name get_name() const { lean_assert(m_ptr); return m_ptr->m_name; }
    unsigned get_arity() const { lean_assert(m_ptr); return m_ptr->m_arity; }
    unsigned get_code_size() const { lean_assert(!is_builtin()); return m_ptr->m_code_size; }
    vm_instr const * get_code() const { lean_assert(!is_builtin()); return m_ptr->m_code; }
    vm_function get_fn() const { lean_assert(is_builtin()); return m_ptr->m_fn; }
    expr const & get_expr() const { lean_assert(!is_builtin()); return m_ptr->m_expr; }
};

/** \brief Virtual machine for executing VM bytecode. */
class vm_state {
    typedef std::vector<vm_decl> decls;
    environment          m_env;
    decls const &        m_decls;
    name_map<unsigned>   m_fn_name2idx;
    vm_instr const *     m_code;   /* code of the current function being executed */
    unsigned             m_fn_idx; /* function idx being executed */
    unsigned             m_pc;     /* program counter */
    unsigned             m_bp;     /* base pointer */
    struct frame {
        vm_instr const * m_code;
        unsigned         m_fn_idx;
        unsigned         m_num;
        unsigned         m_pc;
        unsigned         m_bp;
        frame(vm_instr const * code, unsigned fn_idx, unsigned num, unsigned pc, unsigned bp):
            m_code(code), m_fn_idx(fn_idx), m_num(num), m_pc(pc), m_bp(bp) {}
    };
    std::vector<vm_obj>  m_stack;
    std::vector<frame>   m_call_stack;

    void push_fields(vm_obj const & obj);
    void invoke_builtin(vm_decl const & d);
    void invoke_global(vm_decl const & d);
    void run();

public:
    vm_state(environment const & env);

    environment const & env() const { return m_env; }

    /** \brief Push object into the data stack */
    void push(vm_obj const & o) { m_stack.push_back(o); }

    /** \brief Retrieve object from the call stack */
    vm_obj const & get(int idx) const {
        lean_assert(idx + static_cast<int>(m_bp) >= 0);
        lean_assert(m_bp + idx < m_stack.size());
        return m_stack[m_bp + idx];
    }

    void invoke_fn(name const & fn);
    void invoke_fn(unsigned fn_idx);

    void display_stack(std::ostream & out) const;
    void display(std::ostream & out, vm_obj const & o) const;
};

/** \brief Add builtin implementation for the function named \c n. */
void declare_vm_builtin(name const & n, unsigned arity, vm_function fn);

/** \brief Reserve an index for the given function in the VM, the expression
    \c e is the value of \c fn after preprocessing.
    See library/compiler/pre_proprocess_rec.cpp for details. */
environment reserve_vm_index(environment const & env, name const & fn, expr const & e);

/** \brief Add bytcode for the function named \c fn in \c env.
    \remark The index for \c fn must have been reserved using reserve_vm_index. */
environment update_vm_code(environment const & env, name const & fn, unsigned code_sz, vm_instr const * code);

/** \brief Combines reserve_vm_index and update_vm_code */
environment add_vm_code(environment const & env, name const & fn, expr const & e, unsigned code_sz, vm_instr const * code);

/** \brief Return the internal idx for the given constant. Return none
    if the constant is not builtin nor it has code associated with it. */
optional<unsigned> get_vm_constant_idx(environment const & env, name const & n);

/** \brief Make sure vm_decls structure is optimized. */
environment optimize_vm_decls(environment const & env);

/** \brief Return true iff \c fn is a VM function in the given environment. */
bool is_vm_function(environment const & env, name const & fn);

/** \brief Return true iff \c fn is implemented in C++. */
bool is_vm_builtin_function(name const & fn);

optional<vm_decl> get_vm_decl(environment const & env, name const & n);

void display_vm_code(std::ostream & out, environment const & env, unsigned code_sz, vm_instr const * code);

void initialize_vm_core();
void finalize_vm_core();

void initialize_vm();
void finalize_vm();
}
