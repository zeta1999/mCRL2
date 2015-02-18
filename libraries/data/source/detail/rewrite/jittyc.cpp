// Author(s): Muck van Weerdenburg
// Copyright: see the accompanying file COPYING or copy at
// https://svn.win.tue.nl/trac/MCRL2/browser/trunk/COPYING
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
/// \file jittyc.cpp

#include "mcrl2/data/detail/rewrite.h" // Required fo MCRL2_JITTTYC_AVAILABLE.

#ifdef MCRL2_JITTYC_AVAILABLE

#define NAME "rewr_jittyc"

#include <utility>
#include <string>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cassert>
#include <sstream>
#include <fstream>
#include <sys/stat.h>
#include "mcrl2/utilities/file_utility.h"
#include "mcrl2/utilities/detail/memory_utility.h"
#include "mcrl2/utilities/basename.h"
#include "mcrl2/utilities/logger.h"
#include "mcrl2/core/print.h"
#include "mcrl2/core/detail/function_symbols.h"
#include "mcrl2/data/detail/rewrite/jittyc.h"
#include "mcrl2/data/detail/rewrite/jitty_jittyc.h"
#include "mcrl2/data/replace.h"
#include "mcrl2/data/traverser.h"
#include "mcrl2/data/substitutions/mutable_map_substitution.h"

#ifdef MCRL2_DISPLAY_REWRITE_STATISTICS
#include "mcrl2/data/detail/rewrite_statistics.h"
#endif

using namespace mcrl2::core;
using namespace mcrl2::core::detail;
using namespace std;
using namespace atermpp;
using namespace mcrl2::log;

namespace mcrl2
{
namespace data
{
namespace detail
{

typedef atermpp::term_list<variable_list> variable_list_list;

static const match_tree dummy=match_tree();

template <template <class> class Traverser>
struct double_variable_traverser : public Traverser<double_variable_traverser<Traverser> >
{
  typedef Traverser<double_variable_traverser<Traverser> > super;
  using super::enter;
  using super::leave;
  using super::apply;

  std::set<variable> m_seen;
  std::set<variable> m_doubles;

  void apply(const variable& v)
  {
    if (!m_seen.insert(v).second)
    {
      m_doubles.insert(v);
    }
  }

  const std::set<variable>& result()
  {
    return m_doubles;
  }
};

static std::vector<bool> dep_vars(const data_equation& eqn)
{
  std::vector<bool> result(recursive_number_of_args(eqn.lhs()), true);
  std::set<variable> condition_vars = find_free_variables(eqn.condition());
  double_variable_traverser<data::variable_traverser> lhs_doubles;
  double_variable_traverser<data::variable_traverser> rhs_doubles;
  lhs_doubles.apply(eqn.lhs());
  rhs_doubles.apply(eqn.rhs());

  for (size_t i = 0; i < result.size(); ++i)
  {
    const data_expression& arg_i = get_argument_of_higher_order_term(eqn.lhs(), i);
    if (is_variable(arg_i))
    {
      const variable& v = down_cast<variable>(arg_i);
      if (condition_vars.count(v) == 0 && lhs_doubles.result().count(v) == 0 && rhs_doubles.result().count(v) == 0)
      {
        result[i] = false;
      }
    }
  }
  return result;
}

class always_rewrite_array : public std::vector<atermpp::aterm_appl>
{
public:
  typedef atermpp::aterm_appl node_t;
private:
  const atermpp::function_symbol m_f_true, m_f_false, m_f_and, m_f_or, m_f_var;
  const node_t m_true, m_false;
  std::map<mcrl2::data::function_symbol, size_t> m_indices;

  bool eval(const node_t& expr) const
  {
    if (expr == m_true)
    {
      return true;
    }
    if (expr == m_false)
    {
      return false;
    }
    if (expr.function() == m_f_and)
    {
      return eval(down_cast<node_t>(expr[0])) && eval(down_cast<node_t>(expr[1]));
    }
    if (expr.function() == m_f_or)
    {
      return eval(down_cast<node_t>(expr[0])) || eval(down_cast<node_t>(expr[1]));
    }
    assert(expr.function() == m_f_var);
    return at(down_cast<atermpp::aterm_int>(expr[0]).value()) != m_false;
  }

  inline
  size_t index(const function_symbol& f, const size_t arity, const size_t arg) const
  {
    assert(arg < arity);
    assert(m_indices.find(f) != m_indices.end());
    assert((m_indices.at(f) + ((arity - 1) * arity) / 2 + arg) < size());
    return (m_indices.at(f) + ((arity - 1) * arity) / 2) + arg;
  }

  atermpp::aterm_appl and_(const atermpp::aterm_appl& x, const atermpp::aterm_appl& y) const
  {
    if (x == m_true)
    {
      return y;
    }
    if (y == m_true)
    {
      return x;
    }
    if (x == m_false || y == m_false)
    {
      return m_false;
    }
    return atermpp::aterm_appl(m_f_and, x, y);
  }

  atermpp::aterm_appl or_(const atermpp::aterm_appl& x, const atermpp::aterm_appl& y) const
  {
    if (x == m_false)
    {
      return y;
    }
    else if (y == m_false)
    {
      return x;
    }
    else if (x == m_true || y == m_true)
    {
      return m_true;
    }
    return atermpp::aterm_appl(m_f_or, x, y);
  }

  atermpp::aterm_appl var_(const function_symbol& f, const size_t arity, const size_t arg) const
  {
    return atermpp::aterm_appl(m_f_var, atermpp::aterm_int(index(f, arity, arg)));
  }

  atermpp::aterm_appl build_expr(const data_equation_list& eqns, const size_t arg, const size_t arity) const
  {
    atermpp::aterm_appl result = m_true;
    for (auto i = eqns.begin(); i != eqns.end(); ++i)
    {
      result = and_(build_expr_aux(*i, arg, arity), result);
    }
    return result;
  }

  atermpp::aterm_appl build_expr_aux(const data_equation& eqn, const size_t arg, const size_t arity) const
  {
    size_t eqn_arity = recursive_number_of_args(eqn.lhs());
    if (eqn_arity > arity)
    {
      return m_true;
    }
    if (eqn_arity <= arg)
    {
      const data_expression& rhs = eqn.rhs();
      if (is_function_symbol(rhs))
      {
        return var_(down_cast<function_symbol>(rhs), arity, arg);
      }
      function_symbol head;
      if (head_is_function_symbol(rhs,head))
      {
        int rhs_arity = recursive_number_of_args(rhs) - 1;
        size_t diff_arity = arity - eqn_arity;
        int rhs_new_arity = rhs_arity + diff_arity;
        return var_(head, rhs_new_arity, arg - eqn_arity + rhs_arity);
      }
      else
      {
        return m_false;
      }
    }

    // Here we know that eqn.lhs() must be an application. If it were a function symbol
    // it would have been dealt with above.
    if (!dep_vars(eqn)[arg])
    {
      const application& lhs = down_cast<application>(eqn.lhs());
      const data_expression& arg_term = get_argument_of_higher_order_term(lhs, arg);
      if (is_variable(arg_term))
      {
        return build_expr_internal(eqn.rhs(), down_cast<variable>(arg_term));
      }
    }
    return m_true;
  }

  // The function build_expr_internal returns an and/or tree indicating on which function symbol with which
  // arity the variable var depends. The idea is that if var must be a normal form if any of these arguments
  // must be a normal form. In this way the reduction to normal forms of these argument is not unnecessarily
  // postponed. For example in the expression if(!b,1,0) the typical result is @@and(@@var(149),@@var(720))
  // indicating that b must not be rewritten to a normal form anyhow if the first argument of ! must not always be rewritten
  // to a normalform and the first argument of if must not always be rewritten to normal form.
  atermpp::aterm_appl build_expr_internal(const data_expression& expr, const variable& var) const
  {
    if (is_function_symbol(expr))
    {
      return m_false;
    }
    if (is_variable(expr))
    {
      return expr == var ? m_true : m_false;
    }
    if (is_where_clause(expr) || is_abstraction(expr))
    {
      return m_false;
    }

    assert(is_application(expr));
    const application& appl = atermpp::down_cast<application>(expr);
    function_symbol head;
    if (!head_is_function_symbol(appl, head))
    {
      if (head_is_variable(appl))
      {
        if (get_variable_of_head(appl) == var)
        {
          return m_true;
        }
      }
      return m_false;
    }

    atermpp::aterm_appl result = m_false;
    size_t arity = recursive_number_of_args(appl);
    for (size_t i = 0; i < arity; ++i)
    {
      result = or_(result,
                   and_(var_(head, arity, i),
                        build_expr_internal(get_argument_of_higher_order_term(appl, i), var)));
    }
    return result;
  }

public:

  always_rewrite_array()
    : m_f_true("@@true", 0), m_f_false("@@false", 0), m_f_and("@@and", 2), m_f_or("@@or", 2), m_f_var("@@var", 1),
      m_true(m_f_true), m_false(m_f_false)
  { }

  bool always_rewrite(const function_symbol& f, const size_t arity,  const size_t arg) const
  {
    return at(index(f, arity, arg)) != m_false;
  }

  size_t init(const function_symbol_vector& symbols, std::map<function_symbol, data_equation_list> equations)
  {
    size_t max_arity = 0;
    size_t size = 0;
    for (auto it = symbols.begin(); it != symbols.end(); ++it)
    {
      if (m_indices.insert(std::make_pair(*it, size)).second)
      {
        size_t arity = getArity(*it);
        size += (arity * (arity + 1)) / 2;
        max_arity = std::max(max_arity, arity);
      }
    }

    resize(size);
    for (auto it = m_indices.begin(); it != m_indices.end(); ++it)
    {
      for (size_t i = 1; i <= getArity(it->first); ++i)
      {
        for (size_t j = 0; j < i; ++j)
        {
          at(index(it->first, i, j)) = build_expr(equations[it->first], j, i);
        }
      }
    }

    bool notdone = true;
    while (notdone)
    {
      notdone = false;
      for (size_t i = 0; i < size; ++i)
      {
        if (at(i) != m_false && !eval(at(i)))
        {
          at(i) = m_false;
          notdone = true;
        }
      }
    }

    return max_arity;
  }

};

///
/// \brief arity_is_allowed yields true if the function indicated by the function index can
///        legitemately be used with a arguments. A function f:D1x...xDn->D can be used with 0 and
///        n arguments. A function f:(D1x...xDn)->(E1x...Em)->F can be used with 0, n, and n+m
///        arguments.
/// \param s A function sort
/// \param a The desired number of arguments
/// \return A boolean indicating whether a term of sort s applied to a arguments is a valid term.
///
static bool arity_is_allowed(const sort_expression& s, const size_t a)
{
  if (a == 0)
  {
    return true;
  }
  if (is_function_sort(s))
  {
    const function_sort& fs = atermpp::down_cast<function_sort>(s);
    size_t n = fs.domain().size();
    return n <= a && arity_is_allowed(fs.codomain(), a - n);
  }
  return false;
}

static void term2seq(const data_expression& t, match_tree_list& s, size_t *var_cnt, const bool ommit_head)
{
  if (is_function_symbol(t))
  {
    const function_symbol f(t);
    s.push_front(match_tree_F(f,dummy,dummy));
    s.push_front(match_tree_D(dummy,0));
    return;
  }

  if (is_variable(t))
  {
    const variable v(t);
    match_tree store = match_tree_S(v,dummy);

    if (std::find(s.begin(),s.end(),store) != s.end())
    {
      s.push_front(match_tree_M(v,dummy,dummy));
    }
    else
    {
      (*var_cnt)++;
      s.push_front(store);
    }
    return;
  }

  assert(is_application(t));
  const application ta(t);
  size_t arity = ta.size();

  if (is_application(ta.head()))
  {
    term2seq(ta.head(),s,var_cnt,ommit_head);
    s.push_front(match_tree_N(dummy,0));
  }
  else if (!ommit_head)
  {
    {
      s.push_front(match_tree_F(function_symbol(ta.head()),dummy,dummy));
    }
  }

  size_t j=1;
  for (application::const_iterator i=ta.begin(); i!=ta.end(); ++i,++j)
  {
    term2seq(*i,s,var_cnt,false);
    if (j<arity)
    {
      s.push_front(match_tree_N(dummy,0));
    }
  }

  if (!ommit_head)
  {
    s.push_front(match_tree_D(dummy,0));
  }
}

static variable_or_number_list get_used_vars(const data_expression& t)
{
  std::set <variable> vars = find_free_variables(t);
  return variable_or_number_list(vars.begin(),vars.end());
}

static match_tree_list create_sequence(const data_equation& rule, size_t* var_cnt)
{
  const data_expression lhs_inner = rule.lhs();
  const data_expression cond = rule.condition();
  const data_expression rslt = rule.rhs();
  match_tree_list rseq;

  if (!is_function_symbol(lhs_inner))
  {
    const application lhs_innera(lhs_inner);
    size_t lhs_arity = lhs_innera.size();

    if (is_application(lhs_innera.head()))
    {
      term2seq(lhs_innera.head(),rseq,var_cnt,true);
      rseq.push_front(match_tree_N(dummy,0));
    }

    size_t j=1;
    for (application::const_iterator i=lhs_innera.begin(); i!=lhs_innera.end(); ++i,++j)
    {
      term2seq(*i,rseq,var_cnt,false);
      if (j<lhs_arity)
      {
        rseq.push_front(match_tree_N(dummy,0));
      }
    }
  }

  if (cond==sort_bool::true_())
  {
    rseq.push_front(match_tree_Re(rslt,get_used_vars(rslt)));
  }
  else
  {
    rseq.push_front(match_tree_CRe(cond,rslt, get_used_vars(cond), get_used_vars(rslt)));
  }

  return reverse(rseq);
}


// Structure for build_tree parameters
typedef struct
{
  match_tree_list_list Flist;       // List of sequences of which the first action is an F
  match_tree_list_list Slist;       // List of sequences of which the first action is an S
  match_tree_list_list Mlist;       // List of sequences of which the first action is an M
  match_tree_list_list_list stack;  // Stack to maintain the sequences that do not have to
                                    // do anything in the current term
  match_tree_list_list upstack;     // List of sequences that have done an F at the current
                                    // level
} build_pars;

static void initialise_build_pars(build_pars* p)
{
  p->Flist = match_tree_list_list();
  p->Slist = match_tree_list_list();
  p->Mlist = match_tree_list_list();
  p->stack = make_list<match_tree_list_list>(match_tree_list_list());
  p->upstack = match_tree_list_list();
}

static match_tree_list_list_list add_to_stack(const match_tree_list_list_list& stack, const match_tree_list_list& seqs, match_tree_Re& r, match_tree_list& cr)
{
  if (stack.empty())
  {
    return stack;
  }

  match_tree_list_list l;
  match_tree_list_list h = stack.front();

  for (match_tree_list_list:: const_iterator e=seqs.begin(); e!=seqs.end(); ++e)
  {
    if (e->front().isD())
    {
      l.push_front(e->tail());
    }
    else if (e->front().isN())
    {
      h.push_front(e->tail());
    }
    else if (e->front().isRe())
    {
      r = match_tree_Re(e->front());
    }
    else
    {
      cr.push_front(e->front());
    }
  }

  match_tree_list_list_list result=add_to_stack(stack.tail(),l,r,cr);
  result.push_front(h);
  return result;
}

static void add_to_build_pars(build_pars* pars,  const match_tree_list_list& seqs, match_tree_Re& r, match_tree_list& cr)
{
  match_tree_list_list l;

  for (match_tree_list_list:: const_iterator e=seqs.begin(); e!=seqs.end(); ++e)
  {
    if (e->front().isD() || e->front().isN())
    {
      l.push_front(*e);
    }
    else if (e->front().isS())
    {
      pars->Slist.push_front(*e);
    }
    else if (e->front().isMe())     // M should not appear at the head of a seq
    {
      pars->Mlist.push_front(*e);
    }
    else if (e->front().isF())
    {
      pars->Flist.push_front(*e);
    }
    else if (e->front().isRe())
    {
      r = e->front();
    }
    else
    {
      cr.push_front(e->front());
    }
  }

  pars->stack = add_to_stack(pars->stack,l,r,cr);
}

static char tree_var_str[20];
static variable createFreshVar(const sort_expression& sort, size_t* i)
{
  sprintf(tree_var_str,"@var_%lu",(*i)++);
  return data::variable(tree_var_str, sort);
}

static match_tree_list subst_var(const match_tree_list& l,
                                 const variable& old,
                                 const variable& new_val,
                                 const size_t num,
                                 const mutable_map_substitution<>& substs)
{
  match_tree_vector result;
  for(match_tree_list::const_iterator i=l.begin(); i!=l.end(); ++i)
  {
    match_tree head=*i;
    if (head.isM())
    {
      const match_tree_M headM(head);
      if (headM.match_variable()==old)
      {
        assert(headM.true_tree()==dummy);
        assert(headM.false_tree()==dummy);
        head = match_tree_Me(new_val,num);
      }
    }
    else if (head.isCRe())
    {
      const match_tree_CRe headCRe(head);
      variable_or_number_list l = headCRe.variables_condition(); // This is a list with variables and aterm_ints.
      variable_or_number_list m ;                                // Idem.
      for (; !l.empty(); l=l.tail())
      {
        if (l.front()==old)
        {
          m.push_front(atermpp::aterm_int(num));
        }
        else
        {
          m.push_front(l.front());
        }
      }
      l = headCRe.variables_result();
      variable_or_number_list n;
      for (; !l.empty(); l=l.tail())
      {
        if (l.front()==old)
        {
          n.push_front(atermpp::aterm_int(num));
        }
        else
        {
          n.push_front(l.front());
        }
      }
      head = match_tree_CRe(replace_free_variables(headCRe.condition(),substs),replace_free_variables(headCRe.result(),substs),m, n);
    }
    else if (head.isRe())
    {
      const match_tree_Re& headRe(head);
      variable_or_number_list l = headRe.variables();
      variable_or_number_list m ;
      for (; !l.empty(); l=l.tail())
      {
        if (l.front()==old)
        {
          m.push_front(atermpp::aterm_int(num));
        }
        else
        {
          m.push_front(l.front());
        }
      }
      head = match_tree_Re(replace_free_variables(headRe.result(),substs),m);
    }
    result.push_back(head);
  }
  return match_tree_list(result.begin(),result.end());
}

static std::vector < size_t> treevars_usedcnt;

static void inc_usedcnt(const variable_or_number_list& l)
{
  for (variable_or_number_list::const_iterator i=l.begin(); i!=l.end(); ++i)
  {
    if (i->type_is_int())
    {
      treevars_usedcnt[deprecated_cast<atermpp::aterm_int>(*i).value()]++;
    }
  }
}

static match_tree build_tree(build_pars pars, size_t i)
{
  if (!pars.Slist.empty())
  {
    match_tree_list l;
    match_tree_list_list m;

    size_t k = i;
    const variable v = createFreshVar(match_tree_S(pars.Slist.front().front()).target_variable().sort(),&i);
    treevars_usedcnt[k] = 0;

    for (; !pars.Slist.empty(); pars.Slist=pars.Slist.tail())
    {
      match_tree_list e = pars.Slist.front();

      mutable_map_substitution<> sigma;
      sigma[match_tree_S(e.front()).target_variable()]=v;
      e = subst_var(e,
                    match_tree_S(e.front()).target_variable(),
                    v,
                    k,
                    sigma);

      l.push_front(e.front());
      m.push_front(e.tail());
    }

    match_tree_Re r;
    match_tree ret;
    match_tree_list readies;

    pars.stack = add_to_stack(pars.stack,m,r,readies);

    if (!r.is_defined())
    {
      match_tree tree = build_tree(pars,i);
      for (match_tree_list::const_iterator i=readies.begin(); i!=readies.end(); ++i)
      {
        match_tree_CRe t(*i);
        inc_usedcnt(t.variables_condition());
        inc_usedcnt(t.variables_result());
        tree = match_tree_C(t.condition(),match_tree_R(t.result()),tree);
      }
      ret = tree;
    }
    else
    {

      inc_usedcnt(r.variables());
      ret = match_tree_R(r.result());
    }

    if ((treevars_usedcnt[k] > 0) || ((k == 0) && ret.isR()))
    {
       return match_tree_S(v,ret);
    }
    else
    {
       return ret;
    }
  }
  else if (!pars.Mlist.empty())
  {
    match_tree_Me M(pars.Mlist.front().front());

    match_tree_list_list l;
    match_tree_list_list m;
    for (; !pars.Mlist.empty(); pars.Mlist=pars.Mlist.tail())
    {
      if (M==pars.Mlist.front().front())
      {
        l.push_front(pars.Mlist.front().tail());
      }
      else
      {
        m.push_front(pars.Mlist.front());
      }
    }
    pars.Mlist = m;

    match_tree true_tree,false_tree;
    match_tree_Re r ;
    match_tree_list readies;

    match_tree_list_list_list newstack = add_to_stack(pars.stack,l,r,readies);

    false_tree = build_tree(pars,i);

    if (!r.is_defined())
    {
      pars.stack = newstack;
      true_tree = build_tree(pars,i);
      for (; !readies.empty(); readies=readies.tail())
      {
        match_tree_CRe t(readies.front());
        inc_usedcnt(t.variables_condition());
        inc_usedcnt(t.variables_result());
        true_tree = match_tree_C(t.condition(), match_tree_R(t.result()),true_tree);
      }
    }
    else
    {
      inc_usedcnt(r.variables());
      true_tree = match_tree_R(r.result());
    }

    if (true_tree==false_tree)
    {
       return true_tree;
    }
    else
    {
      treevars_usedcnt[M.variable_index()]++;
      return match_tree_M(M.match_variable(),true_tree,false_tree);
    }
  }
  else if (!pars.Flist.empty())
  {
    match_tree_list F = pars.Flist.front();
    match_tree true_tree,false_tree;

    match_tree_list_list newupstack = pars.upstack;
    match_tree_list_list l;

    for (; !pars.Flist.empty(); pars.Flist=pars.Flist.tail())
    {
      if (pars.Flist.front().front()==F.front())
      {
        newupstack.push_front(pars.Flist.front().tail());
      }
      else
      {
        l.push_front(pars.Flist.front());
      }
    }

    pars.Flist = l;
    false_tree = build_tree(pars,i);
    pars.Flist = match_tree_list_list();
    pars.upstack = newupstack;
    true_tree = build_tree(pars,i);

    if (true_tree==false_tree)
    {
      return true_tree;
    }
    else
    {
      return match_tree_F(match_tree_F(F.front()).function(),true_tree,false_tree);
    }
  }
  else if (!pars.upstack.empty())
  {
    match_tree_list_list l;

    match_tree_Re r;
    match_tree_list readies;

    pars.stack.push_front(match_tree_list_list());
    l = pars.upstack;
    pars.upstack = match_tree_list_list();
    add_to_build_pars(&pars,l,r,readies);


    if (!r.is_defined())
    {
      match_tree t = build_tree(pars,i);

      for (; !readies.empty(); readies=readies.tail())
      {
        match_tree_CRe u(readies.front());
        inc_usedcnt(u.variables_condition());
        inc_usedcnt(u.variables_result());
        t = match_tree_C(u.condition(), match_tree_R(u.result()),t);
      }

      return t;
    }
    else
    {
      inc_usedcnt(r.variables());
      return match_tree_R(r.result());
    }
  }
  else
  {
    if (pars.stack.front().empty())
    {
      if (pars.stack.tail().empty())
      {
        return match_tree_X();
      }
      else
      {
        pars.stack = pars.stack.tail();
        return match_tree_D(build_tree(pars,i),0);
      }
    }
    else
    {
      match_tree_list_list l = pars.stack.front();
      match_tree_Re r ;
      match_tree_list readies;

      pars.stack = pars.stack.tail();
      pars.stack.push_front(match_tree_list_list());
      add_to_build_pars(&pars,l,r,readies);

      match_tree tree;
      if (!r.is_defined())
      {
        tree = build_tree(pars,i);
        for (; !readies.empty(); readies=readies.tail())
        {
          match_tree_CRe t(readies.front());
          inc_usedcnt(t.variables_condition());
          inc_usedcnt(t.variables_result());
          tree = match_tree_C(t.condition(), match_tree_R(t.result()),tree);
        }
      }
      else
      {
        inc_usedcnt(r.variables());
        tree = match_tree_R(r.result());
      }

      return match_tree_N(tree,0);
    }
  }
}

static match_tree create_tree(const data_equation_list& rules)
// Create a match tree for OpId int2term[opid] and update the value of
// *max_vars accordingly.
//
// Pre:  rules is a list of rewrite rules for some function symbol f.
//       max_vars is a valid pointer to an integer
// Post: *max_vars is the maximum of the original *max_vars value and
//       the number of variables in the result tree
// Ret:  A match tree for function symbol f.
{
  // Create sequences representing the trees for each rewrite rule and
  // store the total number of variables used in these sequences.
  // (The total number of variables in all sequences should be an upper
  // bound for the number of variable in the final tree.)
  match_tree_list_list rule_seqs;
  size_t total_rule_vars = 0;
  for (data_equation_list::const_iterator it=rules.begin(); it!=rules.end(); ++it)
  {
    rule_seqs.push_front(create_sequence(*it,&total_rule_vars));
  }
  // Generate initial parameters for built_tree
  build_pars init_pars;
  match_tree_Re r;
  match_tree_list readies;

  initialise_build_pars(&init_pars);
  add_to_build_pars(&init_pars,rule_seqs,r,readies);
  match_tree tree;
  if (!r.is_defined())
  {
    treevars_usedcnt=std::vector < size_t> (total_rule_vars);
    tree = build_tree(init_pars,0);
    for (; !readies.empty(); readies=readies.tail())
    {
      match_tree_CRe u(readies.front());
      tree = match_tree_C(u.condition(), match_tree_R(u.result()), tree);
    }
  }
  else
  {
    tree = match_tree_R(r.result());
  }

  return tree;
}

// This function assigns a unique index to variable v and stores
// v at this position in the vector rewriter_bound_variables. This is
// used in the compiling rewriter to obtain this variable again.
// Note that the static variable variable_indices is not cleared
// during several runs, as generally the variables bound in rewrite
// rules do not change.
size_t RewriterCompilingJitty::bound_variable_index(const variable& v)
{
  if (variable_indices0.count(v)>0)
  {
    return variable_indices0[v];
  }
  const size_t index_for_v=rewriter_bound_variables.size();
  variable_indices0[v]=index_for_v;
  rewriter_bound_variables.push_back(v);
  return index_for_v;
}

// This function assigns a unique index to variable list vl and stores
// vl at this position in the vector rewriter_binding_variable_lists. This is
// used in the compiling rewriter to obtain this variable again.
// Note that the static variable variable_indices is not cleared
// during several runs, as generally the variables bound in rewrite
// rules do not change.
size_t RewriterCompilingJitty::binding_variable_list_index(const variable_list& vl)
{
  if (variable_list_indices1.count(vl)>0)
  {
    return variable_list_indices1[vl];
  }
  const size_t index_for_vl=rewriter_binding_variable_lists.size();
  variable_list_indices1[vl]=index_for_vl;
  rewriter_binding_variable_lists.push_back(vl);
  return index_for_vl;
}

// Put the sorts with indices between actual arity and requested arity in a vector.
sort_list_vector RewriterCompilingJitty::get_residual_sorts(const sort_expression& s1, size_t actual_arity, size_t requested_arity)
{
  sort_expression s=s1;
  sort_list_vector result;
  while (requested_arity>0)
  {
    const function_sort fs(s);
    if (actual_arity==0)
    {
      result.push_back(fs.domain());
      assert(fs.domain().size()<=requested_arity);
      requested_arity=requested_arity-fs.domain().size();
    }
    else
    {
      assert(fs.domain().size()<=actual_arity);
      actual_arity=actual_arity-fs.domain().size();
      requested_arity=requested_arity-fs.domain().size();
    }
    s=fs.codomain();
  }
  return result;
}

/// Rewrite the equation e such that it has exactly a arguments.
/// If the rewrite rule has too many arguments, false is returned, otherwise
/// additional arguments are added.

bool RewriterCompilingJitty::lift_rewrite_rule_to_right_arity(data_equation& e, const size_t requested_arity)
{
  data_expression lhs=e.lhs();
  data_expression rhs=e.rhs();
  variable_list vars=e.variables();

  function_symbol f;
  if (!head_is_function_symbol(lhs,f))
  {
    throw mcrl2::runtime_error("Equation " + pp(e) + " does not start with a function symbol in its left hand side.");
  }

  size_t actual_arity=recursive_number_of_args(lhs);
  if (arity_is_allowed(f.sort(),requested_arity) && actual_arity<=requested_arity)
  {
    if (actual_arity<requested_arity)
    {
      // Supplement the lhs and rhs with requested_arity-actual_arity extra variables.
      sort_list_vector requested_sorts=get_residual_sorts(f.sort(),actual_arity,requested_arity);
      for(sort_list_vector::const_iterator sl=requested_sorts.begin(); sl!=requested_sorts.end(); ++sl)
      {
        variable_vector var_vec;
        for(sort_expression_list::const_iterator s=sl->begin(); s!=sl->end(); ++s)
        {
          variable v=variable(jitty_rewriter.generator("v@r"),*s); // Find a new name for a variable that is temporarily in use.
          var_vec.push_back(v);
          vars.push_front(v);
        }
        lhs=application(lhs,var_vec.begin(),var_vec.end());
        rhs=application(rhs,var_vec.begin(),var_vec.end());
      }
    }
  }
  else
  {
    return false; // This is not an allowed arity, or the actual number of arguments is larger than the requested number.
  }

  e=data_equation(vars,e.condition(),lhs,rhs);
  return true;
}

match_tree_list RewriterCompilingJitty::create_strategy(const data_equation_list& rules, const size_t arity, const nfs_array& nfs)
{
  typedef std::list<size_t> dep_list_t;
  match_tree_list strat;

  // Maintain dependency count (i.e. the number of rules that depend on a given argument)
  std::vector<size_t> arg_use_count(arity, 0);
  std::list<std::pair<data_equation, dep_list_t> > rule_deps;
  for (auto it = rules.begin(); it != rules.end(); ++it)
  {
    if (recursive_number_of_args(it->lhs()) <= arity)
    {
      rule_deps.push_front(std::make_pair(*it, dep_list_t()));
      dep_list_t& deps = rule_deps.front().second;

      const std::vector<bool> is_dependent_arg = dep_vars(*it);
      for (size_t i = 0; i < is_dependent_arg.size(); i++)
      {
        // Only if needed and not already rewritten
        if (is_dependent_arg[i] && !nfs[i])
        {
          deps.push_back(i);
          // Increase dependency count
          arg_use_count[i] += 1;
        }
      }
    }
  }

  // Process all rules with their dependencies
  while (!rule_deps.empty())
  {
    data_equation_list no_deps;
    for (auto it = rule_deps.begin(); it != rule_deps.end(); )
    {
      if (it->second.empty())
      {
        lift_rewrite_rule_to_right_arity(it->first, arity);
        no_deps.push_front(it->first);
        it = rule_deps.erase(it);
      }
      else
      {
        ++it;
      }
    }

    // Create and add tree of collected rules
    if (!no_deps.empty())
    {
      strat.push_front(create_tree(no_deps));
    }

    // Figure out which argument is most useful to rewrite
    size_t max = 0;
    size_t maxidx = 0;
    for (size_t i = 0; i < arity; i++)
    {
      if (arg_use_count[i] > max)
      {
        maxidx = i;
        max = arg_use_count[i];
      }
    }

    // If there is a maximum, add it to the strategy and remove it from the dependency lists
    assert(rule_deps.empty() || max > 0);
    if (max > 0)
    {
      assert(!rule_deps.empty());
      arg_use_count[maxidx] = 0;
      strat.push_front(match_tree_A(maxidx));
      for (auto it = rule_deps.begin(); it != rule_deps.end(); ++it)
      {
        it->second.remove(maxidx);
      }
    }
  }
  return reverse(strat);
}

void RewriterCompilingJitty::add_base_nfs(nfs_array& nfs, const function_symbol& opid, size_t arity)
{
  for (size_t i=0; i<arity; i++)
  {
    if (m_always_rewrite->always_rewrite(opid, arity, i))
    {
      nfs.at(i) = true;
    }
  }
}

void RewriterCompilingJitty::extend_nfs(nfs_array& nfs, const function_symbol& opid, size_t arity)
{
  data_equation_list eqns = jittyc_eqns[opid];
  if (eqns.empty())
  {
    nfs.fill(arity);
    return;
  }
  match_tree_list strat = create_strategy(eqns,arity,nfs);
  while (!strat.empty() && strat.front().isA())
  {
    nfs.at(match_tree_A(strat.front()).variable_index()) = true;
    strat = strat.tail();
  }
}

// Determine whether the opid is a normal form, with the given number of arguments.
bool RewriterCompilingJitty::opid_is_nf(const function_symbol& opid, size_t num_args)
{
  // Check whether there are applicable rewrite rules.
  data_equation_list l = jittyc_eqns[opid];

  if (l.empty())
  {
    return true;
  }

  for (data_equation_list::const_iterator it=l.begin(); it!=l.end(); ++it)
  {
    if (recursive_number_of_args(it->lhs()) <= num_args)
    {
      return false;
    }
  }

  return true;
}

void RewriterCompilingJitty::calc_nfs_list(
                nfs_array& nfs,
                const application& appl,
                variable_or_number_list nnfvars)
{
  for(size_t i=0; i<recursive_number_of_args(appl); ++i)
  {
    nfs.at(i) = calc_nfs(get_argument_of_higher_order_term(appl,i),nnfvars);
  }
}

/// This function returns true if it knows for sure that the data_expresson t is in normal form.
bool RewriterCompilingJitty::calc_nfs(const data_expression& t, variable_or_number_list nnfvars)
{
  if (is_function_symbol(t))
  {
    return opid_is_nf(down_cast<function_symbol>(t),0);
  }
  else if (is_variable(t))
  {
    return (std::find(nnfvars.begin(),nnfvars.end(),variable(t)) == nnfvars.end());
  }
  else if (is_abstraction(t))
  {
    // It the term has the shape lambda x:D.t and t is a normal form, then the whole
    // term is a normal form. An expression with an exists/forall is never a normal form.
    const abstraction& ta(t);
    if (is_lambda_binder(ta.binding_operator()))
    {
      return calc_nfs(ta.body(),nnfvars);
    }
    return false;
  }
  else if (is_where_clause(t))
  {
    return false; // I assume that a where clause is not in normal form by default.
                  // This might be too weak, and may require to be reinvestigated later.
  }

  // t has the shape application(head,t1,...,tn)
  const application ta(t);
  const size_t arity = recursive_number_of_args(ta);
  const data_expression& head=ta.head();
  function_symbol dummy;
  if (head_is_function_symbol(head,dummy))
  {
    assert(arity!=0);
    if (opid_is_nf(down_cast<function_symbol>(head),arity))
    {
      nfs_array args(arity);
      calc_nfs_list(args,ta,nnfvars);
      bool b = args.is_filled();
      return b;
    }
    else
    {
      return false;
    }
  }
  else
  {
    return false;
  }
}

string RewriterCompilingJitty::calc_inner_terms(
              nfs_array& nfs,
              const application& appl,
              const size_t startarg,
              variable_or_number_list nnfvars,
              const nfs_array& rewr)
{
  size_t j=0;
  string result="";
  for(application::const_iterator i=appl.begin(); i!=appl.end(); ++i, ++j)
  {
    pair<bool,string> head = calc_inner_term(*i, startarg+j,nnfvars,rewr.at(j));
    nfs.at(j) = head.first;

    result=result + (j==0?"":",") + head.second;
  }
  return result;
}

// arity is one if there is a single head. Arity is two is there is a head and one argument, etc.
static string calc_inner_appl_head(size_t arity)
{
  stringstream ss;
  if (arity == 1)
  {
    ss << "pass_on(";  // This is to avoid confusion with atermpp::aterm_appl on a function symbol and two iterators.
  }
  else if (arity <= 5)
  {
    ss << "application(";
  }
  else
  {
    ss << "make_term_with_many_arguments(";
  }
  return ss.str();
}

/// This function generates a string of C++ code to calculate the data_expression t.
/// If the result is a normal form the resulting boolean is true, otherwise it is false.
/// The data expression t is the term for which C code is generated.
/// The size_t start_arg gives the index of the position of the current term in the surrounding application.
//                 The head has index 0. The first argument has index 1, etc.
/// The variable_or_number_list nnfvars contains variables that are in normal form, and indices that are not in normal form.
/// The bool rewr indicates whether a normal form is requested. If rewr is valid, then yes.

pair<bool,string> RewriterCompilingJitty::calc_inner_term(
                             const data_expression& t,
                             const size_t startarg,
                             variable_or_number_list nnfvars,
                             const bool rewr)
{
  stringstream ss;
  // Experiment: if the term has no free variables, deliver the normal form directly.
  // This code can be removed, when it does not turn out to be useful.
  // This requires the use of the jitty rewriter to calculate normal forms.

  if (find_free_variables(t).empty())
  {
    return pair<bool,string>(true, m_nf_cache.insert(t));
  }

  if (is_function_symbol(t))
  {
    const function_symbol& f = atermpp::down_cast<function_symbol>(t);
    bool b = opid_is_nf(f,0);

    if (rewr && !b)
    {
      ss << "rewr_" << core::index_traits<data::function_symbol,function_symbol_key_type, 2>::index(f) << "_0_0()";
    }
    else
    {
      ss << "atermpp::down_cast<const data_expression>(aterm(reinterpret_cast<const atermpp::detail::_aterm*>(" << atermpp::detail::address(t) << ")))";
    }
    return pair<bool,string>(rewr || b, ss.str());

  }
  else if (is_variable(t))
  {
    const variable v(t);
    const bool b = (std::find(nnfvars.begin(),nnfvars.end(),v) != nnfvars.end());
    const string variable_name=v.name();
    // Remove the initial @ if it is present in the variable name, because then it is an variable introduced
    // by this rewriter.
    if (variable_name[0]=='@')
    {
      if (rewr && b)
      {
        ss << "rewrite(" << variable_name.substr(1) << ")";
      }
      else
      {
        ss << variable_name.substr(1);
      }
    }
    else
    {
      ss << "this_rewriter->bound_variable_get(" << bound_variable_index(v) << ")";
    }
    return pair<bool,string>(rewr || !b, ss.str());
  }
  else if (is_abstraction(t))
  {
    const abstraction& ta(t);
    if (is_lambda_binder(ta.binding_operator()))
    {
      if (rewr)
      {
        // The resulting term must be rewritten.
        pair<bool,string> r=calc_inner_term(ta.body(),startarg,nnfvars,true);
        ss << "this_rewriter->rewrite_single_lambda(" <<
               "this_rewriter->binding_variable_list_get(" << binding_variable_list_index(ta.variables()) << ")," <<
               r.second << "," << r.first << ",*(this_rewriter->global_sigma))";
        return pair<bool,string>(true,ss.str());
      }
      else
      {
        pair<bool,string> r=calc_inner_term(ta.body(),startarg,nnfvars,false);
        ss << "abstraction(lambda_binder()," <<
               "this_rewriter->binding_variable_list_get(" << binding_variable_list_index(ta.variables()) << ")," <<
               r.second << ")";
        return pair<bool,string>(false,ss.str());
      }
    }
    else if (is_forall_binder(ta.binding_operator()))
    {
      if (rewr)
      {
        // A result in normal form is requested.
        pair<bool,string> r=calc_inner_term(ta.body(),startarg,nnfvars,false);
        ss << "this_rewriter->universal_quantifier_enumeration(" <<
               "this_rewriter->binding_variable_list_get(" << binding_variable_list_index(ta.variables()) << ")," <<
               r.second << "," << r.first << "," << "*(this_rewriter->global_sigma))";
        return pair<bool,string>(true,ss.str());
      }
      else
      {
        // A result which is not a normal form is requested.
        pair<bool,string> r=calc_inner_term(ta.body(),startarg,nnfvars,false);
        ss << "abstraction(forall_binder()," <<
               "this_rewriter->binding_variable_list_get(" << binding_variable_list_index(ta.variables()) << ")," <<
               r.second << ")";
        return pair<bool,string>(false,ss.str());
      }
    }
    else if (is_exists_binder(ta.binding_operator()))
    {
      if (rewr)
      {
        // A result in normal form is requested.
        pair<bool,string> r=calc_inner_term(ta.body(),startarg,nnfvars,false);
        ss << "this_rewriter->existential_quantifier_enumeration(" <<
               "this_rewriter->binding_variable_list_get(" << binding_variable_list_index(ta.variables()) << ")," <<
               r.second << "," << r.first << "," << "*(this_rewriter->global_sigma))";
        return pair<bool,string>(true,ss.str());
      }
      else
      {
        // A result which is not a normal form is requested.
        pair<bool,string> r=calc_inner_term(ta.body(),startarg,nnfvars,false);
        ss << "abstraction(exists_binder()," <<
               "this_rewriter->binding_variable_list_get(" << binding_variable_list_index(ta.variables()) << ")," <<
               r.second << ")";
        return pair<bool,string>(false,ss.str());
      }
    }
  }
  else if (is_where_clause(t))
  {
    const where_clause& w = atermpp::down_cast<where_clause>(t);

    if (rewr)
    {
      // A rewritten result is expected.
      pair<bool,string> r=calc_inner_term(w.body(),startarg,nnfvars,true);

      ss << "this_rewriter->rewrite_where(mcrl2::data::where_clause(" << r.second << ",";

      const assignment_list& assignments(w.assignments());
      for( size_t no_opening_brackets=assignments.size(); no_opening_brackets>0 ; no_opening_brackets--)
      {
        ss << "jittyc_local_push_front(";
      }
      ss << "mcrl2::data::assignment_expression_list()";
      for(assignment_list::const_iterator i=assignments.begin() ; i!=assignments.end(); ++i)
      {
        pair<bool,string> r=calc_inner_term(i->rhs(),startarg,nnfvars,true);
        ss << ",mcrl2::data::assignment(" <<
                 "this_rewriter->bound_variable_get(" << bound_variable_index(i->lhs()) << ")," <<
                 r.second << "))";
      }

      ss << "),*(this_rewriter->global_sigma))";

      return pair<bool,string>(true,ss.str());
    }
    else
    {
      // The result does not need to be rewritten.
      pair<bool,string> r=calc_inner_term(w.body(),startarg,nnfvars,false);
      ss << "mcrl2::data::where_clause(" << r.second << ",";

      const assignment_list& assignments(w.assignments());
      for( size_t no_opening_brackets=assignments.size(); no_opening_brackets>0 ; no_opening_brackets--)
      {
        ss << "jittyc_local_push_front(";
      }
      ss << "mcrl2::data::assignment_expression_list()";
      for(assignment_list::const_iterator i=assignments.begin() ; i!=assignments.end(); ++i)
      {
        pair<bool,string> r=calc_inner_term(i->rhs(),startarg,nnfvars,true);
        ss << ",mcrl2::data::assignment(" <<
                 "this_rewriter->bound_variable_get(" << bound_variable_index(i->lhs()) << ")," <<
                 r.second << "))";
      }

      ss << ")";

      return pair<bool,string>(false,ss.str());
    }
  }

  // t has the shape application(head,t1,...,tn)
  const application& ta = atermpp::down_cast<application>(t);
  bool b;
  size_t arity = ta.size();

  if (is_function_symbol(ta.head()))  // Determine whether the topmost symbol is a function symbol.
  {
    const function_symbol& headfs = atermpp::down_cast<function_symbol>(ta.head());
    size_t cumulative_arity = ta.size();
    b = opid_is_nf(headfs,cumulative_arity+1); // b indicates that headfs is in normal form.

    if (b || !rewr)
    {
      ss << calc_inner_appl_head(arity+1);
    }

    if (arity == 0)
    {
      if (b || !rewr)
      {
        // TODO: This expression might be costly with two increase/decreases of reference counting.
        // Should probably be resolved by a table of function symbols.
        ss << "atermpp::down_cast<data_expression>(atermpp::aterm((const atermpp::detail::_aterm*) " << (void*) atermpp::detail::address(headfs) << "))";
      }
      else
      {
        ss << "rewr_" << core::index_traits<data::function_symbol,function_symbol_key_type, 2>::index(headfs) << "_0_0()";
      }
    }
    else
    {
      // arity != 0
      nfs_array args_nfs(arity);
      calc_nfs_list(args_nfs,ta,nnfvars);

      if (!(b || !rewr))
      {
        ss << "rewr_";
        add_base_nfs(args_nfs,headfs,arity);
        extend_nfs(args_nfs,headfs,arity);
      }
      if (arity > NF_MAX_ARITY)
      {
        args_nfs.fill(false);
      }
      if (args_nfs.is_clear() || b || rewr || (arity > NF_MAX_ARITY))
      {
        if (b || !rewr)
        {
          ss << "atermpp::down_cast<data_expression>(atermpp::aterm((const atermpp::detail::_aterm*) " << (void*) atermpp::detail::address(headfs) << "))";
        }
        else
        {
          ss << core::index_traits<data::function_symbol,function_symbol_key_type, 2>::index(headfs);
        }
      }
      else
      {
        if (b || !rewr)
        {
          if (data_equation_selector(headfs))
          {
            const size_t index=core::index_traits<data::function_symbol,function_symbol_key_type, 2>::index(headfs);
            const data::function_symbol old_head=headfs;
            std::stringstream new_name;
            new_name << "@_rewr" << "_" << index << "_@@@_" << (getArity(headfs) > NF_MAX_ARITY ? 0 : static_cast<size_t>(args_nfs))
                                 << "_term";
            const data::function_symbol f(new_name.str(),old_head.sort());
            if (partially_rewritten_functions.count(f)==0)
            {
              partially_rewritten_functions.insert(f);
            }

            ss << "atermpp::down_cast<data_expression>(atermpp::aterm((const atermpp::detail::_aterm*) " << (void*) atermpp::detail::address(f) << "))";
          }
        }
        else
        {
          ss << (core::index_traits<data::function_symbol,function_symbol_key_type, 2>::index(headfs) + ((1 << arity)-arity-1) + static_cast<size_t>(args_nfs));
        }
      }
      nfs_array args_first(arity);
      if (rewr && b)
      {
        args_nfs.fill(arity);
      }
      string args_second = calc_inner_terms(args_first,ta,startarg,nnfvars,args_nfs);

      assert(!rewr || b || (arity > NF_MAX_ARITY) || args_first>=args_nfs);
      if (rewr && !b)
      {
        ss << "_" << arity << "_" << (arity <= NF_MAX_ARITY ? static_cast<size_t>(args_nfs) : 0) << "(";
      }
      else
      {
        ss << ",";
      }
      ss << args_second << ")";
      if (!args_first.is_filled())
      {
        b = false;
      }
    }
    b = b || rewr;

  }
  else
  {
    // ta.head() is not function symbol. So the first element of this list is
    // either an application, variable, or a lambda term. It cannot be a forall or exists, because in
    // that case there would be a type error.
    assert(arity > 0);

    if (is_abstraction(ta.head()))
    {
      const abstraction& ta1(ta.head());
      assert(is_lambda_binder(ta1.binding_operator()));

      b = rewr;
      nfs_array args_nfs(arity);
      calc_nfs_list(args_nfs,ta,nnfvars);
      if (arity > NF_MAX_ARITY)
      {
        args_nfs.fill(false);
      }

      nfs_array args_first(arity);
      if (rewr && b)
      {
        args_nfs.fill();
      }

      pair<bool,string> head = calc_inner_term(ta1,startarg,nnfvars,false);

      if (rewr)
      {
        ss << "rewrite(";
      }

      ss << calc_inner_appl_head(arity+1) << head.second << "," <<
               calc_inner_terms(args_first,ta,startarg,nnfvars,args_nfs) << ")";
      if (rewr)
      {
        ss << ")";
      }

    }
    else if (is_application(ta.head()))
    {
      const size_t arity=ta.size();
      const size_t total_arity=recursive_number_of_args(ta);
      b = rewr;
      pair<bool,string> head = calc_inner_term(ta.head(),startarg,nnfvars,false);  // XXXX TODO TAKE CARE THAT NORMAL FORMS ADMINISTRATION IS DEALT WITH PROPERLY FOR HIGHER ORDER TERMS.
      nfs_array tail_first(total_arity);
      const nfs_array dummy(total_arity);
      string tail_second = calc_inner_terms(tail_first,ta,startarg,nnfvars,dummy);
      if (rewr)
      {
          ss << "rewrite(";
      }
      ss << calc_inner_appl_head(arity+1) << head.second << "," << tail_second << ")";
      if (rewr)
      {
        ss << ")";
      }
    }
    else // headfs is a single variable.
    {
      // So, the first element of t must be a single variable.
      assert(is_variable(ta.head()));
      const size_t arity=ta.size();
      b = rewr;
      pair<bool,string> head = calc_inner_term(ta.head(),startarg,nnfvars,false);
      nfs_array tail_first(arity);
      const nfs_array dummy(arity);
      string tail_second = calc_inner_terms(tail_first,ta,startarg,nnfvars,dummy);
      ss << "!is_variable(" << head.second << ")?";
      if (rewr)
      {
          ss << "rewrite(";
      }
      ss << calc_inner_appl_head(arity+1) << head.second << "," << tail_second << ")";
      if (rewr)
      {
        ss << ")";
      }
      ss << ":";
      bool c = rewr;
      if (rewr && std::find(nnfvars.begin(),nnfvars.end(), atermpp::aterm_int(startarg)) != nnfvars.end())
      {
        ss << "rewrite(";
        c = false;
      }
      else
      {
        ss << "pass_on(";
      }
      ss << calc_inner_appl_head(arity+1) << head.second << ",";
      if (c)
      {
        tail_first.fill(false);
        nfs_array rewrall(arity);
        rewrall.fill();
        tail_second = calc_inner_terms(tail_first,ta,startarg,nnfvars,rewrall);
      }
      ss << tail_second << ")";
      ss << ")";
    }
  }

  return pair<bool,string>(b,ss.str());
}

class RewriterCompilingJitty::ImplementTree
{
private:
  class padding
  {
  private:
    size_t m_indent;
  public:
    padding(size_t indent) : m_indent(indent) { }
    void indent() { m_indent += 2; }
    void unindent() { m_indent -= 2; }
    
    friend
    std::ostream& operator<<(std::ostream& stream, const padding& p)
    {
      for (size_t i = p.m_indent; i != 0; --i)
      {
        stream << ' ';
      }
      return stream;
    }
  };
  
  RewriterCompilingJitty& m_rewriter;
  std::ostream& m_stream;
  std::vector<bool> m_used;
  std::vector<int> m_stack;
  padding m_padding;
  variable_or_number_list m_nnfvars;
  
  // Print code representing tree to f.
  //
  // cur_arg   Indices refering to the variable that contains the current
  // parent    term. For level 0 this means arg<cur_arg>, for level 1 it
  //           means arg<parent>(<cur_arg) and for higher
  //           levels it means t<parent>(<cur_arg>)
  //
  // parent    Index of cur_arg in the previous level
  //
  // level     Indicates the how deep we are in the term (e.g. in
  //           f(.g(x),y) . indicates level 0 and in f(g(.x),y) level 1
  //
  // cnt       Counter indicating the number of variables t<i> (0<=i<cnt)
  //           used so far (in the current scope)
  void implement_tree_aux(const match_tree& tree, size_t cur_arg, size_t parent, size_t level, size_t cnt)
  {
    if (tree.isS())
    {
      implement_tree_aux(atermpp::down_cast<match_tree_S>(tree), cur_arg, parent, level, cnt);
    }
    else if (tree.isM())
    {
      implement_tree_aux(atermpp::down_cast<match_tree_M>(tree), cur_arg, parent, level, cnt);
    }
    else if (tree.isF())
    {
      implement_tree_aux(atermpp::down_cast<match_tree_F>(tree), cur_arg, parent, level, cnt);
    }
    else if (tree.isD())
    {
      implement_tree_aux(atermpp::down_cast<match_tree_D>(tree), level, cnt);
    }
    else if (tree.isN())
    {
      implement_tree_aux(atermpp::down_cast<match_tree_N>(tree), cur_arg, parent, level, cnt);
    }
    else if (tree.isC())
    {
      implement_tree_aux(atermpp::down_cast<match_tree_C>(tree), cur_arg, parent, level, cnt);
    }
    else if (tree.isR())
    {
      implement_tree_aux(atermpp::down_cast<match_tree_R>(tree), cur_arg, level);
    }
    else 
    {
      // These are the only remaining case, where we do not have to do anything.
      assert(tree.isA() || tree.isX() || tree.isMe()); 
    }    
  }
  
  void implement_tree_aux(const match_tree_S& tree, size_t cur_arg, size_t parent, size_t level, size_t cnt)
  {
    const match_tree_S& treeS(tree);
    m_stream << m_padding << "const data_expression& " << string(treeS.target_variable().name()).c_str() + 1 << " = ";
    if (level == 0)
    {
      if (m_used[cur_arg])
      {
        m_stream << "arg" << cur_arg << "; // S1\n";
      }
      else
      {
        m_stream << "arg_not_nf" << cur_arg << "; // S1\n";
        m_nnfvars.push_front(treeS.target_variable());
      }
    }
    else
    {
      m_stream << "down_cast<data_expression>("
               << (level == 1 ? "arg" : "t") << parent << "[" << cur_arg << "]"
               << "); // S2\n";
    }
    implement_tree_aux(tree.subtree(), cur_arg, parent, level, cnt);
  }
  
  void implement_tree_aux(const match_tree_M& tree, size_t cur_arg, size_t parent, size_t level, size_t cnt)
  {
    m_stream << m_padding << "if (" << string(tree.match_variable().name()).c_str() + 1 << " == ";
    if (level == 0)
    {
      m_stream << "arg" << cur_arg;
    }
    else
    {
      m_stream << (level == 1 ? "arg" : "t") << parent << "[" << cur_arg << "]";
    }
    m_stream << ") // M\n" << m_padding 
             << "{\n";
    m_padding.indent();
    implement_tree_aux(tree.true_tree(), cur_arg, parent, level, cnt);
    m_padding.unindent();
    m_stream << m_padding
             << "}\n" << m_padding 
             << "else\n" << m_padding
             << "{\n";
    m_padding.indent();
    implement_tree_aux(tree.false_tree(), cur_arg, parent, level, cnt);
    m_padding.unindent();
    m_stream << m_padding
             << "}\n";
  }
  
  void implement_tree_aux(const match_tree_F& tree, size_t cur_arg, size_t parent, size_t level, size_t cnt)
  {
    const void* func = (void*)(atermpp::detail::address(tree.function()));
    const char* s_arg = "arg";
    const char* s_t = "t";
    m_stream << m_padding;
    if (level == 0)
    {
      if (!is_function_sort(tree.function().sort()))
      {
        m_stream << "if (uint_address(arg" << cur_arg << ") == " << func << ") // F1\n" << m_padding
                 << "{\n";
      }
      else
      {
        m_stream << "if (uint_address((is_function_symbol(arg" << cur_arg <<  ") ? arg" << cur_arg << " : arg" << cur_arg << "[0])) == "
                 << func << ") // F1\n" << m_padding
                 << "{\n";
      }
    }
    else
    {
      const char* array = level == 1 ? s_arg : s_t;
      if (!is_function_sort(tree.function().sort()))
      {
        m_stream << "if (uint_address(" << array << parent << "[" << cur_arg << "]) == "
                 << func << ") // F2a " << tree.function().name() << "\n" << m_padding
                 << "{\n" << m_padding
                 << "  const data_expression& t" << cnt << " = down_cast<data_expression>(" << array << parent << "[" << cur_arg << "]);\n";
      }
      else
      { 
        m_stream << "if (is_application_no_check(down_cast<data_expression>(" << array << parent << "[" << cur_arg << "])) && "
                 <<     "uint_address(down_cast<data_expression>(" << array << parent << "[" << cur_arg << "])[0]) == "
                 << func << ") // F2b " << tree.function().name() << "\n" << m_padding
                 << "{\n" << m_padding
                 << "  const data_expression& t" << cnt << " = down_cast<data_expression>(" << array << parent << "[" << cur_arg << "]);\n";
      }
    }
    m_stack.push_back(cur_arg);
    m_stack.push_back(parent);
    m_padding.indent();
    implement_tree_aux(tree.true_tree(), 1, level == 0 ? cur_arg : cnt, level + 1, cnt + 1);
    m_padding.unindent();
    m_stack.pop_back();
    m_stack.pop_back();
    m_stream << m_padding
             << "}\n" << m_padding
             << "else\n" << m_padding
             << "{\n";
    m_padding.indent();
    implement_tree_aux(tree.false_tree(), cur_arg, parent, level, cnt);
    m_padding.unindent();
    m_stream << m_padding
             << "}\n";
  }
  
  void implement_tree_aux(const match_tree_D& tree, size_t level, size_t cnt)
  {
    int i = m_stack.back();
    m_stack.pop_back();
    int j = m_stack.back();
    m_stack.pop_back();
    implement_tree_aux(tree.subtree(), j, i, level - 1, cnt);
    m_stack.push_back(j);
    m_stack.push_back(i);
  }

  void implement_tree_aux(const match_tree_N& tree, size_t cur_arg, size_t parent, size_t level, size_t cnt)
  {
    implement_tree_aux(tree.subtree(), cur_arg + 1, parent, level, cnt);
  }

  void implement_tree_aux(const match_tree_C& tree, size_t cur_arg, size_t parent, size_t level, size_t cnt)
  {
    pair<bool, string> p = m_rewriter.calc_inner_term(tree.condition(), 0, m_nnfvars);
    m_stream << m_padding
             << "if (" << p.second << " == sort_bool::true_()) // C\n" << m_padding
             << "{\n";
    
    m_padding.indent();
    implement_tree_aux(tree.true_tree(), cur_arg, parent, level, cnt);
    m_padding.unindent();
    
    m_stream << m_padding
             << "}\n" << m_padding
             << "else\n" << m_padding
             << "{\n";
          
    m_padding.indent();
    implement_tree_aux(tree.false_tree(), cur_arg, parent, level, cnt);
    m_padding.unindent();
    
    m_stream << m_padding
             << "}\n";
  }
  
  void implement_tree_aux(const match_tree_R& tree, size_t cur_arg, size_t level)
  {
    if (level > 0)
    {
      cur_arg = m_stack[2 * level - 1];
    }
    pair<bool, string> p = m_rewriter.calc_inner_term(tree.result(), cur_arg + 1, m_nnfvars);
    m_stream << m_padding << "return " << p.second << "; // R1\n";
  }

  const match_tree& implement_tree(const match_tree_C& tree)
  {
    assert(tree.true_tree().isR());
    pair<bool, string> p = m_rewriter.calc_inner_term(tree.condition(), 0, variable_or_number_list());
    pair<bool, string> r = m_rewriter.calc_inner_term(match_tree_R(tree.true_tree()).result(), 0, m_nnfvars);
    m_stream << m_padding
             << "if (" << p.second << " == sort_bool::true_()) // C\n" << m_padding
             << "{\n" << m_padding
             << "  return " << r.second << ";\n" << m_padding
             << "}\n" << m_padding
             << "else\n" << m_padding
             << "{\n" << m_padding;
    m_padding.indent();
    return tree.false_tree();
  }
  
  void implement_tree(const match_tree_R& tree, size_t arity)
  {
    pair<bool, string> p = m_rewriter.calc_inner_term(tree.result(), 0, m_nnfvars);
    if (arity == 0)
    { 
      // return a reference to an atermpp::aterm_appl
      m_stream << m_padding
               << "static data_expression static_term(rewrite(" << p.second << "));\n" << m_padding
               << "return static_term; // R2a\n";
    }
    else
    { 
      // arity>0
      m_stream << m_padding
               << "return " << p.second << "; // R2b\n";
    }
  }
  
public:
  ImplementTree(RewriterCompilingJitty& rewr, std::ostream& stream)
    : m_rewriter(rewr), m_stream(stream), m_padding(0)
  { }
  
  void implement_tree(match_tree tree, const size_t arity)
  {
    size_t l = 0;
    
    for (size_t i = 0; i < arity; ++i)
    {
      if (!m_used[i])
      {
        m_nnfvars.push_front(atermpp::aterm_int(i));
      }
    }
    
    while (tree.isC())
    {
      tree = implement_tree(down_cast<match_tree_C>(tree));
      l++;
    }
    
    if (tree.isR())
    {
      implement_tree(down_cast<match_tree_R>(tree), arity);
    }
    else
    {
      implement_tree_aux(tree, 0, 0, 0, 0);
    }
    
    // Close braces opened by implement_tree(const match_tree_C&)
    while (0 < l--)
    {
      m_padding.unindent();
      m_stream << m_padding << "}\n";
    }
  }

  void implement_strategy(match_tree_list strat, size_t arity, const function_symbol& opid, const nfs_array& nf_args)
  {
    m_used = nf_args; // This vector maintains which arguments are in normal form. Initially only those in nf_args are in normal form.
    while (!strat.empty())
    {
      m_stream << m_padding << "// " << strat.front() << "\n";
      if (strat.front().isA())
      {
        size_t arg = match_tree_A(strat.front()).variable_index();
        if (!m_used[arg])
        {
          m_stream << m_padding << "const data_expression arg" << arg << " = rewrite(arg_not_nf" << arg << ");\n";
          m_used[arg] = true;
        }
        m_stream << m_padding << "// Considering argument " << arg << "\n";
      }
      else
      {
        m_stream << m_padding << "{\n";
        m_padding.indent();
        implement_tree(strat.front(), arity);
        m_padding.unindent();
        m_stream << m_padding << "}\n";
      }
      strat = strat.tail();
    }
    rewr_function_finish(arity, opid);
  }

  std::string get_heads(const sort_expression& s, const std::string& base_string, const size_t number_of_arguments)
  {
    std::stringstream ss;
    if (is_function_sort(s) && number_of_arguments>0)
    {
      const function_sort fs(s);
      ss << "down_cast<application>(" << get_heads(fs.codomain(),base_string,number_of_arguments-fs.domain().size()) << ".head())";
      return ss.str();
    }
    return base_string;
  }

  ///
  /// \brief get_recursive_argument provides the index-th argument of an expression provided in
  ///        base_string, given that its head symbol has type s and there are number_of_arguments
  ///        arguments. Example: if f:D->E->F and index is 0, base_string is "t", base_string is
  ///        set to "atermpp::down_cast<application>(t[0])[0]
  /// \param s
  /// \param index
  /// \param base_string
  /// \param number_of_arguments
  /// \return
  ///
  void get_recursive_argument(function_sort s, size_t index, const std::string& base_string, size_t number_of_arguments)
  {
    while (index >= s.domain().size())
    {
      assert(is_function_sort(s.codomain()));
      index -= s.domain().size();
      number_of_arguments -= s.domain().size();
      s = down_cast<function_sort>(s.codomain());
    }
    m_stream << get_heads(s.codomain(), base_string, number_of_arguments - s.domain().size()) << "[" << index << "]";
  }

  std::string rewr_function_finish_term(const size_t arity, const std::string& head, const function_sort& s, size_t& used_arguments)
  {
    if (arity == 0)
    {
      return head;
    }

    const size_t domain_size = s.domain().size();
    stringstream ss;
    ss << (domain_size > 5 ? "make_term_with_many_arguments(" : "application(") << head;

    for (size_t i = 0; i < domain_size; ++i)
    {
      if (m_used[used_arguments + i])
      {
        ss << ", arg" << used_arguments + i;
      }
      else
      {
        ss << ", rewrite(arg_not_nf" << used_arguments + i << ")";
      }
    }
    ss << ")";

    used_arguments += domain_size;
    if (is_function_sort(s.codomain()))
    {
      return rewr_function_finish_term(arity - domain_size, ss.str(), down_cast<function_sort>(s.codomain()), used_arguments);
    }
    else
    {
      return ss.str();
    }
  }

  void rewr_function_finish(size_t arity, const data::function_symbol& opid)
  {
    m_stream << m_padding << "return ";
    // Note that arity is the total arity, of all function symbols.
    if (arity == 0)
    {
      m_stream << m_rewriter.m_nf_cache.insert(opid) << ";\n";
    }
    else
    {
      stringstream ss;
      ss << "data_expression((const atermpp::detail::_aterm*)" << (void*)atermpp::detail::address(opid) << ")";
      size_t used_arguments = 0;
      m_stream << rewr_function_finish_term(arity, ss.str(), down_cast<function_sort>(opid.sort()), used_arguments) << ";\n";
      assert(used_arguments == arity);
    }
  }

  void rewr_function_signature(size_t index, size_t arity, const nfs_array& nfs)
  {
    // Constant function symbols (a == 0) can be passed by reference
    m_stream << "static inline "
             << (arity == 0 ? "const data_expression&" : "data_expression")
             << " rewr_" << index << "_" << arity << "_" << static_cast<size_t>(nfs) << "(";

    for (size_t i = 0; i < arity; ++i)
    {
      m_stream << (i == 0 ? "" : ", ")
               << "const data_expression& arg"
               << (nfs.at(i) ? "" : "_not_nf")
               << i;
    }
    m_stream << ")";
  }

  void rewr_function_declaration(const data::function_symbol& func, size_t arity, const nfs_array& nfs)
  {
    size_t index = core::index_traits<data::function_symbol, function_symbol_key_type, 2>::index(func);
    rewr_function_signature(index, arity, nfs);
    m_stream << ";\n"
                "static inline data_expression rewr_" << index << "_" << arity << "_" << nfs << "_term"
                  "(const application&" << (arity == 0 ? "" : " t") << ") "
                  "{ return rewr_" << index << "_" << arity << "_" << nfs << "(";
    for(size_t i = 0; i < arity; ++i)
    {
      assert(is_function_sort(func.sort()));
      m_stream << (i == 0 ? "" : ", ");
      get_recursive_argument(down_cast<function_sort>(func.sort()), i, "t", arity);
    }
    m_stream << "); }\n";
  }

  void rewr_function_implementation(const data::function_symbol& func, size_t arity, const nfs_array& nfs, match_tree_list strategy)
  {
    size_t index = core::index_traits<data::function_symbol, function_symbol_key_type, 2>::index(func);
    m_stream << "// " << index << " " << func << " " << func.sort() << "\n";
    m_stream << m_padding;
    rewr_function_signature(index, arity, nfs);
    m_stream << "\n" << m_padding << "{\n";
    m_padding.indent();
    implement_strategy(strategy, arity, func, nfs);
    m_padding.unindent();
    m_stream << "}\n\n";
  }
};

void RewriterCompilingJitty::CleanupRewriteSystem()
{
  m_nf_cache.clear();
  if (so_rewr_cleanup != NULL)
  {
    so_rewr_cleanup();
  }
  if (rewriter_so != NULL)
  {
    delete rewriter_so;
    rewriter_so = NULL;
  }
}

///
/// \brief generate_cpp_filename creates a filename that is hopefully unique enough not to cause
///        name clashes when more than one instance of the compiling rewriter run at the same
///        time.
/// \param unique A number that will be incorporated into the filename.
/// \return A filename that should be used to store the generated C++ code in.
///
static std::string generate_cpp_filename(size_t unique)
{
  const char* env_dir = std::getenv("MCRL2_COMPILEDIR");
  std::ostringstream filename;
  std::string filedir;
  if (env_dir)
  {
    filedir = env_dir;
    if (*filedir.rbegin() != '/')
    {
      filedir.append("/");
    }
  }
  else
  {
    filedir = "./";
  }
  filename << filedir << "jittyc_" << getpid() << "_" << unique << ".cpp";
  return filename.str();
}

///
/// \brief filter_function_symbols selects the function symbols from source for which filter
///        returns true, and copies them to dest.
/// \param source The input list.
/// \param dest The output list.
/// \param filter A functor of type bool(const function_symbol&)
///
template <class Filter>
void filter_function_symbols(const function_symbol_vector& source, function_symbol_vector& dest, Filter filter)
{
  for (auto it = source.begin(); it != source.end(); ++it)
  {
    if (filter(*it))
    {
      dest.push_back(*it);
    }
  }
}

///
/// \brief generate_make_appl_functions defines functions that create data::application terms
///        for function symbols with more than 5 arguments.
/// \param s The stream to which the generated C++ code is written.
/// \param max_arity The maximum arity of the functions that are to be generated.
///
static void generate_make_appl_functions(std::ostream& s, size_t max_arity)
{
  // The casting magic in these functions is done to avoid triggering the ATerm
  // reference counting mechanism.
  for (size_t i = 5; i <= max_arity; ++i)
  {
    s << "static application make_term_with_many_arguments(const data_expression& head";
    for (size_t j = 1; j <= i; ++j)
    {
      s << ", const data_expression& arg" << j;
    }
    s << ")\n{\n";
    s << "  const atermpp::detail::_aterm* buffer[" << i << "];\n";
    for (size_t j=0; j<i; ++j)
    {
      s << "  buffer[" << j << "] = atermpp::detail::address(arg" << j + 1 << ");\n";
    }
    s << "  return application(head, reinterpret_cast<data_expression*>(buffer), reinterpret_cast<data_expression*>(buffer) + " << i << ");\n"
         "}\n"
         "\n";
  }
}

template <class FunctionSymbolIterator>
class arity_wrapper
{
private:
  FunctionSymbolIterator m_it;
  size_t m_arity;
  size_t m_max_arity;

  void skip_invalid_arity()
  {
    while (m_arity <= m_max_arity && !arity_is_allowed(m_it->sort(), m_arity))
    {
      ++m_arity;
    }
    if (m_arity > m_max_arity)
    {
      ++m_it;
      m_arity = 0;
    }
  }

public:

  arity_wrapper(const FunctionSymbolIterator& beg, const FunctionSymbolIterator& end)
    : m_it(beg), m_arity(0)
  {
    if (m_it != end)
    {
      m_max_arity = getArity(*m_it);
      skip_invalid_arity();
    }
  }

  const FunctionSymbolIterator& it() { return m_it; }

  const function_symbol& symbol() { return *m_it; }

  size_t index() { return core::index_traits<data::function_symbol,function_symbol_key_type, 2>::index(*m_it); }

  size_t arity() { return m_arity; }

  void next()
  {
    if (m_arity == 0)
    {
      m_max_arity = getArity(*m_it);
    }
    ++m_arity;
    skip_invalid_arity();
  }

};

void RewriterCompilingJitty::generate_code(const std::string& filename)
{
  std::ofstream cpp_file(filename);
  std::stringstream rewrite_functions;

  // - Store all used function symbols in a vector
  function_symbol_vector function_symbols;
  filter_function_symbols(m_data_specification_for_enumeration.constructors(), function_symbols, data_equation_selector);
  filter_function_symbols(m_data_specification_for_enumeration.mappings(), function_symbols, data_equation_selector);

  std::size_t max_arity = m_always_rewrite->init(function_symbols, jittyc_eqns);

  // The rewrite functions are first stored in a separate buffer (rewrite_functions),
  // because during the generation process, new function symbols are created. This
  // affects the value that the macro INDEX_BOUND should have before loading
  // jittycpreamble.h.
  //
  // We first generate forward declarations for the rewrite functions, and then the
  // rewrite functions themselves.
  ImplementTree code_generator(*this, rewrite_functions);

  for (arity_wrapper<function_symbol_vector::const_iterator> fs(function_symbols.begin(), function_symbols.end()); fs.it() != function_symbols.end(); fs.next())
  {
    nfs_array nfs(fs.arity());
    do
    {
      code_generator.rewr_function_declaration(fs.symbol(), fs.arity(), nfs);
    }
    while (nfs.next());
  }
  for (arity_wrapper<function_symbol_vector::const_iterator> fs(function_symbols.begin(), function_symbols.end()); fs.it() != function_symbols.end(); fs.next())
  {
    nfs_array nfs(fs.arity());
    do
    {
      match_tree_list strategy = create_strategy(jittyc_eqns[fs.symbol()], fs.arity(), nfs);
      code_generator.rewr_function_implementation(fs.symbol(), fs.arity(), nfs, strategy);
    }
    while (nfs.next());
  }

  const size_t index_bound = core::index_traits<data::function_symbol, function_symbol_key_type, 2>::max_index() + 1;
  cpp_file << "#define INDEX_BOUND " << index_bound << "\n"
              "#define ARITY_BOUND " << max_arity + 1 << "\n";
  cpp_file << "#include \"mcrl2/data/detail/rewrite/jittycpreamble.h\"\n";
  generate_make_appl_functions(cpp_file, max_arity);
  cpp_file << rewrite_functions.str();

  // Also add the created function symbols that represent partly rewritten functions.
  function_symbols.insert(function_symbols.begin(), partially_rewritten_functions.begin(), partially_rewritten_functions.end());

  cpp_file << "void fill_int2func()\n"
              "{\n";

  // Fill tables with the rewrite functions
  for (arity_wrapper<function_symbol_vector::const_iterator> fs(function_symbols.begin(), function_symbols.end()); fs.it() != function_symbols.end(); fs.next())
  {
    if (partially_rewritten_functions.count(fs.symbol()) == 0)
    {
      cpp_file << "  int2func[ARITY_BOUND * " << fs.index() << " + " << fs.arity() << "] = "
                  "rewr_" << fs.index() << "_" << fs.arity() << "_0_term;\n";

      /* There used to be an int2func_head_in_nf array of the same size as int2func.
       * TODO: check if it should be reenstated.

      if (fs.arity() > 0)
      {
        cpp_file << "  int2func_head_in_nf[" << fs.arity() << "][" << fs.index() << "] = "
                         "rewr_" << fs.index() << "_" << fs.arity() << (fs.arity() <= NF_MAX_ARITY ? "_1" : "_0") << "_term;\n";
      }
      */
    }
  }

  // Add extra entries for partially rewritten functions
  for (arity_wrapper<std::set<function_symbol>::const_iterator> fs(partially_rewritten_functions.begin(), partially_rewritten_functions.end()); fs.it() != partially_rewritten_functions.end(); fs.next())
  {
    if (fs.arity() > 0)
    {
      // We are dealing with a partially rewritten function here. Remove the "@_" at the beginning of the string.
      const string function_name = core::pp(fs.symbol().name());
      const size_t aaa_position = function_name.find("@@@");
      cpp_file << "  int2func[ARITY_BOUND * " << fs.index() << " + " << fs.arity() << "] = "
               << function_name.substr(2, aaa_position - 2) << fs.arity() << function_name.substr(aaa_position + 3)
               << "; // Part. rewritten " << function_name << "\n";
    }
  }

  cpp_file << "}\n";
  cpp_file.close();
}

void RewriterCompilingJitty::BuildRewriteSystem()
{
  CleanupRewriteSystem();

  // Try to find out from environment which compile script to use.
  // If not set, choose one of the following two:
  // * if "mcrl2compilerewriter" is in the same directory as the executable,
  //   this is the version we favour. This is especially needed for single
  //   bundle applications on MacOSX. Furthermore, it is the more foolproof
  //   approach on other platforms.
  // * by default, fall back to the system provided mcrl2compilerewriter script.
  //   in this case, we rely on the script being available in the user's
  //   $PATH environment variable.
  std::string compile_script;
  const char* env_compile_script = std::getenv("MCRL2_COMPILEREWRITER");
  if (env_compile_script != NULL)
  {
    compile_script = env_compile_script;
  }
  else if(mcrl2::utilities::file_exists(mcrl2::utilities::get_executable_basename() + "/mcrl2compilerewriter"))
  {
    compile_script = mcrl2::utilities::get_executable_basename() + "/mcrl2compilerewriter";
  }
  else
  {
    compile_script = "mcrl2compilerewriter";
  }

  rewriter_so = new uncompiled_library(compile_script);
  mCRL2log(verbose) << "using '" << compile_script << "' to compile rewriter." << std::endl;

  jittyc_eqns.clear();
  for(auto it = rewrite_rules.begin(); it != rewrite_rules.end(); ++it)
  {
    jittyc_eqns[get_function_symbol_of_head(it->lhs())].push_front(*it);
  }

  std::string cpp_file = generate_cpp_filename(reinterpret_cast<size_t>(this));
  generate_code(cpp_file);

  mCRL2log(verbose) << "compiling " << cpp_file << "..." << std::endl;

  try
  {
    rewriter_so->compile(cpp_file);
  }
  catch(std::runtime_error& e)
  {
    rewriter_so->leave_files();
    throw mcrl2::runtime_error(std::string("Could not compile rewriter: ") + e.what());
  }

  mCRL2log(verbose) << "loading rewriter..." << std::endl;

  bool (*init)(rewriter_interface*);
  rewriter_interface interface = { mcrl2::utilities::get_toolset_version(), "Unknown error when loading rewriter.", this, NULL, NULL };
  try
  {
    init = reinterpret_cast<bool(*)(rewriter_interface *)>(rewriter_so->proc_address("init"));
  }
  catch(std::runtime_error& e)
  {
    rewriter_so->leave_files();
    throw mcrl2::runtime_error(std::string("Could not load rewriter: ") + e.what());
  }

#ifdef NDEBUG // In non debug mode clear compiled files directly after loading.
  try
  {
    rewriter_so->cleanup();
  }
  catch (std::runtime_error& error)
  {
    mCRL2log(mcrl2::log::error) << "Could not cleanup temporary files: " << error.what() << std::endl;
  }
#endif

  if (!init(&interface))
  {
    throw mcrl2::runtime_error(std::string("Could not load rewriter: ") + interface.status);
  }
  so_rewr_cleanup = interface.rewrite_cleanup;
  so_rewr = interface.rewrite_external;

  mCRL2log(verbose) << interface.status << std::endl;
}

RewriterCompilingJitty::RewriterCompilingJitty(
                          const data_specification& data_spec,
                          const used_data_equation_selector& equation_selector)
  : Rewriter(data_spec,equation_selector),
    jitty_rewriter(data_spec,equation_selector),
    m_nf_cache(jitty_rewriter)
{
  so_rewr_cleanup = NULL;
  rewriter_so = NULL;
  m_always_rewrite = new always_rewrite_array();

  made_files = false;
  rewrite_rules.clear();

  const data_equation_vector& l=data_spec.equations();
  for (data_equation_vector::const_iterator j=l.begin(); j!=l.end(); ++j)
  {
    if (data_equation_selector(*j))
    {
      const data_equation rule=*j;
      try
      {
        CheckRewriteRule(rule);
        if (rewrite_rules.insert(rule).second)
        {
          // The equation has been added as a rewrite rule, otherwise the equation was already present.
          // data_equation_selector.add_function_symbols(rule.lhs());
        }
      }
      catch (std::runtime_error& e)
      {
        mCRL2log(warning) << e.what() << std::endl;
      }
    }
  }

  BuildRewriteSystem();
}

RewriterCompilingJitty::~RewriterCompilingJitty()
{
  delete m_always_rewrite;
  CleanupRewriteSystem();
}

data_expression RewriterCompilingJitty::rewrite(
     const data_expression& term,
     substitution_type& sigma)
{
#ifdef MCRL2_DISPLAY_REWRITE_STATISTICS
  data::detail::increment_rewrite_count();
#endif
  // Save global sigma and restore it afterwards, as rewriting might be recursive with different
  // substitutions, due to the enumerator.
  substitution_type *saved_sigma=global_sigma;
  global_sigma=&sigma;
  const data_expression result=so_rewr(term);
  global_sigma=saved_sigma;
  return result;
}

rewrite_strategy RewriterCompilingJitty::getStrategy()
{
  return jitty_compiling;
}

}
}
}

#endif
