// Copyright (c) 2017-2020, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#include "slate/slate.hh"
#include "blas.hh"
#include "test.hh"
#include "print_matrix.hh"
#include "scalapack_support_routines.hh"
#include "band_utils.hh"
#include "grid_utils.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

//------------------------------------------------------------------------------
template <typename scalar_t>
void test_sterf_work(
    Params& params, bool run)
{
    using real_t = blas::real_type<scalar_t>;
    using blas::real;
    using blas::imag;
    // typedef long long llong;

    // get & mark input values
    int64_t n = params.dim.n();
    //int64_t nb = params.nb();
    int p = params.grid.m();
    int q = params.grid.n();
    bool check = params.check() == 'y';
    bool trace = params.trace() == 'y';
    int verbose = params.verbose();

    // mark non-standard output values
    params.time();
    params.gflops();
    params.ref_time();
    params.ref_gflops();

    if (! run)
        return;

    // Local values
    int myrow, mycol;
    int mpi_rank, mpi_size;

    slate_mpi_call(
        MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank));
    slate_mpi_call(
        MPI_Comm_size(MPI_COMM_WORLD, &mpi_size));

    gridinfo(mpi_rank, p, q, &myrow, &mycol);

    std::vector<real_t> D(n), E(n - 1);
    int64_t idist = 3; // normal
    int64_t iseed[4] = { 0, myrow, mycol, 3 };
    lapack::larnv(idist, iseed, D.size(), D.data());
    lapack::larnv(idist, iseed, E.size(), E.data());
    std::vector<real_t> Dref = D;
    std::vector<real_t> Eref = E;

    //---------
    // run test
    if (trace) slate::trace::Trace::on();
    else slate::trace::Trace::off();

    double time = barrier_get_wtime(MPI_COMM_WORLD);

    //==================================================
    // Run SLATE test.
    //==================================================
    slate::sterf(D, E);

    params.time() = barrier_get_wtime(MPI_COMM_WORLD) - time;

    if (trace)
        slate::trace::Trace::finish();

    if (check) {
        //==================================================
        // Test results
        //==================================================
        if (mpi_rank == 0) {
            // set MKL num threads appropriately for parallel BLAS
            int omp_num_threads;
            #pragma omp parallel
            { omp_num_threads = omp_get_num_threads(); }
            int saved_num_threads = slate_set_num_blas_threads(omp_num_threads);

            real_t tol = params.tol() * 0.5 * std::numeric_limits<real_t>::epsilon();

            //==================================================
            // Run LAPACK reference routine.
            //==================================================
            time = barrier_get_wtime(MPI_COMM_WORLD);

            lapack::sterf(n, &Dref[0], &Eref[0]);

            params.ref_time() = barrier_get_wtime(MPI_COMM_WORLD) - time;

            slate_set_num_blas_threads(saved_num_threads);
            if (verbose) {
                // Print first 20 and last 20 rows.
                printf( "%9s  %9s\n", "D", "Dref" );
                for (int64_t i = 0; i < n; ++i) {
                    if (i < 20 || i > n-20) {
                        bool okay = std::abs( D[i] - Dref[i] ) < tol;
                        printf( "%9.6f  %9.6f%s\n",
                                D[i], Dref[i], (okay ? "" : " !!") );
                    }
                }
                printf( "\n" );
            }

            // Relative forward error: || D - Dref || / || Dref ||.
            blas::axpy(D.size(), -1.0, &Dref[0], 1, &D[0], 1);
            params.error() = blas::nrm2(D.size(), &D[0], 1)
                           / blas::nrm2(Dref.size(), &Dref[0], 1);
            params.okay() = (params.error() <= tol);
        }
    }
}

// -----------------------------------------------------------------------------
void test_sterf(Params& params, bool run)
{
    switch (params.datatype()) {
        case testsweeper::DataType::Integer:
            throw std::exception();
            break;

        case testsweeper::DataType::Single:
            test_sterf_work<float> (params, run);
            break;

        case testsweeper::DataType::Double:
            test_sterf_work<double> (params, run);
            break;

        case testsweeper::DataType::SingleComplex:
            test_sterf_work<std::complex<float>> (params, run);
            break;

        case testsweeper::DataType::DoubleComplex:
            test_sterf_work<std::complex<double>> (params, run);
            break;
    }
}
