/* Generate pattern matching and transform code shared between
   GENERIC and GIMPLE folding code from match-and-simplify description.

   Copyright (C) 2014-2015 Free Software Foundation, Inc.
   Contributed by Richard Biener <rguenther@suse.de>
   and Prathamesh Kulkarni  <bilbotheelffriend@gmail.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "bconfig.h"
#include <new>
#include "system.h"
#include "coretypes.h"
#include <cpplib.h>
#include "errors.h"
#include "hash-table.h"
#include "hash-set.h"
#include "is-a.h"


/* Stubs for GGC referenced through instantiations triggered by hash-map.  */
void *ggc_internal_cleared_alloc (size_t, void (*)(void *),
				  size_t, size_t MEM_STAT_DECL)
{
  return NULL;
}
void ggc_free (void *)
{
}


/* Global state.  */

/* Verboseness.  0 is quiet, 1 adds some warnings, 2 is for debugging.  */
unsigned verbose;


/* libccp helpers.  */

static struct line_maps *line_table;

static bool
#if GCC_VERSION >= 4001
__attribute__((format (printf, 6, 0)))
#endif
error_cb (cpp_reader *, int errtype, int, source_location location,
	  unsigned int, const char *msg, va_list *ap)
{
  const line_map_ordinary *map;
  linemap_resolve_location (line_table, location, LRK_SPELLING_LOCATION, &map);
  expanded_location loc = linemap_expand_location (line_table, map, location);
  fprintf (stderr, "%s:%d:%d %s: ", loc.file, loc.line, loc.column,
	   (errtype == CPP_DL_WARNING) ? "warning" : "error");
  vfprintf (stderr, msg, *ap);
  fprintf (stderr, "\n");
  FILE *f = fopen (loc.file, "r");
  if (f)
    {
      char buf[128];
      while (loc.line > 0)
	{
	  if (!fgets (buf, 128, f))
	    goto notfound;
	  if (buf[strlen (buf) - 1] != '\n')
	    {
	      if (loc.line > 1)
		loc.line++;
	    }
	  loc.line--;
	}
      fprintf (stderr, "%s", buf);
      for (int i = 0; i < loc.column - 1; ++i)
	fputc (' ', stderr);
      fputc ('^', stderr);
      fputc ('\n', stderr);
notfound:
      fclose (f);
    }

  if (errtype == CPP_DL_FATAL)
    exit (1);
  return false;
}

static void
#if GCC_VERSION >= 4001
__attribute__((format (printf, 2, 3)))
#endif
fatal_at (const cpp_token *tk, const char *msg, ...)
{
  va_list ap;
  va_start (ap, msg);
  error_cb (NULL, CPP_DL_FATAL, 0, tk->src_loc, 0, msg, &ap);
  va_end (ap);
}

static void
#if GCC_VERSION >= 4001
__attribute__((format (printf, 2, 3)))
#endif
fatal_at (source_location loc, const char *msg, ...)
{
  va_list ap;
  va_start (ap, msg);
  error_cb (NULL, CPP_DL_FATAL, 0, loc, 0, msg, &ap);
  va_end (ap);
}

static void
#if GCC_VERSION >= 4001
__attribute__((format (printf, 2, 3)))
#endif
warning_at (const cpp_token *tk, const char *msg, ...)
{
  va_list ap;
  va_start (ap, msg);
  error_cb (NULL, CPP_DL_WARNING, 0, tk->src_loc, 0, msg, &ap);
  va_end (ap);
}

static void
#if GCC_VERSION >= 4001
__attribute__((format (printf, 2, 3)))
#endif
warning_at (source_location loc, const char *msg, ...)
{
  va_list ap;
  va_start (ap, msg);
  error_cb (NULL, CPP_DL_WARNING, 0, loc, 0, msg, &ap);
  va_end (ap);
}

/* Like fprintf, but print INDENT spaces at the beginning.  */

static void
#if GCC_VERSION >= 4001
__attribute__((format (printf, 3, 4)))
#endif
fprintf_indent (FILE *f, unsigned int indent, const char *format, ...)
{
  va_list ap;
  for (; indent >= 8; indent -= 8)
    fputc ('\t', f);
  fprintf (f, "%*s", indent, "");
  va_start (ap, format);
  vfprintf (f, format, ap);
  va_end (ap);
}

static void
output_line_directive (FILE *f, source_location location,
		       bool dumpfile = false)
{
  const line_map_ordinary *map;
  linemap_resolve_location (line_table, location, LRK_SPELLING_LOCATION, &map);
  expanded_location loc = linemap_expand_location (line_table, map, location);
  if (dumpfile)
    {
      /* When writing to a dumpfile only dump the filename.  */
      const char *file = strrchr (loc.file, DIR_SEPARATOR);
      if (!file)
	file = loc.file;
      else
	++file;
      fprintf (f, "%s:%d", file, loc.line);
    }
  else
    /* Other gen programs really output line directives here, at least for
       development it's right now more convenient to have line information
       from the generated file.  Still keep the directives as comment for now
       to easily back-point to the meta-description.  */
    fprintf (f, "/* #line %d \"%s\" */\n", loc.line, loc.file);
}


/* Pull in tree codes and builtin function codes from their
   definition files.  */

#define DEFTREECODE(SYM, STRING, TYPE, NARGS)   SYM,
enum tree_code {
#include "tree.def"
CONVERT0,
CONVERT1,
CONVERT2,
VIEW_CONVERT0,
VIEW_CONVERT1,
VIEW_CONVERT2,
MAX_TREE_CODES
};
#undef DEFTREECODE

#define DEF_BUILTIN(ENUM, N, C, T, LT, B, F, NA, AT, IM, COND) ENUM,
enum built_in_function {
#include "builtins.def"
END_BUILTINS
};
#undef DEF_BUILTIN

/* Return true if CODE represents a commutative tree code.  Otherwise
   return false.  */
bool
commutative_tree_code (enum tree_code code)
{
  switch (code)
    {
    case PLUS_EXPR:
    case MULT_EXPR:
    case MULT_HIGHPART_EXPR:
    case MIN_EXPR:
    case MAX_EXPR:
    case BIT_IOR_EXPR:
    case BIT_XOR_EXPR:
    case BIT_AND_EXPR:
    case NE_EXPR:
    case EQ_EXPR:
    case UNORDERED_EXPR:
    case ORDERED_EXPR:
    case UNEQ_EXPR:
    case LTGT_EXPR:
    case TRUTH_AND_EXPR:
    case TRUTH_XOR_EXPR:
    case TRUTH_OR_EXPR:
    case WIDEN_MULT_EXPR:
    case VEC_WIDEN_MULT_HI_EXPR:
    case VEC_WIDEN_MULT_LO_EXPR:
    case VEC_WIDEN_MULT_EVEN_EXPR:
    case VEC_WIDEN_MULT_ODD_EXPR:
      return true;

    default:
      break;
    }
  return false;
}

/* Return true if CODE represents a ternary tree code for which the
   first two operands are commutative.  Otherwise return false.  */
bool
commutative_ternary_tree_code (enum tree_code code)
{
  switch (code)
    {
    case WIDEN_MULT_PLUS_EXPR:
    case WIDEN_MULT_MINUS_EXPR:
    case DOT_PROD_EXPR:
    case FMA_EXPR:
      return true;

    default:
      break;
    }
  return false;
}


/* Base class for all identifiers the parser knows.  */

struct id_base : nofree_ptr_hash<id_base>
{
  enum id_kind { CODE, FN, PREDICATE, USER } kind;

  id_base (id_kind, const char *, int = -1);

  hashval_t hashval;
  int nargs;
  const char *id;

  /* hash_table support.  */
  static inline hashval_t hash (const id_base *);
  static inline int equal (const id_base *, const id_base *);
};

inline hashval_t
id_base::hash (const id_base *op)
{
  return op->hashval;
}

inline int
id_base::equal (const id_base *op1,
			const id_base *op2)
{
  return (op1->hashval == op2->hashval
	  && strcmp (op1->id, op2->id) == 0);
}

/* Hashtable of known pattern operators.  This is pre-seeded from
   all known tree codes and all known builtin function ids.  */
static hash_table<id_base> *operators;

id_base::id_base (id_kind kind_, const char *id_, int nargs_)
{
  kind = kind_;
  id = id_;
  nargs = nargs_;
  hashval = htab_hash_string (id);
}

/* Identifier that maps to a tree code.  */

struct operator_id : public id_base
{
  operator_id (enum tree_code code_, const char *id_, unsigned nargs_,
	       const char *tcc_)
      : id_base (id_base::CODE, id_, nargs_), code (code_), tcc (tcc_) {}
  enum tree_code code;
  const char *tcc;
};

/* Identifier that maps to a builtin function code.  */

struct fn_id : public id_base
{
  fn_id (enum built_in_function fn_, const char *id_)
      : id_base (id_base::FN, id_), fn (fn_) {}
  enum built_in_function fn;
};

struct simplify;

/* Identifier that maps to a user-defined predicate.  */

struct predicate_id : public id_base
{
  predicate_id (const char *id_)
    : id_base (id_base::PREDICATE, id_), matchers (vNULL) {}
  vec<simplify *> matchers;
};

/* Identifier that maps to a operator defined by a 'for' directive.  */

struct user_id : public id_base
{
  user_id (const char *id_, bool is_oper_list_ = false)
    : id_base (id_base::USER, id_), substitutes (vNULL),
      used (false), is_oper_list (is_oper_list_) {}
  vec<id_base *> substitutes;
  bool used;
  bool is_oper_list;
};

template<>
template<>
inline bool
is_a_helper <fn_id *>::test (id_base *id)
{
  return id->kind == id_base::FN;
}

template<>
template<>
inline bool
is_a_helper <operator_id *>::test (id_base *id)
{
  return id->kind == id_base::CODE;
}

template<>
template<>
inline bool
is_a_helper <predicate_id *>::test (id_base *id)
{
  return id->kind == id_base::PREDICATE;
}

template<>
template<>
inline bool
is_a_helper <user_id *>::test (id_base *id)
{
  return id->kind == id_base::USER;
}

/* Add a predicate identifier to the hash.  */

static predicate_id *
add_predicate (const char *id)
{
  predicate_id *p = new predicate_id (id);
  id_base **slot = operators->find_slot_with_hash (p, p->hashval, INSERT);
  if (*slot)
    fatal ("duplicate id definition");
  *slot = p;
  return p;
}

/* Add a tree code identifier to the hash.  */

static void
add_operator (enum tree_code code, const char *id,
	      const char *tcc, unsigned nargs)
{
  if (strcmp (tcc, "tcc_unary") != 0
      && strcmp (tcc, "tcc_binary") != 0
      && strcmp (tcc, "tcc_comparison") != 0
      && strcmp (tcc, "tcc_expression") != 0
      /* For {REAL,IMAG}PART_EXPR and VIEW_CONVERT_EXPR.  */
      && strcmp (tcc, "tcc_reference") != 0
      /* To have INTEGER_CST and friends as "predicate operators".  */
      && strcmp (tcc, "tcc_constant") != 0
      /* And allow CONSTRUCTOR for vector initializers.  */
      && !(code == CONSTRUCTOR)
      /* Allow SSA_NAME as predicate operator.  */
      && !(code == SSA_NAME))
    return;
  /* Treat ADDR_EXPR as atom, thus don't allow matching its operand.  */
  if (code == ADDR_EXPR)
    nargs = 0;
  operator_id *op = new operator_id (code, id, nargs, tcc);
  id_base **slot = operators->find_slot_with_hash (op, op->hashval, INSERT);
  if (*slot)
    fatal ("duplicate id definition");
  *slot = op;
}

/* Add a builtin identifier to the hash.  */

static void
add_builtin (enum built_in_function code, const char *id)
{
  fn_id *fn = new fn_id (code, id);
  id_base **slot = operators->find_slot_with_hash (fn, fn->hashval, INSERT);
  if (*slot)
    fatal ("duplicate id definition");
  *slot = fn;
}

/* Helper for easy comparing ID with tree code CODE.  */

static bool
operator==(id_base &id, enum tree_code code)
{
  if (operator_id *oid = dyn_cast <operator_id *> (&id))
    return oid->code == code;
  return false;
}

/* Lookup the identifier ID.  */

id_base *
get_operator (const char *id)
{
  id_base tem (id_base::CODE, id);

  id_base *op = operators->find_with_hash (&tem, tem.hashval);
  if (op)
    {
      /* If this is a user-defined identifier track whether it was used.  */
      if (user_id *uid = dyn_cast<user_id *> (op))
	uid->used = true;
      return op;
    }

  /* Try all-uppercase.  */
  char *id2 = xstrdup (id);
  for (unsigned i = 0; i < strlen (id2); ++i)
    id2[i] = TOUPPER (id2[i]);
  new (&tem) id_base (id_base::CODE, id2);
  op = operators->find_with_hash (&tem, tem.hashval);
  if (op)
    {
      free (id2);
      return op;
    }

  /* Try _EXPR appended.  */
  id2 = (char *)xrealloc (id2, strlen (id2) + sizeof ("_EXPR") + 1);
  strcat (id2, "_EXPR");
  new (&tem) id_base (id_base::CODE, id2);
  op = operators->find_with_hash (&tem, tem.hashval);
  if (op)
    {
      free (id2);
      return op;
    }

  return 0;
}

typedef hash_map<nofree_string_hash, unsigned> cid_map_t;


/* The AST produced by parsing of the pattern definitions.  */

struct dt_operand;
struct capture_info;

/* The base class for operands.  */

struct operand {
  enum op_type { OP_PREDICATE, OP_EXPR, OP_CAPTURE, OP_C_EXPR, OP_IF, OP_WITH };
  operand (enum op_type type_, source_location loc_)
    : type (type_), location (loc_) {}
  enum op_type type;
  source_location location;
  virtual void gen_transform (FILE *, int, const char *, bool, int,
			      const char *, capture_info *,
			      dt_operand ** = 0,
			      bool = true)
    { gcc_unreachable  (); }
};

/* A predicate operand.  Predicates are leafs in the AST.  */

struct predicate : public operand
{
  predicate (predicate_id *p_, source_location loc)
    : operand (OP_PREDICATE, loc), p (p_) {}
  predicate_id *p;
};

/* An operand that constitutes an expression.  Expressions include
   function calls and user-defined predicate invocations.  */

struct expr : public operand
{
  expr (id_base *operation_, source_location loc, bool is_commutative_ = false)
    : operand (OP_EXPR, loc), operation (operation_),
      ops (vNULL), expr_type (NULL), is_commutative (is_commutative_),
      is_generic (false), force_single_use (false) {}
  expr (expr *e)
    : operand (OP_EXPR, e->location), operation (e->operation),
      ops (vNULL), expr_type (e->expr_type), is_commutative (e->is_commutative),
      is_generic (e->is_generic), force_single_use (e->force_single_use) {}
  void append_op (operand *op) { ops.safe_push (op); }
  /* The operator and its operands.  */
  id_base *operation;
  vec<operand *> ops;
  /* An explicitely specified type - used exclusively for conversions.  */
  const char *expr_type;
  /* Whether the operation is to be applied commutatively.  This is
     later lowered to two separate patterns.  */
  bool is_commutative;
  /* Whether the expression is expected to be in GENERIC form.  */
  bool is_generic;
  /* Whether pushing any stmt to the sequence should be conditional
     on this expression having a single-use.  */
  bool force_single_use;
  virtual void gen_transform (FILE *f, int, const char *, bool, int,
			      const char *, capture_info *,
			      dt_operand ** = 0, bool = true);
};

/* An operator that is represented by native C code.  This is always
   a leaf operand in the AST.  This class is also used to represent
   the code to be generated for 'if' and 'with' expressions.  */

struct c_expr : public operand
{
  /* A mapping of an identifier and its replacement.  Used to apply
     'for' lowering.  */
  struct id_tab {
    const char *id;
    const char *oper;
    id_tab (const char *id_, const char *oper_): id (id_), oper (oper_) {}
  };

  c_expr (cpp_reader *r_, source_location loc,
	  vec<cpp_token> code_, unsigned nr_stmts_,
	  vec<id_tab> ids_, cid_map_t *capture_ids_)
    : operand (OP_C_EXPR, loc), r (r_), code (code_),
      capture_ids (capture_ids_), nr_stmts (nr_stmts_), ids (ids_) {}
  /* cpplib tokens and state to transform this back to source.  */
  cpp_reader *r;
  vec<cpp_token> code;
  cid_map_t *capture_ids;
  /* The number of statements parsed (well, the number of ';'s).  */
  unsigned nr_stmts;
  /* The identifier replacement vector.  */
  vec<id_tab> ids;
  virtual void gen_transform (FILE *f, int, const char *, bool, int,
			      const char *, capture_info *,
			      dt_operand ** = 0, bool = true);
};

/* A wrapper around another operand that captures its value.  */

struct capture : public operand
{
  capture (source_location loc, unsigned where_, operand *what_)
      : operand (OP_CAPTURE, loc), where (where_), what (what_) {}
  /* Identifier index for the value.  */
  unsigned where;
  /* The captured value.  */
  operand *what;
  virtual void gen_transform (FILE *f, int, const char *, bool, int,
			      const char *, capture_info *,
			      dt_operand ** = 0, bool = true);
};

/* if expression.  */

struct if_expr : public operand
{
  if_expr (source_location loc)
    : operand (OP_IF, loc), cond (NULL), trueexpr (NULL), falseexpr (NULL) {}
  c_expr *cond;
  operand *trueexpr;
  operand *falseexpr;
};

/* with expression.  */

struct with_expr : public operand
{
  with_expr (source_location loc)
    : operand (OP_WITH, loc), with (NULL), subexpr (NULL) {}
  c_expr *with;
  operand *subexpr;
};

template<>
template<>
inline bool
is_a_helper <capture *>::test (operand *op)
{
  return op->type == operand::OP_CAPTURE;
}

template<>
template<>
inline bool
is_a_helper <predicate *>::test (operand *op)
{
  return op->type == operand::OP_PREDICATE;
}

template<>
template<>
inline bool
is_a_helper <c_expr *>::test (operand *op)
{
  return op->type == operand::OP_C_EXPR;
}

template<>
template<>
inline bool
is_a_helper <expr *>::test (operand *op)
{
  return op->type == operand::OP_EXPR;
}

template<>
template<>
inline bool
is_a_helper <if_expr *>::test (operand *op)
{
  return op->type == operand::OP_IF;
}

template<>
template<>
inline bool
is_a_helper <with_expr *>::test (operand *op)
{
  return op->type == operand::OP_WITH;
}

/* The main class of a pattern and its transform.  This is used to
   represent both (simplify ...) and (match ...) kinds.  The AST
   duplicates all outer 'if' and 'for' expressions here so each
   simplify can exist in isolation.  */

struct simplify
{
  enum simplify_kind { SIMPLIFY, MATCH };

  simplify (simplify_kind kind_, operand *match_, operand *result_,
	    vec<vec<user_id *> > for_vec_, cid_map_t *capture_ids_)
      : kind (kind_), match (match_), result (result_),
      for_vec (for_vec_), for_subst_vec (vNULL),
      capture_ids (capture_ids_), capture_max (capture_ids_->elements () - 1) {}

  simplify_kind kind;
  /* The expression that is matched against the GENERIC or GIMPLE IL.  */
  operand *match;
  /* For a (simplify ...) an expression with ifs and withs with the expression
     produced when the pattern applies in the leafs.
     For a (match ...) the leafs are either empty if it is a simple predicate
     or the single expression specifying the matched operands.  */
  struct operand *result;
  /* Collected 'for' expression operators that have to be replaced
     in the lowering phase.  */
  vec<vec<user_id *> > for_vec;
  vec<std::pair<user_id *, id_base *> > for_subst_vec;
  /* A map of capture identifiers to indexes.  */
  cid_map_t *capture_ids;
  int capture_max;
};

/* Debugging routines for dumping the AST.  */

DEBUG_FUNCTION void
print_operand (operand *o, FILE *f = stderr, bool flattened = false)
{
  if (capture *c = dyn_cast<capture *> (o))
    {
      fprintf (f, "@%u", c->where);
      if (c->what && flattened == false)
	{
	  putc (':', f);
	  print_operand (c->what, f, flattened);
	  putc (' ', f);
	}
    }

  else if (predicate *p = dyn_cast<predicate *> (o))
    fprintf (f, "%s", p->p->id);

  else if (is_a<c_expr *> (o))
    fprintf (f, "c_expr");

  else if (expr *e = dyn_cast<expr *> (o))
    {
      fprintf (f, "(%s", e->operation->id);

      if (flattened == false)
	{
	  putc (' ', f);
	  for (unsigned i = 0; i < e->ops.length (); ++i)
	    {
	      print_operand (e->ops[i], f, flattened);
	      putc (' ', f);
	    }
	}
      putc (')', f);
    }

  else
    gcc_unreachable ();
}

DEBUG_FUNCTION void
print_matches (struct simplify *s, FILE *f = stderr)
{
  fprintf (f, "for expression: ");
  print_operand (s->match, f);
  putc ('\n', f);
}


/* AST lowering.  */

/* Lowering of commutative operators.  */

static void
cartesian_product (const vec< vec<operand *> >& ops_vector,
		   vec< vec<operand *> >& result, vec<operand *>& v, unsigned n)
{
  if (n == ops_vector.length ())
    {
      vec<operand *> xv = v.copy ();
      result.safe_push (xv);
      return;
    }

  for (unsigned i = 0; i < ops_vector[n].length (); ++i)
    {
      v[n] = ops_vector[n][i];
      cartesian_product (ops_vector, result, v, n + 1);
    }
}

/* Lower OP to two operands in case it is marked as commutative.  */

static vec<operand *>
commutate (operand *op)
{
  vec<operand *> ret = vNULL;

  if (capture *c = dyn_cast <capture *> (op))
    {
      if (!c->what)
	{
	  ret.safe_push (op);
	  return ret;
	}
      vec<operand *> v = commutate (c->what);
      for (unsigned i = 0; i < v.length (); ++i)
	{
	  capture *nc = new capture (c->location, c->where, v[i]);
	  ret.safe_push (nc);
	}
      return ret;
    }

  expr *e = dyn_cast <expr *> (op);
  if (!e || e->ops.length () == 0)
    {
      ret.safe_push (op);
      return ret;
    }

  vec< vec<operand *> > ops_vector = vNULL;
  for (unsigned i = 0; i < e->ops.length (); ++i)
    ops_vector.safe_push (commutate (e->ops[i]));

  auto_vec< vec<operand *> > result;
  auto_vec<operand *> v (e->ops.length ());
  v.quick_grow_cleared (e->ops.length ());
  cartesian_product (ops_vector, result, v, 0);


  for (unsigned i = 0; i < result.length (); ++i)
    {
      expr *ne = new expr (e);
      ne->is_commutative = false;
      for (unsigned j = 0; j < result[i].length (); ++j)
	ne->append_op (result[i][j]);
      ret.safe_push (ne);
    }

  if (!e->is_commutative)
    return ret;

  for (unsigned i = 0; i < result.length (); ++i)
    {
      expr *ne = new expr (e);
      ne->is_commutative = false;
      // result[i].length () is 2 since e->operation is binary
      for (unsigned j = result[i].length (); j; --j)
	ne->append_op (result[i][j-1]);
      ret.safe_push (ne);
    }

  return ret;
}

/* Lower operations marked as commutative in the AST of S and push
   the resulting patterns to SIMPLIFIERS.  */

static void
lower_commutative (simplify *s, vec<simplify *>& simplifiers)
{
  vec<operand *> matchers = commutate (s->match);
  for (unsigned i = 0; i < matchers.length (); ++i)
    {
      simplify *ns = new simplify (s->kind, matchers[i], s->result,
				   s->for_vec, s->capture_ids);
      simplifiers.safe_push (ns);
    }
}

/* Strip conditional conversios using operator OPER from O and its
   children if STRIP, else replace them with an unconditional convert.  */

operand *
lower_opt_convert (operand *o, enum tree_code oper,
		   enum tree_code to_oper, bool strip)
{
  if (capture *c = dyn_cast<capture *> (o))
    {
      if (c->what)
	return new capture (c->location, c->where,
			    lower_opt_convert (c->what, oper, to_oper, strip));
      else
	return c;
    }

  expr *e = dyn_cast<expr *> (o);
  if (!e)
    return o;

  if (*e->operation == oper)
    {
      if (strip)
	return lower_opt_convert (e->ops[0], oper, to_oper, strip);

      expr *ne = new expr (e);
      ne->operation = (to_oper == CONVERT_EXPR
		       ? get_operator ("CONVERT_EXPR")
		       : get_operator ("VIEW_CONVERT_EXPR"));
      ne->append_op (lower_opt_convert (e->ops[0], oper, to_oper, strip));
      return ne;
    }

  expr *ne = new expr (e);
  for (unsigned i = 0; i < e->ops.length (); ++i)
    ne->append_op (lower_opt_convert (e->ops[i], oper, to_oper, strip));

  return ne;
}

/* Determine whether O or its children uses the conditional conversion
   operator OPER.  */

static bool
has_opt_convert (operand *o, enum tree_code oper)
{
  if (capture *c = dyn_cast<capture *> (o))
    {
      if (c->what)
	return has_opt_convert (c->what, oper);
      else
	return false;
    }

  expr *e = dyn_cast<expr *> (o);
  if (!e)
    return false;

  if (*e->operation == oper)
    return true;

  for (unsigned i = 0; i < e->ops.length (); ++i)
    if (has_opt_convert (e->ops[i], oper))
      return true;

  return false;
}

/* Lower conditional convert operators in O, expanding it to a vector
   if required.  */

static vec<operand *>
lower_opt_convert (operand *o)
{
  vec<operand *> v1 = vNULL, v2;

  v1.safe_push (o);

  enum tree_code opers[]
    = { CONVERT0, CONVERT_EXPR,
	CONVERT1, CONVERT_EXPR,
	CONVERT2, CONVERT_EXPR,
	VIEW_CONVERT0, VIEW_CONVERT_EXPR,
	VIEW_CONVERT1, VIEW_CONVERT_EXPR,
	VIEW_CONVERT2, VIEW_CONVERT_EXPR };

  /* Conditional converts are lowered to a pattern with the
     conversion and one without.  The three different conditional
     convert codes are lowered separately.  */

  for (unsigned i = 0; i < sizeof (opers) / sizeof (enum tree_code); i += 2)
    {
      v2 = vNULL;
      for (unsigned j = 0; j < v1.length (); ++j)
	if (has_opt_convert (v1[j], opers[i]))
	  {
	    v2.safe_push (lower_opt_convert (v1[j],
					     opers[i], opers[i+1], false));
	    v2.safe_push (lower_opt_convert (v1[j],
					     opers[i], opers[i+1], true));
	  }

      if (v2 != vNULL)
	{
	  v1 = vNULL;
	  for (unsigned j = 0; j < v2.length (); ++j)
	    v1.safe_push (v2[j]);
	}
    }

  return v1;
}

/* Lower conditional convert operators in the AST of S and push
   the resulting multiple patterns to SIMPLIFIERS.  */

static void
lower_opt_convert (simplify *s, vec<simplify *>& simplifiers)
{
  vec<operand *> matchers = lower_opt_convert (s->match);
  for (unsigned i = 0; i < matchers.length (); ++i)
    {
      simplify *ns = new simplify (s->kind, matchers[i], s->result,
				   s->for_vec, s->capture_ids);
      simplifiers.safe_push (ns);
    }
}

/* Lower the compare operand of COND_EXPRs and VEC_COND_EXPRs to a
   GENERIC and a GIMPLE variant.  */

static vec<operand *>
lower_cond (operand *o)
{
  vec<operand *> ro = vNULL;

  if (capture *c = dyn_cast<capture *> (o))
    {
      if (c->what)
	{
	  vec<operand *> lop = vNULL;
	  lop = lower_cond (c->what);

	  for (unsigned i = 0; i < lop.length (); ++i)
	    ro.safe_push (new capture (c->location, c->where, lop[i]));
	  return ro;
	}
    }

  expr *e = dyn_cast<expr *> (o);
  if (!e || e->ops.length () == 0)
    {
      ro.safe_push (o);
      return ro;
    }

  vec< vec<operand *> > ops_vector = vNULL;
  for (unsigned i = 0; i < e->ops.length (); ++i)
    ops_vector.safe_push (lower_cond (e->ops[i]));

  auto_vec< vec<operand *> > result;
  auto_vec<operand *> v (e->ops.length ());
  v.quick_grow_cleared (e->ops.length ());
  cartesian_product (ops_vector, result, v, 0);

  for (unsigned i = 0; i < result.length (); ++i)
    {
      expr *ne = new expr (e);
      for (unsigned j = 0; j < result[i].length (); ++j)
	ne->append_op (result[i][j]);
      ro.safe_push (ne);
      /* If this is a COND with a captured expression or an
         expression with two operands then also match a GENERIC
	 form on the compare.  */
      if ((*e->operation == COND_EXPR
	   || *e->operation == VEC_COND_EXPR)
	  && ((is_a <capture *> (e->ops[0])
	       && as_a <capture *> (e->ops[0])->what
	       && is_a <expr *> (as_a <capture *> (e->ops[0])->what)
	       && as_a <expr *>
	            (as_a <capture *> (e->ops[0])->what)->ops.length () == 2)
	      || (is_a <expr *> (e->ops[0])
		  && as_a <expr *> (e->ops[0])->ops.length () == 2)))
	{
	  expr *ne = new expr (e);
	  for (unsigned j = 0; j < result[i].length (); ++j)
	    ne->append_op (result[i][j]);
	  if (capture *c = dyn_cast <capture *> (ne->ops[0]))
	    {
	      expr *ocmp = as_a <expr *> (c->what);
	      expr *cmp = new expr (ocmp);
	      for (unsigned j = 0; j < ocmp->ops.length (); ++j)
		cmp->append_op (ocmp->ops[j]);
	      cmp->is_generic = true;
	      ne->ops[0] = new capture (c->location, c->where, cmp);
	    }
	  else
	    {
	      expr *ocmp = as_a <expr *> (ne->ops[0]);
	      expr *cmp = new expr (ocmp);
	      for (unsigned j = 0; j < ocmp->ops.length (); ++j)
		cmp->append_op (ocmp->ops[j]);
	      cmp->is_generic = true;
	      ne->ops[0] = cmp;
	    }
	  ro.safe_push (ne);
	}
    }

  return ro;
}

/* Lower the compare operand of COND_EXPRs and VEC_COND_EXPRs to a
   GENERIC and a GIMPLE variant.  */

static void
lower_cond (simplify *s, vec<simplify *>& simplifiers)
{
  vec<operand *> matchers = lower_cond (s->match);
  for (unsigned i = 0; i < matchers.length (); ++i)
    {
      simplify *ns = new simplify (s->kind, matchers[i], s->result,
				   s->for_vec, s->capture_ids);
      simplifiers.safe_push (ns);
    }
}

/* In AST operand O replace operator ID with operator WITH.  */

operand *
replace_id (operand *o, user_id *id, id_base *with)
{
  /* Deep-copy captures and expressions, replacing operations as
     needed.  */
  if (capture *c = dyn_cast<capture *> (o))
    {
      if (!c->what)
	return c;
      return new capture (c->location, c->where,
			  replace_id (c->what, id, with));
    }
  else if (expr *e = dyn_cast<expr *> (o))
    {
      expr *ne = new expr (e);
      if (e->operation == id)
	ne->operation = with;
      for (unsigned i = 0; i < e->ops.length (); ++i)
	ne->append_op (replace_id (e->ops[i], id, with));
      return ne;
    }
  else if (with_expr *w = dyn_cast <with_expr *> (o))
    {
      with_expr *nw = new with_expr (w->location);
      nw->with = as_a <c_expr *> (replace_id (w->with, id, with));
      nw->subexpr = replace_id (w->subexpr, id, with);
      return nw;
    }
  else if (if_expr *ife = dyn_cast <if_expr *> (o))
    {
      if_expr *nife = new if_expr (ife->location);
      nife->cond = as_a <c_expr *> (replace_id (ife->cond, id, with));
      nife->trueexpr = replace_id (ife->trueexpr, id, with);
      if (ife->falseexpr)
	nife->falseexpr = replace_id (ife->falseexpr, id, with);
      return nife;
    }

  /* For c_expr we simply record a string replacement table which is
     applied at code-generation time.  */
  if (c_expr *ce = dyn_cast<c_expr *> (o))
    {
      vec<c_expr::id_tab> ids = ce->ids.copy ();
      ids.safe_push (c_expr::id_tab (id->id, with->id));
      return new c_expr (ce->r, ce->location,
			 ce->code, ce->nr_stmts, ids, ce->capture_ids);
    }

  return o;
}

/* Return true if the binary operator OP is ok for delayed substitution
   during for lowering.  */

static bool
binary_ok (operator_id *op)
{
  switch (op->code)
    {
    case PLUS_EXPR:
    case MINUS_EXPR:
    case MULT_EXPR:
    case TRUNC_DIV_EXPR:
    case CEIL_DIV_EXPR:
    case FLOOR_DIV_EXPR:
    case ROUND_DIV_EXPR:
    case TRUNC_MOD_EXPR:
    case CEIL_MOD_EXPR:
    case FLOOR_MOD_EXPR:
    case ROUND_MOD_EXPR:
    case RDIV_EXPR:
    case EXACT_DIV_EXPR:
    case MIN_EXPR:
    case MAX_EXPR:
    case BIT_IOR_EXPR:
    case BIT_XOR_EXPR:
    case BIT_AND_EXPR:
      return true;
    default:
      return false;
    }
}

/* Lower recorded fors for SIN and output to SIMPLIFIERS.  */

static void
lower_for (simplify *sin, vec<simplify *>& simplifiers)
{
  vec<vec<user_id *> >& for_vec = sin->for_vec;
  unsigned worklist_start = 0;
  auto_vec<simplify *> worklist;
  worklist.safe_push (sin);

  /* Lower each recorded for separately, operating on the
     set of simplifiers created by the previous one.
     Lower inner-to-outer so inner for substitutes can refer
     to operators replaced by outer fors.  */
  for (int fi = for_vec.length () - 1; fi >= 0; --fi)
    {
      vec<user_id *>& ids = for_vec[fi];
      unsigned n_ids = ids.length ();
      unsigned max_n_opers = 0;
      bool can_delay_subst = (sin->kind == simplify::SIMPLIFY);
      for (unsigned i = 0; i < n_ids; ++i)
	{
	  if (ids[i]->substitutes.length () > max_n_opers)
	    max_n_opers = ids[i]->substitutes.length ();
	  /* Require that all substitutes are of the same kind so that
	     if we delay substitution to the result op code generation
	     can look at the first substitute for deciding things like
	     types of operands.  */
	  enum id_base::id_kind kind = ids[i]->substitutes[0]->kind;
	  for (unsigned j = 0; j < ids[i]->substitutes.length (); ++j)
	    if (ids[i]->substitutes[j]->kind != kind)
	      can_delay_subst = false;
	    else if (operator_id *op
		       = dyn_cast <operator_id *> (ids[i]->substitutes[j]))
	      {
		operator_id *op0
		  = as_a <operator_id *> (ids[i]->substitutes[0]);
		if (strcmp (op->tcc, "tcc_comparison") == 0
		    && strcmp (op0->tcc, "tcc_comparison") == 0)
		  ;
		/* Unfortunately we can't just allow all tcc_binary.  */
		else if (strcmp (op->tcc, "tcc_binary") == 0
			 && strcmp (op0->tcc, "tcc_binary") == 0
			 && binary_ok (op)
			 && binary_ok (op0))
		  ;
		else if ((strcmp (op->id + 1, "SHIFT_EXPR") == 0
			  || strcmp (op->id + 1, "ROTATE_EXPR") == 0)
			 && (strcmp (op0->id + 1, "SHIFT_EXPR") == 0
			     || strcmp (op0->id + 1, "ROTATE_EXPR") == 0))
		  ;
		else
		  can_delay_subst = false;
	      }
	    else if (is_a <fn_id *> (ids[i]->substitutes[j]))
	      ;
	    else
	      can_delay_subst = false;
	}

      unsigned worklist_end = worklist.length ();
      for (unsigned si = worklist_start; si < worklist_end; ++si)
	{
	  simplify *s = worklist[si];
	  for (unsigned j = 0; j < max_n_opers; ++j)
	    {
	      operand *match_op = s->match;
	      operand *result_op = s->result;
	      vec<std::pair<user_id *, id_base *> > subst;
	      subst.create (n_ids);
	      for (unsigned i = 0; i < n_ids; ++i)
		{
		  user_id *id = ids[i];
		  id_base *oper = id->substitutes[j % id->substitutes.length ()];
		  subst.quick_push (std::make_pair (id, oper));
		  match_op = replace_id (match_op, id, oper);
		  if (result_op
		      && !can_delay_subst)
		    result_op = replace_id (result_op, id, oper);
		}
	      simplify *ns = new simplify (s->kind, match_op, result_op,
					   vNULL, s->capture_ids);
	      ns->for_subst_vec.safe_splice (s->for_subst_vec);
	      if (result_op
		  && can_delay_subst)
		ns->for_subst_vec.safe_splice (subst);
	      else
		subst.release ();
	      worklist.safe_push (ns);
	    }
	}
      worklist_start = worklist_end;
    }

  /* Copy out the result from the last for lowering.  */
  for (unsigned i = worklist_start; i < worklist.length (); ++i)
    simplifiers.safe_push (worklist[i]);
}

/* Lower the AST for everything in SIMPLIFIERS.  */

static void
lower (vec<simplify *>& simplifiers, bool gimple)
{
  auto_vec<simplify *> out_simplifiers;
  for (unsigned i = 0; i < simplifiers.length (); ++i)
    lower_opt_convert (simplifiers[i], out_simplifiers);

  simplifiers.truncate (0);
  for (unsigned i = 0; i < out_simplifiers.length (); ++i)
    lower_commutative (out_simplifiers[i], simplifiers);

  out_simplifiers.truncate (0);
  if (gimple)
    for (unsigned i = 0; i < simplifiers.length (); ++i)
      lower_cond (simplifiers[i], out_simplifiers);
  else
    out_simplifiers.safe_splice (simplifiers);


  simplifiers.truncate (0);
  for (unsigned i = 0; i < out_simplifiers.length (); ++i)
    lower_for (out_simplifiers[i], simplifiers);
}




/* The decision tree built for generating GIMPLE and GENERIC pattern
   matching code.  It represents the 'match' expression of all
   simplifies and has those as its leafs.  */

struct dt_simplify;

/* A hash-map collecting semantically equivalent leafs in the decision
   tree for splitting out to separate functions.  */
struct sinfo
{
  dt_simplify *s;

  const char *fname;
  unsigned cnt;
};

struct sinfo_hashmap_traits : simple_hashmap_traits <pointer_hash <dt_simplify> >
{
  static inline hashval_t hash (const key_type &);
  static inline bool equal_keys (const key_type &, const key_type &);
  template <typename T> static inline void remove (T &) {}
};

typedef hash_map<void * /* unused */, sinfo *, sinfo_hashmap_traits>
  sinfo_map_t;


/* Decision tree base class, used for DT_TRUE and DT_NODE.  */

struct dt_node
{
  enum dt_type { DT_NODE, DT_OPERAND, DT_TRUE, DT_MATCH, DT_SIMPLIFY };

  enum dt_type type;
  unsigned level;
  vec<dt_node *> kids;

  /* Statistics.  */
  unsigned num_leafs;
  unsigned total_size;
  unsigned max_level;

  dt_node (enum dt_type type_): type (type_), level (0), kids (vNULL) {}

  dt_node *append_node (dt_node *);
  dt_node *append_op (operand *, dt_node *parent = 0, unsigned pos = 0);
  dt_node *append_true_op (dt_node *parent = 0, unsigned pos = 0);
  dt_node *append_match_op (dt_operand *, dt_node *parent = 0, unsigned pos = 0);
  dt_node *append_simplify (simplify *, unsigned, dt_operand **);

  virtual void gen (FILE *, int, bool) {}

  void gen_kids (FILE *, int, bool);
  void gen_kids_1 (FILE *, int, bool,
		   vec<dt_operand *>, vec<dt_operand *>, vec<dt_operand *>,
		   vec<dt_operand *>, vec<dt_operand *>, vec<dt_node *>);

  void analyze (sinfo_map_t &);
};

/* Generic decision tree node used for DT_OPERAND and DT_MATCH.  */

struct dt_operand : public dt_node
{
  operand *op;
  dt_operand *match_dop;
  dt_operand *parent;
  unsigned pos;

  dt_operand (enum dt_type type, operand *op_, dt_operand *match_dop_,
	      dt_operand *parent_ = 0, unsigned pos_ = 0)
      : dt_node (type), op (op_), match_dop (match_dop_),
      parent (parent_), pos (pos_) {}

  void gen (FILE *, int, bool);
  unsigned gen_predicate (FILE *, int, const char *, bool);
  unsigned gen_match_op (FILE *, int, const char *);

  unsigned gen_gimple_expr (FILE *, int);
  unsigned gen_generic_expr (FILE *, int, const char *);

  char *get_name (char *);
  void gen_opname (char *, unsigned);
};

/* Leaf node of the decision tree, used for DT_SIMPLIFY.  */

struct dt_simplify : public dt_node
{
  simplify *s;
  unsigned pattern_no;
  dt_operand **indexes;
  sinfo *info;

  dt_simplify (simplify *s_, unsigned pattern_no_, dt_operand **indexes_)
	: dt_node (DT_SIMPLIFY), s (s_), pattern_no (pattern_no_),
	  indexes (indexes_), info (NULL)  {}

  void gen_1 (FILE *, int, bool, operand *);
  void gen (FILE *f, int, bool);
};

template<>
template<>
inline bool
is_a_helper <dt_operand *>::test (dt_node *n)
{
  return (n->type == dt_node::DT_OPERAND
	  || n->type == dt_node::DT_MATCH);
}

template<>
template<>
inline bool
is_a_helper <dt_simplify *>::test (dt_node *n)
{
  return n->type == dt_node::DT_SIMPLIFY;
}



/* A container for the actual decision tree.  */

struct decision_tree
{
  dt_node *root;

  void insert (struct simplify *, unsigned);
  void gen (FILE *f, bool gimple);
  void print (FILE *f = stderr);

  decision_tree () { root = new dt_node (dt_node::DT_NODE); }

  static dt_node *insert_operand (dt_node *, operand *, dt_operand **indexes,
				  unsigned pos = 0, dt_node *parent = 0);
  static dt_node *find_node (vec<dt_node *>&, dt_node *);
  static bool cmp_node (dt_node *, dt_node *);
  static void print_node (dt_node *, FILE *f = stderr, unsigned = 0);
};

/* Compare two AST operands O1 and O2 and return true if they are equal.  */

bool
cmp_operand (operand *o1, operand *o2)
{
  if (!o1 || !o2 || o1->type != o2->type)
    return false;

  if (o1->type == operand::OP_PREDICATE)
    {
      predicate *p1 = as_a<predicate *>(o1);
      predicate *p2 = as_a<predicate *>(o2);
      return p1->p == p2->p;
    }
  else if (o1->type == operand::OP_EXPR)
    {
      expr *e1 = static_cast<expr *>(o1);
      expr *e2 = static_cast<expr *>(o2);
      return (e1->operation == e2->operation
	      && e1->is_generic == e2->is_generic);
    }
  else
    return false;
}

/* Compare two decision tree nodes N1 and N2 and return true if they
   are equal.  */

bool
decision_tree::cmp_node (dt_node *n1, dt_node *n2)
{
  if (!n1 || !n2 || n1->type != n2->type)
    return false;

  if (n1 == n2)
    return true;

  if (n1->type == dt_node::DT_TRUE)
    return false;

  if (n1->type == dt_node::DT_OPERAND)
    return cmp_operand ((as_a<dt_operand *> (n1))->op,
			(as_a<dt_operand *> (n2))->op);
  else if (n1->type == dt_node::DT_MATCH)
    return ((as_a<dt_operand *> (n1))->match_dop
	    == (as_a<dt_operand *> (n2))->match_dop);
  return false;
}

/* Search OPS for a decision tree node like P and return it if found.  */

dt_node *
decision_tree::find_node (vec<dt_node *>& ops, dt_node *p)
{
  /* We can merge adjacent DT_TRUE.  */
  if (p->type == dt_node::DT_TRUE
      && !ops.is_empty ()
      && ops.last ()->type == dt_node::DT_TRUE)
    return ops.last ();
  for (int i = ops.length () - 1; i >= 0; --i)
    {
      /* But we can't merge across DT_TRUE nodes as they serve as
         pattern order barriers to make sure that patterns apply
	 in order of appearance in case multiple matches are possible.  */
      if (ops[i]->type == dt_node::DT_TRUE)
	return NULL;
      if (decision_tree::cmp_node (ops[i], p))
	return ops[i];
    }
  return NULL;
}

/* Append N to the decision tree if it there is not already an existing
   identical child.  */

dt_node *
dt_node::append_node (dt_node *n)
{
  dt_node *kid;

  kid = decision_tree::find_node (kids, n);
  if (kid)
    return kid;

  kids.safe_push (n);
  n->level = this->level + 1;

  return n;
}

/* Append OP to the decision tree.  */

dt_node *
dt_node::append_op (operand *op, dt_node *parent, unsigned pos)
{
  dt_operand *parent_ = safe_as_a<dt_operand *> (parent);
  dt_operand *n = new dt_operand (DT_OPERAND, op, 0, parent_, pos);
  return append_node (n);
}

/* Append a DT_TRUE decision tree node.  */

dt_node *
dt_node::append_true_op (dt_node *parent, unsigned pos)
{
  dt_operand *parent_ = safe_as_a<dt_operand *> (parent);
  dt_operand *n = new dt_operand (DT_TRUE, 0, 0, parent_, pos);
  return append_node (n);
}

/* Append a DT_MATCH decision tree node.  */

dt_node *
dt_node::append_match_op (dt_operand *match_dop, dt_node *parent, unsigned pos)
{
  dt_operand *parent_ = as_a<dt_operand *> (parent);
  dt_operand *n = new dt_operand (DT_MATCH, 0, match_dop, parent_, pos);
  return append_node (n);
}

/* Append S to the decision tree.  */

dt_node *
dt_node::append_simplify (simplify *s, unsigned pattern_no,
			  dt_operand **indexes)
{
  dt_simplify *n = new dt_simplify (s, pattern_no, indexes);
  return append_node (n);
}

/* Analyze the node and its children.  */

void
dt_node::analyze (sinfo_map_t &map)
{
  num_leafs = 0;
  total_size = 1;
  max_level = level;

  if (type == DT_SIMPLIFY)
    {
      /* Populate the map of equivalent simplifies.  */
      dt_simplify *s = as_a <dt_simplify *> (this);
      bool existed;
      sinfo *&si = map.get_or_insert (s, &existed);
      if (!existed)
	{
	  si = new sinfo;
	  si->s = s;
	  si->cnt = 1;
	  si->fname = NULL;
	}
      else
	si->cnt++;
      s->info = si;
      num_leafs = 1;
      return;
    }

  for (unsigned i = 0; i < kids.length (); ++i)
    {
      kids[i]->analyze (map);
      num_leafs += kids[i]->num_leafs;
      total_size += kids[i]->total_size;
      max_level = MAX (max_level, kids[i]->max_level);
    }
}

/* Insert O into the decision tree and return the decision tree node found
   or created.  */

dt_node *
decision_tree::insert_operand (dt_node *p, operand *o, dt_operand **indexes,
			       unsigned pos, dt_node *parent)
{
  dt_node *q, *elm = 0;

  if (capture *c = dyn_cast<capture *> (o))
    {
      unsigned capt_index = c->where;

      if (indexes[capt_index] == 0)
	{
	  if (c->what)
	    q = insert_operand (p, c->what, indexes, pos, parent);
	  else
	    {
	      q = elm = p->append_true_op (parent, pos);
	      goto at_assert_elm;
	    }
	  // get to the last capture
	  for (operand *what = c->what;
	       what && is_a<capture *> (what);
	       c = as_a<capture *> (what), what = c->what)
	    ;

	  if (!c->what)
	    {
	      unsigned cc_index = c->where;
	      dt_operand *match_op = indexes[cc_index];

	      dt_operand temp (dt_node::DT_TRUE, 0, 0);
	      elm = decision_tree::find_node (p->kids, &temp);

	      if (elm == 0)
		{
		  dt_operand temp (dt_node::DT_MATCH, 0, match_op);
		  elm = decision_tree::find_node (p->kids, &temp);
		}
	    }
	  else
	    {
	      dt_operand temp (dt_node::DT_OPERAND, c->what, 0);
	      elm = decision_tree::find_node (p->kids, &temp);
	    }

at_assert_elm:
	  gcc_assert (elm->type == dt_node::DT_TRUE
		      || elm->type == dt_node::DT_OPERAND
		      || elm->type == dt_node::DT_MATCH);
	  indexes[capt_index] = static_cast<dt_operand *> (elm);
	  return q;
	}
      else
	{
	  p = p->append_match_op (indexes[capt_index], parent, pos);
	  if (c->what)
	    return insert_operand (p, c->what, indexes, 0, p);
	  else
	    return p;
	}
    }
  p = p->append_op (o, parent, pos);
  q = p;

  if (expr *e = dyn_cast <expr *>(o))
    {
      for (unsigned i = 0; i < e->ops.length (); ++i)
	q = decision_tree::insert_operand (q, e->ops[i], indexes, i, p);
    }

  return q;
}

/* Insert S into the decision tree.  */

void
decision_tree::insert (struct simplify *s, unsigned pattern_no)
{
  dt_operand **indexes = XCNEWVEC (dt_operand *, s->capture_max + 1);
  dt_node *p = decision_tree::insert_operand (root, s->match, indexes);
  p->append_simplify (s, pattern_no, indexes);
}

/* Debug functions to dump the decision tree.  */

DEBUG_FUNCTION void
decision_tree::print_node (dt_node *p, FILE *f, unsigned indent)
{
  if (p->type == dt_node::DT_NODE)
    fprintf (f, "root");
  else
    {
      fprintf (f, "|");
      for (unsigned i = 0; i < indent; i++)
	fprintf (f, "-");

      if (p->type == dt_node::DT_OPERAND)
	{
	  dt_operand *dop = static_cast<dt_operand *>(p);
	  print_operand (dop->op, f, true);
	}
      else if (p->type == dt_node::DT_TRUE)
	fprintf (f, "true");
      else if (p->type == dt_node::DT_MATCH)
	fprintf (f, "match (%p)", (void *)((as_a<dt_operand *>(p))->match_dop));
      else if (p->type == dt_node::DT_SIMPLIFY)
	{
	  dt_simplify *s = static_cast<dt_simplify *> (p);
	  fprintf (f, "simplify_%u { ", s->pattern_no);
	  for (int i = 0; i <= s->s->capture_max; ++i)
	    fprintf (f, "%p, ", (void *) s->indexes[i]);
	  fprintf (f, " } ");
	}
    }

  fprintf (stderr, " (%p), %u, %u\n", (void *) p, p->level, p->kids.length ());

  for (unsigned i = 0; i < p->kids.length (); ++i)
    decision_tree::print_node (p->kids[i], f, indent + 2);
}

DEBUG_FUNCTION void
decision_tree::print (FILE *f)
{
  return decision_tree::print_node (root, f);
}


/* For GENERIC we have to take care of wrapping multiple-used
   expressions with side-effects in save_expr and preserve side-effects
   of expressions with omit_one_operand.  Analyze captures in
   match, result and with expressions and perform early-outs
   on the outermost match expression operands for cases we cannot
   handle.  */

struct capture_info
{
  capture_info (simplify *s, operand *, bool);
  void walk_match (operand *o, unsigned toplevel_arg, bool, bool);
  bool walk_result (operand *o, bool, operand *);
  void walk_c_expr (c_expr *);

  struct cinfo
    {
      bool expr_p;
      bool cse_p;
      bool force_no_side_effects_p;
      bool force_single_use;
      bool cond_expr_cond_p;
      unsigned long toplevel_msk;
      int result_use_count;
      unsigned same_as;
      capture *c;
    };

  auto_vec<cinfo> info;
  unsigned long force_no_side_effects;
  bool gimple;
};

/* Analyze captures in S.  */

capture_info::capture_info (simplify *s, operand *result, bool gimple_)
{
  gimple = gimple_;

  expr *e;
  if (s->kind == simplify::MATCH)
    {
      force_no_side_effects = -1;
      return;
    }

  force_no_side_effects = 0;
  info.safe_grow_cleared (s->capture_max + 1);
  for (int i = 0; i <= s->capture_max; ++i)
    info[i].same_as = i;

  e = as_a <expr *> (s->match);
  for (unsigned i = 0; i < e->ops.length (); ++i)
    walk_match (e->ops[i], i,
		(i != 0 && *e->operation == COND_EXPR)
		|| *e->operation == TRUTH_ANDIF_EXPR
		|| *e->operation == TRUTH_ORIF_EXPR,
		i == 0
		&& (*e->operation == COND_EXPR
		    || *e->operation == VEC_COND_EXPR));

  walk_result (s->result, false, result);
}

/* Analyze captures in the match expression piece O.  */

void
capture_info::walk_match (operand *o, unsigned toplevel_arg,
			  bool conditional_p, bool cond_expr_cond_p)
{
  if (capture *c = dyn_cast <capture *> (o))
    {
      unsigned where = c->where;
      info[where].toplevel_msk |= 1 << toplevel_arg;
      info[where].force_no_side_effects_p |= conditional_p;
      info[where].cond_expr_cond_p |= cond_expr_cond_p;
      if (!info[where].c)
	info[where].c = c;
      if (!c->what)
	return;
      /* Recurse to exprs and captures.  */
      if (is_a <capture *> (c->what)
	  || is_a <expr *> (c->what))
	walk_match (c->what, toplevel_arg, conditional_p, false);
      /* We need to look past multiple captures to find a captured
	 expression as with conditional converts two captures
	 can be collapsed onto the same expression.  Also collect
	 what captures capture the same thing.  */
      while (c->what && is_a <capture *> (c->what))
	{
	  c = as_a <capture *> (c->what);
	  if (info[c->where].same_as != c->where
	      && info[c->where].same_as != info[where].same_as)
	    fatal_at (c->location, "cannot handle this collapsed capture");
	  info[c->where].same_as = info[where].same_as;
	}
      /* Mark expr (non-leaf) captures and forced single-use exprs.  */
      expr *e;
      if (c->what
	  && (e = dyn_cast <expr *> (c->what)))
	{
	  info[where].expr_p = true;
	  info[where].force_single_use |= e->force_single_use;
	}
    }
  else if (expr *e = dyn_cast <expr *> (o))
    {
      for (unsigned i = 0; i < e->ops.length (); ++i)
	{
	  bool cond_p = conditional_p;
	  bool cond_expr_cond_p = false;
	  if (i != 0 && *e->operation == COND_EXPR)
	    cond_p = true;
	  else if (*e->operation == TRUTH_ANDIF_EXPR
		   || *e->operation == TRUTH_ORIF_EXPR)
	    cond_p = true;
	  if (i == 0
	      && (*e->operation == COND_EXPR
		  || *e->operation == VEC_COND_EXPR))
	    cond_expr_cond_p = true;
	  walk_match (e->ops[i], toplevel_arg, cond_p, cond_expr_cond_p);
	}
    }
  else if (is_a <predicate *> (o))
    {
      /* Mark non-captured leafs toplevel arg for checking.  */
      force_no_side_effects |= 1 << toplevel_arg;
      if (verbose >= 1
	  && !gimple)
	warning_at (o->location,
		    "forcing no side-effects on possibly lost leaf");
    }
  else
    gcc_unreachable ();
}

/* Analyze captures in the result expression piece O.  Return true
   if RESULT was visited in one of the children.  Only visit
   non-if/with children if they are rooted on RESULT.  */

bool
capture_info::walk_result (operand *o, bool conditional_p, operand *result)
{
  if (capture *c = dyn_cast <capture *> (o))
    {
      unsigned where = info[c->where].same_as;
      info[where].result_use_count++;
      /* If we substitute an expression capture we don't know
         which captures this will end up using (well, we don't
	 compute that).  Force the uses to be side-effect free
	 which means forcing the toplevels that reach the
	 expression side-effect free.  */
      if (info[where].expr_p)
	force_no_side_effects |= info[where].toplevel_msk;
      /* Mark CSE capture uses as forced to have no side-effects. */
      if (c->what
	  && is_a <expr *> (c->what))
	{
	  info[where].cse_p = true;
	  walk_result (c->what, true, result);
	}
    }
  else if (expr *e = dyn_cast <expr *> (o))
    {
      id_base *opr = e->operation;
      if (user_id *uid = dyn_cast <user_id *> (opr))
	opr = uid->substitutes[0];
      for (unsigned i = 0; i < e->ops.length (); ++i)
	{
	  bool cond_p = conditional_p;
	  if (i != 0 && *e->operation == COND_EXPR)
	    cond_p = true;
	  else if (*e->operation == TRUTH_ANDIF_EXPR
		   || *e->operation == TRUTH_ORIF_EXPR)
	    cond_p = true;
	  walk_result (e->ops[i], cond_p, result);
	}
    }
  else if (if_expr *e = dyn_cast <if_expr *> (o))
    {
      /* 'if' conditions should be all fine.  */
      if (e->trueexpr == result)
	{
	  walk_result (e->trueexpr, false, result);
	  return true;
	}
      if (e->falseexpr == result)
	{
	  walk_result (e->falseexpr, false, result);
	  return true;
	}
      bool res = false;
      if (is_a <if_expr *> (e->trueexpr)
	  || is_a <with_expr *> (e->trueexpr))
	res |= walk_result (e->trueexpr, false, result);
      if (e->falseexpr
	  && (is_a <if_expr *> (e->falseexpr)
	      || is_a <with_expr *> (e->falseexpr)))
	res |= walk_result (e->falseexpr, false, result);
      return res;
    }
  else if (with_expr *e = dyn_cast <with_expr *> (o))
    {
      bool res = (e->subexpr == result);
      if (res
	  || is_a <if_expr *> (e->subexpr)
	  || is_a <with_expr *> (e->subexpr))
	res |= walk_result (e->subexpr, false, result);
      if (res)
	walk_c_expr (e->with);
      return res;
    }
  else if (c_expr *e = dyn_cast <c_expr *> (o))
    walk_c_expr (e);
  else
    gcc_unreachable ();

  return false;
}

/* Look for captures in the C expr E.  */

void
capture_info::walk_c_expr (c_expr *e)
{
  /* Give up for C exprs mentioning captures not inside TREE_TYPE,
     TREE_REAL_CST, TREE_CODE or a predicate where they cannot
     really escape through.  */
  unsigned p_depth = 0;
  for (unsigned i = 0; i < e->code.length (); ++i)
    {
      const cpp_token *t = &e->code[i];
      const cpp_token *n = i < e->code.length () - 1 ? &e->code[i+1] : NULL;
      id_base *id;
      if (t->type == CPP_NAME
	  && (strcmp ((const char *)CPP_HASHNODE
		      (t->val.node.node)->ident.str, "TREE_TYPE") == 0
	      || strcmp ((const char *)CPP_HASHNODE
			 (t->val.node.node)->ident.str, "TREE_CODE") == 0
	      || strcmp ((const char *)CPP_HASHNODE
			 (t->val.node.node)->ident.str, "TREE_REAL_CST") == 0
	      || ((id = get_operator ((const char *)CPP_HASHNODE
				      (t->val.node.node)->ident.str))
		  && is_a <predicate_id *> (id)))
	  && n->type == CPP_OPEN_PAREN)
	p_depth++;
      else if (t->type == CPP_CLOSE_PAREN
	       && p_depth > 0)
	p_depth--;
      else if (p_depth == 0
	       && t->type == CPP_ATSIGN
	       && (n->type == CPP_NUMBER
		   || n->type == CPP_NAME)
	       && !(n->flags & PREV_WHITE))
	{
	  const char *id;
	  if (n->type == CPP_NUMBER)
	    id = (const char *)n->val.str.text;
	  else
	    id = (const char *)CPP_HASHNODE (n->val.node.node)->ident.str;
	  unsigned where = *e->capture_ids->get(id);
	  info[info[where].same_as].force_no_side_effects_p = true;
	  if (verbose >= 1
	      && !gimple)
	    warning_at (t, "capture escapes");
	}
    }
}


/* Code generation off the decision tree and the refered AST nodes.  */

bool
is_conversion (id_base *op)
{
  return (*op == CONVERT_EXPR
	  || *op == NOP_EXPR
	  || *op == FLOAT_EXPR
	  || *op == FIX_TRUNC_EXPR
	  || *op == VIEW_CONVERT_EXPR);
}

/* Get the type to be used for generating operands of OP from the
   various sources.  */

static const char *
get_operand_type (id_base *op, const char *in_type,
		  const char *expr_type,
		  const char *other_oprnd_type)
{
  /* Generally operands whose type does not match the type of the
     expression generated need to know their types but match and
     thus can fall back to 'other_oprnd_type'.  */
  if (is_conversion (op))
    return other_oprnd_type;
  else if (*op == REALPART_EXPR
	   || *op == IMAGPART_EXPR)
    return other_oprnd_type;
  else if (is_a <operator_id *> (op)
	   && strcmp (as_a <operator_id *> (op)->tcc, "tcc_comparison") == 0)
    return other_oprnd_type;
  else
    {
      /* Otherwise all types should match - choose one in order of
         preference.  */
      if (expr_type)
	return expr_type;
      else if (in_type)
	return in_type;
      else
	return other_oprnd_type;
    }
}

/* Generate transform code for an expression.  */

void
expr::gen_transform (FILE *f, int indent, const char *dest, bool gimple,
		     int depth, const char *in_type, capture_info *cinfo,
		     dt_operand **indexes, bool)
{
  id_base *opr = operation;
  /* When we delay operator substituting during lowering of fors we
     make sure that for code-gen purposes the effects of each substitute
     are the same.  Thus just look at that.  */
  if (user_id *uid = dyn_cast <user_id *> (opr))
    opr = uid->substitutes[0];

  bool conversion_p = is_conversion (opr);
  const char *type = expr_type;
  char optype[64];
  if (type)
    /* If there was a type specification in the pattern use it.  */
    ;
  else if (conversion_p)
    /* For conversions we need to build the expression using the
       outer type passed in.  */
    type = in_type;
  else if (*opr == REALPART_EXPR
	   || *opr == IMAGPART_EXPR)
    {
      /* __real and __imag use the component type of its operand.  */
      sprintf (optype, "TREE_TYPE (TREE_TYPE (ops%d[0]))", depth);
      type = optype;
    }
  else if (is_a <operator_id *> (opr)
	   && !strcmp (as_a <operator_id *> (opr)->tcc, "tcc_comparison"))
    {
      /* comparisons use boolean_type_node (or what gets in), but
         their operands need to figure out the types themselves.  */
      sprintf (optype, "boolean_type_node");
      type = optype;
    }
  else if (*opr == COND_EXPR
	   || *opr == VEC_COND_EXPR)
    {
      /* Conditions are of the same type as their first alternative.  */
      sprintf (optype, "TREE_TYPE (ops%d[1])", depth);
      type = optype;
    }
  else
    {
      /* Other operations are of the same type as their first operand.  */
      sprintf (optype, "TREE_TYPE (ops%d[0])", depth);
      type = optype;
    }
  if (!type)
    fatal_at (location, "cannot determine type of operand");

  fprintf_indent (f, indent, "{\n");
  indent += 2;
  fprintf_indent (f, indent, "tree ops%d[%u], res;\n", depth, ops.length ());
  char op0type[64];
  snprintf (op0type, 64, "TREE_TYPE (ops%d[0])", depth);
  for (unsigned i = 0; i < ops.length (); ++i)
    {
      char dest[32];
      snprintf (dest, 32, "ops%d[%u]", depth, i);
      const char *optype
	= get_operand_type (opr, in_type, expr_type,
			    i == 0 ? NULL : op0type);
      ops[i]->gen_transform (f, indent, dest, gimple, depth + 1, optype,
			     cinfo, indexes,
			     ((!(*opr == COND_EXPR)
			       && !(*opr == VEC_COND_EXPR))
			      || i != 0));
    }

  const char *opr_name;
  if (*operation == CONVERT_EXPR)
    opr_name = "NOP_EXPR";
  else
    opr_name = operation->id;

  if (gimple)
    {
      if (*opr == CONVERT_EXPR)
	{
	  fprintf_indent (f, indent,
			  "if (%s != TREE_TYPE (ops%d[0])\n",
			  type, depth);
	  fprintf_indent (f, indent,
			  "    && !useless_type_conversion_p (%s, TREE_TYPE (ops%d[0])))\n",
			  type, depth);
	  fprintf_indent (f, indent + 2, "{\n");
	  indent += 4;
	}
      /* ???  Building a stmt can fail for various reasons here, seq being
         NULL or the stmt referencing SSA names occuring in abnormal PHIs.
	 So if we fail here we should continue matching other patterns.  */
      fprintf_indent (f, indent, "code_helper tem_code = %s;\n", opr_name);
      fprintf_indent (f, indent, "tree tem_ops[3] = { ");
      for (unsigned i = 0; i < ops.length (); ++i)
	fprintf (f, "ops%d[%u]%s", depth, i,
		 i == ops.length () - 1 ? " };\n" : ", ");
      fprintf_indent (f, indent,
		      "gimple_resimplify%d (lseq, &tem_code, %s, tem_ops, valueize);\n",
		      ops.length (), type);
      fprintf_indent (f, indent,
		      "res = maybe_push_res_to_seq (tem_code, %s, tem_ops, lseq);\n",
		      type);
      fprintf_indent (f, indent,
		      "if (!res) return false;\n");
      if (*opr == CONVERT_EXPR)
	{
	  indent -= 4;
	  fprintf_indent (f, indent, "  }\n");
	  fprintf_indent (f, indent, "else\n");
	  fprintf_indent (f, indent, "  res = ops%d[0];\n", depth);
	}
    }
  else
    {
      if (*opr == CONVERT_EXPR)
	{
	  fprintf_indent (f, indent, "if (TREE_TYPE (ops%d[0]) != %s)\n",
			  depth, type);
	  indent += 2;
	}
      if (opr->kind == id_base::CODE)
	fprintf_indent (f, indent, "res = fold_build%d_loc (loc, %s, %s",
			ops.length(), opr_name, type);
      else
	{
	  fprintf_indent (f, indent, "{\n");
	  fprintf_indent (f, indent, "  tree decl = builtin_decl_implicit (%s);\n",
			  opr_name);
	  fprintf_indent (f, indent, "  if (!decl) return NULL_TREE;\n");
	  fprintf_indent (f, indent, "  res = build_call_expr_loc (loc, "
			  "decl, %d", ops.length());
	}
      for (unsigned i = 0; i < ops.length (); ++i)
	fprintf (f, ", ops%d[%u]", depth, i);
      fprintf (f, ");\n");
      if (opr->kind != id_base::CODE)
	fprintf_indent (f, indent, "}\n");
      if (*opr == CONVERT_EXPR)
	{
	  indent -= 2;
	  fprintf_indent (f, indent, "else\n");
	  fprintf_indent (f, indent, "  res = ops%d[0];\n", depth);
	}
    }
  fprintf_indent (f, indent, "%s = res;\n", dest);
  indent -= 2;
  fprintf_indent (f, indent, "}\n");
}

/* Generate code for a c_expr which is either the expression inside
   an if statement or a sequence of statements which computes a
   result to be stored to DEST.  */

void
c_expr::gen_transform (FILE *f, int indent, const char *dest,
		       bool, int, const char *, capture_info *,
		       dt_operand **, bool)
{
  if (dest && nr_stmts == 1)
    fprintf_indent (f, indent, "%s = ", dest);

  unsigned stmt_nr = 1;
  for (unsigned i = 0; i < code.length (); ++i)
    {
      const cpp_token *token = &code[i];

      /* Replace captures for code-gen.  */
      if (token->type == CPP_ATSIGN)
	{
	  const cpp_token *n = &code[i+1];
	  if ((n->type == CPP_NUMBER
	       || n->type == CPP_NAME)
	      && !(n->flags & PREV_WHITE))
	    {
	      if (token->flags & PREV_WHITE)
		fputc (' ', f);
	      const char *id;
	      if (n->type == CPP_NUMBER)
		id = (const char *)n->val.str.text;
	      else
		id = (const char *)CPP_HASHNODE (n->val.node.node)->ident.str;
	      unsigned *cid = capture_ids->get (id);
	      if (!cid)
		fatal_at (token, "unknown capture id");
	      fprintf (f, "captures[%u]", *cid);
	      ++i;
	      continue;
	    }
	}

      if (token->flags & PREV_WHITE)
	fputc (' ', f);

      if (token->type == CPP_NAME)
	{
	  const char *id = (const char *) NODE_NAME (token->val.node.node);
	  unsigned j;
	  for (j = 0; j < ids.length (); ++j)
	    {
	    if (strcmp (id, ids[j].id) == 0)
	      {
		fprintf (f, "%s", ids[j].oper);
		break;
	      }
	    }
	  if (j < ids.length ())
	    continue;
	}

      /* Output the token as string.  */
      char *tk = (char *)cpp_token_as_text (r, token);
      fputs (tk, f);

      if (token->type == CPP_SEMICOLON)
	{
	  stmt_nr++;
	  fputc ('\n', f);
	  if (dest && stmt_nr == nr_stmts)
	    fprintf_indent (f, indent, "%s = ", dest);
	}
    }
}

/* Generate transform code for a capture.  */

void
capture::gen_transform (FILE *f, int indent, const char *dest, bool gimple,
			int depth, const char *in_type, capture_info *cinfo,
			dt_operand **indexes, bool expand_compares)
{
  if (what && is_a<expr *> (what))
    {
      if (indexes[where] == 0)
	{
	  char buf[20];
	  sprintf (buf, "captures[%u]", where);
	  what->gen_transform (f, indent, buf, gimple, depth, in_type,
			       cinfo, NULL);
	}
    }

  fprintf_indent (f, indent, "%s = captures[%u];\n", dest, where);

  /* ???  Stupid tcc_comparison GENERIC trees in COND_EXPRs.  Deal
     with substituting a capture of that.
     ???  Returning false here will also not allow any other patterns
     to match.  */
  if (gimple && expand_compares
      && cinfo->info[where].cond_expr_cond_p)
    {
      fprintf_indent (f, indent, "if (COMPARISON_CLASS_P (%s))\n", dest);
      fprintf_indent (f, indent, "  {\n");
      fprintf_indent (f, indent, "    if (!seq) return false;\n");
      fprintf_indent (f, indent, "    %s = gimple_build (seq, TREE_CODE (%s),"
		                 " TREE_TYPE (%s), TREE_OPERAND (%s, 0),"
				 " TREE_OPERAND (%s, 1));\n",
				 dest, dest, dest, dest, dest);
      fprintf_indent (f, indent, "  }\n");
    }
}

/* Return the name of the operand representing the decision tree node.
   Use NAME as space to generate it.  */

char *
dt_operand::get_name (char *name)
{
  if (!parent)
    sprintf (name, "t");
  else if (parent->level == 1)
    sprintf (name, "op%u", pos);
  else if (parent->type == dt_node::DT_MATCH)
    return parent->get_name (name);
  else
    sprintf (name, "o%u%u", parent->level, pos);
  return name;
}

/* Fill NAME with the operand name at position POS.  */

void
dt_operand::gen_opname (char *name, unsigned pos)
{
  if (!parent)
    sprintf (name, "op%u", pos);
  else
    sprintf (name, "o%u%u", level, pos);
}

/* Generate matching code for the decision tree operand which is
   a predicate.  */

unsigned
dt_operand::gen_predicate (FILE *f, int indent, const char *opname, bool gimple)
{
  predicate *p = as_a <predicate *> (op);

  if (p->p->matchers.exists ())
    {
      /* If this is a predicate generated from a pattern mangle its
	 name and pass on the valueize hook.  */
      if (gimple)
	fprintf_indent (f, indent, "if (gimple_%s (%s, valueize))\n",
			p->p->id, opname);
      else
	fprintf_indent (f, indent, "if (tree_%s (%s))\n", p->p->id, opname);
    }
  else
    fprintf_indent (f, indent, "if (%s (%s))\n", p->p->id, opname);
  fprintf_indent (f, indent + 2, "{\n");
  return 1;
}

/* Generate matching code for the decision tree operand which is
   a capture-match.  */

unsigned
dt_operand::gen_match_op (FILE *f, int indent, const char *opname)
{
  char match_opname[20];
  match_dop->get_name (match_opname);
  fprintf_indent (f, indent, "if (%s == %s || operand_equal_p (%s, %s, 0))\n",
		  opname, match_opname, opname, match_opname);
  fprintf_indent (f, indent + 2, "{\n");
  return 1;
}

/* Generate GIMPLE matching code for the decision tree operand.  */

unsigned
dt_operand::gen_gimple_expr (FILE *f, int indent)
{
  expr *e = static_cast<expr *> (op);
  id_base *id = e->operation;
  unsigned n_ops = e->ops.length ();

  for (unsigned i = 0; i < n_ops; ++i)
    {
      char child_opname[20];
      gen_opname (child_opname, i);

      if (id->kind == id_base::CODE)
	{
	  if (e->is_generic
	      || *id == REALPART_EXPR || *id == IMAGPART_EXPR
	      || *id == BIT_FIELD_REF || *id == VIEW_CONVERT_EXPR)
	    {
	      /* ???  If this is a memory operation we can't (and should not)
		 match this.  The only sensible operand types are
		 SSA names and invariants.  */
	      fprintf_indent (f, indent,
			      "tree %s = TREE_OPERAND (gimple_assign_rhs1 (def), %i);\n",
			      child_opname, i);
	      fprintf_indent (f, indent,
			      "if ((TREE_CODE (%s) == SSA_NAME\n",
			      child_opname);
	      fprintf_indent (f, indent,
			      "     || is_gimple_min_invariant (%s))\n",
			      child_opname);
	      fprintf_indent (f, indent,
			      "    && (%s = do_valueize (valueize, %s)))\n",
			      child_opname, child_opname);
	      fprintf_indent (f, indent,
			      "  {\n");
	      indent += 4;
	      continue;
	    }
	  else
	    fprintf_indent (f, indent,
			    "tree %s = gimple_assign_rhs%u (def);\n",
			    child_opname, i + 1);
	}
      else
	fprintf_indent (f, indent,
			"tree %s = gimple_call_arg (def, %u);\n",
			child_opname, i);
      fprintf_indent (f, indent,
		      "if ((%s = do_valueize (valueize, %s)))\n",
		      child_opname, child_opname);
      fprintf_indent (f, indent, "  {\n");
      indent += 4;
    }
  /* While the toplevel operands are canonicalized by the caller
     after valueizing operands of sub-expressions we have to
     re-canonicalize operand order.  */
  if (operator_id *code = dyn_cast <operator_id *> (id))
    {
      /* ???  We can't canonicalize tcc_comparison operands here
         because that requires changing the comparison code which
	 we already matched...  */
      if (commutative_tree_code (code->code)
	  || commutative_ternary_tree_code (code->code))
	{
	  char child_opname0[20], child_opname1[20];
	  gen_opname (child_opname0, 0);
	  gen_opname (child_opname1, 1);
	  fprintf_indent (f, indent,
			  "if (tree_swap_operands_p (%s, %s, false))\n",
			  child_opname0, child_opname1);
	  fprintf_indent (f, indent,
			  "  std::swap (%s, %s);\n",
			  child_opname0, child_opname1);
	}
    }

  return n_ops;
}

/* Generate GENERIC matching code for the decision tree operand.  */

unsigned
dt_operand::gen_generic_expr (FILE *f, int indent, const char *opname)
{
  expr *e = static_cast<expr *> (op);
  unsigned n_ops = e->ops.length ();

  for (unsigned i = 0; i < n_ops; ++i)
    {
      char child_opname[20];
      gen_opname (child_opname, i);

      if (e->operation->kind == id_base::CODE)
	fprintf_indent (f, indent, "tree %s = TREE_OPERAND (%s, %u);\n",
			child_opname, opname, i);
      else
	fprintf_indent (f, indent, "tree %s = CALL_EXPR_ARG (%s, %u);\n",
			child_opname, opname, i);
    }

  return 0;
}

/* Generate matching code for the children of the decision tree node.  */

void
dt_node::gen_kids (FILE *f, int indent, bool gimple)
{
  auto_vec<dt_operand *> gimple_exprs;
  auto_vec<dt_operand *> generic_exprs;
  auto_vec<dt_operand *> fns;
  auto_vec<dt_operand *> generic_fns;
  auto_vec<dt_operand *> preds;
  auto_vec<dt_node *> others;

  for (unsigned i = 0; i < kids.length (); ++i)
    {
      if (kids[i]->type == dt_node::DT_OPERAND)
	{
	  dt_operand *op = as_a<dt_operand *> (kids[i]);
	  if (expr *e = dyn_cast <expr *> (op->op))
	    {
	      if (e->ops.length () == 0
		  && (!gimple || !(*e->operation == CONSTRUCTOR)))
		generic_exprs.safe_push (op);
	      else if (e->operation->kind == id_base::FN)
		{
		  if (gimple)
		    fns.safe_push (op);
		  else
		    generic_fns.safe_push (op);
		}
	      else if (e->operation->kind == id_base::PREDICATE)
		preds.safe_push (op);
	      else
		{
		  if (gimple)
		    gimple_exprs.safe_push (op);
		  else
		    generic_exprs.safe_push (op);
		}
	    }
	  else if (op->op->type == operand::OP_PREDICATE)
	    others.safe_push (kids[i]);
	  else
	    gcc_unreachable ();
	}
      else if (kids[i]->type == dt_node::DT_SIMPLIFY)
	others.safe_push (kids[i]);
      else if (kids[i]->type == dt_node::DT_MATCH
	       || kids[i]->type == dt_node::DT_TRUE)
	{
	  /* A DT_TRUE operand serves as a barrier - generate code now
	     for what we have collected sofar.
	     Like DT_TRUE, DT_MATCH serves as a barrier as it can cause
	     dependent matches to get out-of-order.  Generate code now
	     for what we have collected sofar.  */
	  gen_kids_1 (f, indent, gimple, gimple_exprs, generic_exprs,
		      fns, generic_fns, preds, others);
	  /* And output the true operand itself.  */
	  kids[i]->gen (f, indent, gimple);
	  gimple_exprs.truncate (0);
	  generic_exprs.truncate (0);
	  fns.truncate (0);
	  generic_fns.truncate (0);
	  preds.truncate (0);
	  others.truncate (0);
	}
      else
	gcc_unreachable ();
    }

  /* Generate code for the remains.  */
  gen_kids_1 (f, indent, gimple, gimple_exprs, generic_exprs,
	      fns, generic_fns, preds, others);
}

/* Generate matching code for the children of the decision tree node.  */

void
dt_node::gen_kids_1 (FILE *f, int indent, bool gimple,
		     vec<dt_operand *> gimple_exprs,
		     vec<dt_operand *> generic_exprs,
		     vec<dt_operand *> fns,
		     vec<dt_operand *> generic_fns,
		     vec<dt_operand *> preds,
		     vec<dt_node *> others)
{
  char buf[128];
  char *kid_opname = buf;

  unsigned exprs_len = gimple_exprs.length ();
  unsigned gexprs_len = generic_exprs.length ();
  unsigned fns_len = fns.length ();
  unsigned gfns_len = generic_fns.length ();

  if (exprs_len || fns_len || gexprs_len || gfns_len)
    {
      if (exprs_len)
	gimple_exprs[0]->get_name (kid_opname);
      else if (fns_len)
	fns[0]->get_name (kid_opname);
      else if (gfns_len)
	generic_fns[0]->get_name (kid_opname);
      else
	generic_exprs[0]->get_name (kid_opname);

      fprintf_indent (f, indent, "switch (TREE_CODE (%s))\n", kid_opname);
      fprintf_indent (f, indent, "  {\n");
      indent += 2;
    }

  if (exprs_len || fns_len)
    {
      fprintf_indent (f, indent,
		      "case SSA_NAME:\n");
      fprintf_indent (f, indent,
		      "  if (do_valueize (valueize, %s) != NULL_TREE)\n",
		      kid_opname);
      fprintf_indent (f, indent,
		      "    {\n");
      fprintf_indent (f, indent,
		      "      gimple def_stmt = SSA_NAME_DEF_STMT (%s);\n",
		      kid_opname);

      indent += 6;
      if (exprs_len)
	{
	  fprintf_indent (f, indent,
			  "if (gassign *def = dyn_cast <gassign *> (def_stmt))\n");
	  fprintf_indent (f, indent,
			  "  switch (gimple_assign_rhs_code (def))\n");
	  indent += 4;
	  fprintf_indent (f, indent, "{\n");
	  for (unsigned i = 0; i < exprs_len; ++i)
	    {
	      expr *e = as_a <expr *> (gimple_exprs[i]->op);
	      id_base *op = e->operation;
	      if (*op == CONVERT_EXPR || *op == NOP_EXPR)
		fprintf_indent (f, indent, "CASE_CONVERT:\n");
	      else
		fprintf_indent (f, indent, "case %s:\n", op->id);
	      fprintf_indent (f, indent, "  {\n");
	      gimple_exprs[i]->gen (f, indent + 4, true);
	      fprintf_indent (f, indent, "    break;\n");
	      fprintf_indent (f, indent, "  }\n");
	    }
	  fprintf_indent (f, indent, "default:;\n");
	  fprintf_indent (f, indent, "}\n");
	  indent -= 4;
	}

      if (fns_len)
	{
	  fprintf_indent (f, indent,
			  "%sif (gimple_call_builtin_p (def_stmt, BUILT_IN_NORMAL))\n",
			  exprs_len ? "else " : "");
	  fprintf_indent (f, indent,
			  "  {\n");
	  fprintf_indent (f, indent,
			  "    gcall *def = as_a <gcall *> (def_stmt);\n");
	  fprintf_indent (f, indent,
			  "    tree fndecl = gimple_call_fndecl (def);\n");
	  fprintf_indent (f, indent,
			  "    switch (DECL_FUNCTION_CODE (fndecl))\n");
	  fprintf_indent (f, indent,
			  "      {\n");

	  indent += 6;
	  for (unsigned i = 0; i < fns_len; ++i)
	    {
	      expr *e = as_a <expr *>(fns[i]->op);
	      fprintf_indent (f, indent, "case %s:\n", e->operation->id);
	      fprintf_indent (f, indent, "  {\n");
	      fns[i]->gen (f, indent + 4, true);
	      fprintf_indent (f, indent, "    break;\n");
	      fprintf_indent (f, indent, "  }\n");
	    }

	  fprintf_indent (f, indent, "default:;\n");
	  fprintf_indent (f, indent, "}\n");
	  indent -= 6;
	  fprintf_indent (f, indent, "  }\n");
	}

      indent -= 6;
      fprintf_indent (f, indent, "    }\n");
      fprintf_indent (f, indent, "  break;\n");
    }

  for (unsigned i = 0; i < generic_exprs.length (); ++i)
    {
      expr *e = as_a <expr *>(generic_exprs[i]->op);
      id_base *op = e->operation;
      if (*op == CONVERT_EXPR || *op == NOP_EXPR)
	fprintf_indent (f, indent, "CASE_CONVERT:\n");
      else
	fprintf_indent (f, indent, "case %s:\n", op->id);
      fprintf_indent (f, indent, "  {\n");
      generic_exprs[i]->gen (f, indent + 4, gimple);
      fprintf_indent (f, indent, "    break;\n");
      fprintf_indent (f, indent, "  }\n");
    }

  if (gfns_len)
    {
      fprintf_indent (f, indent,
		      "case CALL_EXPR:\n");
      fprintf_indent (f, indent,
		      "  {\n");
      fprintf_indent (f, indent,
		      "    tree fndecl = get_callee_fndecl (%s);\n",
		      kid_opname);
      fprintf_indent (f, indent,
		      "    if (fndecl && DECL_BUILT_IN_CLASS (fndecl) == BUILT_IN_NORMAL)\n");
      fprintf_indent (f, indent,
		      "      switch (DECL_FUNCTION_CODE (fndecl))\n");
      fprintf_indent (f, indent,
		      "        {\n");
      indent += 8;

      for (unsigned j = 0; j < generic_fns.length (); ++j)
	{
	  expr *e = as_a <expr *>(generic_fns[j]->op);
	  gcc_assert (e->operation->kind == id_base::FN);

	  fprintf_indent (f, indent, "case %s:\n", e->operation->id);
	  fprintf_indent (f, indent, "  {\n");
	  generic_fns[j]->gen (f, indent + 4, false);
	  fprintf_indent (f, indent, "    break;\n");
	  fprintf_indent (f, indent, "  }\n");
	}

      indent -= 8;
      fprintf_indent (f, indent, "          default:;\n");
      fprintf_indent (f, indent, "        }\n");
      fprintf_indent (f, indent, "    break;\n");
      fprintf_indent (f, indent, "  }\n");
    }

  /* Close switch (TREE_CODE ()).  */
  if (exprs_len || fns_len || gexprs_len || gfns_len)
    {
      indent -= 4;
      fprintf_indent (f, indent, "    default:;\n");
      fprintf_indent (f, indent, "    }\n");
    }

  for (unsigned i = 0; i < preds.length (); ++i)
    {
      expr *e = as_a <expr *> (preds[i]->op);
      predicate_id *p = as_a <predicate_id *> (e->operation);
      preds[i]->get_name (kid_opname);
      fprintf_indent (f, indent, "tree %s_pops[%d];\n", kid_opname, p->nargs);
      fprintf_indent (f, indent, "if (%s_%s (%s, %s_pops%s))\n",
	       gimple ? "gimple" : "tree",
	       p->id, kid_opname, kid_opname,
	       gimple ? ", valueize" : "");
      fprintf_indent (f, indent, "  {\n");
      for (int j = 0; j < p->nargs; ++j)
	{
	  char child_opname[20];
	  preds[i]->gen_opname (child_opname, j);
	  fprintf_indent (f, indent + 4, "tree %s = %s_pops[%d];\n",
			  child_opname, kid_opname, j);
	}
      preds[i]->gen_kids (f, indent + 4, gimple);
      fprintf (f, "}\n");
    }

  for (unsigned i = 0; i < others.length (); ++i)
    others[i]->gen (f, indent, gimple);
}

/* Generate matching code for the decision tree operand.  */

void
dt_operand::gen (FILE *f, int indent, bool gimple)
{
  char opname[20];
  get_name (opname);

  unsigned n_braces = 0;

  if (type == DT_OPERAND)
    switch (op->type)
      {
	case operand::OP_PREDICATE:
	  n_braces = gen_predicate (f, indent, opname, gimple);
	  break;

	case operand::OP_EXPR:
	  if (gimple)
	    n_braces = gen_gimple_expr (f, indent);
	  else
	    n_braces = gen_generic_expr (f, indent, opname);
	  break;

	default:
	  gcc_unreachable ();
      }
  else if (type == DT_TRUE)
    ;
  else if (type == DT_MATCH)
    n_braces = gen_match_op (f, indent, opname);
  else
    gcc_unreachable ();

  indent += 4 * n_braces;
  gen_kids (f, indent, gimple);

  for (unsigned i = 0; i < n_braces; ++i)
    {
      indent -= 4;
      if (indent < 0)
	indent = 0;
      fprintf_indent (f, indent, "  }\n");
    }
}


/* Generate code for the '(if ...)', '(with ..)' and actual transform
   step of a '(simplify ...)' or '(match ...)'.  This handles everything
   that is not part of the decision tree (simplify->match).
   Main recursive worker.  */

void
dt_simplify::gen_1 (FILE *f, int indent, bool gimple, operand *result)
{
  if (result)
    {
      if (with_expr *w = dyn_cast <with_expr *> (result))
	{
	  fprintf_indent (f, indent, "{\n");
	  indent += 4;
	  output_line_directive (f, w->location);
	  w->with->gen_transform (f, indent, NULL, true, 1, "type", NULL);
	  gen_1 (f, indent, gimple, w->subexpr);
	  indent -= 4;
	  fprintf_indent (f, indent, "}\n");
	  return;
	}
      else if (if_expr *ife = dyn_cast <if_expr *> (result))
	{
	  output_line_directive (f, ife->location);
	  fprintf_indent (f, indent, "if (");
	  ife->cond->gen_transform (f, indent, NULL, true, 1, "type", NULL);
	  fprintf (f, ")\n");
	  fprintf_indent (f, indent + 2, "{\n");
	  indent += 4;
	  gen_1 (f, indent, gimple, ife->trueexpr);
	  indent -= 4;
	  fprintf_indent (f, indent + 2, "}\n");
	  if (ife->falseexpr)
	    {
	      fprintf_indent (f, indent, "else\n");
	      fprintf_indent (f, indent + 2, "{\n");
	      indent += 4;
	      gen_1 (f, indent, gimple, ife->falseexpr);
	      indent -= 4;
	      fprintf_indent (f, indent + 2, "}\n");
	    }
	  return;
	}
    }

  /* Analyze captures and perform early-outs on the incoming arguments
     that cover cases we cannot handle.  */
  capture_info cinfo (s, result, gimple);
  if (s->kind == simplify::SIMPLIFY)
    {
      if (!gimple)
	{
	  for (unsigned i = 0; i < as_a <expr *> (s->match)->ops.length (); ++i)
	    if (cinfo.force_no_side_effects & (1 << i))
	      {
		fprintf_indent (f, indent,
				"if (TREE_SIDE_EFFECTS (op%d)) return NULL_TREE;\n",
				i);
		if (verbose >= 1)
		  warning_at (as_a <expr *> (s->match)->ops[i]->location,
			      "forcing toplevel operand to have no "
			      "side-effects");
	      }
	  for (int i = 0; i <= s->capture_max; ++i)
	    if (cinfo.info[i].cse_p)
	      ;
	    else if (cinfo.info[i].force_no_side_effects_p
		     && (cinfo.info[i].toplevel_msk
			 & cinfo.force_no_side_effects) == 0)
	      {
		fprintf_indent (f, indent,
				"if (TREE_SIDE_EFFECTS (captures[%d])) "
				"return NULL_TREE;\n", i);
		if (verbose >= 1)
		  warning_at (cinfo.info[i].c->location,
			      "forcing captured operand to have no "
			      "side-effects");
	      }
	    else if ((cinfo.info[i].toplevel_msk
		      & cinfo.force_no_side_effects) != 0)
	      /* Mark capture as having no side-effects if we had to verify
		 that via forced toplevel operand checks.  */
	      cinfo.info[i].force_no_side_effects_p = true;
	}
      if (gimple)
	{
	  /* Force single-use restriction by only allowing simple
	     results via setting seq to NULL.  */
	  fprintf_indent (f, indent, "gimple_seq *lseq = seq;\n");
	  bool first_p = true;
	  for (int i = 0; i <= s->capture_max; ++i)
	    if (cinfo.info[i].force_single_use)
	      {
		if (first_p)
		  {
		    fprintf_indent (f, indent, "if (lseq\n");
		    fprintf_indent (f, indent, "    && (");
		    first_p = false;
		  }
		else
		  {
		    fprintf (f, "\n");
		    fprintf_indent (f, indent, "        || ");
		  }
		fprintf (f, "!single_use (captures[%d])", i);
	      }
	  if (!first_p)
	    {
	      fprintf (f, "))\n");
	      fprintf_indent (f, indent, "  lseq = NULL;\n");
	    }
	}
    }

  fprintf_indent (f, indent, "if (dump_file && (dump_flags & TDF_DETAILS)) "
	   "fprintf (dump_file, \"Applying pattern ");
  output_line_directive (f,
			 result ? result->location : s->match->location, true);
  fprintf (f, ", %%s:%%d\\n\", __FILE__, __LINE__);\n");

  if (!result)
    {
      /* If there is no result then this is a predicate implementation.  */
      fprintf_indent (f, indent, "return true;\n");
    }
  else if (gimple)
    {
      /* For GIMPLE simply drop NON_LVALUE_EXPR (which only appears
         in outermost position).  */
      if (result->type == operand::OP_EXPR
	  && *as_a <expr *> (result)->operation == NON_LVALUE_EXPR)
	result = as_a <expr *> (result)->ops[0];
      if (result->type == operand::OP_EXPR)
	{
	  expr *e = as_a <expr *> (result);
	  id_base *opr = e->operation;
	  bool is_predicate = false;
	  /* When we delay operator substituting during lowering of fors we
	     make sure that for code-gen purposes the effects of each substitute
	     are the same.  Thus just look at that.  */
	  if (user_id *uid = dyn_cast <user_id *> (opr))
	    opr = uid->substitutes[0];
	  else if (is_a <predicate_id *> (opr))
	    is_predicate = true;
	  if (!is_predicate)
	    fprintf_indent (f, indent, "*res_code = %s;\n",
			    *e->operation == CONVERT_EXPR
			    ? "NOP_EXPR" : e->operation->id);
	  for (unsigned j = 0; j < e->ops.length (); ++j)
	    {
	      char dest[32];
	      snprintf (dest, 32, "res_ops[%d]", j);
	      const char *optype
		= get_operand_type (opr,
				    "type", e->expr_type,
				    j == 0 ? NULL : "TREE_TYPE (res_ops[0])");
	      /* We need to expand GENERIC conditions we captured from
	         COND_EXPRs.  */
	      bool expand_generic_cond_exprs_p
	        = (!is_predicate
		   /* But avoid doing that if the GENERIC condition is
		      valid - which it is in the first operand of COND_EXPRs
		      and VEC_COND_EXRPs.  */
		   && ((!(*opr == COND_EXPR)
			&& !(*opr == VEC_COND_EXPR))
		       || j != 0));
	      e->ops[j]->gen_transform (f, indent, dest, true, 1, optype,
					&cinfo,
					indexes, expand_generic_cond_exprs_p);
	    }

	  /* Re-fold the toplevel result.  It's basically an embedded
	     gimple_build w/o actually building the stmt.  */
	  if (!is_predicate)
	    fprintf_indent (f, indent,
			    "gimple_resimplify%d (lseq, res_code, type, "
			    "res_ops, valueize);\n", e->ops.length ());
	}
      else if (result->type == operand::OP_CAPTURE
	       || result->type == operand::OP_C_EXPR)
	{
	  result->gen_transform (f, indent, "res_ops[0]", true, 1, "type",
				 &cinfo, indexes, false);
	  fprintf_indent (f, indent, "*res_code = TREE_CODE (res_ops[0]);\n");
	  if (is_a <capture *> (result)
	      && cinfo.info[as_a <capture *> (result)->where].cond_expr_cond_p)
	    {
	      /* ???  Stupid tcc_comparison GENERIC trees in COND_EXPRs.  Deal
		 with substituting a capture of that.  */
	      fprintf_indent (f, indent,
			      "if (COMPARISON_CLASS_P (res_ops[0]))\n");
	      fprintf_indent (f, indent,
			      "  {\n");
	      fprintf_indent (f, indent,
			      "    tree tem = res_ops[0];\n");
	      fprintf_indent (f, indent,
			      "    res_ops[0] = TREE_OPERAND (tem, 0);\n");
	      fprintf_indent (f, indent,
			      "    res_ops[1] = TREE_OPERAND (tem, 1);\n");
	      fprintf_indent (f, indent,
			      "  }\n");
	    }
	}
      else
	gcc_unreachable ();
      fprintf_indent (f, indent, "return true;\n");
    }
  else /* GENERIC */
    {
      bool is_predicate = false;
      if (result->type == operand::OP_EXPR)
	{
	  expr *e = as_a <expr *> (result);
	  id_base *opr = e->operation;
	  /* When we delay operator substituting during lowering of fors we
	     make sure that for code-gen purposes the effects of each substitute
	     are the same.  Thus just look at that.  */
	  if (user_id *uid = dyn_cast <user_id *> (opr))
	    opr = uid->substitutes[0];
	  else if (is_a <predicate_id *> (opr))
	    is_predicate = true;
	  /* Search for captures used multiple times in the result expression
	     and dependent on TREE_SIDE_EFFECTS emit a SAVE_EXPR.  */
	  if (!is_predicate)
	    for (int i = 0; i < s->capture_max + 1; ++i)
	      {
		if (cinfo.info[i].same_as != (unsigned)i)
		  continue;
		if (!cinfo.info[i].force_no_side_effects_p
		    && cinfo.info[i].result_use_count > 1)
		  {
		    fprintf_indent (f, indent,
				    "if (TREE_SIDE_EFFECTS (captures[%d]))\n",
				    i);
		    fprintf_indent (f, indent,
				    "  captures[%d] = save_expr (captures[%d]);\n",
				    i, i);
		  }
	      }
	  for (unsigned j = 0; j < e->ops.length (); ++j)
	    {
	      char dest[32];
	      if (is_predicate)
		snprintf (dest, 32, "res_ops[%d]", j);
	      else
		{
		  fprintf_indent (f, indent, "tree res_op%d;\n", j);
		  snprintf (dest, 32, "res_op%d", j);
		}
	      const char *optype
	        = get_operand_type (opr,
				    "type", e->expr_type,
				    j == 0
				    ? NULL : "TREE_TYPE (res_op0)");
	      e->ops[j]->gen_transform (f, indent, dest, false, 1, optype,
					&cinfo, indexes);
	    }
	  if (is_predicate)
	    fprintf_indent (f, indent, "return true;\n");
	  else
	    {
	      fprintf_indent (f, indent, "tree res;\n");
	      /* Re-fold the toplevel result.  Use non_lvalue to
	         build NON_LVALUE_EXPRs so they get properly
		 ignored when in GIMPLE form.  */
	      if (*opr == NON_LVALUE_EXPR)
		fprintf_indent (f, indent,
				"res = non_lvalue_loc (loc, res_op0);\n");
	      else
		{
		  if (is_a <operator_id *> (opr))
		    fprintf_indent (f, indent,
				    "res = fold_build%d_loc (loc, %s, type",
				    e->ops.length (),
				    *e->operation == CONVERT_EXPR
				    ? "NOP_EXPR" : e->operation->id);
		  else
		    {
		      fprintf_indent (f, indent,
				      "{\n");
		      fprintf_indent (f, indent,
				      "  tree decl = builtin_decl_implicit (%s);\n",
				      e->operation->id);
		      fprintf_indent (f, indent,
				      "  if (!decl) return NULL_TREE;\n");
		      fprintf_indent (f, indent,
				      "  res = build_call_expr_loc "
				      "(loc, decl, %d",
				      e->ops.length());
		    }
		  for (unsigned j = 0; j < e->ops.length (); ++j)
		    fprintf (f, ", res_op%d", j);
		  fprintf (f, ");\n");
		  if (!is_a <operator_id *> (opr))
		    fprintf_indent (f, indent, "}\n");
		}
	    }
	}
      else if (result->type == operand::OP_CAPTURE
	       || result->type == operand::OP_C_EXPR)

	{
	  fprintf_indent (f, indent, "tree res;\n");
	  result->gen_transform (f, indent, "res", false, 1, "type",
				    &cinfo, indexes);
	}
      else
	gcc_unreachable ();
      if (!is_predicate)
	{
	  /* Search for captures not used in the result expression and dependent
	     on TREE_SIDE_EFFECTS emit omit_one_operand.  */
	  for (int i = 0; i < s->capture_max + 1; ++i)
	    {
	      if (cinfo.info[i].same_as != (unsigned)i)
		continue;
	      if (!cinfo.info[i].force_no_side_effects_p
		  && !cinfo.info[i].expr_p
		  && cinfo.info[i].result_use_count == 0)
		{
		  fprintf_indent (f, indent,
				  "if (TREE_SIDE_EFFECTS (captures[%d]))\n",
				  i);
		  fprintf_indent (f, indent + 2,
				  "res = build2_loc (loc, COMPOUND_EXPR, type, "
				  "fold_ignored_result (captures[%d]), res);\n",
				  i);
		}
	    }
	  fprintf_indent (f, indent, "return res;\n");
	}
    }
}

/* Generate code for the '(if ...)', '(with ..)' and actual transform
   step of a '(simplify ...)' or '(match ...)'.  This handles everything
   that is not part of the decision tree (simplify->match).  */

void
dt_simplify::gen (FILE *f, int indent, bool gimple)
{
  fprintf_indent (f, indent, "{\n");
  indent += 2;
  output_line_directive (f,
			 s->result ? s->result->location : s->match->location);
  if (s->capture_max >= 0)
    {
      char opname[20];
      fprintf_indent (f, indent, "tree captures[%u] ATTRIBUTE_UNUSED = { %s",
		      s->capture_max + 1, indexes[0]->get_name (opname));

      for (int i = 1; i <= s->capture_max; ++i)
	fprintf (f, ", %s", indexes[i]->get_name (opname));
      fprintf (f, " };\n");
    }

  /* If we have a split-out function for the actual transform, call it.  */
  if (info && info->fname)
    {
      if (gimple)
	{
	  fprintf_indent (f, indent, "if (%s (res_code, res_ops, seq, "
			  "valueize, type, captures", info->fname);
	  for (unsigned i = 0; i < s->for_subst_vec.length (); ++i)
	    fprintf (f, ", %s", s->for_subst_vec[i].second->id);
	  fprintf (f, "))\n");
	  fprintf_indent (f, indent, "  return true;\n");
	}
      else
	{
	  fprintf_indent (f, indent, "tree res = %s (loc, type",
			  info->fname);
	  for (unsigned i = 0; i < as_a <expr *> (s->match)->ops.length (); ++i)
	    fprintf (f, ", op%d", i);
	  fprintf (f, ", captures");
	  for (unsigned i = 0; i < s->for_subst_vec.length (); ++i)
	    fprintf (f, ", %s", s->for_subst_vec[i].second->id);
	  fprintf (f, ");\n");
	  fprintf_indent (f, indent, "if (res) return res;\n");
	}
    }
  else
    {
      for (unsigned i = 0; i < s->for_subst_vec.length (); ++i)
	{
	  if (is_a <operator_id *> (s->for_subst_vec[i].second))
	    fprintf_indent (f, indent, "enum tree_code %s = %s;\n",
			    s->for_subst_vec[i].first->id,
			    s->for_subst_vec[i].second->id);
	  else if (is_a <fn_id *> (s->for_subst_vec[i].second))
	    fprintf_indent (f, indent, "enum built_in_function %s = %s;\n",
			    s->for_subst_vec[i].first->id,
			    s->for_subst_vec[i].second->id);
	  else
	    gcc_unreachable ();
	}
      gen_1 (f, indent, gimple, s->result);
    }

  indent -= 2;
  fprintf_indent (f, indent, "}\n");
}


/* Hash function for finding equivalent transforms.  */

hashval_t
sinfo_hashmap_traits::hash (const key_type &v)
{
  /* Only bother to compare those originating from the same source pattern.  */
  return v->s->result->location;
}

/* Compare function for finding equivalent transforms.  */

static bool
compare_op (operand *o1, simplify *s1, operand *o2, simplify *s2)
{
  if (o1->type != o2->type)
    return false;

  switch (o1->type)
    {
    case operand::OP_IF:
      {
	if_expr *if1 = as_a <if_expr *> (o1);
	if_expr *if2 = as_a <if_expr *> (o2);
	/* ???  Properly compare c-exprs.  */
	if (if1->cond != if2->cond)
	  return false;
	if (!compare_op (if1->trueexpr, s1, if2->trueexpr, s2))
	  return false;
	if (if1->falseexpr != if2->falseexpr
	    || (if1->falseexpr
		&& !compare_op (if1->falseexpr, s1, if2->falseexpr, s2)))
	  return false;
	return true;
      }
    case operand::OP_WITH:
      {
	with_expr *with1 = as_a <with_expr *> (o1);
	with_expr *with2 = as_a <with_expr *> (o2);
	if (with1->with != with2->with)
	  return false;
	return compare_op (with1->subexpr, s1, with2->subexpr, s2);
      }
    default:;
    }

  /* We've hit a result.  Time to compare capture-infos - this is required
     in addition to the conservative pointer-equivalency of the result IL.  */
  capture_info cinfo1 (s1, o1, true);
  capture_info cinfo2 (s2, o2, true);

  if (cinfo1.force_no_side_effects != cinfo2.force_no_side_effects
      || cinfo1.info.length () != cinfo2.info.length ())
    return false;

  for (unsigned i = 0; i < cinfo1.info.length (); ++i)
    {
      if (cinfo1.info[i].expr_p != cinfo2.info[i].expr_p
	  || cinfo1.info[i].cse_p != cinfo2.info[i].cse_p
	  || (cinfo1.info[i].force_no_side_effects_p
	      != cinfo2.info[i].force_no_side_effects_p)
	  || cinfo1.info[i].force_single_use != cinfo2.info[i].force_single_use
	  || cinfo1.info[i].cond_expr_cond_p != cinfo2.info[i].cond_expr_cond_p
	  /* toplevel_msk is an optimization */
	  || cinfo1.info[i].result_use_count != cinfo2.info[i].result_use_count
	  || cinfo1.info[i].same_as != cinfo2.info[i].same_as
	  /* the pointer back to the capture is for diagnostics only */)
	return false;
    }

  /* ???  Deep-compare the actual result.  */
  return o1 == o2;
}

bool
sinfo_hashmap_traits::equal_keys (const key_type &v,
				  const key_type &candidate)
{
  return compare_op (v->s->result, v->s, candidate->s->result, candidate->s);
}


/* Main entry to generate code for matching GIMPLE IL off the decision
   tree.  */

void
decision_tree::gen (FILE *f, bool gimple)
{
  sinfo_map_t si;

  root->analyze (si);

  fprintf (stderr, "%s decision tree has %u leafs, maximum depth %u and "
	   "a total number of %u nodes\n",
	   gimple ? "GIMPLE" : "GENERIC", 
	   root->num_leafs, root->max_level, root->total_size);

  /* First split out the transform part of equal leafs.  */
  unsigned rcnt = 0;
  unsigned fcnt = 1;
  for (sinfo_map_t::iterator iter = si.begin ();
       iter != si.end (); ++iter)
    {
      sinfo *s = (*iter).second;
      /* Do not split out single uses.  */
      if (s->cnt <= 1)
	continue;

      rcnt += s->cnt - 1;
      if (verbose >= 1)
	{
	  fprintf (stderr, "found %u uses of", s->cnt);
	  output_line_directive (stderr, s->s->s->result->location);
	}

      /* Generate a split out function with the leaf transform code.  */
      s->fname = xasprintf ("%s_simplify_%u", gimple ? "gimple" : "generic",
			    fcnt++);
      if (gimple)
	fprintf (f, "\nstatic bool\n"
		 "%s (code_helper *res_code, tree *res_ops,\n"
		 "                 gimple_seq *seq, tree (*valueize)(tree) "
		 "ATTRIBUTE_UNUSED,\n"
		 "                 tree ARG_UNUSED (type), tree *ARG_UNUSED "
		 "(captures)\n",
		 s->fname);
      else
	{
	  fprintf (f, "\nstatic tree\n"
		   "%s (location_t ARG_UNUSED (loc), tree ARG_UNUSED (type),\n",
		   (*iter).second->fname);
	  for (unsigned i = 0;
	       i < as_a <expr *>(s->s->s->match)->ops.length (); ++i)
	    fprintf (f, " tree ARG_UNUSED (op%d),", i);
	  fprintf (f, " tree *captures\n");
	}
      for (unsigned i = 0; i < s->s->s->for_subst_vec.length (); ++i)
	{
	  if (is_a <operator_id *> (s->s->s->for_subst_vec[i].second))
	    fprintf (f, ", enum tree_code ARG_UNUSED (%s)",
		     s->s->s->for_subst_vec[i].first->id);
	  else if (is_a <fn_id *> (s->s->s->for_subst_vec[i].second))
	    fprintf (f, ", enum built_in_function ARG_UNUSED (%s)",
		     s->s->s->for_subst_vec[i].first->id);
	}

      fprintf (f, ")\n{\n");
      s->s->gen_1 (f, 2, gimple, s->s->s->result);
      if (gimple)
	fprintf (f, "  return false;\n");
      else
	fprintf (f, "  return NULL_TREE;\n");
      fprintf (f, "}\n");
    }
  fprintf (stderr, "removed %u duplicate tails\n", rcnt);

  for (unsigned n = 1; n <= 3; ++n)
    {
      /* First generate split-out functions.  */
      for (unsigned i = 0; i < root->kids.length (); i++)
	{
	  dt_operand *dop = static_cast<dt_operand *>(root->kids[i]);
	  expr *e = static_cast<expr *>(dop->op);
	  if (e->ops.length () != n
	      /* Builtin simplifications are somewhat premature on
		 GENERIC.  The following drops patterns with outermost
		 calls.  It's easy to emit overloads for function code
		 though if necessary.  */
	      || (!gimple
		  && e->operation->kind != id_base::CODE))
	    continue;

	  if (gimple)
	    fprintf (f, "\nstatic bool\n"
		     "gimple_simplify_%s (code_helper *res_code, tree *res_ops,\n"
		     "                 gimple_seq *seq, tree (*valueize)(tree) "
		     "ATTRIBUTE_UNUSED,\n"
		     "                 code_helper ARG_UNUSED (code), tree "
		     "ARG_UNUSED (type)\n",
		     e->operation->id);
	  else
	    fprintf (f, "\nstatic tree\n"
		     "generic_simplify_%s (location_t ARG_UNUSED (loc), enum "
		     "tree_code ARG_UNUSED (code), tree ARG_UNUSED (type)",
		     e->operation->id);
	  for (unsigned i = 0; i < n; ++i)
	    fprintf (f, ", tree op%d", i);
	  fprintf (f, ")\n");
	  fprintf (f, "{\n");
	  dop->gen_kids (f, 2, gimple);
	  if (gimple)
	    fprintf (f, "  return false;\n");
	  else
	    fprintf (f, "  return NULL_TREE;\n");
	  fprintf (f, "}\n");
	}

      /* Then generate the main entry with the outermost switch and
         tail-calls to the split-out functions.  */
      if (gimple)
	fprintf (f, "\nstatic bool\n"
		 "gimple_simplify (code_helper *res_code, tree *res_ops,\n"
		 "                 gimple_seq *seq, tree (*valueize)(tree),\n"
		 "                 code_helper code, tree type");
      else
	fprintf (f, "\ntree\n"
		 "generic_simplify (location_t loc, enum tree_code code, "
		 "tree type ATTRIBUTE_UNUSED");
      for (unsigned i = 0; i < n; ++i)
	fprintf (f, ", tree op%d", i);
      fprintf (f, ")\n");
      fprintf (f, "{\n");

      if (gimple)
	fprintf (f, "  switch (code.get_rep())\n"
		 "    {\n");
      else
	fprintf (f, "  switch (code)\n"
		 "    {\n");
      for (unsigned i = 0; i < root->kids.length (); i++)
	{
	  dt_operand *dop = static_cast<dt_operand *>(root->kids[i]);
	  expr *e = static_cast<expr *>(dop->op);
	  if (e->ops.length () != n
	      /* Builtin simplifications are somewhat premature on
		 GENERIC.  The following drops patterns with outermost
		 calls.  It's easy to emit overloads for function code
		 though if necessary.  */
	      || (!gimple
		  && e->operation->kind != id_base::CODE))
	    continue;

	  if (*e->operation == CONVERT_EXPR
	      || *e->operation == NOP_EXPR)
	    fprintf (f, "    CASE_CONVERT:\n");
	  else
	    fprintf (f, "    case %s%s:\n",
		     is_a <fn_id *> (e->operation) ? "-" : "",
		     e->operation->id);
	  if (gimple)
	    fprintf (f, "      return gimple_simplify_%s (res_code, res_ops, "
		     "seq, valueize, code, type", e->operation->id);
	  else
	    fprintf (f, "      return generic_simplify_%s (loc, code, type",
		     e->operation->id);
	  for (unsigned i = 0; i < n; ++i)
	    fprintf (f, ", op%d", i);
	  fprintf (f, ");\n");
	}
      fprintf (f,       "    default:;\n"
	                "    }\n");

      if (gimple)
	fprintf (f, "  return false;\n");
      else
	fprintf (f, "  return NULL_TREE;\n");
      fprintf (f, "}\n");
    }
}

/* Output code to implement the predicate P from the decision tree DT.  */

void
write_predicate (FILE *f, predicate_id *p, decision_tree &dt, bool gimple)
{
  fprintf (f, "\nbool\n"
	   "%s%s (tree t%s%s)\n"
	   "{\n", gimple ? "gimple_" : "tree_", p->id,
	   p->nargs > 0 ? ", tree *res_ops" : "",
	   gimple ? ", tree (*valueize)(tree)" : "");
  /* Conveniently make 'type' available.  */
  fprintf_indent (f, 2, "tree type = TREE_TYPE (t);\n");

  if (!gimple)
    fprintf_indent (f, 2, "if (TREE_SIDE_EFFECTS (t)) return false;\n");
  dt.root->gen_kids (f, 2, gimple);

  fprintf_indent (f, 2, "return false;\n"
	   "}\n");
}

/* Write the common header for the GIMPLE/GENERIC IL matching routines.  */

static void
write_header (FILE *f, const char *head)
{
  fprintf (f, "/* Generated automatically by the program `genmatch' from\n");
  fprintf (f, "   a IL pattern matching and simplification description.  */\n");

  /* Include the header instead of writing it awkwardly quoted here.  */
  fprintf (f, "\n#include \"%s\"\n", head);
}



/* AST parsing.  */

class parser
{
public:
  parser (cpp_reader *);

private:
  const cpp_token *next ();
  const cpp_token *peek (unsigned = 1);
  const cpp_token *peek_ident (const char * = NULL, unsigned = 1);
  const cpp_token *expect (enum cpp_ttype);
  const cpp_token *eat_token (enum cpp_ttype);
  const char *get_string ();
  const char *get_ident ();
  const cpp_token *eat_ident (const char *);
  const char *get_number ();

  id_base *parse_operation ();
  operand *parse_capture (operand *, bool);
  operand *parse_expr ();
  c_expr *parse_c_expr (cpp_ttype);
  operand *parse_op ();

  void record_operlist (source_location, user_id *);

  void parse_pattern ();
  operand *parse_result (operand *, predicate_id *);
  void push_simplify (simplify::simplify_kind,
		      vec<simplify *>&, operand *, operand *);
  void parse_simplify (simplify::simplify_kind,
		       vec<simplify *>&, predicate_id *, operand *);
  void parse_for (source_location);
  void parse_if (source_location);
  void parse_predicates (source_location);
  void parse_operator_list (source_location);

  cpp_reader *r;
  vec<c_expr *> active_ifs;
  vec<vec<user_id *> > active_fors;
  hash_set<user_id *> *oper_lists_set;
  vec<user_id *> oper_lists;

  cid_map_t *capture_ids;

public:
  vec<simplify *> simplifiers;
  vec<predicate_id *> user_predicates;
  bool parsing_match_operand;
};

/* Lexing helpers.  */

/* Read the next non-whitespace token from R.  */

const cpp_token *
parser::next ()
{
  const cpp_token *token;
  do
    {
      token = cpp_get_token (r);
    }
  while (token->type == CPP_PADDING
	 && token->type != CPP_EOF);
  return token;
}

/* Peek at the next non-whitespace token from R.  */

const cpp_token *
parser::peek (unsigned num)
{
  const cpp_token *token;
  unsigned i = 0;
  do
    {
      token = cpp_peek_token (r, i++);
    }
  while ((token->type == CPP_PADDING
	  && token->type != CPP_EOF)
	 || (--num > 0));
  /* If we peek at EOF this is a fatal error as it leaves the
     cpp_reader in unusable state.  Assume we really wanted a
     token and thus this EOF is unexpected.  */
  if (token->type == CPP_EOF)
    fatal_at (token, "unexpected end of file");
  return token;
}

/* Peek at the next identifier token (or return NULL if the next
   token is not an identifier or equal to ID if supplied).  */

const cpp_token *
parser::peek_ident (const char *id, unsigned num)
{
  const cpp_token *token = peek (num);
  if (token->type != CPP_NAME)
    return 0;

  if (id == 0)
    return token;

  const char *t = (const char *) CPP_HASHNODE (token->val.node.node)->ident.str;
  if (strcmp (id, t) == 0)
    return token;

  return 0;
}

/* Read the next token from R and assert it is of type TK.  */

const cpp_token *
parser::expect (enum cpp_ttype tk)
{
  const cpp_token *token = next ();
  if (token->type != tk)
    fatal_at (token, "expected %s, got %s",
	      cpp_type2name (tk, 0), cpp_type2name (token->type, 0));

  return token;
}

/* Consume the next token from R and assert it is of type TK.  */

const cpp_token *
parser::eat_token (enum cpp_ttype tk)
{
  return expect (tk);
}

/* Read the next token from R and assert it is of type CPP_STRING and
   return its value.  */

const char *
parser::get_string ()
{
  const cpp_token *token = expect (CPP_STRING);
  return (const char *)token->val.str.text;
}

/* Read the next token from R and assert it is of type CPP_NAME and
   return its value.  */

const char *
parser::get_ident ()
{
  const cpp_token *token = expect (CPP_NAME);
  return (const char *)CPP_HASHNODE (token->val.node.node)->ident.str;
}

/* Eat an identifier token with value S from R.  */

const cpp_token *
parser::eat_ident (const char *s)
{
  const cpp_token *token = peek ();
  const char *t = get_ident ();
  if (strcmp (s, t) != 0)
    fatal_at (token, "expected '%s' got '%s'\n", s, t);
  return token;
}

/* Read the next token from R and assert it is of type CPP_NUMBER and
   return its value.  */

const char *
parser::get_number ()
{
  const cpp_token *token = expect (CPP_NUMBER);
  return (const char *)token->val.str.text;
}


/* Record an operator-list use for transparent for handling.  */

void
parser::record_operlist (source_location loc, user_id *p)
{
  if (!oper_lists_set->add (p))
    {
      if (!oper_lists.is_empty ()
	  && oper_lists[0]->substitutes.length () != p->substitutes.length ())
	fatal_at (loc, "User-defined operator list does not have the "
		  "same number of entries as others used in the pattern");
      oper_lists.safe_push (p);
    }
}

/* Parse the operator ID, special-casing convert?, convert1? and
   convert2?  */

id_base *
parser::parse_operation ()
{
  const cpp_token *id_tok = peek ();
  const char *id = get_ident ();
  const cpp_token *token = peek ();
  if (strcmp (id, "convert0") == 0)
    fatal_at (id_tok, "use 'convert?' here");
  else if (strcmp (id, "view_convert0") == 0)
    fatal_at (id_tok, "use 'view_convert?' here");
  if (token->type == CPP_QUERY
      && !(token->flags & PREV_WHITE))
    {
      if (strcmp (id, "convert") == 0)
	id = "convert0";
      else if (strcmp (id, "convert1") == 0)
	;
      else if (strcmp (id, "convert2") == 0)
	;
      else if (strcmp (id, "view_convert") == 0)
	id = "view_convert0";
      else if (strcmp (id, "view_convert1") == 0)
	;
      else if (strcmp (id, "view_convert2") == 0)
	;
      else
	fatal_at (id_tok, "non-convert operator conditionalized");

      if (!parsing_match_operand)
	fatal_at (id_tok, "conditional convert can only be used in "
		  "match expression");
      eat_token (CPP_QUERY);
    }
  else if (strcmp (id, "convert1") == 0
	   || strcmp (id, "convert2") == 0
	   || strcmp (id, "view_convert1") == 0
	   || strcmp (id, "view_convert2") == 0)
    fatal_at (id_tok, "expected '?' after conditional operator");
  id_base *op = get_operator (id);
  if (!op)
    fatal_at (id_tok, "unknown operator %s", id);

  user_id *p = dyn_cast<user_id *> (op);
  if (p && p->is_oper_list)
    {
      if (active_fors.length() == 0)
	record_operlist (id_tok->src_loc, p);
      else
	fatal_at (id_tok, "operator-list %s cannot be exapnded inside 'for'", id);
    }
  return op;
}

/* Parse a capture.
     capture = '@'<number>  */

struct operand *
parser::parse_capture (operand *op, bool require_existing)
{
  source_location src_loc = eat_token (CPP_ATSIGN)->src_loc;
  const cpp_token *token = peek ();
  const char *id = NULL;
  if (token->type == CPP_NUMBER)
    id = get_number ();
  else if (token->type == CPP_NAME)
    id = get_ident ();
  else
    fatal_at (token, "expected number or identifier");
  unsigned next_id = capture_ids->elements ();
  bool existed;
  unsigned &num = capture_ids->get_or_insert (id, &existed);
  if (!existed)
    {
      if (require_existing)
	fatal_at (src_loc, "unknown capture id");
      num = next_id;
    }
  return new capture (src_loc, num, op);
}

/* Parse an expression
     expr = '(' <operation>[capture][flag][type] <operand>... ')'  */

struct operand *
parser::parse_expr ()
{
  const cpp_token *token = peek ();
  expr *e = new expr (parse_operation (), token->src_loc);
  token = peek ();
  operand *op;
  bool is_commutative = false;
  bool force_capture = false;
  const char *expr_type = NULL;

  if (token->type == CPP_COLON
      && !(token->flags & PREV_WHITE))
    {
      eat_token (CPP_COLON);
      token = peek ();
      if (token->type == CPP_NAME
	  && !(token->flags & PREV_WHITE))
	{
	  const char *s = get_ident ();
	  if (!parsing_match_operand)
	    expr_type = s;
	  else
	    {
	      const char *sp = s;
	      while (*sp)
		{
		  if (*sp == 'c')
		    is_commutative = true;
		  else if (*sp == 's')
		    {
		      e->force_single_use = true;
		      force_capture = true;
		    }
	      	  else
		    fatal_at (token, "flag %c not recognized", *sp);
		  sp++;
		}
	    }
	  token = peek ();
	}
      else
	fatal_at (token, "expected flag or type specifying identifier");
    }

  if (token->type == CPP_ATSIGN
      && !(token->flags & PREV_WHITE))
    op = parse_capture (e, !parsing_match_operand);
  else if (force_capture)
    {
      unsigned num = capture_ids->elements ();
      char id[8];
      bool existed;
      sprintf (id, "__%u", num);
      capture_ids->get_or_insert (xstrdup (id), &existed);
      if (existed)
	fatal_at (token, "reserved capture id '%s' already used", id);
      op = new capture (token->src_loc, num, e);
    }
  else
    op = e;
  do
    {
      const cpp_token *token = peek ();
      if (token->type == CPP_CLOSE_PAREN)
	{
	  if (e->operation->nargs != -1
	      && e->operation->nargs != (int) e->ops.length ())
	    fatal_at (token, "'%s' expects %u operands, not %u",
		      e->operation->id, e->operation->nargs, e->ops.length ());
	  if (is_commutative)
	    {
	      if (e->ops.length () == 2)
		e->is_commutative = true;
	      else
		fatal_at (token, "only binary operators or function with "
			  "two arguments can be marked commutative");
	    }
	  e->expr_type = expr_type;
	  return op;
	}
      else if (!(token->flags & PREV_WHITE))
	fatal_at (token, "expected expression operand");

      e->append_op (parse_op ());
    }
  while (1);
}

/* Lex native C code delimited by START recording the preprocessing tokens
   for later processing.
     c_expr = ('{'|'(') <pp token>... ('}'|')')  */

c_expr *
parser::parse_c_expr (cpp_ttype start)
{
  const cpp_token *token;
  cpp_ttype end;
  unsigned opencnt;
  vec<cpp_token> code = vNULL;
  unsigned nr_stmts = 0;
  source_location loc = eat_token (start)->src_loc;
  if (start == CPP_OPEN_PAREN)
    end = CPP_CLOSE_PAREN;
  else if (start == CPP_OPEN_BRACE)
    end = CPP_CLOSE_BRACE;
  else
    gcc_unreachable ();
  opencnt = 1;
  do
    {
      token = next ();

      /* Count brace pairs to find the end of the expr to match.  */
      if (token->type == start)
	opencnt++;
      else if (token->type == end
	       && --opencnt == 0)
	break;

      /* This is a lame way of counting the number of statements.  */
      if (token->type == CPP_SEMICOLON)
	nr_stmts++;

      /* If this is possibly a user-defined identifier mark it used.  */
      if (token->type == CPP_NAME)
	{
	  id_base *idb = get_operator ((const char *)CPP_HASHNODE
				      (token->val.node.node)->ident.str);
	  user_id *p;
	  if (idb && (p = dyn_cast<user_id *> (idb)) && p->is_oper_list)
	    record_operlist (token->src_loc, p);
	}

      /* Record the token.  */
      code.safe_push (*token);
    }
  while (1);
  return new c_expr (r, loc, code, nr_stmts, vNULL, capture_ids);
}

/* Parse an operand which is either an expression, a predicate or
   a standalone capture.
     op = predicate | expr | c_expr | capture  */

struct operand *
parser::parse_op ()
{
  const cpp_token *token = peek ();
  struct operand *op = NULL;
  if (token->type == CPP_OPEN_PAREN)
    {
      eat_token (CPP_OPEN_PAREN);
      op = parse_expr ();
      eat_token (CPP_CLOSE_PAREN);
    }
  else if (token->type == CPP_OPEN_BRACE)
    {
      op = parse_c_expr (CPP_OPEN_BRACE);
    }
  else
    {
      /* Remaining ops are either empty or predicates  */
      if (token->type == CPP_NAME)
	{
	  const char *id = get_ident ();
	  id_base *opr = get_operator (id);
	  if (!opr)
	    fatal_at (token, "expected predicate name");
	  if (operator_id *code = dyn_cast <operator_id *> (opr))
	    {
	      if (code->nargs != 0)
		fatal_at (token, "using an operator with operands as predicate");
	      /* Parse the zero-operand operator "predicates" as
		 expression.  */
	      op = new expr (opr, token->src_loc);
	    }
	  else if (user_id *code = dyn_cast <user_id *> (opr))
	    {
	      if (code->nargs != 0)
		fatal_at (token, "using an operator with operands as predicate");
	      /* Parse the zero-operand operator "predicates" as
		 expression.  */
	      op = new expr (opr, token->src_loc);
	    }
	  else if (predicate_id *p = dyn_cast <predicate_id *> (opr))
	    op = new predicate (p, token->src_loc);
	  else
	    fatal_at (token, "using an unsupported operator as predicate");
	  if (!parsing_match_operand)
	    fatal_at (token, "predicates are only allowed in match expression");
	  token = peek ();
	  if (token->flags & PREV_WHITE)
	    return op;
	}
      else if (token->type != CPP_COLON
	       && token->type != CPP_ATSIGN)
	fatal_at (token, "expected expression or predicate");
      /* optionally followed by a capture and a predicate.  */
      if (token->type == CPP_COLON)
	fatal_at (token, "not implemented: predicate on leaf operand");
      if (token->type == CPP_ATSIGN)
	op = parse_capture (op, !parsing_match_operand);
    }

  return op;
}

/* Create a new simplify from the current parsing state and MATCH,
   MATCH_LOC, RESULT and RESULT_LOC and push it to SIMPLIFIERS.  */

void
parser::push_simplify (simplify::simplify_kind kind,
		       vec<simplify *>& simplifiers,
		       operand *match, operand *result)
{
  /* Build and push a temporary for operator list uses in expressions.  */
  if (!oper_lists.is_empty ())
    active_fors.safe_push (oper_lists);

  simplifiers.safe_push
    (new simplify (kind, match, result,
		   active_fors.copy (), capture_ids));

  if (!oper_lists.is_empty ())
    active_fors.pop ();
}

/* Parse
     <result-op> = <op> | <if> | <with>
     <if> = '(' 'if' '(' <c-expr> ')' <result-op> ')'
     <with> = '(' 'with' '{' <c-expr> '}' <result-op> ')'
   and return it.  */

operand *
parser::parse_result (operand *result, predicate_id *matcher)
{
  const cpp_token *token = peek ();
  if (token->type != CPP_OPEN_PAREN)
    return parse_op ();

  eat_token (CPP_OPEN_PAREN);
  if (peek_ident ("if"))
    {
      eat_ident ("if");
      if_expr *ife = new if_expr (token->src_loc);
      ife->cond = parse_c_expr (CPP_OPEN_PAREN);
      if (peek ()->type == CPP_OPEN_PAREN)
	{
	  ife->trueexpr = parse_result (result, matcher);
	  if (peek ()->type == CPP_OPEN_PAREN)
	    ife->falseexpr = parse_result (result, matcher);
	  else if (peek ()->type != CPP_CLOSE_PAREN)
	    ife->falseexpr = parse_op ();
	}
      else if (peek ()->type != CPP_CLOSE_PAREN)
	{
	  ife->trueexpr = parse_op ();
	  if (peek ()->type == CPP_OPEN_PAREN)
	    ife->falseexpr = parse_result (result, matcher);
	  else if (peek ()->type != CPP_CLOSE_PAREN)
	    ife->falseexpr = parse_op ();
	}
      /* If this if is immediately closed then it contains a
	 manual matcher or is part of a predicate definition.  */
      else /* if (peek ()->type == CPP_CLOSE_PAREN) */
	{
	  if (!matcher)
	    fatal_at (peek (), "manual transform not implemented");
	  ife->trueexpr = result;
	}
      eat_token (CPP_CLOSE_PAREN);
      return ife;
    }
  else if (peek_ident ("with"))
    {
      eat_ident ("with");
      with_expr *withe = new with_expr (token->src_loc);
      /* Parse (with c-expr expr) as (if-with (true) expr).  */
      withe->with = parse_c_expr (CPP_OPEN_BRACE);
      withe->with->nr_stmts = 0;
      withe->subexpr = parse_result (result, matcher);
      eat_token (CPP_CLOSE_PAREN);
      return withe;
    }
  else if (peek_ident ("switch"))
    {
      token = eat_ident ("switch");
      source_location ifloc = eat_token (CPP_OPEN_PAREN)->src_loc;
      eat_ident ("if");
      if_expr *ife = new if_expr (ifloc);
      operand *res = ife;
      ife->cond = parse_c_expr (CPP_OPEN_PAREN);
      if (peek ()->type == CPP_OPEN_PAREN)
	ife->trueexpr = parse_result (result, matcher);
      else
	ife->trueexpr = parse_op ();
      eat_token (CPP_CLOSE_PAREN);
      if (peek ()->type != CPP_OPEN_PAREN
	  || !peek_ident ("if", 2))
	fatal_at (token, "switch can be implemented with a single if");
      while  (peek ()->type != CPP_CLOSE_PAREN)
	{
	  if (peek ()->type == CPP_OPEN_PAREN)
	    {
	      if (peek_ident ("if", 2))
		{
		  ifloc = eat_token (CPP_OPEN_PAREN)->src_loc;
		  eat_ident ("if");
		  ife->falseexpr = new if_expr (ifloc);
		  ife = as_a <if_expr *> (ife->falseexpr);
		  ife->cond = parse_c_expr (CPP_OPEN_PAREN);
		  if (peek ()->type == CPP_OPEN_PAREN)
		    ife->trueexpr = parse_result (result, matcher);
		  else
		    ife->trueexpr = parse_op ();
		  eat_token (CPP_CLOSE_PAREN);
		}
	      else
		{
		  /* switch default clause */
		  ife->falseexpr = parse_result (result, matcher);
		  eat_token (CPP_CLOSE_PAREN);
		  return res;
		}
	    }
	  else
	    {
	      /* switch default clause */
	      ife->falseexpr = parse_op ();
	      eat_token (CPP_CLOSE_PAREN);
	      return res;
	    }
	}
      eat_token (CPP_CLOSE_PAREN);
      return res;
    }
  else
    {
      operand *op = result;
      if (!matcher)
	op = parse_expr ();
      eat_token (CPP_CLOSE_PAREN);
      return op;
    }
}

/* Parse
     simplify = 'simplify' <expr> <result-op>
   or
     match = 'match' <ident> <expr> [<result-op>]
   and fill SIMPLIFIERS with the results.  */

void
parser::parse_simplify (simplify::simplify_kind kind,
			vec<simplify *>& simplifiers, predicate_id *matcher,
			operand *result)
{
  /* Reset the capture map.  */
  if (!capture_ids)
    capture_ids = new cid_map_t;
  /* Reset oper_lists and set.  */
  hash_set <user_id *> olist;
  oper_lists_set = &olist;
  oper_lists = vNULL;

  const cpp_token *loc = peek ();
  parsing_match_operand = true;
  struct operand *match = parse_op ();
  parsing_match_operand = false;
  if (match->type == operand::OP_CAPTURE && !matcher)
    fatal_at (loc, "outermost expression cannot be captured");
  if (match->type == operand::OP_EXPR
      && is_a <predicate_id *> (as_a <expr *> (match)->operation))
    fatal_at (loc, "outermost expression cannot be a predicate");

  /* Splice active_ifs onto result and continue parsing the
     "then" expr.  */
  if_expr *active_if = NULL;
  for (int i = active_ifs.length (); i > 0; --i)
    {
      if_expr *ifc = new if_expr (active_ifs[i-1]->location);
      ifc->cond = active_ifs[i-1];
      ifc->trueexpr = active_if;
      active_if = ifc;
    }
  if_expr *outermost_if = active_if;
  while (active_if && active_if->trueexpr)
    active_if = as_a <if_expr *> (active_if->trueexpr);

  const cpp_token *token = peek ();

  /* If this if is immediately closed then it is part of a predicate
     definition.  Push it.  */
  if (token->type == CPP_CLOSE_PAREN)
    {
      if (!matcher)
	fatal_at (token, "expected transform expression");
      if (active_if)
	{
	  active_if->trueexpr = result;
	  result = outermost_if;
	}
      push_simplify (kind, simplifiers, match, result);
      return;
    }

  operand *tem = parse_result (result, matcher);
  if (active_if)
    {
      active_if->trueexpr = tem;
      result = outermost_if;
    }
  else
    result = tem;

  push_simplify (kind, simplifiers, match, result);
}

/* Parsing of the outer control structures.  */

/* Parse a for expression
     for = '(' 'for' <subst>... <pattern> ')'
     subst = <ident> '(' <ident>... ')'  */

void
parser::parse_for (source_location)
{
  auto_vec<const cpp_token *> user_id_tokens;
  vec<user_id *> user_ids = vNULL;
  const cpp_token *token;
  unsigned min_n_opers = 0, max_n_opers = 0;

  while (1)
    {
      token = peek ();
      if (token->type != CPP_NAME)
	break;

      /* Insert the user defined operators into the operator hash.  */
      const char *id = get_ident ();
      if (get_operator (id) != NULL)
	fatal_at (token, "operator already defined");
      user_id *op = new user_id (id);
      id_base **slot = operators->find_slot_with_hash (op, op->hashval, INSERT);
      *slot = op;
      user_ids.safe_push (op);
      user_id_tokens.safe_push (token);

      eat_token (CPP_OPEN_PAREN);

      int arity = -1;
      while ((token = peek_ident ()) != 0)
	{
	  const char *oper = get_ident ();
	  id_base *idb = get_operator (oper);
	  if (idb == NULL)
	    fatal_at (token, "no such operator '%s'", oper);
	  if (*idb == CONVERT0 || *idb == CONVERT1 || *idb == CONVERT2
	      || *idb == VIEW_CONVERT0 || *idb == VIEW_CONVERT1
	      || *idb == VIEW_CONVERT2)
	    fatal_at (token, "conditional operators cannot be used inside for");

	  if (arity == -1)
	    arity = idb->nargs;
	  else if (idb->nargs == -1)
	    ;
	  else if (idb->nargs != arity)
	    fatal_at (token, "operator '%s' with arity %d does not match "
		      "others with arity %d", oper, idb->nargs, arity);

	  user_id *p = dyn_cast<user_id *> (idb);
	  if (p)
	    {
	      if (p->is_oper_list)
		op->substitutes.safe_splice (p->substitutes);
	      else
		fatal_at (token, "iterator cannot be used as operator-list");
	    }
	  else 
	    op->substitutes.safe_push (idb);
	}
      op->nargs = arity;
      token = expect (CPP_CLOSE_PAREN);

      unsigned nsubstitutes = op->substitutes.length ();
      if (nsubstitutes == 0)
	fatal_at (token, "A user-defined operator must have at least "
		  "one substitution");
      if (max_n_opers == 0)
	{
	  min_n_opers = nsubstitutes;
	  max_n_opers = nsubstitutes;
	}
      else
	{
	  if (nsubstitutes % min_n_opers != 0
	      && min_n_opers % nsubstitutes != 0)
	    fatal_at (token, "All user-defined identifiers must have a "
		      "multiple number of operator substitutions of the "
		      "smallest number of substitutions");
	  if (nsubstitutes < min_n_opers)
	    min_n_opers = nsubstitutes;
	  else if (nsubstitutes > max_n_opers)
	    max_n_opers = nsubstitutes;
	}
    }

  unsigned n_ids = user_ids.length ();
  if (n_ids == 0)
    fatal_at (token, "for requires at least one user-defined identifier");

  token = peek ();
  if (token->type == CPP_CLOSE_PAREN)
    fatal_at (token, "no pattern defined in for");

  active_fors.safe_push (user_ids);
  while (1)
    {
      token = peek ();
      if (token->type == CPP_CLOSE_PAREN)
 	break;
      parse_pattern ();
    }
  active_fors.pop ();

  /* Remove user-defined operators from the hash again.  */
  for (unsigned i = 0; i < user_ids.length (); ++i)
    {
      if (!user_ids[i]->used)
	warning_at (user_id_tokens[i],
		    "operator %s defined but not used", user_ids[i]->id);
      operators->remove_elt (user_ids[i]);
    }
}

/* Parse an identifier associated with a list of operators.
     oprs = '(' 'define_operator_list' <ident> <ident>... ')'  */

void
parser::parse_operator_list (source_location)
{
  const cpp_token *token = peek (); 
  const char *id = get_ident ();

  if (get_operator (id) != 0)
    fatal_at (token, "operator %s already defined", id);

  user_id *op = new user_id (id, true);
  int arity = -1;
  
  while ((token = peek_ident ()) != 0)
    {
      token = peek (); 
      const char *oper = get_ident ();
      id_base *idb = get_operator (oper);
      
      if (idb == 0)
	fatal_at (token, "no such operator '%s'", oper);

      if (arity == -1)
	arity = idb->nargs;
      else if (idb->nargs == -1)
	;
      else if (arity != idb->nargs)
	fatal_at (token, "operator '%s' with arity %d does not match "
			 "others with arity %d", oper, idb->nargs, arity);

      /* We allow composition of multiple operator lists.  */
      if (user_id *p = dyn_cast<user_id *> (idb))
	op->substitutes.safe_splice (p->substitutes);
      else
	op->substitutes.safe_push (idb);
    }

  // Check that there is no junk after id-list
  token = peek();
  if (token->type != CPP_CLOSE_PAREN)
    fatal_at (token, "expected identifier got %s", cpp_type2name (token->type, 0));

  if (op->substitutes.length () == 0)
    fatal_at (token, "operator-list cannot be empty");

  op->nargs = arity;
  id_base **slot = operators->find_slot_with_hash (op, op->hashval, INSERT);
  *slot = op;
}

/* Parse an outer if expression.
     if = '(' 'if' '(' <c-expr> ')' <pattern> ')'  */

void
parser::parse_if (source_location)
{
  c_expr *ifexpr = parse_c_expr (CPP_OPEN_PAREN);

  const cpp_token *token = peek ();
  if (token->type == CPP_CLOSE_PAREN)
    fatal_at (token, "no pattern defined in if");

  active_ifs.safe_push (ifexpr);
  while (1)
    {
      const cpp_token *token = peek ();
      if (token->type == CPP_CLOSE_PAREN)
	break;

      parse_pattern ();
    }
  active_ifs.pop ();
}

/* Parse a list of predefined predicate identifiers.
     preds = '(' 'define_predicates' <ident>... ')'  */

void
parser::parse_predicates (source_location)
{
  do
    {
      const cpp_token *token = peek ();
      if (token->type != CPP_NAME)
	break;

      add_predicate (get_ident ());
    }
  while (1);
}

/* Parse outer control structures.
     pattern = <preds>|<for>|<if>|<simplify>|<match>  */

void
parser::parse_pattern ()
{
  /* All clauses start with '('.  */
  eat_token (CPP_OPEN_PAREN);
  const cpp_token *token = peek ();
  const char *id = get_ident ();
  if (strcmp (id, "simplify") == 0)
    {
      parse_simplify (simplify::SIMPLIFY, simplifiers, NULL, NULL);
      capture_ids = NULL;
    }
  else if (strcmp (id, "match") == 0)
    {
      bool with_args = false;
      source_location e_loc = peek ()->src_loc;
      if (peek ()->type == CPP_OPEN_PAREN)
	{
	  eat_token (CPP_OPEN_PAREN);
	  with_args = true;
	}
      const char *name = get_ident ();
      id_base *id = get_operator (name);
      predicate_id *p;
      if (!id)
	{
	  p = add_predicate (name);
	  user_predicates.safe_push (p);
	}
      else if ((p = dyn_cast <predicate_id *> (id)))
	;
      else
	fatal_at (token, "cannot add a match to a non-predicate ID");
      /* Parse (match <id> <arg>... (match-expr)) here.  */
      expr *e = NULL;
      if (with_args)
	{
	  capture_ids = new cid_map_t;
	  e = new expr (p, e_loc);
	  while (peek ()->type == CPP_ATSIGN)
	    e->append_op (parse_capture (NULL, false));
	  eat_token (CPP_CLOSE_PAREN);
	}
      if (p->nargs != -1
	  && ((e && e->ops.length () != (unsigned)p->nargs)
	      || (!e && p->nargs != 0)))
	fatal_at (token, "non-matching number of match operands");
      p->nargs = e ? e->ops.length () : 0;
      parse_simplify (simplify::MATCH, p->matchers, p, e);
      capture_ids = NULL;
    }
  else if (strcmp (id, "for") == 0)
    parse_for (token->src_loc);
  else if (strcmp (id, "if") == 0)
    parse_if (token->src_loc);
  else if (strcmp (id, "define_predicates") == 0)
    {
      if (active_ifs.length () > 0
	  || active_fors.length () > 0)
	fatal_at (token, "define_predicates inside if or for is not supported");
      parse_predicates (token->src_loc);
    }
  else if (strcmp (id, "define_operator_list") == 0)
    {
      if (active_ifs.length () > 0
	  || active_fors.length () > 0)
	fatal_at (token, "operator-list inside if or for is not supported");
      parse_operator_list (token->src_loc);
    }
  else
    fatal_at (token, "expected %s'simplify', 'match', 'for' or 'if'",
	      active_ifs.length () == 0 && active_fors.length () == 0
	      ? "'define_predicates', " : "");

  eat_token (CPP_CLOSE_PAREN);
}

/* Main entry of the parser.  Repeatedly parse outer control structures.  */

parser::parser (cpp_reader *r_)
{
  r = r_;
  active_ifs = vNULL;
  active_fors = vNULL;
  simplifiers = vNULL;
  oper_lists_set = NULL;
  oper_lists = vNULL;
  capture_ids = NULL;
  user_predicates = vNULL;
  parsing_match_operand = false;

  const cpp_token *token = next ();
  while (token->type != CPP_EOF)
    {
      _cpp_backup_tokens (r, 1);
      parse_pattern ();
      token = next ();
    }
}


/* Helper for the linemap code.  */

static size_t
round_alloc_size (size_t s)
{
  return s;
}


/* The genmatch generator progam.  It reads from a pattern description
   and outputs GIMPLE or GENERIC IL matching and simplification routines.  */

int
main (int argc, char **argv)
{
  cpp_reader *r;

  progname = "genmatch";

  if (argc < 2)
    return 1;

  bool gimple = true;
  char *input = argv[argc-1];
  for (int i = 1; i < argc - 1; ++i)
    {
      if (strcmp (argv[i], "--gimple") == 0)
	gimple = true;
      else if (strcmp (argv[i], "--generic") == 0)
	gimple = false;
      else if (strcmp (argv[i], "-v") == 0)
	verbose = 1;
      else if (strcmp (argv[i], "-vv") == 0)
	verbose = 2;
      else
	{
	  fprintf (stderr, "Usage: genmatch "
		   "[--gimple] [--generic] [-v[v]] input\n");
	  return 1;
	}
    }

  line_table = XCNEW (struct line_maps);
  linemap_init (line_table, 0);
  line_table->reallocator = xrealloc;
  line_table->round_alloc_size = round_alloc_size;

  r = cpp_create_reader (CLK_GNUC99, NULL, line_table);
  cpp_callbacks *cb = cpp_get_callbacks (r);
  cb->error = error_cb;

  if (!cpp_read_main_file (r, input))
    return 1;
  cpp_define (r, gimple ? "GIMPLE=1": "GENERIC=1");
  cpp_define (r, gimple ? "GENERIC=0": "GIMPLE=0");

  /* Pre-seed operators.  */
  operators = new hash_table<id_base> (1024);
#define DEFTREECODE(SYM, STRING, TYPE, NARGS) \
  add_operator (SYM, # SYM, # TYPE, NARGS);
#define END_OF_BASE_TREE_CODES
#include "tree.def"
add_operator (CONVERT0, "CONVERT0", "tcc_unary", 1);
add_operator (CONVERT1, "CONVERT1", "tcc_unary", 1);
add_operator (CONVERT2, "CONVERT2", "tcc_unary", 1);
add_operator (VIEW_CONVERT0, "VIEW_CONVERT0", "tcc_unary", 1);
add_operator (VIEW_CONVERT1, "VIEW_CONVERT1", "tcc_unary", 1);
add_operator (VIEW_CONVERT2, "VIEW_CONVERT2", "tcc_unary", 1);
#undef END_OF_BASE_TREE_CODES
#undef DEFTREECODE

  /* Pre-seed builtin functions.
     ???  Cannot use N (name) as that is targetm.emultls.get_address
     for BUILT_IN_EMUTLS_GET_ADDRESS ... */
#define DEF_BUILTIN(ENUM, N, C, T, LT, B, F, NA, AT, IM, COND) \
  add_builtin (ENUM, # ENUM);
#include "builtins.def"
#undef DEF_BUILTIN

  /* Parse ahead!  */
  parser p (r);

  if (gimple)
    write_header (stdout, "gimple-match-head.c");
  else
    write_header (stdout, "generic-match-head.c");

  /* Go over all predicates defined with patterns and perform
     lowering and code generation.  */
  for (unsigned i = 0; i < p.user_predicates.length (); ++i)
    {
      predicate_id *pred = p.user_predicates[i];
      lower (pred->matchers, gimple);

      if (verbose == 2)
	for (unsigned i = 0; i < pred->matchers.length (); ++i)
	  print_matches (pred->matchers[i]);

      decision_tree dt;
      for (unsigned i = 0; i < pred->matchers.length (); ++i)
	dt.insert (pred->matchers[i], i);

      if (verbose == 2)
	dt.print (stderr);

      write_predicate (stdout, pred, dt, gimple);
    }

  /* Lower the main simplifiers and generate code for them.  */
  lower (p.simplifiers, gimple);

  if (verbose == 2)
    for (unsigned i = 0; i < p.simplifiers.length (); ++i)
      print_matches (p.simplifiers[i]);

  decision_tree dt;
  for (unsigned i = 0; i < p.simplifiers.length (); ++i)
    dt.insert (p.simplifiers[i], i);

  if (verbose == 2)
    dt.print (stderr);

  dt.gen (stdout, gimple);

  /* Finalize.  */
  cpp_finish (r, NULL);
  cpp_destroy (r);

  delete operators;

  return 0;
}
