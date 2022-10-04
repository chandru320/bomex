#include <AMReX.H>
#include <AMReX_MultiFab.H>
#include <AMReX_MultiFabUtil.H>
#include <AMReX_ArrayLim.H>
#include <AMReX_BC_TYPES.H>
#include <EOS.H>
#include <ERF_Constants.H>
#include <IndexDefines.H>
#include <TerrainMetrics.H>
#include <TimeIntegration.H>
#include <prob_common.H>

using namespace amrex;

void erf_fast_rhs_MT (int step, int level,
                      Vector<MultiFab>& S_slow_rhs,                  // the slow RHS already computed
                      const Vector<MultiFab>& S_prev,                // if step == 0, this is S_old, else the previous solution
                      Vector<MultiFab>& S_stg_data,                  // at last RK stg: S^n, S^* or S^**
                      const MultiFab& S_stg_prim,                    // Primitive version of S_stg_data[IntVar::cons]
                      const MultiFab& pi_stg,                        // Exner function evaluated at last RK stg
                      const MultiFab& fast_coeffs,                   // Coeffs for tridiagonal solve
                      Vector<MultiFab>& S_data,                      // S_sum = state at end of this substep
                      Vector<MultiFab>& S_scratch,                   // S_sum_old at most recent fast timestep for (rho theta)
                      const amrex::Geometry geom,
                      amrex::InterpFaceRegister* ifr,
                      const SolverChoice& solverChoice,
                            MultiFab& Omega,
                      std::unique_ptr<MultiFab>& z_t_rk,             // evaluated from previous RK stg to next RK stg
                      const MultiFab* z_t_pert,                      // evaluated from tau to (tau + delta tau) - z_t_rk
                      std::unique_ptr<MultiFab>& z_phys_nd_old,      // at previous substep time (tau)
                      std::unique_ptr<MultiFab>& z_phys_nd_new,      // at      new substep time (tau + delta tau)
                      std::unique_ptr<MultiFab>& z_phys_nd_stg,      // at last RK stg
                      std::unique_ptr<MultiFab>& detJ_cc_old,        // at previous substep time (tau)
                      std::unique_ptr<MultiFab>& detJ_cc_new,        // at      new substep time (tau + delta tau)
                      std::unique_ptr<MultiFab>& detJ_cc_stg,        // at last RK stg
                      const amrex::Real dtau, const amrex::Real facinv,
                      bool ingested_bcs)
{
    BL_PROFILE_REGION("erf_fast_rhs_MT()");

    AMREX_ASSERT(solverChoice.terrain_type == 1);

    // Per p2902 of Klemp-Skamarock-Dudhia-2007
    // beta_s = -1.0 : fully explicit
    // beta_s =  1.0 : fully implicit
    Real beta_s = 0.1;
    Real beta_1 = 0.5 * (1.0 - beta_s);  // multiplies explicit terms
    Real beta_2 = 0.5 * (1.0 + beta_s);  // multiplies implicit terms

    // How much do we project forward the (rho theta) that is used in the horizontal momentum equations
    Real beta_d = 0.1;

    const Box domain(geom.Domain());
    const GpuArray<Real, AMREX_SPACEDIM> dxInv = geom.InvCellSizeArray();

    Real dxi = dxInv[0];
    Real dyi = dxInv[1];
    Real dzi = dxInv[2];

    MultiFab     coeff_A_mf(fast_coeffs, amrex::make_alias, 0, 1);
    MultiFab inv_coeff_B_mf(fast_coeffs, amrex::make_alias, 1, 1);
    MultiFab     coeff_C_mf(fast_coeffs, amrex::make_alias, 2, 1);
    MultiFab     coeff_P_mf(fast_coeffs, amrex::make_alias, 3, 1);
    MultiFab     coeff_Q_mf(fast_coeffs, amrex::make_alias, 4, 1);

    // *************************************************************************
    // Set gravity as a vector
    const    Array<Real,AMREX_SPACEDIM> grav{0.0, 0.0, -solverChoice.gravity};
    const GpuArray<Real,AMREX_SPACEDIM> grav_gpu{grav[0], grav[1], grav[2]};

    const iMultiFab *mlo_mf_x, *mhi_mf_x;
    const iMultiFab *mlo_mf_y, *mhi_mf_y;

    if (level > 0)
    {
        mlo_mf_x = &(ifr->mask(Orientation(0,Orientation::low)));
        mhi_mf_x = &(ifr->mask(Orientation(0,Orientation::high)));
        mlo_mf_y = &(ifr->mask(Orientation(1,Orientation::low)));
        mhi_mf_y = &(ifr->mask(Orientation(1,Orientation::high)));
    }

    MultiFab extrap(S_data[IntVar::cons].boxArray(),S_data[IntVar::cons].DistributionMap(),1,1);

    // *************************************************************************
    // Define updates in the current RK stg
    // *************************************************************************
#ifdef _OPENMP
#pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
#endif
    {

    FArrayBox temp_rhs_fab;

    FArrayBox RHS_fab;
    FArrayBox soln_fab;

    for ( MFIter mfi(S_stg_data[IntVar::cons],TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        const Box& valid_bx = mfi.validbox();

        Box  bx = mfi.tilebox();

        bool left_edge_dirichlet = false;
        bool rght_edge_dirichlet = false;
        bool  bot_edge_dirichlet = false;
        bool  top_edge_dirichlet = false;

        if (level == 0 && ingested_bcs) {
            left_edge_dirichlet = (bx.smallEnd(0) == domain.smallEnd(0));
            rght_edge_dirichlet = (bx.bigEnd(1)   == domain.bigEnd(0));
            bot_edge_dirichlet  = (bx.smallEnd(0) == domain.smallEnd(1));
            top_edge_dirichlet  = (bx.bigEnd(1)   == domain.bigEnd(1));
        } else if (level > 0) {
            int vlo_x = valid_bx.smallEnd(0);
            int vhi_x = valid_bx.bigEnd(0);
            int vlo_y = valid_bx.smallEnd(1);
            int vhi_y = valid_bx.bigEnd(1);
            int vlo_z = valid_bx.smallEnd(2);
            int vhi_z = valid_bx.bigEnd(2);

            auto mlo_x = (level > 0) ? mlo_mf_x->const_array(mfi) : Array4<const int>{};
            auto mhi_x = (level > 0) ? mhi_mf_x->const_array(mfi) : Array4<const int>{};
            auto mlo_y = (level > 0) ? mlo_mf_y->const_array(mfi) : Array4<const int>{};
            auto mhi_y = (level > 0) ? mhi_mf_y->const_array(mfi) : Array4<const int>{};

            // ******************************************************************
            // This assumes that refined regions are always rectangular
            // ******************************************************************
            left_edge_dirichlet = mlo_x(vlo_x  ,vlo_y  ,vlo_z);
            rght_edge_dirichlet = mhi_x(vhi_x+1,vhi_y  ,vhi_z);
            bot_edge_dirichlet  = mlo_y(vlo_x  ,vlo_y  ,vlo_z);
            top_edge_dirichlet  = mhi_y(vhi_x  ,vhi_y+1,vhi_z);
        } // level > 0

        if (left_edge_dirichlet) {
            bx.growLo(0,-1);
        }
        if (rght_edge_dirichlet) {
            bx.growHi(0,-1);
        }
        if (bot_edge_dirichlet) {
            bx.growLo(1,-1);
        }
        if (top_edge_dirichlet) {
            bx.growHi(1,-1);
        }

        Box tbx = surroundingNodes(bx,0);
        Box tby = surroundingNodes(bx,1);
        Box tbz = surroundingNodes(bx,2);

        const Array4<const Real> & stg_cons = S_stg_data[IntVar::cons].const_array(mfi);
        const Array4<const Real> & stg_xmom = S_stg_data[IntVar::xmom].const_array(mfi);
        const Array4<const Real> & stg_ymom = S_stg_data[IntVar::ymom].const_array(mfi);
        const Array4<const Real> & stg_zmom = S_stg_data[IntVar::zmom].const_array(mfi);
        const Array4<const Real> & prim       = S_stg_prim.const_array(mfi);

        const Array4<const Real>& slow_rhs_cons  = S_slow_rhs[IntVar::cons].const_array(mfi);
        const Array4<const Real>& slow_rhs_rho_u = S_slow_rhs[IntVar::xmom].const_array(mfi);
        const Array4<const Real>& slow_rhs_rho_v = S_slow_rhs[IntVar::ymom].const_array(mfi);
        const Array4<const Real>& slow_rhs_rho_w = S_slow_rhs[IntVar::zmom].const_array(mfi);

        const Array4<Real>& cur_cons = S_data[IntVar::cons].array(mfi);
        const Array4<Real>& cur_xmom = S_data[IntVar::xmom].array(mfi);
        const Array4<Real>& cur_ymom = S_data[IntVar::ymom].array(mfi);
        const Array4<Real>& cur_zmom = S_data[IntVar::zmom].array(mfi);

        const Array4<Real>& lagged_rtheta = S_scratch[IntVar::cons].array(mfi);

        const Array4<const Real>& prev_cons = S_prev[IntVar::cons].const_array(mfi);
        const Array4<const Real>& prev_xmom = S_prev[IntVar::xmom].const_array(mfi);
        const Array4<const Real>& prev_ymom = S_prev[IntVar::ymom].const_array(mfi);
        const Array4<const Real>& prev_zmom = S_prev[IntVar::zmom].const_array(mfi);

        // These store the advection momenta which we will use to update the slow variables
        const Array4<Real>& avg_xmom = S_scratch[IntVar::xmom].array(mfi);
        const Array4<Real>& avg_ymom = S_scratch[IntVar::ymom].array(mfi);
        const Array4<Real>& avg_zmom = S_scratch[IntVar::zmom].array(mfi);

        const Array4<const Real>& z_nd_old = z_phys_nd_old->const_array(mfi);
        const Array4<const Real>& z_nd_new = z_phys_nd_new->const_array(mfi);
        const Array4<const Real>& z_nd_stg = z_phys_nd_stg->const_array(mfi);
        const Array4<const Real>& detJ_old =   detJ_cc_old->const_array(mfi);
        const Array4<const Real>& detJ_new =   detJ_cc_new->const_array(mfi);
        const Array4<const Real>& detJ_stg =   detJ_cc_stg->const_array(mfi);

        const Array4<const Real>& z_t_arr  = z_t_rk->const_array(mfi);
        const Array4<const Real>& zp_t_arr = z_t_pert->const_array(mfi);

        const Array4<      Real>& omega_arr = Omega.array(mfi);

        const Array4<const Real>& pi_stg_ca = pi_stg.const_array(mfi);

        const Array4<Real>& theta_extrap = extrap.array(mfi);

        Box gbx   = mfi.growntilebox(1);
        Box gtbx  = mfi.nodaltilebox(0).grow(1); gtbx.setSmall(2,0);
        Box gtby  = mfi.nodaltilebox(1).grow(1); gtby.setSmall(2,0);

        {
        BL_PROFILE("fast_rhs_copies_0");
        if (step == 0) {
            amrex::ParallelFor(gbx,
            [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                cur_cons(i,j,k,Rho_comp)            = prev_cons(i,j,k,Rho_comp);
                cur_cons(i,j,k,RhoTheta_comp)       = prev_cons(i,j,k,RhoTheta_comp);
                lagged_rtheta(i,j,k,RhoTheta_comp) = prev_cons(i,j,k,RhoTheta_comp);
            });
        }
        } // end profile

        {
        BL_PROFILE("fast_rhs_copies_2");
        amrex::ParallelFor(gbx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
            theta_extrap(i,j,k)     = (1.0 + beta_d) * cur_cons(i,j,k,RhoTheta_comp)
                                     -                 stg_cons(i,j,k,RhoTheta_comp)
                                            -beta_d  * lagged_rtheta(i,j  ,k,RhoTheta_comp);
        });
        } // end profile

        RHS_fab.resize(tbz,1);
        soln_fab.resize(tbz,1);
        temp_rhs_fab.resize(tbz,2);

        Elixir rCeli        =      RHS_fab.elixir();
        Elixir sCeli        =     soln_fab.elixir();
        Elixir temp_rhs_eli = temp_rhs_fab.elixir();

        auto const& RHS_a        =      RHS_fab.array();
        auto const& soln_a       =     soln_fab.array();
        auto const& temp_rhs_arr = temp_rhs_fab.array();

        auto const&     coeffA_a =     coeff_A_mf.array(mfi);
        auto const& inv_coeffB_a = inv_coeff_B_mf.array(mfi);
        auto const&     coeffC_a =     coeff_C_mf.array(mfi);
        auto const&     coeffP_a =     coeff_P_mf.array(mfi);
        auto const&     coeffQ_a =     coeff_Q_mf.array(mfi);

        // *********************************************************************
        // Define updates in the RHS of {x, y, z}-momentum equations
        // *********************************************************************
        {
        BL_PROFILE("fast_rhs_xymom_T");
        amrex::ParallelFor(tbx, tby,
        [=] AMREX_GPU_DEVICE (int i, int j, int k)
        {
                // Add (negative) gradient of (rho theta) multiplied by lagged "pi"
                Real h_xi_old   = Compute_h_xi_AtIface(i, j, k, dxInv, z_nd_old);
                Real h_zeta_old = Compute_h_zeta_AtIface(i, j, k, dxInv, z_nd_old);
                Real gp_xi = (theta_extrap(i,j,k) - theta_extrap(i-1,j,k)) * dxi;
                Real gp_zeta_on_iface = (k == 0) ?
                   0.5  * dzi * ( theta_extrap(i-1,j,k+1) + theta_extrap(i,j,k+1)
                                 -theta_extrap(i-1,j,k  ) - theta_extrap(i,j,k  ) ) :
                   0.25 * dzi * ( theta_extrap(i-1,j,k+1) + theta_extrap(i,j,k+1)
                                 -theta_extrap(i-1,j,k-1) - theta_extrap(i,j,k-1) );
                Real gpx = h_zeta_old * gp_xi - h_xi_old * gp_zeta_on_iface;

#ifdef ERF_USE_MOISTURE
                Real q = 0.5 * ( prim(i,j,k,PrimQv_comp) + prim(i-1,j,k,PrimQv_comp)
                                +prim(i,j,k,PrimQc_comp) + prim(i-1,j,k,PrimQc_comp) );
                gpx /= (1.0 + q);
#endif
                Real pi_c =  0.5 * (pi_stg_ca(i-1,j,k,0) + pi_stg_ca(i  ,j,k,0));
                Real fast_rhs_rho_u = -Gamma * R_d * pi_c * gpx;

                // We have already scaled the source terms to have the extra factor of dJ
                cur_xmom(i,j,k) = h_zeta_old * prev_xmom(i,j,k) + dtau * fast_rhs_rho_u
                                                                + dtau * slow_rhs_rho_u(i,j,k);
        },
        [=] AMREX_GPU_DEVICE (int i, int j, int k)
        {
                // Add (negative) gradient of (rho theta) multiplied by lagged "pi"
                Real h_eta_old  = Compute_h_eta_AtJface(i, j, k, dxInv, z_nd_old);
                Real h_zeta_old = Compute_h_zeta_AtJface(i, j, k, dxInv, z_nd_old);
                Real gp_eta = (theta_extrap(i,j,k) -theta_extrap(i,j-1,k)) * dyi;
                Real gp_zeta_on_jface = (k == 0) ?
                    0.5  * dzi * ( theta_extrap(i,j,k+1) + theta_extrap(i,j-1,k+1)
                                  -theta_extrap(i,j,k  ) - theta_extrap(i,j-1,k  ) ) :
                    0.25 * dzi * ( theta_extrap(i,j,k+1) + theta_extrap(i,j-1,k+1)
                                  -theta_extrap(i,j,k-1) - theta_extrap(i,j-1,k-1) );
                Real gpy = h_zeta_old * gp_eta - h_eta_old  * gp_zeta_on_jface;

#ifdef ERF_USE_MOISTURE
                Real q = 0.5 * ( prim(i,j,k,PrimQv_comp) + prim(i,j-1,k,PrimQv_comp)
                                +prim(i,j,k,PrimQc_comp) + prim(i,j-1,k,PrimQc_comp) );
                gpy /= (1.0 + q);
#endif
                Real pi_c =  0.5 * (pi_stg_ca(i,j-1,k,0) + pi_stg_ca(i,j  ,k,0));
                Real fast_rhs_rho_v = -Gamma * R_d * pi_c * gpy;

                // We have already scaled the source terms to have the extra factor of dJ
                cur_ymom(i, j, k) = h_zeta_old * prev_ymom(i,j,k) + dtau * fast_rhs_rho_v
                                                                  + dtau * slow_rhs_rho_v(i,j,k);
        });
        } // end profile

        // *********************************************************************
        {
        BL_PROFILE("fast_T_making_rho_rhs");
        amrex::ParallelFor(bx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
        {
            Real h_zeta_stg_xlo = Compute_h_zeta_AtIface(i,  j  , k, dxInv, z_nd_stg);
            Real h_zeta_stg_xhi = Compute_h_zeta_AtIface(i+1,j  , k, dxInv, z_nd_stg);
            Real xflux_lo = cur_xmom(i  ,j,k) - stg_xmom(i  ,j,k)*h_zeta_stg_xlo;
            Real xflux_hi = cur_xmom(i+1,j,k) - stg_xmom(i+1,j,k)*h_zeta_stg_xhi;

            Real h_zeta_stg_yhi = Compute_h_zeta_AtJface(i,  j+1, k, dxInv, z_nd_stg);
            Real h_zeta_stg_ylo = Compute_h_zeta_AtJface(i,  j  , k, dxInv, z_nd_stg);
            Real yflux_lo = cur_ymom(i,j  ,k) - stg_ymom(i,j  ,k)*h_zeta_stg_ylo;
            Real yflux_hi = cur_ymom(i,j+1,k) - stg_ymom(i,j+1,k)*h_zeta_stg_yhi;

            // NOTE: we are saving the (1/J) weighting for later when we add this to rho and theta
            temp_rhs_arr(i,j,k,0) =  ( xflux_hi - xflux_lo ) * dxi + ( yflux_hi - yflux_lo ) * dyi;
            temp_rhs_arr(i,j,k,1) = (( xflux_hi * (prim(i,j,k,0) + prim(i+1,j,k,0)) -
                                       xflux_lo * (prim(i,j,k,0) + prim(i-1,j,k,0)) ) * dxi +
                                     ( yflux_hi * (prim(i,j,k,0) + prim(i,j+1,k,0)) -
                                       yflux_lo * (prim(i,j,k,0) + prim(i,j-1,k,0)) ) * dyi) * 0.5;
        });
        } // end profile

        // *********************************************************************
        // This must be done before we set cur_xmom and cur_ymom, since those
        //      in fact point to the same array as prev_xmom and prev_ymom
        // *********************************************************************
        Box gbxo = mfi.nodaltilebox(2);gbxo.grow(IntVect(1,1,0));
        {
        BL_PROFILE("fast_T_making_omega");
        amrex::ParallelFor(gbxo, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
            omega_arr(i,j,k) = (k == 0) ? 0. :
               (OmegaFromW(i,j,k, prev_zmom(i,j,k), prev_xmom, prev_ymom,z_nd_old,dxInv)
               -OmegaFromW(i,j,k,stg_zmom(i,j,k),stg_xmom,stg_ymom,z_nd_old,dxInv)) - zp_t_arr(i,j,k);
        });
        } // end profile
        // *********************************************************************

        amrex::ParallelFor(tbx, tby,
        [=] AMREX_GPU_DEVICE (int i, int j, int k)
        {
            Real h_zeta_new = Compute_h_zeta_AtIface(i, j, k, dxInv, z_nd_new);
            cur_xmom(i, j, k) /= h_zeta_new;
            avg_xmom(i,j,k) += facinv*(cur_xmom(i,j,k) - stg_xmom(i,j,k));
        },
        [=] AMREX_GPU_DEVICE (int i, int j, int k)
        {
            Real h_zeta_new = Compute_h_zeta_AtJface(i, j, k, dxInv, z_nd_new);
            cur_ymom(i, j, k) /= h_zeta_new;
            avg_ymom(i,j,k) += facinv*(cur_ymom(i,j,k) - stg_ymom(i,j,k));
        });

        Box bx_shrunk_in_k = bx;
        int klo = tbz.smallEnd(2);
        int khi = tbz.bigEnd(2);
        bx_shrunk_in_k.setSmall(2,klo+1);
        bx_shrunk_in_k.setBig(2,khi-1);

        // Note that the notes use "g" to mean the magnitude of gravity, so it is positive
        // We set grav_gpu[2] to be the vector component which is negative
        // We define halfg to match the notes (which is why we take the absolute value)
        Real halfg = std::abs(0.5 * grav_gpu[2]);

        {
        BL_PROFILE("fast_loop_on_shrunk_t");
        //Note we don't act on the bottom or top boundaries of the domain
        ParallelFor(bx_shrunk_in_k, [=] AMREX_GPU_DEVICE (int i, int j, int k)
        {
            Real     dJ_old_kface = 0.5 * (detJ_old(i,j,k) + detJ_old(i,j,k-1));
            Real     dJ_new_kface = 0.5 * (detJ_new(i,j,k) + detJ_new(i,j,k-1));
            Real     dJ_stg_kface = 0.5 * (detJ_stg(i,j,k) + detJ_stg(i,j,k-1));

            Real coeff_P = coeffP_a(i,j,k);
            Real coeff_Q = coeffQ_a(i,j,k);

#ifdef ERF_USE_MOISTURE
            Real q = 0.5 * ( prim(i,j,k,PrimQv_comp) + prim(i,j,k-1,PrimQv_comp)
                            +prim(i,j,k,PrimQc_comp) + prim(i,j,k-1,PrimQc_comp) );
            coeff_P /= (1.0 + q);
            coeff_Q /= (1.0 + q);
#endif

            Real theta_t_lo  = 0.5 * ( prim(i,j,k-2,PrimTheta_comp) + prim(i,j,k-1,PrimTheta_comp) );
            Real theta_t_mid = 0.5 * ( prim(i,j,k-1,PrimTheta_comp) + prim(i,j,k  ,PrimTheta_comp) );
            Real theta_t_hi  = 0.5 * ( prim(i,j,k  ,PrimTheta_comp) + prim(i,j,k+1,PrimTheta_comp) );

            // line 2 last two terms (order dtau)
            Real R0_tmp  = coeff_P * cur_cons(i,j,k  ,RhoTheta_comp) * dJ_old_kface
                         + coeff_Q * cur_cons(i,j,k-1,RhoTheta_comp) * dJ_old_kface
                         - coeff_P * stg_cons(i,j,k  ,RhoTheta_comp) * dJ_stg_kface
                         - coeff_Q * stg_cons(i,j,k-1,RhoTheta_comp) * dJ_stg_kface
                       - halfg   * ( cur_cons(i,j,k,Rho_comp) + cur_cons(i,j,k-1,Rho_comp) ) * dJ_old_kface
                       + halfg   * ( stg_cons(i,j,k,Rho_comp) + stg_cons(i,j,k-1,Rho_comp) ) * dJ_stg_kface;

            // line 3 residuals (order dtau^2) 1.0 <-> beta_2
            Real R1_tmp = - halfg * ( slow_rhs_cons(i,j,k  ,Rho_comp) +
                                      slow_rhs_cons(i,j,k-1,Rho_comp) )
                      +   ( coeff_P * slow_rhs_cons(i,j,k  ,RhoTheta_comp) +
                            coeff_Q * slow_rhs_cons(i,j,k-1,RhoTheta_comp) );

            Real Omega_kp1 = omega_arr(i,j,k+1);
            Real Omega_k   = omega_arr(i,j,k  );
            Real Omega_km1 = omega_arr(i,j,k-1);

            // consolidate lines 4&5 (order dtau^2)
            R1_tmp += ( halfg ) *
                      ( beta_1 * dzi * (Omega_kp1 - Omega_km1) + temp_rhs_arr(i,j,k,Rho_comp) + temp_rhs_arr(i,j,k-1,Rho_comp));

            // consolidate lines 6&7 (order dtau^2)
            R1_tmp += -(
                 coeff_P * ( beta_1 * dzi * (Omega_kp1*theta_t_hi - Omega_k*theta_t_mid) + temp_rhs_arr(i,j,k  ,RhoTheta_comp) ) +
                 coeff_Q * ( beta_1 * dzi * (Omega_k*theta_t_mid - Omega_km1*theta_t_lo) + temp_rhs_arr(i,j,k-1,RhoTheta_comp) ) );

            // line 1
            RHS_a(i,j,k) = dJ_old_kface * prev_zmom(i,j,k) - dJ_stg_kface * stg_zmom(i,j,k)
                            + dtau *(slow_rhs_rho_w(i,j,k) + R0_tmp + dtau*beta_2*R1_tmp );

            // We cannot use omega_arr here since that was built with old_rho_u and old_rho_v ...
            RHS_a(i,j,k) += dJ_new_kface * OmegaFromW(i,j,k,0.,cur_xmom,cur_ymom,z_nd_new,dxInv)
                           -dJ_stg_kface * OmegaFromW(i,j,k,0.,stg_xmom,stg_ymom,z_nd_stg,dxInv);
        });
        } // end profile

        amrex::Box b2d = tbz; // Copy constructor
        b2d.setRange(2,0);

        auto const lo = amrex::lbound(bx);
        auto const hi = amrex::ubound(bx);

        {
        BL_PROFILE("fast_rhs_b2d_loop_t");

#ifdef AMREX_USE_GPU
        ParallelFor(b2d, [=] AMREX_GPU_DEVICE (int i, int j, int)
        {
            // Moving terrain
            Real rho_on_bdy = 0.5 * ( prev_cons(i,j,0) + prev_cons(i,j,-1) );
            RHS_a(i,j,0) = rho_on_bdy * zp_t_arr(i,j,0);

            soln_a(i,j,0) = RHS_a(i,j,0) * inv_coeffB_a(i,j,0);

            // w_khi = 0
            RHS_a(i,j,hi.z+1)     =  0.0;

            for (int k = 1; k <= hi.z+1; k++) {
                soln_a(i,j,k) = (RHS_a(i,j,k)-coeffA_a(i,j,k)*soln_a(i,j,k-1)) * inv_coeffB_a(i,j,k);
            }

            for (int k = hi.z; k >= 0; k--) {
                soln_a(i,j,k) -= ( coeffC_a(i,j,k) * inv_coeffB_a(i,j,k) ) * soln_a(i,j,k+1);
            }

           // We assume that Omega == w at the top boundary and that changes in J there are irrelevant
            cur_zmom(i,j,hi.z+1) = stg_zmom(i,j,hi.z+1) + soln_a(i,j,hi.z+1);
        });
#else
        for (int j = lo.y; j <= hi.y; ++j) {
             AMREX_PRAGMA_SIMD
             for (int i = lo.x; i <= hi.x; ++i) {

                 Real rho_on_bdy = 0.5 * ( prev_cons(i,j,0) + prev_cons(i,j,-1) );
                 RHS_a(i,j,0) = rho_on_bdy * zp_t_arr(i,j,0);

                 soln_a(i,j,0) = RHS_a(i,j,0) * inv_coeffB_a(i,j,0);
             }
        }

        for (int j = lo.y; j <= hi.y; ++j) {
             AMREX_PRAGMA_SIMD
             for (int i = lo.x; i <= hi.x; ++i) {
                 RHS_a (i,j,hi.z+1) =  0.0;
             }
        }
        for (int k = lo.z+1; k <= hi.z+1; ++k) {
             for (int j = lo.y; j <= hi.y; ++j) {
                 AMREX_PRAGMA_SIMD
                 for (int i = lo.x; i <= hi.x; ++i) {
                     soln_a(i,j,k) = (RHS_a(i,j,k)-coeffA_a(i,j,k)*soln_a(i,j,k-1)) * inv_coeffB_a(i,j,k);
                 }
           }
        }
        for (int k = hi.z; k >= lo.z; --k) {
             for (int j = lo.y; j <= hi.y; ++j) {
                 AMREX_PRAGMA_SIMD
                 for (int i = lo.x; i <= hi.x; ++i) {
                     soln_a(i,j,k) -= ( coeffC_a(i,j,k) * inv_coeffB_a(i,j,k) ) * soln_a(i,j,k+1);
                 }
             }
        }

        // We assume that Omega == w at the top boundary and that changes in J there are irrelevant
        for (int j = lo.y; j <= hi.y; ++j) {
             AMREX_PRAGMA_SIMD
             for (int i = lo.x; i <= hi.x; ++i) {
                cur_zmom(i,j,hi.z+1) = stg_zmom(i,j,hi.z+1) + soln_a(i,j,hi.z+1);
            }
        }
#endif
        } // end profile

        {
        BL_PROFILE("fast_rhs_new_drhow");
        tbz.setBig(2,hi.z);
        ParallelFor(tbz, [=] AMREX_GPU_DEVICE (int i, int j, int k)
        {
             Real rho_on_face = 0.5 * (cur_cons(i,j,k,Rho_comp) + cur_cons(i,j,k-1,Rho_comp));

             if (k == 0) {
                 cur_zmom(i,j,k) = WFromOmega(i,j,k,rho_on_face*(z_t_arr(i,j,k)+zp_t_arr(i,j,k)),
                                              cur_xmom,cur_ymom,z_nd_new,dxInv);

                 // We need to set this here because it is used to define zflux_lo below
                 soln_a(i,j,k) = 0.;

             } else {

                 Real wpp = WFromOmega(i,j,k,soln_a(i,j,k),cur_xmom,cur_ymom,z_nd_new,dxInv)
                           -WFromOmega(i,j,k,           0.,stg_xmom,stg_ymom,z_nd_stg,dxInv);
                 Real dJ_old_kface = 0.5 * (detJ_old(i,j,k) + detJ_old(i,j,k-1));
                 Real dJ_new_kface = 0.5 * (detJ_new(i,j,k) + detJ_new(i,j,k-1));

                 cur_zmom(i,j,k) = dJ_old_kface * (stg_zmom(i,j,k) + wpp);
                 cur_zmom(i,j,k) /= dJ_new_kface;

                 soln_a(i,j,k) = OmegaFromW(i,j,k,cur_zmom(i,j,k),cur_xmom,cur_ymom,z_nd_new,dxInv)
                               - OmegaFromW(i,j,k,stg_zmom(i,j,k),stg_xmom,stg_ymom,z_nd_stg,dxInv);
                 soln_a(i,j,k) -= rho_on_face * zp_t_arr(i,j,k);
             }
        });
        } // end profile

        // **************************************************************************
        // Define updates in the RHS of rho and (rho theta)
        // **************************************************************************

        // We note that valid_bx is the actual grid, while bx may be a tile within that grid
        // const auto& vbx_hi = amrex::ubound(valid_bx);

        {
        BL_PROFILE("fast_rho_final_update");
        amrex::ParallelFor(bx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
        {
              Real zflux_lo = beta_2 * soln_a(i,j,k  ) + beta_1 * omega_arr(i,j,k);
              Real zflux_hi = beta_2 * soln_a(i,j,k+1) + beta_1 * omega_arr(i,j,k+1);

              // Note that in the solve we effectively impose new_drho_w(i,j,vbx_hi.z+1)=0
              // so we don't update avg_zmom at k=vbx_hi.z+1
              avg_zmom(i,j,k) += facinv*zflux_lo;

              // Note that the factor of (1/J) in the fast source term is canceled
              // when we multiply old and new by detJ_old and detJ_new , respectively
              // We have already scaled the slow source term to have the extra factor of dJ
              Real fast_rhs_rho = -(temp_rhs_arr(i,j,k,0) + ( zflux_hi - zflux_lo ) * dzi);
              Real temp_rho = detJ_old(i,j,k) * cur_cons(i,j,k,0) +
                              dtau * ( slow_rhs_cons(i,j,k,0) + fast_rhs_rho );
              cur_cons(i,j,k,0) = temp_rho / detJ_new(i,j,k);

              // Note that the factor of (1/J) in the fast source term is canceled
              // when we multiply old and new by detJ_old and detJ_new , respectively
              // We have already scaled the slow source term to have the extra factor of dJ
              Real fast_rhs_rhotheta = -( temp_rhs_arr(i,j,k,1) + 0.5 *
                                        ( zflux_hi * (prim(i,j,k) + prim(i,j,k+1))
                                        - zflux_lo * (prim(i,j,k) + prim(i,j,k-1)) ) * dzi );
              Real temp_rth = detJ_old(i,j,k) * cur_cons(i,j,k,1) +
                              dtau * ( slow_rhs_cons(i,j,k,1) + fast_rhs_rhotheta );
              cur_cons(i,j,k,1) = temp_rth / detJ_new(i,j,k);

        });
        } // end profile
    } // mfi
    }
}