# ------------------  INPUTS TO MAIN PROGRAM  -------------------
max_step = 10

amrex.fpe_trap_invalid = 1

fabarray.mfiter_tile_size = 1024 1024 1024

# PROBLEM SIZE & GEOMETRY
geometry.prob_extent =  1024     1024    1024
amr.n_cell           =    64       64      64

geometry.is_periodic = 1 1 0

# MOST BOUNDARY (DEFAULT IS ADIABATIC FOR THETA)
zlo.type      = "Most"
erf.most.z0   = 0.1
erf.most.zref = 8.0

zhi.type = "SlipWall"

# TIME STEP CONTROL
erf.fixed_dt = 0.1  # fixed time step depending on grid resolution

# DIAGNOSTICS & VERBOSITY
erf.sum_interval   = 1       # timesteps between computing mass
erf.v              = 1       # verbosity in ERF.cpp
amr.v              = 1       # verbosity in Amr.cpp

# REFINEMENT / REGRIDDING
amr.max_level       = 0       # maximum level number allowed

# CHECKPOINT FILES
erf.check_file      = chk        # root name of checkpoint file
erf.check_int       = 100        # number of timesteps between checkpoints

# PLOTFILES
erf.plot_file_1     = plt       # prefix of plotfile name
erf.plot_int_1      = 10        # number of timesteps between plotfiles
erf.plot_vars_1     = density rhoadv_0 x_velocity y_velocity z_velocity pressure temp theta

# SOLVER CHOICE
erf.alpha_T = 0.0
erf.alpha_C = 1.0
erf.use_gravity = false

erf.molec_diff_type = "None"
erf.les_type = "Deardorff"
erf.Ck       = 0.1
erf.sigma_k  = 1.0
erf.Ce       = 0.1
erf.KE_0     = 0.1

erf.init_type = "uniform"

# PROBLEM PARAMETERS
prob.rho_0 = 1.0
prob.A_0 = 1.0

prob.U_0 = 10.0
prob.V_0 = 0.0
prob.W_0 = 0.0
prob.T_0 = 300.0

# Higher values of perturbations lead to instability
# Instability seems to be coming from BC
prob.U_0_Pert_Mag = 0.0
prob.V_0_Pert_Mag = 0.0
prob.W_0_Pert_Mag = 0.0