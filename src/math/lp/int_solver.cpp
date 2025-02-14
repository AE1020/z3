/*
  Copyright (c) 2017 Microsoft Corporation
  Author: Lev Nachmanson
*/
#include "math/lp/int_solver.h"
#include "math/lp/lar_solver.h"
#include "math/lp/lp_utils.h"
#include "math/lp/monic.h"
#include "math/lp/gomory.h"
#include "math/lp/int_branch.h"
#include "math/lp/int_cube.h"

namespace lp {

    int_solver::patcher::patcher(int_solver& lia):
        lia(lia),
        lra(lia.lra),
        lrac(lia.lrac)
    {}
    
    unsigned int_solver::patcher::count_non_int() {
        unsigned non_int = 0;
        for (auto j : lra.r_basis()) 
            if (lra.column_is_int(j) && !lra.column_value_is_int(j))
                ++non_int;
        return non_int;
    }

    lia_move int_solver::patcher::patch_basic_columns() {
        lia.settings().stats().m_patches++;
        lra.remove_fixed_vars_from_base();
        lp_assert(lia.is_feasible());
        for (unsigned j : lra.r_basis()) 
            if (!lra.get_value(j).is_int() && lra.column_is_int(j) && !lia.is_fixed(j))
                patch_basic_column(j);
        if (!lia.has_inf_int()) {
            lia.settings().stats().m_patches_success++;
            return lia_move::sat;
        }
        return lia_move::undef;
    }

    // clang-format on
    /**
     * \brief find integral and minimal, in the absolute values, deltas such that x - alpha*delta is integral too.
     */
    bool get_patching_deltas(const rational& x, const rational& alpha,
                             rational& delta_plus, rational& delta_minus) {
        auto a1 = numerator(alpha);
        auto a2 = denominator(alpha);
        auto x1 = numerator(x);
        auto x2 = denominator(x);
        if (!divides(x2, a2))
            return false;

        // delta has to be integral.
        // We need to find delta such that x1/x2 + (a1/a2)*delta is integral (we are going to flip the delta sign later).
        // Then a2*x1/x2 + a1*delta is integral, but x2 and x1 are coprime:
        // that means that t = a2/x2 is
        // integral. We established that a2 = x2*t Then x1 + a1*delta*(x2/a2)  = x1
        // + a1*(delta/t)  is integral. Taking into account that t and a1 are
        // coprime we have delta = t*k, where k is an integer.
        rational t = a2 / x2;
        // Now we have x1/x2 + (a1/x2)*k is integral, or (x1 + a1*k)/x2 is integral.
        // It is equivalent to x1 + a1*k = x2*m, where m is an integer
        // We know that a2 and a1 are coprime, and x2 divides a2, so x2 and a1 are
        // coprime. We can find u and v such that u*a1 + v*x2 = 1.
        rational u, v;
        gcd(a1, x2, u, v);
        lp_assert(gcd(a1, x2, u, v).is_one());
        lp_assert((x + (a1 / a2) * (-u * t) * x1).is_int());
        // 1 = (u- l*x2 ) * a1 + (v + l*a1)*x2, for every integer l.
        rational d = u * t * x1;
        // We can prove that x+alpha*d is integral,
        // and any other delta, satisfying x+alpha*delta, is equal to d modulo a2.
        delta_plus = mod(d, a2);
        lp_assert(delta_plus > 0);
        delta_minus = delta_plus - a2;
        lp_assert(delta_minus < 0);

        return true;
    }
    /**
     * \brief try to patch the basic column v
     */
    bool int_solver::patcher::patch_basic_column_on_row_cell(unsigned v, row_cell<mpq> const& c) {
        if (v == c.var())
            return false;
        if (!lra.column_is_int(c.var()))  // could use real to patch integer
            return false;
        if (c.coeff().is_int())
            return false;
        mpq a = fractional_part(c.coeff());
        mpq r = fractional_part(lra.get_value(v));
        lp_assert(0 < r && r < 1);
        lp_assert(0 < a && a < 1);
        mpq delta_plus, delta_minus;
        if (!get_patching_deltas(r, a, delta_plus, delta_minus))
            return false;

        if (lia.random() % 2) 
            return try_patch_column(v, c.var(), delta_plus) ||
                   try_patch_column(v, c.var(), delta_minus);
        else 
            return try_patch_column(v, c.var(), delta_minus) ||
                   try_patch_column(v, c.var(), delta_plus);
    }
        
    bool int_solver::patcher::try_patch_column(unsigned v, unsigned j, mpq const& delta) {
        const auto & A = lra.A_r();
        if (delta < 0) {
            if (lia.has_lower(j) && lia.get_value(j) + impq(delta) < lra.get_lower_bound(j))
                return false;
        }
        else {
            if (lia.has_upper(j) && lia.get_value(j) + impq(delta) > lra.get_upper_bound(j))
                return false;
        }
        for (auto const& c : A.column(j)) {
            unsigned row_index = c.var();
            unsigned bj = lrac.m_r_basis[row_index];
            auto old_val = lia.get_value(bj);
            auto new_val = old_val - impq(c.coeff()*delta);
            if (lia.has_lower(bj) && new_val < lra.get_lower_bound(bj))
                return false;
            if (lia.has_upper(bj) && new_val > lra.get_upper_bound(bj))
                return false;
            if (old_val.is_int() && !new_val.is_int()){
                return false; // do not waste resources on this case
            }
            // if bj == v, then, because we are patching the lra.get_value(v),
            // we just need to assert that the lra.get_value(v) would be integral.
            lp_assert(bj != v || lra.from_model_in_impq_to_mpq(new_val).is_int());
        }
        
        lra.set_value_for_nbasic_column(j, lia.get_value(j) + impq(delta));
        return true;
    }
    
    void int_solver::patcher::patch_basic_column(unsigned v) {
        SASSERT(!lia.is_fixed(v));
        for (auto const& c : lra.basic2row(v))
            if (patch_basic_column_on_row_cell(v, c))
                return;                                       
    }


    int_solver::int_solver(lar_solver& lar_slv) :
        lra(lar_slv),
        lrac(lra.m_mpq_lar_core_solver),
        m_gcd(*this),
        m_patcher(*this),
        m_number_of_calls(0),
        m_hnf_cutter(*this),
        m_hnf_cut_period(settings().hnf_cut_period()) {
        lra.set_int_solver(this);
    }

    // this will allow to enable and disable tracking of the pivot rows
    struct check_return_helper {
        lar_solver&      lra;
        bool             m_track_touched_rows;
        check_return_helper(lar_solver& ls) :
            lra(ls),
            m_track_touched_rows(lra.touched_rows_are_tracked()) {
            lra.track_touched_rows(false);
        }
        ~check_return_helper() {
            lra.track_touched_rows(m_track_touched_rows);
        }
    };

    lia_move int_solver::check(lp::explanation * e) {
        SASSERT(lra.ax_is_correct());
        if (!has_inf_int())
            return lia_move::sat;

        m_t.clear();
        m_k.reset();
        m_ex = e;
        m_ex->clear();
        m_upper = false;
        lia_move r = lia_move::undef;

        if (m_gcd.should_apply())
            r = m_gcd();

        check_return_helper pc(lra);

        if (settings().get_cancel_flag())
            return lia_move::undef;

        ++m_number_of_calls;
        if (r == lia_move::undef && m_patcher.should_apply()) r = m_patcher();
        if (r == lia_move::undef && should_find_cube()) r = int_cube(*this)();
        if (r == lia_move::undef && should_hnf_cut()) r = hnf_cut();
#if 1
        if (r == lia_move::undef && should_gomory_cut()) r = gomory(*this)();
#else
        if (r == lia_move::undef && should_gomory_cut()) r = local_gomory();
#endif
        if (r == lia_move::undef) r = int_branch(*this)();
        return r;
    }

    std::ostream& int_solver::display_inf_rows(std::ostream& out) const {
        unsigned num = lra.A_r().column_count();
        for (unsigned v = 0; v < num; v++) {
            if (column_is_int(v) && !get_value(v).is_int()) {
                display_column(out, v);
            }
        }

        num = 0;
        for (unsigned i = 0; i < lra.A_r().row_count(); i++) {
            unsigned j = lrac.m_r_basis[i];
            if (column_is_int_inf(j)) {
                num++;
                lra.print_row(lra.A_r().m_rows[i], out);
                out << "\n";
            }
        }
        out << "num of int infeasible: " << num << "\n";
        return out;
    }

    bool int_solver::cut_indices_are_columns() const {
        for (lar_term::ival p : m_t) {
            if (p.column().index() >= lra.A_r().column_count())
                return false;
        }
        return true;
    }

    bool int_solver::current_solution_is_inf_on_cut() const {
        SASSERT(cut_indices_are_columns());
        const auto & x = lrac.m_r_x;
        impq v = m_t.apply(x);
        mpq sign = m_upper ? one_of_type<mpq>()  : -one_of_type<mpq>();
        CTRACE("current_solution_is_inf_on_cut", v * sign <= impq(m_k) * sign,
               tout << "m_upper = " << m_upper << std::endl;
               tout << "v = " << v << ", k = " << m_k << std::endl;
               );
        return v * sign > impq(m_k) * sign;
    }

    bool int_solver::has_inf_int() const {
        return lra.has_inf_int();
    }

    u_dependency* int_solver::column_upper_bound_constraint(unsigned j) const {
        return lra.get_column_upper_bound_witness(j);
    }

    u_dependency* int_solver::column_lower_bound_constraint(unsigned j) const {
        return lra.get_column_lower_bound_witness(j);
    }

    unsigned int_solver::row_of_basic_column(unsigned j) const {
        return lra.row_of_basic_column(j);
    }

    lp_settings& int_solver::settings() {
        return lra.settings();
    }

    const lp_settings& int_solver::settings() const {
        return lra.settings();
    }

    bool int_solver::column_is_int(column_index const& j) const {
        return lra.column_is_int(j);
    }

    bool int_solver::is_real(unsigned j) const {
        return !column_is_int(j);
    }

    bool int_solver::value_is_int(unsigned j) const {
        return lra.column_value_is_int(j);
    }

    unsigned int_solver::random() {
        return settings().random_next();
    }

    const impq& int_solver::upper_bound(unsigned j) const {
        return lra.column_upper_bound(j);
    }

    const impq& int_solver::lower_bound(unsigned j) const {
        return lra.column_lower_bound(j);
    }

    bool int_solver::is_term(unsigned j) const {
        return lra.column_corresponds_to_term(j);
    }

    unsigned int_solver::column_count() const  {
        return lra.column_count();
    }

    bool int_solver::should_find_cube() {
        return m_number_of_calls % settings().m_int_find_cube_period == 0;
    }

    bool int_solver::should_gomory_cut() {
        return m_number_of_calls % settings().m_int_gomory_cut_period == 0;
    }

    bool int_solver::should_hnf_cut() {
        return settings().enable_hnf() && m_number_of_calls % m_hnf_cut_period == 0;
    }

    lia_move int_solver::hnf_cut() {
        lia_move r = m_hnf_cutter.make_hnf_cut();
        if (r == lia_move::undef) 
            m_hnf_cut_period *= 2;
        else 
            m_hnf_cut_period = settings().hnf_cut_period();
        return r;
    }

    bool int_solver::has_lower(unsigned j) const {
        switch (lrac.m_column_types()[j]) {
        case column_type::fixed:
        case column_type::boxed:
        case column_type::lower_bound:
            return true;
        default:
            return false;
        }
    }

    bool int_solver::has_upper(unsigned j) const {
        switch (lrac.m_column_types()[j]) {
        case column_type::fixed:
        case column_type::boxed:
        case column_type::upper_bound:
            return true;
        default:
            return false;
        }
    }

    static void set_lower(impq & l, bool & inf_l, impq const & v ) {
        if (inf_l || v > l) {
            l = v;
            inf_l = false;
        }
    }

    static void set_upper(impq & u, bool & inf_u, impq const & v) {
        if (inf_u || v < u) {
            u = v;
            inf_u = false;
        }
    }

    // this function assumes that all basic columns dependend on j are feasible
    bool int_solver::get_freedom_interval_for_column(unsigned j, bool & inf_l, impq & l, bool & inf_u, impq & u, mpq & m) {
        if (lrac.m_r_heading[j] >= 0 || is_fixed(j)) // basic or fixed var
            return false;

        TRACE("random_update", display_column(tout, j) << ", is_int = " << column_is_int(j) << "\n";);
        impq const & xj = get_value(j);

        inf_l = true;
        inf_u = true;
        l = u = zero_of_type<impq>();
        m = mpq(1);

        if (has_lower(j)) 
            set_lower(l, inf_l, lower_bound(j) - xj);
    
        if (has_upper(j)) 
            set_upper(u, inf_u, upper_bound(j) - xj);
    

        const auto & A = lra.A_r();
        TRACE("random_update", tout <<  "m = " << m << "\n";);

        auto delta = [](mpq const& x, impq const& y, impq const& z) {
            if (x.is_one())
                return y - z;
            if (x.is_minus_one())
                return z - y;
            return (y - z) / x;
        };

        for (auto c : A.column(j)) {
            unsigned row_index = c.var();
            const mpq & a = c.coeff();
            unsigned i = lrac.m_r_basis[row_index];
            impq const & xi = get_value(i);
            lp_assert(lrac.m_r_solver.column_is_feasible(i));
            if (column_is_int(i) && !a.is_int() && xi.is_int()) 
                m = lcm(m, denominator(a));
        
            if (!inf_l && !inf_u && l == u) 
                continue;                        

            if (a.is_neg()) {
                if (has_lower(i)) 
                    set_lower(l, inf_l, delta(a, xi, lra.get_lower_bound(i)));
                if (has_upper(i)) 
                    set_upper(u, inf_u, delta(a, xi, lra.get_upper_bound(i)));
            }
            else {
                if (has_upper(i)) 
                    set_lower(l, inf_l, delta(a, xi, lra.get_upper_bound(i)));
                if (has_lower(i)) 
                    set_upper(u, inf_u, delta(a, xi, lra.get_lower_bound(i)));
            }
        }

        l += xj;
        u += xj;

        TRACE("freedom_interval",
              tout << "freedom variable for:\n";
              tout << lra.get_variable_name(j);
              tout << "[";
              if (inf_l) tout << "-oo"; else tout << l;
              tout << "; ";
              if (inf_u) tout << "oo";  else tout << u;
              tout << "]\n";
              tout << "val = " << get_value(j) << "\n";
              tout << "return " << (inf_l || inf_u || l <= u);
              );
        return (inf_l || inf_u || l <= u);
    }


    bool int_solver::is_feasible() const {
        lp_assert(
            lrac.m_r_solver.calc_current_x_is_feasible_include_non_basis() ==
            lrac.m_r_solver.current_x_is_feasible());
        return lrac.m_r_solver.current_x_is_feasible();
    }

    const impq & int_solver::get_value(unsigned j) const {
        return lrac.m_r_x[j];
    }

    std::ostream& int_solver::display_column(std::ostream & out, unsigned j) const {
        return lrac.m_r_solver.print_column_info(j, out);
    }

    bool int_solver::column_is_int_inf(unsigned j) const {
        return column_is_int(j) && (!value_is_int(j));
    }

    bool int_solver::is_base(unsigned j) const {
        return lrac.m_r_heading[j] >= 0;
    }

    bool int_solver::is_boxed(unsigned j) const {
        return lrac.m_column_types[j] == column_type::boxed;
    }

    bool int_solver::is_fixed(unsigned j) const {
        return lrac.m_column_types[j] == column_type::fixed;
    }

    bool int_solver::is_free(unsigned j) const {
        return lrac.m_column_types[j] == column_type::free_column;
    }

    bool int_solver::at_bound(unsigned j) const {
        auto & mpq_solver = lrac.m_r_solver;
        switch (mpq_solver.m_column_types[j] ) {
        case column_type::fixed:
        case column_type::boxed:
            return
                mpq_solver.m_lower_bounds[j] == get_value(j) ||
                mpq_solver.m_upper_bounds[j] == get_value(j);
        case column_type::lower_bound:
            return mpq_solver.m_lower_bounds[j] == get_value(j);
        case column_type::upper_bound:
            return  mpq_solver.m_upper_bounds[j] == get_value(j);
        default:
            return false;
        }
    }

    bool int_solver::at_lower(unsigned j) const {
        auto & mpq_solver = lrac.m_r_solver;
        switch (mpq_solver.m_column_types[j] ) {
        case column_type::fixed:
        case column_type::boxed:
        case column_type::lower_bound:
            return mpq_solver.m_lower_bounds[j] == get_value(j);
        default:
            return false;
        }
    }

    bool int_solver::at_upper(unsigned j) const {
        auto & mpq_solver = lrac.m_r_solver;
        switch (mpq_solver.m_column_types[j] ) {
        case column_type::fixed:
        case column_type::boxed:
        case column_type::upper_bound:
            return mpq_solver.m_upper_bounds[j] == get_value(j);
        default:
            return false;
        }
    }

    std::ostream & int_solver::display_row(std::ostream & out, lp::row_strip<rational> const & row) const {
        bool first = true;
        auto & rslv = lrac.m_r_solver;
        for (const auto &c : row) {
            if (is_fixed(c.var())) {
                if (!get_value(c.var()).is_zero()) {
                    impq val = get_value(c.var()) * c.coeff();
                    if (!first && val.is_pos())
                        out << "+";
                    if (val.y.is_zero())
                        out << val.x << " ";
                    else
                        out << val << " ";
                }
                first = false;
                continue;
            }
            if (c.coeff().is_one()) {
                if (!first)
                    out << "+";
            }
            else if (c.coeff().is_minus_one())
                out << "-";
            else {
                if (c.coeff().is_pos() && !first)
                    out << "+";
                if (c.coeff().is_big())
                    out << " b*";
                else
                    out << c.coeff();
            }
            out << rslv.column_name(c.var()) << " ";
            first = false;
        }
        out << "\n";
        for (const auto &c : row) {
            if (is_fixed(c.var()))
                continue;
            rslv.print_column_info(c.var(), out);
            if (is_base(c.var()))
                out << "j" << c.var() << " base\n";
        }
        return out;
    }
    
    std::ostream& int_solver::display_row_info(std::ostream & out, unsigned row_index) const  {    
        auto & rslv = lrac.m_r_solver;
        auto const& row = rslv.m_A.m_rows[row_index];
        return display_row(out, row);
    }

    bool int_solver::shift_var(unsigned j, unsigned range) {
        if (is_fixed(j) || is_base(j))
            return false;
        if (settings().get_cancel_flag())
            return false;
        bool inf_l = false, inf_u = false;
        impq l, u;
        mpq m;
        if (!get_freedom_interval_for_column(j, inf_l, l, inf_u, u, m))
            return false;
        if (settings().get_cancel_flag())
            return false;
        const impq & x = get_value(j);
        // x, the value of j column, might be shifted on a multiple of m

        if (inf_l && inf_u) {
            impq new_val = m * impq(random() % (range + 1)) + x;
            lra.set_value_for_nbasic_column(j, new_val);
            return true;
        }
        if (column_is_int(j)) {
            if (!inf_l) 
                l = impq(ceil(l));
            if (!inf_u) 
                u = impq(floor(u));
        }
        if (!inf_l && !inf_u && l >= u)
            return false;


        if (inf_u) {
            SASSERT(!inf_l);
            impq new_val = x + m * impq(random() % (range + 1));
            lra.set_value_for_nbasic_column(j, new_val);
            return true;
        }

        if (inf_l) {
            SASSERT(!inf_u);
            impq new_val = x - m * impq(random() % (range + 1));
            lra.set_value_for_nbasic_column(j, new_val);
            return true;
        }

        SASSERT(!inf_l && !inf_u);
        // The shift has to be a multiple of m: let us look for s, such that the shift is m*s.
        // We have new_val = x+m*s <= u, so m*s <= u-x and, finally, s <= floor((u- x)/m) = a
        // The symmetric reasoning gives us s >= ceil((l-x)/m) = b
        // We randomly pick s in the segment [b, a]
        mpq a = floor((u - x) / m);
        mpq b = ceil((l - x) / m);
        mpq r = a - b;
        if (!r.is_pos())
            return false;
        TRACE("int_solver", tout << "a = " << a << ", b = " << b << ", r = " << r<< ", m = " << m << "\n";);
        if (r < mpq(range))
            range = static_cast<unsigned>(r.get_uint64());

        mpq s = b + mpq(random() % (range + 1));
        impq new_val = x + m * impq(s);
        TRACE("int_solver", tout << "new_val = " << new_val << "\n";);
        SASSERT(l <= new_val && new_val <= u);
        lra.set_value_for_nbasic_column(j, new_val);
        return true;
    }


    int int_solver::select_int_infeasible_var() {
        int result = -1;
        mpq range;
        mpq new_range;
        mpq small_value(1024);
        unsigned n = 0;
        lar_core_solver & lcs = lra.m_mpq_lar_core_solver;
        unsigned prev_usage = 0; // to quiet down the compile

        enum state { small_box, is_small_value,  any_value, not_found };
        state st = not_found;

        for (unsigned j : lra.r_basis()) {
            if (!column_is_int_inf(j))
                continue;
            unsigned usage = lra.usage_in_terms(j);
            if (is_boxed(j) && (new_range = lcs.m_r_upper_bounds()[j].x - lcs.m_r_lower_bounds()[j].x - rational(2*usage)) <= small_value) {
                SASSERT(!is_fixed(j));
                if (st != small_box) {
                    n = 0;
                    st = small_box;
                }
                if (n == 0 || new_range < range) {
                    result = j;
                    range = new_range;
                    n = 1;
                }
                else if (new_range == range && (random() % (++n) == 0)) {
                    result = j;
                }
                continue;
            }
            if (st == small_box)
                continue;
            impq const& value = get_value(j);
            if (abs(value.x) < small_value ||
                (has_upper(j) && small_value > upper_bound(j).x - value.x) ||
                (has_lower(j) && small_value > value.x - lower_bound(j).x)) {
                if (st != is_small_value) {
                    n = 0;
                    st = is_small_value;
                }
                if (random() % (++n) == 0)
                    result = j;
            }
            if (st == is_small_value)
                continue;
            SASSERT(st == not_found || st == any_value);
            st = any_value;
            if (n == 0 || usage > prev_usage) {
                result = j;
                prev_usage = usage;
                n = 1;
            } 
            else if (usage > 0 && usage == prev_usage && (random() % (++n) == 0)) 
                result = j;
        }
    
        return result;
    }

    void int_solver::simplify(std::function<bool(unsigned)>& is_root) {

        return;
#if 1

        // in-processing simplification can go here, such as bounds improvements.

        if (!lra.is_feasible()) {
            lra.find_feasible_solution();
            if (!lra.is_feasible())
                return;
        }

        
#endif

#if 1
        lp::explanation exp;
        m_ex = &exp;
        m_t.clear();
        m_k.reset();

        if (has_inf_int()) 
            local_gomory();            
#endif

#if 0
        stopwatch sw;
        explanation exp1, exp2;

        //
        // tighten integer bounds
        // It is a weak method as it ownly strengthens bounds if 
        // variables are already at one of the to-be discovered bounds.
        //
        sw.start();
        unsigned changes = 0;
        auto const& constraints = lra.constraints();
        auto print_var = [this](lpvar j) {
            if (lra.column_corresponds_to_term(j)) {
                std::stringstream strm;
                lra.print_column_info(j, strm);
                return strm.str();
            }
            else
                return std::string("j") + std::to_string(j);
        };
        unsigned start = random();
        unsigned num_checks = 0;
        for (lpvar j0 = 0; j0 < lra.column_count(); ++j0) {
            lpvar j = (j0 + start) % lra.column_count();                       

            if (num_checks > 1000)
                break;
            if (is_fixed(j))
                continue;
            if (!lra.column_is_int(j))
                continue;
            rational value = get_value(j).x;
            bool tight_lower = false, tight_upper = false;
            u_dependency* dep;

            if (!value.is_int())
                continue;

            bool at_up = at_upper(j);
            
            if (!at_lower(j)) {
                ++num_checks;
                lra.push();
                auto k = lp::lconstraint_kind::LE;
                lra.update_column_type_and_bound(j, k, (value - 1).to_mpq(), nullptr);
                lra.find_feasible_solution();
                if (!lra.is_feasible()) {
                    tight_upper = true;
                    ++changes;
                    lra.get_infeasibility_explanation(exp1);  
#if 0
                    display_column(std::cout, j);
                    std::cout << print_var(j) << " >= " << value << "\n";
                    unsigned i = 0;
                    for (auto p : exp1) {
                        std::cout << "(" << p.ci() << ")";
                        constraints.display(std::cout, print_var, p.ci());
                        if (++i < exp1.size())
                            std::cout << "      ";
                    }
#endif
                }
                lra.pop(1);
                if (tight_upper) {
                    dep = nullptr;
                    for (auto& cc : exp1)
                        dep = lra.join_deps(dep, constraints[cc.ci()].dep());
                    lra.update_column_type_and_bound(j, lp::lconstraint_kind::GE, value.to_mpq(), dep);
                }
            }
            
            if (!at_up) {
                ++num_checks;
                lra.push();
                auto k = lp::lconstraint_kind::GE;
                lra.update_column_type_and_bound(j, k, (value + 1).to_mpq(), nullptr);
                lra.find_feasible_solution();
                if (!lra.is_feasible()) {
                    tight_lower = true;
                    ++changes;
                    lra.get_infeasibility_explanation(exp1);      
#if 0
                    display_column(std::cout, j);
                    std::cout << print_var(j) << " <= " << value << "\n";
                    unsigned i = 0;
                    for (auto p : exp1) {
                        std::cout << "(" << p.ci() << ")";
                        constraints.display(std::cout, print_var, p.ci());
                        if (++i < exp1.size())
                            std::cout << "      ";
                    }
#endif
                }
                lra.pop(1);
                if (tight_lower) {
                    dep = nullptr;
                    for (auto& cc : exp1)
                        dep = lra.join_deps(dep, constraints[cc.ci()].dep());
                    lra.update_column_type_and_bound(j, lp::lconstraint_kind::LE, value.to_mpq(), dep);
                }
            }
        }
        sw.stop();
        std::cout << "changes " << changes << " columns " << lra.column_count() << " time: " << sw.get_seconds() << "\n";
        std::cout.flush();

        // 
        // identify equalities
        //

        m_equalities.reset();
        map<rational, unsigned_vector, rational::hash_proc, rational::eq_proc> value2roots;

        vector<std::pair<lp::mpq, unsigned>> coeffs;        
        coeffs.push_back({-rational::one(), 0});
        coeffs.push_back({rational::one(), 0});

        num_checks = 0;

        // make sure values are sampled with respect to the same state of the Simplex.
        vector<rational> values;
        for (lpvar j = 0; j < lra.column_count(); ++j) 
            values.push_back(get_value(j).x);

        sw.reset();
        sw.start();
        start = random();
        for (lpvar j0 = 0; j0 < lra.column_count(); ++j0) {
            lpvar j = (j0 + start) % lra.column_count();
            if (is_fixed(j))
                continue;
            if (!lra.column_is_int(j))
                continue;
            if (!is_root(j))
                continue;
            rational value = values[j];
            if (!value2roots.contains(value)) {
                unsigned_vector vec;
                vec.push_back(j);
                value2roots.insert(value, vec);
                continue;
            }
            auto& roots = value2roots.find(value);
            bool has_eq = false;
            //
            // Super inefficient check. There are better ways.
            // 1. call into equality finder:
            // the cheap equality finder can also be used.
            // 2. value sweeping:
            // update partitions of values based on feasible tableaus
            // instead of having just the values vector use the values 
            // collected when the find_feasible_solution succeeds with
            // a new assignment. 
            // 3. a more expensive equality finder:
            // use the tableau to extract equalities from tight rows.
            // If x = y is implied, there is a set of rows that link x and y
            // and such that the variables are at their bounds.
            // 4. retain information between calls:
            // If simplification is invoked at the same backtracking level (or above)
            // form the previous call and it is established that x <= y (but not x == y), then no need to
            // recheck the inequality x <= y.
            for (auto k : roots) {
                bool le = false, ge = false;
                u_dependency* dep = nullptr;
                lra.push();
                coeffs[0].second = j;
                coeffs[1].second = k;
                lp::lpvar term_index = lra.add_term(coeffs, UINT_MAX);
                term_index = lra.map_term_index_to_column_index(term_index);
                lra.push();
                lra.update_column_type_and_bound(term_index, lp::lconstraint_kind::GE, mpq(1), nullptr);
                lra.find_feasible_solution();
                if (!lra.is_feasible()) {
                    lra.get_infeasibility_explanation(exp1);
                    le = true;
                }
                lra.pop(1);
                ++num_checks;
                if (le) {
                    lra.push();
                    lra.update_column_type_and_bound(term_index, lp::lconstraint_kind::LE, mpq(-1), nullptr);
                    lra.find_feasible_solution();
                    if (!lra.is_feasible()) {
                        lra.get_infeasibility_explanation(exp2);
                        exp1.add_expl(exp2);
                        ge = true;           
                    }                             
                    lra.pop(1);
                    ++num_checks;
                }
                lra.pop(1);
                if (le && ge) {
                    has_eq = true;
                    m_equalities.push_back({j, k, exp1});
                    break;
                }
                // artificial throttle.
                if (num_checks > 10000)
                    break;
            }
            if (!has_eq) 
                roots.push_back(j);

            // artificial throttle.
            if (num_checks > 10000)
                break;
        }

        sw.stop();
        std::cout << "equalities " << m_equalities.size() << " num checks " << num_checks << " time: " << sw.get_seconds() << "\n";
        std::cout.flush();

        //
        // Cuts? Eg. for 0-1 variables or bounded integers?
        // 

#endif
    }

    lia_move int_solver::local_gomory() {
        for (unsigned i = 0; i < 2 && has_inf_int() && !settings().get_cancel_flag(); ++i) {
            m_ex->clear();
            m_t.clear();
            m_k.reset();
            auto r = gomory(*this)();
            IF_VERBOSE(3, verbose_stream() << i << " " << r << "\n");
            if (r != lia_move::cut) 
                return r;
            u_dependency* dep = nullptr;
            for (auto c : *m_ex) 
                dep = lra.join_deps(lra.dep_manager().mk_leaf(c.ci()), dep);
            lp::lpvar term_index = lra.add_term(get_term().coeffs_as_vector(), UINT_MAX);
            term_index = lra.map_term_index_to_column_index(term_index);
            lra.update_column_type_and_bound(term_index, is_upper() ? lp::lconstraint_kind::LE : lp::lconstraint_kind::GE, get_offset(), dep);
            lra.find_feasible_solution();
            if (!lra.is_feasible()) {
                lra.get_infeasibility_explanation(*m_ex);
                return lia_move::conflict;
            }
            //r = m_patcher();
            //if (r != lia_move::undef)
            //    return r;
        }
        m_ex->clear();
        m_t.clear();
        m_k.reset();
        if (!has_inf_int())
            return lia_move::sat;
        return lia_move::undef;
    }



}
