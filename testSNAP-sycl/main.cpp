// ----------------------------------------------------------------------
// Copyright (2019) Sandia Corporation.
// Under the terms of Contract DE-AC04-94AL85000
// with Sandia Corporation, the U.S. Government
// retains certain rights in this software. This
// software is distributed under the Zero Clause
// BSD License
//
// TestSNAP - A prototype for the SNAP force kernel
// Version 0.0.2
// Main changes: Y array trick, memory compaction
//
// Original author: Aidan P. Thompson, athomps@sandia.gov
// http://www.cs.sandia.gov/~athomps, Sandia National Laboratories
//
// ----------------------------------------------------------------------
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sycl/sycl.hpp>
#include "snap.h"
#include "utils.cpp"

#if REFDATA_TWOJ == 14
#include "refdata_2J14_W.h"
#elif REFDATA_TWOJ == 8
#include "refdata_2J8_W.h"
#elif REFDATA_TWOJ == 4
#include "refdata_2J4_W.h"
#else
#include "refdata_2J2_W.h"
#endif

int nsteps = 1; // num of force evaluations

// Copyright (C) 2018 - 2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// See https://llvm.org/LICENSE.txt for license information.

/// Atomically add the value operand to the value at the addr and assign the
/// result to the value at addr, Float version.
/// \param [in, out] addr The pointer to the data.
/// \param operand The value to add to the value at \p addr.
/// \param memoryOrder The memory ordering used.
/// \returns The value at the \p addr before the call.
inline double atomicAdd( double *addr, double operand )
{
  sycl::atomic_ref<long, sycl::memory_order::relaxed, sycl::memory_scope::device, sycl::access::address_space::global_space> obj(
    (*reinterpret_cast<long *>(addr)));

  long old_value;
  double old_double_value;

  do {
    old_value = obj.load(sycl::memory_order::relaxed);
    old_double_value = *reinterpret_cast<const double *>(&old_value);
    const double new_double_value = old_double_value + operand;
    const long new_value = *reinterpret_cast<const long *>(&new_double_value);
    if (obj.compare_exchange_strong(old_value, new_value))
      break;
  } while (true);

  return old_double_value;
}


int main(int argc, char* argv[])
{
  options(argc, argv);

  const int switch_flag = 1;     // SNAP parameter

  // record timings of individual routines
  double elapsed_ui = 0.0,
         elapsed_yi = 0.0,
         elapsed_duidrj = 0.0,
         elapsed_deidrj = 0.0;

  const int ninside = refdata.ninside;
  const int ncoeff = refdata.ncoeff;
  const int nlocal = refdata.nlocal;
  const int nghost = refdata.nghost;
  const int ntotal = nlocal + nghost;
  const int twojmax = refdata.twojmax;
  const double rcutfac = refdata.rcutfac;

  const double wself = 1.0;
  const int num_atoms = nlocal;
  const int num_nbor = ninside;

  //coeffi = memory->grow(coeffi, ncoeff + 1, "coeffi");
  double* coeffi = (double*) malloc (sizeof(double) * (ncoeff+1));

  for (int icoeff = 0; icoeff < ncoeff + 1; icoeff++)
    coeffi[icoeff] = refdata.coeff[icoeff];

  double* beta = coeffi + 1;

  // build index list
  const int jdim = twojmax + 1;

  // index list for cglist

  int *idxcg_block = (int*) malloc(sizeof(int) * jdim * jdim * jdim);

  int idxcg_count = 0;
  for (int j1 = 0; j1 <= twojmax; j1++)
    for (int j2 = 0; j2 <= j1; j2++)
      for (int j = abs(j1 - j2); j <= MIN(twojmax, j1 + j2); j += 2) {
        idxcg_block[j1 + j2 *jdim + jdim*jdim*j] = idxcg_count;
        for (int m1 = 0; m1 <= j1; m1++)
          for (int m2 = 0; m2 <= j2; m2++)
            idxcg_count++;
      }
  const int idxcg_max = idxcg_count;

  // index list for uarray
  // need to include both halves
  // **** only place rightside is used is in compute_yi() ***

  int* idxu_block = (int*) malloc (sizeof(int) * jdim);
  int idxu_count = 0;

  for (int j = 0; j <= twojmax; j++) {
    idxu_block[j] = idxu_count;
    for (int mb = 0; mb <= j; mb++)
      for (int ma = 0; ma <= j; ma++)
        idxu_count++;
  }
  const int idxu_max = idxu_count;

  // parity list for uarray inversion symmetry
  // parity +1: u[ma-j][mb-j] = +Conj([u[ma][mb])
  // parity -1: u[ma-j][mb-j] = -Conj([u[ma][mb])

  // ulist_parity.resize(idxu_max);
  int* ulist_parity = (int*) malloc (sizeof(int) * idxu_max);
  idxu_count = 0;
  for (int j = 0; j <= twojmax; j++) {
    int mbpar = 1;
    for (int mb = 0; mb <= j; mb++) {
      int mapar = mbpar;
      for (int ma = 0; ma <= j; ma++) {
        ulist_parity[idxu_count] = mapar;
        mapar = -mapar;
        idxu_count++;
      }
      mbpar = -mbpar;
    }
  }

  // index list for duarray, yarray
  // only include left half
  // NOTE: idxdu indicates lefthalf only
  //       idxu indicates both halves

  int* idxdu_block = (int*) malloc (sizeof(int) * jdim);
  int idxdu_count = 0;

  for (int j = 0; j <= twojmax; j++) {
    idxdu_block[j] = idxdu_count;
    for (int mb = 0; 2 * mb <= j; mb++)
      for (int ma = 0; ma <= j; ma++)
        idxdu_count++;
  }
  const int idxdu_max = idxdu_count;

  // index list for beta and B

  int idxb_count = 0;
  for (int j1 = 0; j1 <= twojmax; j1++)
    for (int j2 = 0; j2 <= j1; j2++)
      for (int j = abs(j1 - j2); j <= MIN(twojmax, j1 + j2); j += 2)
        if (j >= j1)
          idxb_count++;

  const int idxb_max = idxb_count;
  SNA_BINDICES* idxb = (SNA_BINDICES*) malloc (sizeof(SNA_BINDICES) * idxb_max);

  idxb_count = 0;
  for (int j1 = 0; j1 <= twojmax; j1++)
    for (int j2 = 0; j2 <= j1; j2++)
      for (int j = abs(j1 - j2); j <= MIN(twojmax, j1 + j2); j += 2)
        if (j >= j1) {
          idxb[idxb_count].j1 = j1;
          idxb[idxb_count].j2 = j2;
          idxb[idxb_count].j = j;
          idxb_count++;
        }

  // reverse index list for beta and b

  int* idxb_block = (int*) malloc (sizeof(int) * jdim * jdim * jdim);
  idxb_count = 0;
  for (int j1 = 0; j1 <= twojmax; j1++)
    for (int j2 = 0; j2 <= j1; j2++)
      for (int j = abs(j1 - j2); j <= MIN(twojmax, j1 + j2); j += 2) {
        if (j < j1)
          continue;
        idxb_block[j1*jdim*jdim+j2*jdim+j] = idxb_count;
        idxb_count++;
      }


  // index list for zlist

  int idxz_count = 0;

  for (int j1 = 0; j1 <= twojmax; j1++)
    for (int j2 = 0; j2 <= j1; j2++)
      for (int j = abs(j1 - j2); j <= MIN(twojmax, j1 + j2); j += 2)
        for (int mb = 0; 2 * mb <= j; mb++)
          for (int ma = 0; ma <= j; ma++)
            idxz_count++;

  const int idxz_max = idxz_count;
  //idxz.resize(idxz_max, 9);
  int* idxz = (int*) malloc (sizeof(int) * idxz_max * 9);

  //idxzbeta.resize(idxz_max);
  double* idxzbeta = (double*) malloc (sizeof(double) * idxz_max);

  //memory->create(idxz_block, jdim, jdim, jdim, "sna:idxz_block");
  int* idxz_block = (int*) malloc (sizeof(int) * jdim * jdim * jdim);

  idxz_count = 0;
  for (int j1 = 0; j1 <= twojmax; j1++)
    for (int j2 = 0; j2 <= j1; j2++)
      for (int j = abs(j1 - j2); j <= MIN(twojmax, j1 + j2); j += 2) {
        idxz_block[j1*jdim*jdim+j2*jdim+j] = idxz_count;

        // find right beta[jjb] entries
        // multiply and divide by j+1 factors
        // account for multiplicity of 1, 2, or 3
        // this should not be computed here

        double betaj;
        if (j >= j1) {
          const int jjb = idxb_block[j1*jdim*jdim+j2*jdim+j];
          if (j1 == j) {
            if (j2 == j) {
              betaj = 3 * beta[jjb];
            }
            else {
              betaj = 2 * beta[jjb];
            }
          } else {
            betaj = beta[jjb];
          }
        } else if (j >= j2) {
          const int jjb = idxb_block[j*jdim*jdim+j2*jdim+j1];
          if (j2 == j) {
            betaj = 2 * beta[jjb] * (j1 + 1) / (j + 1.0);
          }
          else {
            betaj = beta[jjb] * (j1 + 1) / (j + 1.0);
          }
        } else {
          const int jjb = idxb_block[j2*jdim*jdim+j*jdim+j1];
          betaj = beta[jjb] * (j1 + 1) / (j + 1.0);
        }

        for (int mb = 0; 2 * mb <= j; mb++)
          for (int ma = 0; ma <= j; ma++) {

            idxz[IDXZ_INDEX(idxz_count, 0)] = j1;
            idxz[IDXZ_INDEX(idxz_count, 1)] = j2;
            idxz[IDXZ_INDEX(idxz_count, 2)] = j;

            int ma1min = MAX(0, (2 * ma - j - j2 + j1) / 2);
            idxz[IDXZ_INDEX(idxz_count, 3)] = ma1min;
            idxz[IDXZ_INDEX(idxz_count, 4)] = (2 * ma - j - (2 * ma1min - j1) + j2) / 2;
            idxz[IDXZ_INDEX(idxz_count, 5)] =
              MIN(j1, (2 * ma - j + j2 + j1) / 2) - ma1min + 1;

            int mb1min = MAX(0, (2 * mb - j - j2 + j1) / 2);
            idxz[IDXZ_INDEX(idxz_count, 6)] = mb1min;
            idxz[IDXZ_INDEX(idxz_count, 7)] = (2 * mb - j - (2 * mb1min - j1) + j2) / 2;
            idxz[IDXZ_INDEX(idxz_count, 8)] =
              MIN(j1, (2 * mb - j + j2 + j1) / 2) - mb1min + 1;

            idxzbeta[idxz_count] = betaj;

            idxz_count++;
          }
      }
  // omit beta0 from beta vector


  if (compute_ncoeff(twojmax) != ncoeff) {
    printf("ERROR: ncoeff from SNA does not match reference data\n");
    exit(1);
  }

  //snaptr->grow_rij(ninside);

  double *rij    = (double*) malloc(sizeof(double) * (num_atoms * num_nbor * 3));
  double *inside = (double*) malloc(sizeof(double) * (num_atoms * num_nbor));
  double *wj     = (double*) malloc(sizeof(double) * (num_atoms * num_nbor));
  double *rcutij = (double*) malloc(sizeof(double) * (num_atoms * num_nbor));

  const int jdimpq = twojmax + 2;
  double* rootpqarray = (double*) malloc(sizeof(double) * jdimpq * jdimpq);
  double* cglist = (double*) malloc (sizeof(double) * idxcg_max);
  double* dedr = (double*) malloc (sizeof(double) * num_atoms * num_nbor * 3);

  COMPLEX* ulist = (COMPLEX*) malloc (sizeof(COMPLEX) * num_atoms * num_nbor * idxu_max);
  COMPLEX* ylist = (COMPLEX*) malloc (sizeof(COMPLEX) * num_atoms * idxdu_max);
  COMPLEX* ulisttot = (COMPLEX*) malloc (sizeof(COMPLEX) * num_atoms * idxu_max);
  COMPLEX* dulist = (COMPLEX*) malloc (sizeof(COMPLEX) * num_atoms * num_nbor * 3 * idxdu_max);

  // init rootpqarray
  for (int p = 1; p <= twojmax; p++)
    for (int q = 1; q <= twojmax; q++)
      rootpqarray[ROOTPQ_INDEX(p, q)] = sqrt(static_cast<double>(p) / q);

  // init_clebsch_gordan()
  double sum, dcg, sfaccg;
  int m, aa2, bb2, cc2;
  int ifac;

  idxcg_count = 0;
  for (int j1 = 0; j1 <= twojmax; j1++)
    for (int j2 = 0; j2 <= j1; j2++)
      for (int j = abs(j1 - j2); j <= MIN(twojmax, j1 + j2); j += 2) {
        for (int m1 = 0; m1 <= j1; m1++) {
          aa2 = 2 * m1 - j1;

          for (int m2 = 0; m2 <= j2; m2++) {

            // -c <= cc <= c

            bb2 = 2 * m2 - j2;
            m = (aa2 + bb2 + j) / 2;

            if (m < 0 || m > j) {
              cglist[idxcg_count] = 0.0;
              idxcg_count++;
              continue;
            }

            sum = 0.0;

            for (int z = MAX(0, MAX(-(j - j2 + aa2) / 2, -(j - j1 - bb2) / 2));
                z <=
                MIN((j1 + j2 - j) / 2, MIN((j1 - aa2) / 2, (j2 + bb2) / 2));
                z++) {
              ifac = z % 2 ? -1 : 1;
              sum += ifac / (factorial(z) * factorial((j1 + j2 - j) / 2 - z) *
                  factorial((j1 - aa2) / 2 - z) *
                  factorial((j2 + bb2) / 2 - z) *
                  factorial((j - j2 + aa2) / 2 + z) *
                  factorial((j - j1 - bb2) / 2 + z));
            }

            cc2 = 2 * m - j;
            dcg = deltacg(j1, j2, j);
            sfaccg = sqrt(
                factorial((j1 + aa2) / 2) * factorial((j1 - aa2) / 2) *
                factorial((j2 + bb2) / 2) * factorial((j2 - bb2) / 2) *
                factorial((j + cc2) / 2) * factorial((j - cc2) / 2) * (j + 1));

            cglist[idxcg_count] = sum * dcg * sfaccg;
            idxcg_count++;
          }
        }
      }

  double* f = (double*) malloc (sizeof(double) * ntotal * 3);

  // initialize error tally
  double sumsqferr = 0.0;

#ifdef USE_GPU
  sycl::queue q(sycl::gpu_selector_v);
#else
  sycl::queue q(sycl::cpu_selector_v);
#endif

  sycl::buffer<int, 1> d_idxu_block(idxu_block, jdim);
  sycl::buffer<int, 1> d_ulist_parity(ulist_parity, idxu_max);
  sycl::buffer<double, 1> d_rootpqarray(rootpqarray, jdimpq * jdimpq);
  sycl::buffer<int, 1> d_idxz(idxz, idxz_max * 9);
  sycl::buffer<double, 1> d_idxzbeta(idxzbeta, idxz_max);
  sycl::buffer<int, 1> d_idxcg_block(idxcg_block, jdim * jdim * jdim);
  sycl::buffer<int, 1> d_idxdu_block(idxdu_block, jdim);
  sycl::buffer<double, 1> d_cglist(cglist, idxcg_max);

  sycl::buffer<COMPLEX, 1> d_dulist(dulist, num_atoms * num_nbor * 3 * idxdu_max);
  sycl::buffer<COMPLEX, 1> d_ulist(ulist, num_atoms * num_nbor * idxu_max);
  sycl::buffer<double, 1> d_dedr(dedr, num_atoms * num_nbor * 3);

  d_ulist.set_final_data(nullptr);
  d_dulist.set_final_data(nullptr);
  d_dedr.set_final_data(nullptr);

  sycl::buffer<COMPLEX, 1> d_ulisttot(num_atoms * idxu_max);
  sycl::buffer<COMPLEX, 1> d_ylist(num_atoms * idxdu_max);

  sycl::buffer<double, 1> d_rij(num_atoms*num_nbor*3);
  sycl::buffer<double, 1> d_rcutij(num_atoms*num_nbor);
  sycl::buffer<double, 1> d_wj(num_atoms*num_nbor);

  // loop over steps

  auto begin = myclock::now();
  for (int istep = 0; istep < nsteps; istep++) {

    time_point<system_clock> start, end;
    duration<double> elapsed;

    for (int j = 0; j < ntotal * 3; j++) {
      f[j] = 0.0;
    }

    int jt = 0, jjt = 0;
    for (int natom = 0; natom < num_atoms; natom++) {
      for (int nbor = 0; nbor < num_nbor; nbor++) {
        rij[ULIST_INDEX(natom, nbor, 0)] = refdata.rij[jt++];
        rij[ULIST_INDEX(natom, nbor, 1)] = refdata.rij[jt++];
        rij[ULIST_INDEX(natom, nbor, 2)] = refdata.rij[jt++];
        inside[INDEX_2D(natom, nbor)] = refdata.jlist[jjt++];
        wj[INDEX_2D(natom, nbor)] = 1.0;
        rcutij[INDEX_2D(natom, nbor)] = rcutfac;
      }
    }

    q.submit([&] (sycl::handler &cgh) {
      auto acc = d_rij.get_access<sycl::access::mode::discard_write>(cgh);
      cgh.copy(rij, acc);
    });

    q.submit([&] (sycl::handler &cgh) {
      auto acc = d_rcutij.get_access<sycl::access::mode::discard_write>(cgh);
      cgh.copy(rcutij, acc);
    });

    q.submit([&] (sycl::handler &cgh) {
      auto acc = d_wj.get_access<sycl::access::mode::discard_write>(cgh);
      cgh.copy(wj, acc);
    });

    // compute_ui
    start = system_clock::now();
    // compute_ui();
    // utot(j,ma,mb) = 0 for all j,ma,ma
    // utot(j,ma,ma) = 1 for all j,ma
    // for j in neighbors of i:
    //   compute r0 = (x,y,z,z0)
    //   utot(j,ma,mb) += u(r0;j,ma,mb) for all j,ma,mb

    sycl::range<1> gws_k1 ((num_atoms*idxu_max+255)/256*256);
    sycl::range<1> lws_k1 (256);
    q.submit([&] (sycl::handler &cgh) {
      auto acc = d_ulisttot.get_access<sycl::access::mode::write>(cgh);
      cgh.parallel_for<class reset_ulisttot>(sycl::nd_range<1>(gws_k1, lws_k1), [=] (sycl::nd_item<1> item) {
        int i = item.get_global_id(0);
        if (i < num_atoms*idxu_max) acc[i] = {0.0, 0.0};
      });
    });

    sycl::range<1> gws_k2 ((num_atoms+255)/256*256);
    sycl::range<1> lws_k2 (256);
    q.submit([&] (sycl::handler &cgh) {
      auto ulisttot = d_ulisttot.get_access<sycl::access::mode::write>(cgh);
      auto idxu_block = d_idxu_block.get_access<sycl::access::mode::read>(cgh);
      cgh.parallel_for<class set_ulisttot>(sycl::nd_range<1>(gws_k2, lws_k2), [=] (sycl::nd_item<1> item) {
        int natom = item.get_global_id(0);
        if (natom < num_atoms)
          for (int j = 0; j <= twojmax; j++) {
            int jju = idxu_block[j];
            for (int ma = 0; ma <= j; ma++) {
              ulisttot[INDEX_2D(natom, jju)] = { wself, 0.0 };
              jju += j + 2;
            }
          }
      });
    });

    sycl::range<2> gws_k3 ((num_nbor+15)/16*16, (num_atoms+15)/16*16);
    sycl::range<2> lws_k3 (16, 16);

    q.submit([&] (sycl::handler &cgh) {
      auto wj = d_wj.get_access<sycl::access::mode::read>(cgh);
      auto rij = d_rij.get_access<sycl::access::mode::read>(cgh);
      auto rcutij = d_rcutij.get_access<sycl::access::mode::read>(cgh);
      auto ulist_parity = d_ulist_parity.get_access<sycl::access::mode::read>(cgh);
      auto idxu_block = d_idxu_block.get_access<sycl::access::mode::read>(cgh);
      auto rootpqarray = d_rootpqarray.get_access<sycl::access::mode::read>(cgh);
      auto ulist = d_ulist.get_access<sycl::access::mode::read_write>(cgh);
      auto ulisttot = d_ulisttot.get_access<sycl::access::mode::read_write>(cgh);

      cgh.parallel_for<class update_ulisttot>(sycl::nd_range<2>(gws_k3, lws_k3), [=] (sycl::nd_item<2> item) {
	int nbor = item.get_global_id(0);
	int natom = item.get_global_id(1);
        if (natom < num_atoms && nbor < num_nbor) {
          double x = rij[ULIST_INDEX(natom, nbor, 0)];
          double y = rij[ULIST_INDEX(natom, nbor, 1)];
          double z = rij[ULIST_INDEX(natom, nbor, 2)];
          double rsq = x * x + y * y + z * z;
          double r = sycl::sqrt(rsq);

          double theta0 = (r - rmin0) * rfac0 * MY_PI / (rcutij[INDEX_2D(natom, nbor)] - rmin0);
          double z0 = r / sycl::tan(theta0);

          double rootpq;
          int jju, jjup;

          // compute Cayley-Klein parameters for unit quaternion

          double r0inv = 1.0 / sycl::sqrt(r * r + z0 * z0);
          double a_r = r0inv * z0;
          double a_i = -r0inv * z;
          double b_r = r0inv * y;
          double b_i = -r0inv * x;

          double sfac;

          sfac = compute_sfac(r, rcutij[INDEX_2D(natom, nbor)], switch_flag);
          sfac *= wj[INDEX_2D(natom, nbor)];

          // Recursion relations
          // VMK Section 4.8.2

          //   u[j,ma,mb] = Sqrt((j-ma)/(j-mb)) a* u[j-1,ma,mb]
          //               -Sqrt((ma)/(j-mb)) b* u[j-1,ma-1,mb]

          //   u[j,ma,mb] = Sqrt((j-ma)/(mb)) b u[j-1,ma,mb-1]
          //                Sqrt((ma)/(mb)) a u[j-1,ma-1,mb-1]

          // initialize first entry
          // initialize top row of each layer to zero
          ulist[ULIST_INDEX(natom, nbor, 0)].re = 1.0;
          ulist[ULIST_INDEX(natom, nbor, 0)].im = 0.0;

          // skip over right half of each uarray
          jju = 1;
          for (int j = 1; j <= twojmax; j++) {
            int deljju = j + 1;
            for (int mb = 0; 2 * mb <= j; mb++) {
              ulist[ULIST_INDEX(natom, nbor, jju)].re = 0.0;
              ulist[ULIST_INDEX(natom, nbor, jju)].im = 0.0;
              jju += deljju;
            }
            int ncolhalf = deljju / 2;
            jju += deljju * ncolhalf;
          }

          jju = 1;
          jjup = 0;
          for (int j = 1; j <= twojmax; j++) {
            int deljju = j + 1;
            int deljjup = j;
            int mb_max = (j + 1) / 2;
            int ma_max = j;
            int m_max = ma_max * mb_max;

            // fill in left side of matrix layer from previous layer
            for (int m_iter = 0; m_iter < m_max; ++m_iter) {
              int mb = m_iter / ma_max;
              int ma = m_iter % ma_max;
              double up_r = ulist[ULIST_INDEX(natom, nbor, jjup)].re;
              double up_i = ulist[ULIST_INDEX(natom, nbor, jjup)].im;

              rootpq = rootpqarray[ROOTPQ_INDEX(j - ma, j - mb)];
              ulist[ULIST_INDEX(natom, nbor, jju)].re += rootpq * (a_r * up_r + a_i * up_i);
              ulist[ULIST_INDEX(natom, nbor, jju)].im += rootpq * (a_r * up_i - a_i * up_r);

              rootpq = rootpqarray[ROOTPQ_INDEX(ma + 1, j - mb)];
              ulist[ULIST_INDEX(natom, nbor, jju+1)].re = -rootpq * (b_r * up_r + b_i * up_i);
              ulist[ULIST_INDEX(natom, nbor, jju+1)].im = -rootpq * (b_r * up_i - b_i * up_r);

              // assign middle column i.e. mb+1

              if (2 * (mb + 1) == j) {
                rootpq = rootpqarray[ROOTPQ_INDEX(j - ma, mb + 1)];
                ulist[ULIST_INDEX(natom, nbor, jju+deljju)].re += rootpq * (b_r * up_r - b_i * up_i);
                ulist[ULIST_INDEX(natom, nbor, jju+deljju)].im += rootpq * (b_r * up_i + b_i * up_r);

                rootpq = rootpqarray[ROOTPQ_INDEX(ma + 1, mb + 1)];
                ulist[ULIST_INDEX(natom, nbor, jju+deljju+1)].re = rootpq * (a_r * up_r - a_i * up_i);
                ulist[ULIST_INDEX(natom, nbor, jju+deljju+1)].im = rootpq * (a_r * up_i + a_i * up_r);
              }

              jju++;
              jjup++;

              if (ma == ma_max - 1)
                jju++;
            }

            // copy left side to right side with inversion symmetry VMK 4.4(2)
            // u[ma-j][mb-j] = (-1)^(ma-mb)*Conj([u[ma][mb])
            // dependence on idxu_block could be removed
            // renamed counters b/c can not modify jju, jjup
            int jjui = idxu_block[j];
            int jjuip = jjui + (j + 1) * (j + 1) - 1;
            for (int mb = 0; 2 * mb < j; mb++) {
              for (int ma = 0; ma <= j; ma++) {
                ulist[ULIST_INDEX(natom, nbor, jjuip)].re = ulist_parity[jjui] * ulist[ULIST_INDEX(natom, nbor, jjui)].re;
                ulist[ULIST_INDEX(natom, nbor, jjuip)].im = ulist_parity[jjui] * -ulist[ULIST_INDEX(natom, nbor, jjui)].im;
                jjui++;
                jjuip--;
              }
            }

            // skip middle and right half cols
            // b/c no longer using idxu_block
            if (j % 2 == 0)
              jju += deljju;
            int ncolhalf = deljju / 2;
            jju += deljju * ncolhalf;
            int ncolhalfp = deljjup / 2;
            jjup += deljjup * ncolhalfp;
          }

          sfac = compute_sfac(r, rcutij[INDEX_2D(natom, nbor)], switch_flag);
          sfac *= wj[INDEX_2D(natom, nbor)];

          for (int j = 0; j <= twojmax; j++) {
            int jju = idxu_block[j];
            for (int mb = 0; mb <= j; mb++)
              for (int ma = 0; ma <= j; ma++) {
                atomicAdd(&(ulisttot[INDEX_2D(natom, jju)].re), sfac * ulist[ULIST_INDEX(natom, nbor, jju)].re);
                atomicAdd(&(ulisttot[INDEX_2D(natom, jju)].im), sfac * ulist[ULIST_INDEX(natom, nbor, jju)].im);
                jju++;
              }
          }
        }
      });
    });

    q.wait();
    end = system_clock::now();
    elapsed = end - start;
    elapsed_ui += elapsed.count();

    start = system_clock::now();

    //compute_yi(beta);

    // Initialize ylist elements to zeros
    sycl::range<1> gws_k4 ((num_atoms*idxdu_max+255)/256*256);
    sycl::range<1> lws_k4 (256);
    q.submit([&] (sycl::handler &cgh) {
      auto acc = d_ylist.get_access<sycl::access::mode::write>(cgh);
      cgh.parallel_for<class reset_ylist>(sycl::nd_range<1>(gws_k4, lws_k4), [=] (sycl::nd_item<1> item) {
        int i = item.get_global_id(0);
        if (i < num_atoms*idxdu_max) acc[i] = {0.0, 0.0};
      });
    });

    sycl::range<2> gws_k5 ((idxz_max+15)/16*16, (num_atoms+15)/16*16);
    sycl::range<2> lws_k5 (16, 16);

    q.submit([&] (sycl::handler &cgh) {
      auto idxz = d_idxz.get_access<sycl::access::mode::read>(cgh);
      auto idxzbeta = d_idxzbeta.get_access<sycl::access::mode::read>(cgh);
      auto idxcg_block = d_idxcg_block.get_access<sycl::access::mode::read>(cgh);
      auto cglist = d_cglist.get_access<sycl::access::mode::read>(cgh);
      auto idxu_block = d_idxu_block.get_access<sycl::access::mode::read>(cgh);
      auto idxdu_block = d_idxdu_block.get_access<sycl::access::mode::read>(cgh);
      auto ulisttot = d_ulisttot.get_access<sycl::access::mode::read>(cgh);
      auto ylist = d_ylist.get_access<sycl::access::mode::read_write>(cgh);

      cgh.parallel_for<class compute_yi>(sycl::nd_range<2>(gws_k5, lws_k5), [=] (sycl::nd_item<2> item) {
        int jjz = item.get_global_id(0);
        int natom = item.get_global_id(1);
        if (jjz < idxz_max && natom < num_atoms) {
          const int j1 = idxz[IDXZ_INDEX(jjz, 0)];
          const int j2 = idxz[IDXZ_INDEX(jjz, 1)];
          const int j = idxz[IDXZ_INDEX(jjz, 2)];
          const int ma1min = idxz[IDXZ_INDEX(jjz, 3)];
          const int ma2max = idxz[IDXZ_INDEX(jjz, 4)];
          const int na = idxz[IDXZ_INDEX(jjz, 5)];
          const int mb1min = idxz[IDXZ_INDEX(jjz, 6)];
          const int mb2max = idxz[IDXZ_INDEX(jjz, 7)];
          const int nb = idxz[IDXZ_INDEX(jjz, 8)];

          const double betaj = idxzbeta[jjz];

          //const double* cgblock = cglist.dptr + idxcg_block(j1, j2, j);
          const double* cgblock = cglist.get_pointer() + idxcg_block[j1 + jdim*j2 + jdim*jdim*j];

          int mb = (2 * (mb1min + mb2max) - j1 - j2 + j) / 2;
          int ma = (2 * (ma1min + ma2max) - j1 - j2 + j) / 2;
          const int jjdu = idxdu_block[j] + (j + 1) * mb + ma;

          int jju1 = idxu_block[j1] + (j1 + 1) * mb1min;
          int jju2 = idxu_block[j2] + (j2 + 1) * mb2max;
          int icgb = mb1min * (j2 + 1) + mb2max;

          double ztmp_r = 0.0;
          double ztmp_i = 0.0;

          // loop over columns of u1 and corresponding
          // columns of u2 satisfying Clebsch-Gordan constraint
          //      2*mb-j = 2*mb1-j1 + 2*mb2-j2

          for (int ib = 0; ib < nb; ib++) {

            double suma1_r = 0.0;
            double suma1_i = 0.0;

            int ma1 = ma1min;
            int ma2 = ma2max;
            int icga = ma1min * (j2 + 1) + ma2max;

            // loop over elements of row u1[mb1] and corresponding elements
            // of row u2[mb2] satisfying Clebsch-Gordan constraint
            //      2*ma-j = 2*ma1-j1 + 2*ma2-j2

            for (int ia = 0; ia < na; ia++) {
              suma1_r += cgblock[icga] *
                (ulisttot[INDEX_2D(natom, jju1 + ma1)].re * ulisttot[INDEX_2D(natom, jju2 + ma2)].re -
                 ulisttot[INDEX_2D(natom, jju1 + ma1)].im * ulisttot[INDEX_2D(natom, jju2 + ma2)].im);

              suma1_i += cgblock[icga] *
                (ulisttot[INDEX_2D(natom, jju1 + ma1)].re * ulisttot[INDEX_2D(natom, jju2 + ma2)].im +
                 ulisttot[INDEX_2D(natom, jju1 + ma1)].im * ulisttot[INDEX_2D(natom, jju2 + ma2)].re);

              ma1++;
              ma2--;
              icga += j2;
           } // end loop over ia

           ztmp_r += cgblock[icgb] * suma1_r;
           ztmp_i += cgblock[icgb] * suma1_i;
           jju1 += j1 + 1;
           jju2 -= j2 + 1;
           icgb += j2;
          } // end loop over ib

            // apply z(j1,j2,j,ma,mb) to unique element of y(j)

          atomicAdd(&(ylist[INDEX_2D(natom, jjdu)].re), betaj * ztmp_r);
          atomicAdd(&(ylist[INDEX_2D(natom, jjdu)].im), betaj * ztmp_i);

        } // end jjz and natom loop
      });
    });

    q.wait();
    end = system_clock::now();
    elapsed = end - start;
    elapsed_yi += elapsed.count();

    // compute_duidrj
    start = system_clock::now();

    sycl::range<2> gws_k6 ((num_nbor+15)/16*16, (num_atoms+15)/16*16);
    sycl::range<2> lws_k6 (16, 16);

    q.submit([&] (sycl::handler &cgh) {
      auto wj = d_wj.get_access<sycl::access::mode::read>(cgh);
      auto rij = d_rij.get_access<sycl::access::mode::read>(cgh);
      auto rcutij = d_rcutij.get_access<sycl::access::mode::read>(cgh);
      auto rootpqarray = d_rootpqarray.get_access<sycl::access::mode::read>(cgh);
      auto ulist = d_ulist.get_access<sycl::access::mode::read>(cgh);
      auto dulist = d_dulist.get_access<sycl::access::mode::read_write>(cgh);

      cgh.parallel_for<class compute_duidrj>(sycl::nd_range<2>(gws_k6, lws_k6), [=] (sycl::nd_item<2> item) {
	int nbor = item.get_global_id(0);
	int natom = item.get_global_id(1);
        if (natom < num_atoms && nbor < num_nbor) {
          double wj_in = wj[INDEX_2D(natom, nbor)];
          double rcut = rcutij[INDEX_2D(natom, nbor)];

          double x = rij[ULIST_INDEX(natom, nbor, 0)];
          double y = rij[ULIST_INDEX(natom, nbor, 1)];
          double z = rij[ULIST_INDEX(natom, nbor, 2)];
          double rsq = x * x + y * y + z * z;
          double r = sycl::sqrt(rsq);
          double rscale0 = rfac0 * MY_PI / (rcut - rmin0);
          double theta0 = (r - rmin0) * rscale0;
          double cs = sycl::cos(theta0);
          double sn = sycl::sin(theta0);
          double z0 = r * cs / sn;
          double dz0dr = z0 / r - (r * rscale0) * (rsq + z0 * z0) / rsq;

          compute_duarray(natom, nbor, num_atoms, num_nbor, twojmax,
                          idxdu_max, jdimpq, switch_flag,
                          x, y, z, z0, r, dz0dr, wj_in, rcut,
                          rootpqarray.get_pointer(),
                          ulist.get_pointer(),
                          dulist.get_pointer());
         }
      });
    });

    q.wait();
    end = system_clock::now();
    elapsed = end - start;
    elapsed_duidrj += elapsed.count();

    start = system_clock::now();
    // compute_deidrj();
    sycl::range<2> gws_k7 ((num_nbor+15)/16*16, (num_atoms+15)/16*16);
    sycl::range<2> lws_k7 (16, 16);

    q.submit([&] (sycl::handler &cgh) {
      auto idxdu_block = d_idxdu_block.get_access<sycl::access::mode::read>(cgh);
      auto dulist = d_dulist.get_access<sycl::access::mode::read>(cgh);
      auto ylist = d_ylist.get_access<sycl::access::mode::read>(cgh);
      auto dedr = d_dedr.get_access<sycl::access::mode::read_write>(cgh);

      cgh.parallel_for<class compute_deidrj>(sycl::nd_range<2>(gws_k7, lws_k7), [=] (sycl::nd_item<2> item) {
	int nbor = item.get_global_id(0);
	int natom = item.get_global_id(1);
        if (natom < num_atoms && nbor < num_nbor) {
          for (int k = 0; k < 3; k++)
            dedr[ULIST_INDEX(natom, nbor, k)] = 0.0;

          for (int j = 0; j <= twojmax; j++) {
            int jjdu = idxdu_block[j];

            for (int mb = 0; 2 * mb < j; mb++)
              for (int ma = 0; ma <= j; ma++) {

                double jjjmambyarray_r = ylist[INDEX_2D(natom, jjdu)].re;
                double jjjmambyarray_i = ylist[INDEX_2D(natom, jjdu)].im;

                for (int k = 0; k < 3; k++)
                  dedr[ULIST_INDEX(natom, nbor, k)] +=
                    dulist[DULIST_INDEX(natom, nbor, jjdu, k)].re * jjjmambyarray_r +
                    dulist[DULIST_INDEX(natom, nbor, jjdu, k)].im * jjjmambyarray_i;
                jjdu++;
              } // end loop over ma mb

            // For j even, handle middle column

            if (j % 2 == 0) {

              int mb = j / 2;
              for (int ma = 0; ma < mb; ma++) {
                double jjjmambyarray_r = ylist[INDEX_2D(natom, jjdu)].re;
                double jjjmambyarray_i = ylist[INDEX_2D(natom, jjdu)].im;

                for (int k = 0; k < 3; k++)
                  dedr[ULIST_INDEX(natom, nbor, k)] +=
                    dulist[DULIST_INDEX(natom, nbor, jjdu, k)].re * jjjmambyarray_r +
                    dulist[DULIST_INDEX(natom, nbor, jjdu, k)].im * jjjmambyarray_i;
                jjdu++;
              }

              double jjjmambyarray_r = ylist[INDEX_2D(natom, jjdu)].re;
              double jjjmambyarray_i = ylist[INDEX_2D(natom, jjdu)].im;

              for (int k = 0; k < 3; k++)
                dedr[ULIST_INDEX(natom, nbor, k)] +=
                  (dulist[DULIST_INDEX(natom, nbor, jjdu, k)].re * jjjmambyarray_r +
                   dulist[DULIST_INDEX(natom, nbor, jjdu, k)].im * jjjmambyarray_i) *
                  0.5;
              jjdu++;

            } // end if jeven

          } // end loop over j

          for (int k = 0; k < 3; k++)
            dedr[ULIST_INDEX(natom, nbor, k)] *= 2.0;
        }
      });
    });

    q.wait();
    end = system_clock::now();
    elapsed = end - start;
    elapsed_deidrj += elapsed.count();

    q.submit([&] (sycl::handler &cgh) {
      auto acc = d_dedr.get_access<sycl::access::mode::read>(cgh);
      cgh.copy(acc, dedr);
    });
    q.wait();

    // Compute forces and error
    //compute_forces(snaptr);
    for (int natom = 0; natom < num_atoms; natom++) {
      for (int nbor = 0; nbor < num_nbor; nbor++) {
        int j = inside[INDEX_2D(natom, nbor)];
        f[F_INDEX(natom, 0)] += dedr[ULIST_INDEX(natom, nbor, 0)];
        f[F_INDEX(natom, 1)] += dedr[ULIST_INDEX(natom, nbor, 1)];
        f[F_INDEX(natom, 2)] += dedr[ULIST_INDEX(natom, nbor, 2)];
        f[F_INDEX(j, 0)] -= dedr[ULIST_INDEX(natom, nbor, 0)];
        f[F_INDEX(j, 1)] -= dedr[ULIST_INDEX(natom, nbor, 1)];
        f[F_INDEX(j, 2)] -= dedr[ULIST_INDEX(natom, nbor, 2)];

      } // loop over neighbor forces
    }   // loop over atoms
    //    compute_error(snaptr);
    jt = 0;
    for (int j = 0; j < ntotal; j++) {
      double ferrx = f[F_INDEX(j, 0)] - refdata.fj[jt++];
      double ferry = f[F_INDEX(j, 1)] - refdata.fj[jt++];
      double ferrz = f[F_INDEX(j, 2)] - refdata.fj[jt++];
      sumsqferr += ferrx * ferrx + ferry * ferry + ferrz * ferrz;
    }
  }
  auto stop = myclock::now();
  myduration elapsed = stop - begin;
  double duration = elapsed.count();

  printf("-----------------------\n");
  printf("Summary of TestSNAP run\n");
  printf("-----------------------\n");
  printf("natoms = %d \n", nlocal);
  printf("nghostatoms = %d \n", nghost);
  printf("nsteps = %d \n", nsteps);
  printf("nneighs = %d \n", ninside);
  printf("twojmax = %d \n", twojmax);
  printf("duration = %g [sec]\n", duration);

  // step time includes host, device, and host-data transfer time
  double ktime = elapsed_ui + elapsed_yi + elapsed_duidrj + elapsed_deidrj;
  printf("step time = %g [msec/step]\n", 1000.0 * duration / nsteps);
  printf("\n Individual kernel timings for each step\n");
  printf("   compute_ui = %g [msec/step]\n", 1000.0 * elapsed_ui / nsteps);
  printf("   compute_yi = %g [msec/step]\n", 1000.0 * elapsed_yi / nsteps);
  printf("   compute_duidrj = %g [msec/step]\n", 1000.0 * elapsed_duidrj / nsteps);
  printf("   compute_deidrj = %g [msec/step]\n", 1000.0 * elapsed_deidrj / nsteps);
  printf("   Total kernel time = %g [msec/step]\n", 1000.0 * ktime / nsteps);
  printf("   Percentage of step time = %g%%\n\n", ktime / duration * 100.0);
  printf("grind time = %g [msec/atom-step]\n", 1000.0 * duration / (nlocal * nsteps));
  printf("RMS |Fj| deviation %g [eV/A]\n", sqrt(sumsqferr / (ntotal * nsteps)));

  free(coeffi);
  free(idxcg_block);
  free(idxu_block);
  free(ulist_parity);
  free(idxdu_block);
  free(idxb);
  free(idxb_block);
  free(idxz);
  free(idxzbeta);
  free(idxz_block);
  free(rij);
  free(inside);
  free(wj);
  free(rcutij);
  free(rootpqarray);
  free(cglist);
  free(dedr);
  free(ulist);
  free(ylist);
  free(ulisttot);
  free(dulist);
  free(f);

  return 0;
}
