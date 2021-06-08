#include <AMReX.H>
#include <AMReX_MultiFab.H>
#include <AMReX_ArrayLim.H>
#include <AMReX_BC_TYPES.H>
//#include <AMReX_VisMF.H>

#include <Constants.H>

#include <ERF.H>
#include <RK3.H>

using namespace amrex;

void RK3_stage  (MultiFab& cons_old,  MultiFab& cons_upd,
                 MultiFab& xmom_old, MultiFab& ymom_old, MultiFab& zmom_old, 
                 MultiFab& xmom_upd, MultiFab& ymom_upd, MultiFab& zmom_upd, 
                 MultiFab& xvel    , MultiFab& yvel    , MultiFab& zvel    ,  
                 MultiFab& prim    , MultiFab& source,
                 MultiFab& eta, MultiFab& zeta, MultiFab& kappa,
                 std::array< MultiFab, AMREX_SPACEDIM>& faceflux,
                 std::array< MultiFab, 2 >& edgeflux_x,
                 std::array< MultiFab, 2 >& edgeflux_y,
                 std::array< MultiFab, 2 >& edgeflux_z,
                 std::array< MultiFab, AMREX_SPACEDIM>& cenflux,
                 const amrex::Geometry geom, const amrex::Real* dxp, const amrex::Real dt,
                 const SolverChoice& solverChoice)
{
    BL_PROFILE_VAR("RK3_stage()",RK3_stage);

    int nvars = cons_old.nComp(); 

    // ************************************************************************************
    // Fill the ghost cells/faces of the MultiFabs we will need
    // Apply BC on state data at cells
    // ************************************************************************************ 
    cons_old.FillBoundary(geom.periodicity());

    // Apply BC on velocity data on faces
    // Note that in RK3_advance, the BC was applied on momentum
    xvel.FillBoundary(geom.periodicity());
    yvel.FillBoundary(geom.periodicity());
    zvel.FillBoundary(geom.periodicity());

    // ************************************************************************************** 

    const GpuArray<Real, AMREX_SPACEDIM> dx = geom.CellSizeArray();
    Real gravity = solverChoice.use_gravity? CONST_GRAV: 0.0;
    const    Array<Real,AMREX_SPACEDIM> grav{0.0, 0.0, gravity};
    const GpuArray<Real,AMREX_SPACEDIM> grav_gpu{grav[0], grav[1], grav[2]};

    // ************************************************************************************** 
    // 
    // Calculate face-based fluxes to update cell-centered quantities, and 
    //           edge-based and cell-based fluxes to update face-centered quantities
    // 
    // ************************************************************************************** 
    CalcAdvFlux(cons_old, xmom_old, ymom_old, zmom_old, 
                xvel    , yvel    , zvel    , 
                faceflux, edgeflux_x, edgeflux_y, edgeflux_z, cenflux, 
                geom, dxp, dt,
                solverChoice);
//#if 0
//    CalcDiffFlux(cons_old, xmom_old, ymom_old, zmom_old,
//                 xvel    , yvel    , zvel    ,
//                 eta, zeta, kappa,
//                 faceflux, edgeflux_x, edgeflux_y, edgeflux_z, cenflux,
//                 geom, dxp, dt,
//                 solverChoice);
//#endif

    // **************************************************************************************
    // Define updates in the current RK stage, after fluxes have been computed
    // ************************************************************************************** 
    for ( MFIter mfi(cons_old,TilingIfNotGPU()); mfi.isValid(); ++mfi) {
        
        const Box& bx = mfi.tilebox();
        const Box& tbx = mfi.nodaltilebox(0);
        const Box& tby = mfi.nodaltilebox(1);
        const Box& tbz = mfi.nodaltilebox(2);

        const Array4<Real> & cu_old     = cons_old.array(mfi);
        const Array4<Real> & cu_upd     = cons_upd.array(mfi);
        const Array4<Real> & source_fab = source.array(mfi);

        const Array4<Real> & u = xvel.array(mfi);
        const Array4<Real> & v = yvel.array(mfi);
        const Array4<Real> & w = zvel.array(mfi);

        const Array4<Real>& rho_u = xmom_old.array(mfi);
        const Array4<Real>& rho_v = ymom_old.array(mfi);
        const Array4<Real>& rho_w = zmom_old.array(mfi);

        const Array4<Real>& mompx = xmom_upd.array(mfi);
        const Array4<Real>& mompy = ymom_upd.array(mfi);
        const Array4<Real>& mompz = zmom_upd.array(mfi);

//        Array4<Real const> const& xflux = faceflux[0].array(mfi);
//        Array4<Real const> const& yflux = faceflux[1].array(mfi);
//        Array4<Real const> const& zflux = faceflux[2].array(mfi);

        Array4<Real const> const& edgex_v = edgeflux_x[0].array(mfi);
        Array4<Real const> const& edgex_w = edgeflux_x[1].array(mfi);

        Array4<Real const> const& edgey_u = edgeflux_y[0].array(mfi);
        Array4<Real const> const& edgey_w = edgeflux_y[1].array(mfi);

        Array4<Real const> const& edgez_u = edgeflux_z[0].array(mfi);
        Array4<Real const> const& edgez_v = edgeflux_z[1].array(mfi);

        Array4<Real const> const& cenx_u = cenflux[0].array(mfi);
        Array4<Real const> const& ceny_v = cenflux[1].array(mfi);
        Array4<Real const> const& cenz_w = cenflux[2].array(mfi);

//        amrex::ParallelFor(bx, nvars, [=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
//        {
//            cu_upd(i,j,k,n) = - dt *
//                ( AMREX_D_TERM(  (xflux(i+1,j,k,n) - xflux(i,j,k,n)) / dx[0],
//                               + (yflux(i,j+1,k,n) - yflux(i,j,k,n)) / dx[1],
//                               + (zflux(i,j,k+1,n) - zflux(i,j,k,n)) / dx[2])
//                                                                                       )
//                + dt*source_fab(i,j,k,n);
//        });

        amrex::ParallelFor(bx, nvars,
       [=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
           {
               Real xflux_next, xflux_prev, yflux_next, yflux_prev, zflux_next, zflux_prev;
               switch(n) {
                case Density_comp:
                    xflux_next = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::next, AdvectedQuantity::unity, AdvectingQuantity::rho_u, solverChoice.spatial_order);
                    xflux_prev = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::prev, AdvectedQuantity::unity, AdvectingQuantity::rho_u, solverChoice.spatial_order);
                    yflux_next = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::next, AdvectedQuantity::unity, AdvectingQuantity::rho_v, solverChoice.spatial_order);
                    yflux_prev = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::prev, AdvectedQuantity::unity, AdvectingQuantity::rho_v, solverChoice.spatial_order);
                    zflux_next = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::next, AdvectedQuantity::unity, AdvectingQuantity::rho_w, solverChoice.spatial_order);
                    zflux_prev = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::prev, AdvectedQuantity::unity, AdvectingQuantity::rho_w, solverChoice.spatial_order);
                    break;
                case RhoTheta_comp:
                    xflux_next = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::next, AdvectedQuantity::theta, AdvectingQuantity::rho_u, solverChoice.spatial_order);
                    xflux_prev = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::prev, AdvectedQuantity::theta, AdvectingQuantity::rho_u, solverChoice.spatial_order);
                    yflux_next = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::next, AdvectedQuantity::theta, AdvectingQuantity::rho_v, solverChoice.spatial_order);
                    yflux_prev = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::prev, AdvectedQuantity::theta, AdvectingQuantity::rho_v, solverChoice.spatial_order);
                    zflux_next = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::next, AdvectedQuantity::theta, AdvectingQuantity::rho_w, solverChoice.spatial_order);
                    zflux_prev = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::prev, AdvectedQuantity::theta, AdvectingQuantity::rho_w, solverChoice.spatial_order);
                    break;
                case Scalar_comp:
                    xflux_next = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::next, AdvectedQuantity::scalar, AdvectingQuantity::rho_u, solverChoice.spatial_order);
                    xflux_prev = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::prev, AdvectedQuantity::scalar, AdvectingQuantity::rho_u, solverChoice.spatial_order);
                    yflux_next = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::next, AdvectedQuantity::scalar, AdvectingQuantity::rho_v, solverChoice.spatial_order);
                    yflux_prev = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::prev, AdvectedQuantity::scalar, AdvectingQuantity::rho_v, solverChoice.spatial_order);
                    zflux_next = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::next, AdvectedQuantity::scalar, AdvectingQuantity::rho_w, solverChoice.spatial_order);
                    zflux_prev = ComputeAdvectedQuantityForState(i, j, k, rho_u, rho_v, rho_w, cu_old, NextOrPrev::prev, AdvectedQuantity::scalar, AdvectingQuantity::rho_w, solverChoice.spatial_order);
                    break;
                default:
                    amrex::Abort("Error: Conserved quantity index is unrecognized");
            }
            cu_upd(i,j,k,n) = 0.0;
            // Add advective terms. Put this under a if condition
            cu_upd(i,j,k,n) += (-dt) *
                    (  (xflux_next - xflux_prev) / dx[0]
                     + (yflux_next - yflux_prev) / dx[1]
                     + (zflux_next - zflux_prev) / dx[2]
                    );
            // Add diffusive terms. Put this under a if condition

            // Add source terms. Put this under a if condition
            cu_upd(i,j,k,n) += dt*source_fab(i,j,k,n);
           }
        );

        // momentum flux
        amrex::ParallelFor(tbx, tby, tbz,
        [=] AMREX_GPU_DEVICE (int i, int j, int k) {
            mompx(i,j,k) = 
                    -dt*(cenx_u(i,j,k) - cenx_u(i-1,j,k))/dx[0]
                    -dt*(edgey_u(i,j+1,k) - edgey_u(i,j,k))/dx[1]
                    -dt*(edgez_u(i,j,k+1) - edgez_u(i,j,k))/dx[2]
                    +0.5*dt*grav_gpu[0]*(cu_old(i-1,j,k,0)+cu_old(i,j,k,0));
        },
        [=] AMREX_GPU_DEVICE (int i, int j, int k) {
            mompy(i,j,k) = 
                    -dt*(edgex_v(i+1,j,k) - edgex_v(i,j,k))/dx[0]
                    -dt*(ceny_v(i,j,k) - ceny_v(i,j-1,k))/dx[1]
                    -dt*(edgez_v(i,j,k+1) - edgez_v(i,j,k))/dx[2]
                    +0.5*dt*grav_gpu[1]*(cu_old(i,j-1,k,0)+cu_old(i,j,k,0));
        },
        [=] AMREX_GPU_DEVICE (int i, int j, int k) {
            mompz(i,j,k) = 
                    -dt*(edgex_w(i+1,j,k) - edgex_w(i,j,k))/dx[0]
                    -dt*(edgey_w(i,j+1,k) - edgey_w(i,j,k))/dx[1]
                    -dt*(cenz_w(i,j,k) - cenz_w(i,j,k-1))/dx[2] 
                    +0.5*dt*grav_gpu[2]*(cu_old(i,j,k-1,0)+cu_old(i,j,k,0));
        });
    }
}
