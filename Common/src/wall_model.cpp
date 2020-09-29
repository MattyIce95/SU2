/*!
 * \file wall_model.cpp
 * \brief File, which contains the implementation for the wall model functions
 *        for large eddy simulations.
 * \author E. van der Weide, T. Economon, P. Urbanczyk, E. Molina
 * \version 7.0.3 "Blackbird"
 *
 * SU2 Project Website: https://su2code.github.io
 *
 * The SU2 Project is maintained by the SU2 Foundation
 * (http://su2foundation.org)
 *
 * Copyright 2012-2020, SU2 Contributors (cf. AUTHORS.md)
 *
 * SU2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../include/wall_model.hpp"
#include "../../SU2_CFD/include/fluid_model.hpp"
#include <Eigen/Dense>

typedef Eigen::ArrayXd Array;
/* Prototypes for Lapack functions, if MKL or LAPACK is used. */
#if defined (HAVE_MKL) || defined(HAVE_LAPACK)
extern "C" void dgtsv_(int*, int*, passivedouble*, passivedouble*,
                       passivedouble*, passivedouble*, int*, int*);
#endif

CWallModel::CWallModel(CConfig *config) {

  /* Get the laminar and turbulent Prandtl number from config. */
  Pr_lam  = config->GetPrandtl_Lam();
  Pr_turb = config->GetPrandtl_Turb();

  /* von Karman constant -> k = 0.41; or 0.38 */
  karman = 0.41;

  /*--- Get the dimensional Gas Constant.---*/
  RGas = config->GetGas_Constant();
  Gamma = config->GetGamma();
  Cv = RGas/(Gamma-1.0);
  Cp = Gamma*Cv;

  Pref = config->GetPressure_Ref();
  Tref = config->GetTemperature_Ref();
  Uref = config->GetVelocity_Ref();
}

void CWallModel::UpdateExchangeLocation(const su2double h_wm_new){}

void CWallModel::WallShearStressAndHeatFlux(const su2double rhoExchange,
                                            const su2double velExchange,
                                            const su2double muExchange,
                                            const su2double pExchange,
                                            const su2double dPds,
                                            const su2double Wall_HeatFlux,
                                            const bool      HeatFlux_Prescribed,
                                            const su2double Wall_Temperature,
                                            const bool      Temperature_Prescribed,
                                            CFluidModel     *FluidModel,
                                            su2double       &tauWall,
                                            su2double       &qWall,
                                            su2double       &ViscosityWall,
                                            su2double       &kOverCvWall) {}

CWallModel1DEQ::CWallModel1DEQ(CConfig      *config,
                               const string &Marker_Tag)
  :  CWallModel(config) {

  /* Retrieve the integer and floating point information for this
     boundary marker. */
  const unsigned short *intInfo    = config->GetWallFunction_IntInfo(Marker_Tag);
  const su2double      *doubleInfo = config->GetWallFunction_DoubleInfo(Marker_Tag);

  /* Copy the data into the member variables. */
  numPoints      = intInfo[0];
  h_wm           = doubleInfo[0];
  expansionRatio = doubleInfo[1];

  unsigned short nfa = numPoints + 1;
  su2double currentHeight = 1.0;

  /* Allocate the memory for the coordinates of the grid points used
     in the 1D equilibrium wall model. */
  y_cv.resize(numPoints);
  y_fa.resize(numPoints+1);

  /* Determine the scaled version of the normal coordinates, where the
   first normal coordinate is simply 1.0. */

  y_fa[0] = 0.0;
  for(unsigned short i=1; i<nfa; ++i) {
    y_fa[i] = y_fa[i-1] + currentHeight;
    y_cv[i-1] = 0.5 * (y_fa[i] + y_fa[i-1]);
    currentHeight = currentHeight*expansionRatio;
  }

  su2double y_max = y_cv[numPoints-1];
  for(unsigned short i=0; i<nfa; ++i) {
    y_fa[i] = y_fa[i]/y_max * h_wm;
  }
  for(unsigned short i=0; i<numPoints; ++i) {
    y_cv[i] = y_cv[i]/y_max * h_wm;
  }
}

void CWallModel1DEQ::UpdateExchangeLocation(const su2double h_wm_new){}

void CWallModel1DEQ::WallShearStressAndHeatFlux(const su2double tExchange,
                                                const su2double velExchange,
                                                const su2double muExchange,
                                                const su2double pExchange,
                                                const su2double dPds,
                                                const su2double Wall_HeatFlux,
                                                const bool      HeatFlux_Prescribed,
                                                const su2double Wall_Temperature,
                                                const bool      Temperature_Prescribed,
                                                CFluidModel     *FluidModel,
                                                su2double       &tauWall,
                                                su2double       &qWall,
                                                su2double       &ViscosityWall,
                                                su2double       &kOverCvWall) {

  /* Set the wall temperature, depending whether or not the temperature
     was prescribed and initialize the fluid model. */
  const su2double TWall = Temperature_Prescribed ? Wall_Temperature : tExchange;

  /* Set tau wall to initial guess
   */
  tauWall = 0.5;
  qWall = 0.0;
  ViscosityWall = 0.0;
  kOverCvWall = 0.0;

  /* Set some constants.
   */
  su2double A = 17.0;
  su2double h_wall = Cp * TWall;
  su2double h_bc   = Cp * tExchange;
  unsigned short nfa = numPoints + 1;

  /* Set up vectors
   */
  vector<su2double> T(nfa, tExchange);
  vector<su2double> mu_fa(nfa, muExchange);
  vector<su2double> tmp(nfa, 0.0);
  vector<su2double> u(numPoints, 0.0);
  vector<su2double> lower(numPoints-1,0.0);
  vector<su2double> upper(numPoints-1,0.0);
  vector<su2double> diagonal(numPoints,0.0);
  vector<su2double> rhs(numPoints,0.0);

  /* Set parameters for control
   */
  bool converged = false;
  unsigned short iter = 0, max_iter = 25;
  su2double tauWall_prev = 0.0, tol = 1e-3,  aux_rhs=0.0;
  su2double qWall_prev=0.0;
  su2double mut, nu, mu_lam, rho, utau, y_plus, D;

  while (converged == false){

    iter += 1;
    if (iter == max_iter) converged = true;

    tauWall_prev = tauWall;
    qWall_prev = qWall;

    /* Calculate total viscosity
    note: rho and mu_lam will be a function of temperature when solving an energy equation
     */
    for(unsigned short i=0; i < nfa; ++i){
        FluidModel->SetTDState_PT(pExchange/Pref, T[i]/Tref);
        mu_fa[i] = FluidModel->GetLaminarViscosity() * Pref / Uref;
    }

    for(unsigned short i=1; i < nfa; ++i){
      rho = pExchange / (RGas*T[i]);
      nu = mu_fa[i] / rho;
      utau = sqrt(tauWall / rho);
      y_plus = y_fa[i] * utau / nu;
      D = pow(1.0 - exp((-y_plus)/A),2.0);
      mut = rho * karman * y_fa[i] * utau * D;
      mu_fa[i] += mut;
    }

    /* Momentum matrix
     The solution vector is u at y_cv
     */
    lower.assign(numPoints-1,0.0);
    upper.assign(numPoints-1,0.0);
    diagonal.assign(numPoints,0.0);
    rhs.assign(numPoints,0.0);

    /* Top bc
     */
    diagonal[numPoints - 1] = 1.0;
    rhs[numPoints - 1] = velExchange;

    /* Internal cvs
     */
    for (unsigned short i=1; i < (numPoints - 1); ++i){
      upper[i]  =  mu_fa[i + 1] / (y_cv[i + 1] -  y_cv[i] );
      lower[i-1]  = mu_fa[i] / (y_cv[i] -  y_cv[i - 1] );
      diagonal[i] = -1.0 * (upper[i] + lower[i - 1]);
    }

    /* Wall BC
     */
    upper[0] = mu_fa[1]/(y_cv[1] - y_cv[0]);
    diagonal[0] = -1.0 * (upper[0] + mu_fa[0]/(y_cv[0]-y_fa[0]) );
    rhs[0] = 0.0;

    /* Solve the matrix problem to get the velocity field
       - rhs returns the solution
    */
#if (defined(HAVE_MKL) || defined(HAVE_LAPACK)) && !(defined(CODI_REVERSE_TYPE) || defined(CODI_FORWARD_TYPE))
    int info, nrhs = 1;

    dgtsv_(&numPoints,&nrhs,lower.data(),diagonal.data(),upper.data(),rhs.data(),&numPoints, &info);
    if (info != 0)
      SU2_MPI::Error("Unsuccessful call to dgtsv_", CURRENT_FUNCTION);
#else
    SU2_MPI::Error("Not compiled with MKL or LAPACK support", CURRENT_FUNCTION);
#endif

    u = rhs;

    /* Update total viscosity
     */
    for(unsigned short i=0; i < nfa; ++i){
      FluidModel->SetTDState_PT(pExchange/Pref, T[i]/Tref);
      mu_fa[i] = FluidModel->GetLaminarViscosity() * Pref / Uref;
      tmp[i] = mu_fa[i]/Pr_lam;
    }
    /* Update tauWall
     */
    tauWall = mu_fa[0] * (u[0] - 0.0)/(y_cv[0]-y_fa[0]);
    for(unsigned short i=1; i < nfa; ++i){
      rho = pExchange / (RGas*T[i]);
      nu = mu_fa[i] / rho;
      utau = sqrt(tauWall / rho);
      y_plus = y_fa[i] * utau / nu;
      D = pow(1.0 - exp((-y_plus)/A),2.0);
      mut = rho * karman * y_fa[i] * utau * D;
      mu_fa[i] += mut;
      tmp[i] += mut/Pr_turb;
    }

    /* Energy matrix
     The Solution vector is Enthalpy at y_cv
     */
    lower.assign(numPoints-1,0.0);
    upper.assign(numPoints-1,0.0);
    diagonal.assign(numPoints,0.0);
    rhs.assign(numPoints,0.0);

    /* Internal cvs
     */
    for (unsigned short i=1; i < (numPoints - 1); ++i){
      upper[i]  =  tmp[i + 1] / (y_cv[i + 1] -  y_cv[i] );
      lower[i-1]  = tmp[i] / (y_cv[i] -  y_cv[i - 1] );
      diagonal[i] = -1.0 * (upper[i] + lower[i - 1]);
    }

    /* Top BC
     */
    diagonal[numPoints - 1] = 1.0;

    /* Wall BC
     */
    upper[0] = tmp[1]/(y_cv[1] - y_cv[0]);
    diagonal[0] = -1.0 * (upper[0] + tmp[0]/(y_cv[0]-y_fa[0]) );
    aux_rhs = tmp[0]/(y_cv[0]-y_fa[0]);

    /* RHS of the Energy equation
       - Compute flux -- (mu + mu_t) * u * du/dy --
     */

    /* Zero flux at the wall
     */
    tmp[0] = 0. ;
    for (unsigned short i=1; i < numPoints; ++i){
      tmp[i] = 0.5* (mu_fa[i]) * (u[i] + u[i-1]) * (u[i] -u[i-1])/(y_cv[i] -  y_cv[i - 1] )  ;
    }
    for (unsigned short i=0; i < (numPoints - 1); ++i){
      rhs[i] = -tmp[i+1] + tmp[i];
    }

    if (HeatFlux_Prescribed == true){
      /* dT/dy = 0 -> Twall = T[1] */
      h_wall = Cp * T[1];
    }

    rhs[0] -= aux_rhs * h_wall;
    rhs[numPoints-1] = h_bc;

    /* Solve the matrix problem to get the Enthalpy field
     */
#if (defined(HAVE_MKL) || defined(HAVE_LAPACK)) && !(defined(CODI_REVERSE_TYPE) || defined(CODI_FORWARD_TYPE))
    dgtsv_(&numPoints,&nrhs,lower.data(),diagonal.data(),upper.data(),rhs.data(),&numPoints, &info);
    if (info != 0)
      SU2_MPI::Error("Unsuccessful call to dgtsv_", CURRENT_FUNCTION);
#else
    SU2_MPI::Error("Not compiled with MKL or LAPACK support", CURRENT_FUNCTION);
#endif

    /* Get Temperature from enthalpy
      - Temperature will be at face
     */
    T[0] = h_wall/Cp;
    T[numPoints] = h_bc/Cp;
    for (unsigned short i=0; i < numPoints-1; i++){
      T[i+1] = 0.5 * (rhs[i] + rhs[i+1])/Cp;
    }

    /* Final update tauWall
     */
    FluidModel->SetTDState_PT(pExchange/Pref, T[0]/Tref);
    mu_lam = FluidModel->GetLaminarViscosity() * Pref / Uref;

    /* These quantities will be returned.
     */
    tauWall = mu_lam * (u[0] - 0.0)/(y_cv[0]-y_fa[0]);
    qWall = mu_lam * (Cp / Pr_lam) * -(T[1] - T[0]) / (y_fa[1]-y_fa[0]);
    ViscosityWall = mu_lam;
    //kOverCvWall = c_p / c_v * (mu[0]/Pr_lam + muTurb[0]/Pr_turb);
    kOverCvWall = Cp / Cv * (mu_lam/Pr_lam);

    /* Final check of the Y+
     */
    rho = pExchange / (RGas * T[0]);
    if (y_cv[0] * sqrt(tauWall/rho) / (mu_lam/rho) > 1.0)
      SU2_MPI::Error("Y+ greater than one: Increase the number of points or growth ratio.", CURRENT_FUNCTION);

    /* Define a norm
     */
    if (fabs(1.0 - tauWall/tauWall_prev) < tol && fabs(1.0 - qWall/qWall_prev) < tol){
      converged = true;
    }
  }
}

CWallModelLogLaw::CWallModelLogLaw(CConfig      *config,
                                   const string &Marker_Tag)
  :  CWallModel(config) {

  C = 5.25; /* Constant to match the Reichardt BL profile ->  C = 4.1;  or 5.25. */

  /* Retrieve the floating point information for this boundary marker
     and set the exchange height. */
  const su2double *doubleInfo = config->GetWallFunction_DoubleInfo(Marker_Tag);
  h_wm = doubleInfo[0];

}

void CWallModelLogLaw::UpdateExchangeLocation(const su2double h_wm_new){
  h_wm = h_wm_new;
}

void CWallModelLogLaw::WallShearStressAndHeatFlux(const su2double tExchange,
                                                  const su2double velExchange,
                                                  const su2double muExchange,
                                                  const su2double pExchange,
                                                  const su2double dPds,
                                                  const su2double Wall_HeatFlux,
                                                  const bool      HeatFlux_Prescribed,
                                                  const su2double Wall_Temperature,
                                                  const bool      Temperature_Prescribed,
                                                  CFluidModel     *FluidModel,
                                                  su2double       &tauWall,
                                                  su2double       &qWall,
                                                  su2double       &ViscosityWall,
                                                  su2double       &kOverCvWall) {

  /* Set the wall temperature, depending whether or not the temperature
     was prescribed and initialize the fluid model. */
  const su2double TWall = Temperature_Prescribed ? Wall_Temperature : tExchange;

  FluidModel->SetTDState_PT(pExchange/Pref, TWall/Tref);

  /* Get the required data from the fluid model.
   */
  const su2double mu_wall  = FluidModel->GetLaminarViscosity() * Pref / Uref;;
  const su2double rho_wall = pExchange / (TWall * RGas);
  const su2double nu_wall  = mu_wall / rho_wall;

  /* Initial guess of the friction velocity. */
  su2double u_tau = max(0.01*velExchange, 1.e-5);

  /* Set parameters for control of the Newton iteration. */
  bool converged = false;
  unsigned short iter = 0, max_iter = 50;
  const su2double tol=1e-3;

  while (converged == false){

    iter += 1;
    if (iter == max_iter) converged = true;

    const su2double u_tau0 = u_tau;
    const su2double y_plus = u_tau0*h_wm/nu_wall;

    /* Reichardt boundary layer analytical law
       fprime is the differentiation of the Reichardt law with repect to u_tau.
     */
    const su2double fval = velExchange/u_tau0 - ((C - log(karman)/karman)*(1.0 - exp(-y_plus/11.0)
                         - (y_plus/11.0)*exp(-0.33*y_plus))) - log(karman*y_plus + 1.0)/karman;
    const su2double fprime = -velExchange/pow(u_tau0,2.0)
                           + (- C + log(karman)/karman)*(-(1.0/11.0)*h_wm*exp(-0.33*y_plus)/nu_wall
                           +                              (1.0/11.0)*h_wm*exp(-(1.0/11.0)*y_plus)/nu_wall
                           +                              (1.0/33.0)*u_tau0*pow(h_wm,2.0)*exp(-0.33*y_plus)/pow(nu_wall, 2.0))
                           - 1.0*h_wm/(nu_wall*(karman*y_plus + 1.0));

    /* Newton method */
    const su2double newton_step = fval/fprime;
    u_tau = u_tau0 - newton_step;

    /* Define a norm */
    if (fabs(1.0 - u_tau/u_tau0) < tol) converged = true;
  }

  tauWall = rho_wall * pow(u_tau,2.0);

  if (Temperature_Prescribed){
    /* The Kader's law will be used to approximate the variations of the temperature inside the boundary layer.
     */
    const su2double y_plus = u_tau*h_wm/nu_wall;
    const su2double lhs = - ((tExchange - TWall) * rho_wall * Cp * u_tau);
    const su2double Gamma = - (0.01 * (pow(Pr_lam * y_plus,4.0))/(1.0 + 5.0*y_plus*pow(Pr_lam,3.0)));
    const su2double rhs_1 = Pr_lam * y_plus * exp(Gamma);
    const su2double rhs_2 = (2.12*log(1.0+y_plus) + pow((3.85*pow(Pr_lam,(1.0/3.0)) - 1.3),2.0) + 2.12*log(Pr_lam)) * exp(1./Gamma);
    qWall = lhs/(rhs_1 + rhs_2);
  }
  else{
    qWall = Wall_HeatFlux;
  }

  ViscosityWall = mu_wall;
  kOverCvWall   = (mu_wall*Cp/Pr_lam)/Cv;
}

CWallModelAlgebraic::CWallModelAlgebraic(CConfig      *config,
                                   const string &Marker_Tag)
  :  CWallModel(config) {

  C = 5.2; /* Constant to match the BL profile. */

  /* Retrieve the floating point information for this boundary marker
     and set the exchange height. */
  const su2double *doubleInfo = config->GetWallFunction_DoubleInfo(Marker_Tag);
  h_wm = doubleInfo[0];

}

void CWallModelAlgebraic::UpdateExchangeLocation(const su2double h_wm_new){
  h_wm = h_wm_new;
}

void CWallModelAlgebraic::WallShearStressAndHeatFlux(const su2double tExchange,
                                                  const su2double velExchange,
                                                  const su2double muExchange,
                                                  const su2double pExchange,
                                                  const su2double dPds,
                                                  const su2double Wall_HeatFlux,
                                                  const bool      HeatFlux_Prescribed,
                                                  const su2double Wall_Temperature,
                                                  const bool      Temperature_Prescribed,
                                                  CFluidModel     *FluidModel,
                                                  su2double       &tauWall,
                                                  su2double       &qWall,
                                                  su2double       &ViscosityWall,
                                                  su2double       &kOverCvWall) {

  /* Set the wall temperature, depending whether or not the temperature
     was prescribed and initialize the fluid model. */
  const su2double TWall = Temperature_Prescribed ? Wall_Temperature : tExchange;

  FluidModel->SetTDState_PT(pExchange/Pref, TWall/Tref);

  /* Get the required data from the fluid model.
   */
  const su2double mu_wall  = FluidModel->GetLaminarViscosity() * Pref / Uref;;
  const su2double rho_wall = pExchange / (TWall * RGas);
  const su2double nu_wall  = mu_wall / rho_wall;

  /* Initial guess of the friction velocity. */
  su2double u_tau = max(0.01*velExchange, 1.e-5);

  /* Set parameters for control of the Newton iteration. */
  bool converged = false;
  unsigned short iter = 0, max_iter = 50;
  const su2double tol=1e-3;
  const su2double y_star = 23.0;

  const su2double a_1 = (1./(2. * y_star)) * ((1./(karman * y_star)) - 1.);
  while (converged == false){

    iter += 1;
    if (iter == max_iter) converged = true;

    const su2double u_tau0 = u_tau;
    const su2double y_plus = u_tau0*h_wm/nu_wall;

    /* Algebraic boundary layer analytical law
       fprime is the differentiation of the Algebraic law with repect to u_tau.
     */
    su2double fval, fprime;
    if (y_plus < y_star){
      fval   = velExchange/u_tau0 - y_plus - a_1*pow(y_plus, 2.0);
      fprime = -2.0*a_1*pow(y_plus, 2.0)/u_tau0 - velExchange/pow(u_tau0, 2) - h_wm/nu_wall;
    }
    else{
      fval   = velExchange/u_tau0 - 1.0*log(y_plus)/karman - C;
      fprime =-velExchange/pow(u_tau0, 2) - 1.0/(karman*u_tau0);
    }

    /* Newton method */
    const su2double newton_step = fval/fprime;
    u_tau = u_tau0 - newton_step;

    /* Define a norm */
    if (fabs(1.0 - u_tau/u_tau0) < tol) converged = true;
  }

  tauWall = rho_wall * pow(u_tau,2.0);

  if (Temperature_Prescribed){
    /* The Kader's law will be used to approximate the variations of the temperature inside the boundary layer.
     */
    const su2double y_plus = u_tau*h_wm/nu_wall;
    const su2double lhs = - ((tExchange - TWall) * rho_wall * Cp * u_tau);
    const su2double Gamma = - (0.01 * (pow(Pr_lam * y_plus,4.0))/(1.0 + 5.0*y_plus*pow(Pr_lam,3.0)));
    const su2double rhs_1 = Pr_lam * y_plus * exp(Gamma);
    const su2double rhs_2 = (2.12*log(1.0+y_plus) + pow((3.85*pow(Pr_lam,(1.0/3.0)) - 1.3),2.0) + 2.12*log(Pr_lam)) * exp(1./Gamma);
    qWall = lhs/(rhs_1 + rhs_2);
  }
  else{
    qWall = Wall_HeatFlux;
  }

  ViscosityWall = mu_wall;
  kOverCvWall   = (mu_wall*Cp/Pr_lam)/Cv;
}

CWallModelAPGLL::CWallModelAPGLL(CConfig      *config,
                                   const string &Marker_Tag)
  :  CWallModel(config) {

  C = 5.2; /* Constant to match the BL profile. */

  /* Retrieve the floating point information for this boundary marker
     and set the exchange height. */
  const su2double *doubleInfo = config->GetWallFunction_DoubleInfo(Marker_Tag);
  h_wm = doubleInfo[0];

}

void CWallModelAPGLL::UpdateExchangeLocation(const su2double h_wm_new){
  h_wm = h_wm_new;
}

void CWallModelAPGLL::WallShearStressAndHeatFlux(const su2double tExchange,
                                                  const su2double velExchange,
                                                  const su2double muExchange,
                                                  const su2double pExchange,
                                                  const su2double dPds,
                                                  const su2double Wall_HeatFlux,
                                                  const bool      HeatFlux_Prescribed,
                                                  const su2double Wall_Temperature,
                                                  const bool      Temperature_Prescribed,
                                                  CFluidModel     *FluidModel,
                                                  su2double       &tauWall,
                                                  su2double       &qWall,
                                                  su2double       &ViscosityWall,
                                                  su2double       &kOverCvWall) {

  /* Set the wall temperature, depending whether or not the temperature
     was prescribed and initialize the fluid model. */
  const su2double TWall = Temperature_Prescribed ? Wall_Temperature : tExchange;

  FluidModel->SetTDState_PT(pExchange/Pref, TWall/Tref);

  /* Get the required data from the fluid model.
   */
  const su2double mu_wall  = FluidModel->GetLaminarViscosity() * Pref / Uref;;
  const su2double rho_wall = pExchange / (TWall * RGas);
  const su2double nu_wall  = mu_wall / rho_wall;

  /* Initial guess of the friction velocity. */
  su2double u_tau = max(0.01*velExchange, 1.e-5);

  /* Set parameters for control of the Newton iteration. */
  bool converged = false;
  unsigned short iter = 0, max_iter = 50;
  const su2double tol=1e-3;

  /* Van Driest damping constant */
  const su2double a_plus = 26.0;

  /* Tangential wall pressure gradient */
  const su2double u_p = pow(dPds * nu_wall / rho_wall, 1./3.);

  while (converged == false){

    iter += 1;
    if (iter == max_iter){
      converged = true;
    }

    const su2double u_tau0 = u_tau;
    const su2double y_plus = u_tau0*h_wm/nu_wall;

    /* Adverse Pressure Gradient Log Law (APGLL)
       fprime is the differentiation with repect to u_tau.
     */

    const su2double fval = + velExchange/u_tau0
                           - (1.0 - exp(-y_plus/a_plus))*(C + 2.0*(sqrt(1.0 + y_plus*pow(u_p/u_tau0, 3.0)) - 1.0)/karman
                           + 1.0*log(y_plus)/karman - 2.0*log(0.5*sqrt(1.0 +  y_plus*pow(u_p/u_tau0, 3.0)) + 0.5)/karman);


    // const su2double fprime = -velExchange/pow(u_tau0, 2) - (1.0 - exp(-y_plus/a_plus))*(1.0/(karman*u_tau0)
    //                          - 2.0*h_wm*pow(u_p/u_tau0, 3.0)*pow(1.0 + y_plus*pow(u_p/u_tau0, 3.0), -0.5)/(karman*nu_wall)
    //                          + 1.0*h_wm*pow(u_p/u_tau0, 3.0)*pow(1.0 + y_plus*pow(u_p/u_tau0, 3.0), -0.5)/(karman*nu_wall*(0.5*sqrt(1.0 + y_plus*pow(u_p/u_tau0, 3.0)) + 0.5)))
    //                          - h_wm*(C + 2.0*(sqrt(1.0 + y_plus*pow(u_p/u_tau0, 3.0)) - 1.0)/karman + 1.0*log(y_plus)/karman - 2.0*log(0.5*sqrt(1.0 + y_plus*pow(u_p/u_tau0, 3.0)) + 0.5)/karman)
    //                          *exp(-y_plus/a_plus)/(a_plus*nu_wall);

    const su2double fprime =-velExchange/pow(u_tau0, 2) - (1.0 - exp(-y_plus/a_plus))*(1.0/(karman*u_tau0));

    /* Newton method */
    const su2double newton_step = fval/fprime;
    u_tau = u_tau0 - newton_step;

    u_tau = max(u_tau,1e-10);

    /* Define a norm */
    if (fabs(1.0 - u_tau/u_tau0) < tol) converged = true;

    if (std::isnan(u_tau)){
      SU2_MPI::Error("u_tau is nan at APGLL Wall Model", CURRENT_FUNCTION);
    }
  }

  tauWall = rho_wall * pow(u_tau,2.0);

  if (Temperature_Prescribed){
    /* The Kader's law will be used to approximate the variations of the temperature inside the boundary layer.
     */
    const su2double y_plus = u_tau*h_wm/nu_wall;
    const su2double lhs = - ((tExchange - TWall) * rho_wall * Cp * u_tau);
    const su2double Gamma = - (0.01 * (pow(Pr_lam * y_plus,4.0))/(1.0 + 5.0*y_plus*pow(Pr_lam,3.0)));
    const su2double rhs_1 = Pr_lam * y_plus * exp(Gamma);
    const su2double rhs_2 = (2.12*log(1.0+y_plus) + pow((3.85*pow(Pr_lam,(1.0/3.0)) - 1.3),2.0) + 2.12*log(Pr_lam)) * exp(1./Gamma);
    qWall = lhs/(rhs_1 + rhs_2);
  }
  else{
    qWall = Wall_HeatFlux;
  }

  ViscosityWall = mu_wall;
  kOverCvWall   = (mu_wall*Cp/Pr_lam)/Cv;
}

CWallModelMixinglength::CWallModelMixinglength(CConfig      *config,
                                   const string &Marker_Tag)
  :  CWallModel(config) {

  C = 5.25; /* Constant to match the Reichardt BL profile ->  C = 4.1;  or 5.25. */
  a = 22.8673;
  b = 0.4649;
  k = 0.4086;
  m = 1.2569;
  n = 1.3972;

  // ----------------------Chebyshev Transform for integral computation-----------------------//
  const int N = 1024;
  Re_tau = 5200;
  Array theta(N+1);
  theta = Array::LinSpaced(N+1, 0, M_PI);
  Array X = theta.cos();
  Array Xi = Re_tau/2*(X+1);

  Array num = k*Xi*(1-(-(Xi/a).pow(m)).exp());
  Array dem = (1 + (Xi/(b*Re_tau)).pow(n)).pow(1/n);
  Array lambda = num/dem;
  Array f = -0.5/lambda.pow(2) + 0.5/lambda.pow(2)*(1 + 4*lambda.pow(2)*(1-Xi/Re_tau)).sqrt(); //might have Nan error during run
  f *= Re_tau/2;
  Eigen::VectorXd f_d = f.matrix();

  Eigen::MatrixXd D(N+1, N+1); 
  double ci, cj;
  for (int i = 0; i < N+1; ++i) {
    for (int j = 0; j < N+1; ++j) {
      if (i == j) {
        switch(i) {
          case 0:
            D(i,j) = (2.0*N*N + 1.0)/6;
            break;
          case N:
            D(i,j) = -(2.0*N*N + 1.0)/6;
            break;
          default:
            D(i,j) = -X(i)/(2.0*(1.0-X(i)*X(i)));
        }
      }
      else {
        if ((i == 0) || (i == N)) {
          ci = 2;
        }
        else {
          ci = 1;
        }
        if ((j == 0) || (j == N)) {
          cj = 2;
        }
        else {
          cj = 1;
        }
        D(i,j) = ci*std::pow(-1, i+j)/(cj*(X(i)-X(j)));
      }
    }
  }

  Eigen::VectorXd f_i = Eigen::VectorXd::Zero(N+1);
  f_i.head(N) = D.block(0,0,N,N).lu().solve(f_d.head(N));

  f_int = std::vector<su2double>{f_i.data(), f_i.data()+f_i.size()};
  y_wm = std::vector<su2double>{Xi.data(), Xi.data() + Xi.size()};

  sort(y_wm.begin(), y_wm.end());
  sort(f_int.begin(), f_int.end());
  // -----------------------------------------------------------------------------------------//

  /* Retrieve the floating point information for this boundary marker
     and set the exchange height. */
  const su2double *doubleInfo = config->GetWallFunction_DoubleInfo(Marker_Tag);
  h_wm = doubleInfo[0];

}

void CWallModelMixinglength::UpdateExchangeLocation(const su2double h_wm_new){
  h_wm = h_wm_new;
}

void CWallModelMixinglength::WallShearStressAndHeatFlux(const su2double tExchange,
                                                  const su2double velExchange,
                                                  const su2double muExchange,
                                                  const su2double pExchange,
                                                  const su2double dPds,
                                                  const su2double Wall_HeatFlux,
                                                  const bool      HeatFlux_Prescribed,
                                                  const su2double Wall_Temperature,
                                                  const bool      Temperature_Prescribed,
                                                  CFluidModel     *FluidModel,
                                                  su2double       &tauWall,
                                                  su2double       &qWall,
                                                  su2double       &ViscosityWall,
                                                  su2double       &kOverCvWall) {

  /* Set the wall temperature, depending whether or not the temperature
     was prescribed and initialize the fluid model. */
  const su2double TWall = Temperature_Prescribed ? Wall_Temperature : tExchange;

  FluidModel->SetTDState_PT(pExchange/Pref, TWall/Tref);

  /* Get the required data from the fluid model.
   */
  const su2double mu_wall  = FluidModel->GetLaminarViscosity() * Pref / Uref;;
  const su2double rho_wall = pExchange / (TWall * RGas);
  const su2double nu_wall  = mu_wall / rho_wall;

  /* Initial guess of the friction velocity. */
  su2double u_tau = max(0.01*velExchange, 1.e-5);

  /* Set parameters for control of the Newton iteration. */
  bool converged = false;
  unsigned short iter = 0, max_iter = 50;
  const su2double tol=1e-3;

  while (converged == false){

    iter += 1;
    if (iter == max_iter) converged = true;

    const su2double u_tau0 = u_tau;
    const su2double y_plus = u_tau0*h_wm/nu_wall;

    /* Reichardt boundary layer analytical law
       fprime is the differentiation of the Reichardt law with repect to u_tau.
     */
    auto upper = upper_bound(y_wm.begin(), y_wm.end(), y_plus);
    int index = distance(y_wm.begin(), upper);
    const su2double fval = velExchange/u_tau0 - (f_int[index-1] + (y_plus - y_wm[index-1])*(f_int[index] - f_int[index-1])/(y_wm[index] - y_wm[index-1])); 
    const su2double lambda = k*y_plus*(1-exp(-pow(y_plus/a, m)))/pow(1 + pow(y_plus/(b*Re_tau), n), 1.0/n);
    const su2double f = -0.5/pow(lambda,2) + 0.5/pow(lambda, 2)*sqrt(1 + 4*pow(lambda,2)*(1-y_plus/Re_tau));
    const su2double fprime = -velExchange/pow(u_tau0,2.0) - f*h_wm/nu_wall;

    /* Newton method */
    const su2double newton_step = fval/fprime;
    u_tau = u_tau0 - newton_step;

    /* Define a norm */
    if (fabs(1.0 - u_tau/u_tau0) < tol) converged = true;
  }

  tauWall = rho_wall * pow(u_tau,2.0);

  if (Temperature_Prescribed){
    /* The Kader's law will be used to approximate the variations of the temperature inside the boundary layer.
     */
    const su2double y_plus = u_tau*h_wm/nu_wall;
    const su2double lhs = - ((tExchange - TWall) * rho_wall * Cp * u_tau);
    const su2double Gamma = - (0.01 * (pow(Pr_lam * y_plus,4.0))/(1.0 + 5.0*y_plus*pow(Pr_lam,3.0)));
    const su2double rhs_1 = Pr_lam * y_plus * exp(Gamma);
    const su2double rhs_2 = (2.12*log(1.0+y_plus) + pow((3.85*pow(Pr_lam,(1.0/3.0)) - 1.3),2.0) + 2.12*log(Pr_lam)) * exp(1./Gamma);
    qWall = lhs/(rhs_1 + rhs_2);
  }
  else{
    qWall = Wall_HeatFlux;
  }

  ViscosityWall = mu_wall;
  kOverCvWall   = (mu_wall*Cp/Pr_lam)/Cv;
}