// Copyright (c) 2017-2020, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#include "slate/slate.hh"
#include "auxiliary/Debug.hh"
#include "slate/Matrix.hh"
#include "slate/HermitianMatrix.hh"
#include "slate/TriangularMatrix.hh"
#include "internal/internal.hh"

namespace slate {

// specialization namespace differentiates, e.g.,
// internal::potrf from internal::specialization::potrf
namespace internal {
namespace specialization {

//------------------------------------------------------------------------------
/// Distributed parallel Cholesky factorization.
/// Generic implementation for any target.
/// Panel and lookahead computed on host using Host OpenMP task.
/// @ingroup posv_specialization
///
template <Target target, typename scalar_t>
void potrf(slate::internal::TargetType<target>,
           HermitianMatrix<scalar_t> A, int64_t lookahead, Options const& opts)
{
    using real_t = blas::real_type<scalar_t>;
    using BcastListTag = typename Matrix<scalar_t>::BcastListTag;

    // Assumes column major
    const Layout layout = Layout::ColMajor;

    // if upper, change to lower
    if (A.uplo() == Uplo::Upper) {
        A = conjTranspose(A);
    }
    int64_t A_nt = A.nt();

    // OpenMP needs pointer types, but vectors are exception safe
    std::vector< uint8_t > column_vector(A_nt);
    uint8_t* column = column_vector.data();

    #pragma omp parallel
    #pragma omp master
    {
        omp_set_nested(1);
        for (int64_t k = 0; k < A_nt; ++k) {
            // panel, high priority
            #pragma omp task depend(inout:column[k]) priority(1)
            {
                // factor A(k, k)
                internal::potrf<Target::HostTask>(A.sub(k, k), 1);

                // send A(k, k) down col A(k+1:nt-1, k)
                if (k+1 <= A_nt-1)
                    A.tileBcast(k, k, A.sub(k+1, A_nt-1, k, k), layout);

                // A(k+1:nt-1, k) * A(k, k)^{-H}
                if (k+1 <= A_nt-1) {
                    auto Akk = A.sub(k, k);
                    auto Tkk = TriangularMatrix< scalar_t >(Diag::NonUnit, Akk);
                    internal::trsm<Target::HostTask>(
                        Side::Right,
                        scalar_t(1.0), conjTranspose(Tkk),
                        A.sub(k+1, A_nt-1, k, k), 1);
                }

                BcastListTag bcast_list_A;
                for (int64_t i = k+1; i < A_nt; ++i) {
                    // send A(i, k) across row A(i, k+1:i) and down
                    // col A(i:nt-1, i) with msg tag i
                    bcast_list_A.push_back({i, k, {A.sub(i, i, k+1, i),
                                                   A.sub(i, A_nt-1, i, i)},
                                            i});
                }
                A.template listBcastMT(bcast_list_A, layout);
            }
            // update lookahead column(s), high priority
            for (int64_t j = k+1; j < k+1+lookahead && j < A_nt; ++j) {
                #pragma omp task depend(in:column[k]) \
                                 depend(inout:column[j]) priority(1)
                {
                    // A(j, j) -= A(j, k) * A(j, k)^H
                    internal::herk<Target::HostTask>(
                        real_t(-1.0), A.sub(j, j, k, k),
                        real_t( 1.0), A.sub(j, j), 1);

                    // A(j+1:nt-1, j) -= A(j+1:nt-1, k) * A(j, k)^H
                    if (j+1 <= A_nt-1) {
                        auto Ajk = A.sub(j, j, k, k);
                        internal::gemm<Target::HostTask>(
                            scalar_t(-1.0), A.sub(j+1, A_nt-1, k, k),
                                            conjTranspose(Ajk),
                            scalar_t(1.0), A.sub(j+1, A_nt-1, j, j),
                            layout, 1);
                    }
                }
            }
            // update trailing submatrix, normal priority
            if (k+1+lookahead < A_nt) {
                #pragma omp task depend(in:column[k]) \
                                 depend(inout:column[k+1+lookahead]) \
                                 depend(inout:column[A_nt-1])
                {
                    // A(kl+1:nt-1, kl+1:nt-1) -=
                    //     A(kl+1:nt-1, k) * A(kl+1:nt-1, k)^H
                    // where kl = k + lookahead
                    internal::herk<target>(
                        real_t(-1.0), A.sub(k+1+lookahead, A_nt-1, k, k),
                        real_t( 1.0), A.sub(k+1+lookahead, A_nt-1));
                }
            }
        }

        // TODO: causes issues on summit Target::HostTask
        // #pragma omp taskwait
        // A.tileUpdateAllOrigin();
    }

    // Debug::checkTilesLives(A);
    // Debug::printTilesLives(A);
    A.tileUpdateAllOrigin();
    A.releaseWorkspace();
}

template <typename scalar_t>
void potrfCleanTiles(HermitianMatrix<scalar_t> A, int64_t k)
{
    int64_t A_nt = A.nt();
    for (int64_t i = k; i < A_nt; ++i) {
        if (A.tileIsLocal(i, k)) {
            A.tileUpdateOrigin(i, k);
        }
        {

            std::set<int> dev_set;
            A.sub(i, i, k, i).getLocalDevices(&dev_set);
            A.sub(i, A_nt-1, i, i).getLocalDevices(&dev_set);

            // Unset hold on devices and release the tile
            for (auto device : dev_set) {
                A.tileUnsetHold(i, k, device);
                A.tileRelease(i, k, device);
            }
            // Unset hold on host and release the tile
            A.tileUnsetHold(i, k);
            A.tileRelease(i, k);
        }
    }
}

//------------------------------------------------------------------------------
/// An auxiliary routine to release the panel tiles that are broadcasted. Since
/// the broadcasted tiles are flagged to be hold on the devices memories to be
/// accessed by multiple internal kernels while preventing the tileRelease call
/// in these routine to release them before the others finish accessing
/// them. Note: this function update the tiles origin to make sure that
/// the origin memory is up-to-date and the coherency is kept consistent
/// across multiple address spaces.
/// @param[in] A
///     The n-by-n Hermitian positive definite matrix $A$, which is
///     a sub of the input matrix $A$.
///
/// @param[in] k
///     Current column k of the input matrix $A$.
///
/// @ingroup posv_computational
///
template <typename scalar_t>
void potrfReleasePanel(HermitianMatrix<scalar_t> A, int64_t k)
{
    int64_t A_nt = A.nt();
    for (int64_t i = k+1; i < A_nt; ++i) {
        if (A.tileIsLocal(i, k)) {
            A.tileUpdateOrigin(i, k);

            std::set<int> dev_set;
            A.sub(i, i, k+1, i).getLocalDevices(&dev_set);
            A.sub(i, A_nt-1, i, i).getLocalDevices(&dev_set);

            for (auto device : dev_set) {
                A.tileUnsetHold(i, k, device);
                A.tileRelease(i, k, device);
            }
        }
    }
}

//------------------------------------------------------------------------------
/// Distributed parallel Cholesky factorization.
/// GPU device batched cuBLAS implementation.
/// @ingroup posv_specialization
///
template <typename scalar_t>
void potrf(slate::internal::TargetType<Target::Devices>,
           HermitianMatrix<scalar_t> A, int64_t lookahead,
           Options const& opts)
{
    using real_t = blas::real_type<scalar_t>;
    using BcastListTag = typename Matrix<scalar_t>::BcastListTag;

    TileReleaseStrategy tile_release_strategy = get_option( opts, 
            Option::TileReleaseStrategy, TileReleaseStrategy::All );

    // Assumes column major
    const Layout layout = Layout::ColMajor;

    // if upper, change to lower
    if (A.uplo() == Uplo::Upper) {
        A = conjTranspose(A);
    }
    int64_t A_nt = A.nt();

    // OpenMP needs pointer types, but vectors are exception safe
    std::vector< uint8_t > column_vector(A_nt);
    uint8_t* column = column_vector.data();

    const int priority_zero = 0;
    const int life_factor_one = 1;
    const int queue_0 = 0;
    const int queue_1 = 1;
    const int64_t batch_size_zero = 0;
    const int num_queues = 2 + lookahead;  // Number of kernels with lookahead
    const bool is_shared = lookahead > 0;  // Do tileGetAndHold in the bcast

    // Allocate batch arrays = number of kernels without lookahead + lookahead
    // number of kernels without lookahead = 2 (internal::gemm & internal::trsm)
    // whereas internal::herk will be executed as many as lookaheads, thus
    // internal::herk needs batch arrays equal to the number of lookaheads
    // and the batch_arrays_index starts from
    // the number of kernels without lookahead, and then incremented by 1
    // for every execution for the internal::herk
    A.allocateBatchArrays(batch_size_zero, num_queues);
    A.reserveDeviceWorkspace();

    #pragma omp parallel
    #pragma omp master
    {
        omp_set_nested(1);
        for (int64_t k = 0; k < A_nt; ++k) {
            // Panel, normal priority
            #pragma omp task depend(inout:column[k])
            {
                // factor A(k, k)
                internal::potrf<Target::HostTask>(A.sub(k, k));

                // send A(k, k) down col A(k+1:nt-1, k)
                if (k+1 <= A_nt-1)
                    A.tileBcast(k, k, A.sub(k+1, A_nt-1, k, k), layout);

                // A(k+1:nt-1, k) * A(k, k)^{-H}
                if (k+1 <= A_nt-1) {
                    auto Akk = A.sub(k, k);
                    auto Tkk = TriangularMatrix< scalar_t >(Diag::NonUnit, Akk);
                    internal::trsm<Target::Devices>(
                        Side::Right,
                        scalar_t(1.0), conjTranspose(Tkk),
                                       A.sub(k+1, A_nt-1, k, k),
                        priority_zero, layout, queue_1, opts);
                }

                BcastListTag bcast_list_A;
                for (int64_t i = k+1; i < A_nt; ++i) {
                    // send A(i, k) across row A(i, k+1:i) and
                    //                down col A(i:nt-1, i) with msg tag i
                    bcast_list_A.push_back({i, k, {A.sub(i, i, k+1, i),
                                                   A.sub(i, A_nt-1, i, i)},
                                            i});
                }

                // "is_shared" is to request copying the tiles to the devices,
                // and set them on-hold, which avoids releasing them by either
                // internal::gemm or internal::herk
                // (avoiding possible race condition)
                A.template listBcastMT<Target::Devices>(
                  bcast_list_A, layout, life_factor_one, is_shared);
                for(int rk = 0; rk < 2; rk++) {
                    if(A.mpiRank() == rk){
                //        Debug::printTilesLives(A);
                //        Debug::printTilesMaps(A);
                    }
                }
                // std::cout << -(k+1) << std::endl;
            }

            // update trailing submatrix, normal priority
            if (k+1+lookahead < A_nt) {
                #pragma omp task depend(in:column[k]) \
                                 depend(inout:column[k+1+lookahead]) \
                                 depend(inout:column[A_nt-1])
                {
                    // A(kl+1:nt-1, kl+1:nt-1) -=
                    //     A(kl+1:nt-1, k) * A(kl+1:nt-1, k)^H
                    // where kl = k + lookahead
                    internal::herk<Target::Devices>(
                        real_t(-1.0), A.sub(k+1+lookahead, A_nt-1, k, k),
                        real_t( 1.0), A.sub(k+1+lookahead, A_nt-1),
                        priority_zero, queue_0, layout, opts);
                }
            }

            // update lookahead column(s), normal priority
            // the batch_arrays_index_la must be initialized to the
            // lookahead base index (i.e, number of kernels without lookahead),
            // which is equal to "2" for slate::potrf, and then the variable is
            // incremented with every lookahead column "j" ( j-k+1 = 2+j-(k+1) )
            for (int64_t j = k+1; j < k+1+lookahead && j < A_nt; ++j) {
                #pragma omp task depend(in:column[k]) \
                                 depend(inout:column[j])
                {
                    // A(j, j) -= A(j, k) * A(j, k)^H
                    internal::herk<Target::Devices>(
                        real_t(-1.0), A.sub(j, j, k, k),
                        real_t( 1.0), A.sub(j, j), 
                        priority_zero, j-k+1, layout, opts);

                    // A(j+1:nt, j) -= A(j+1:nt-1, k) * A(j, k)^H
                    if (j+1 <= A_nt-1) {
                        auto Ajk = A.sub(j, j, k, k);
                        internal::gemm<Target::Devices>(
                            scalar_t(-1.0), A.sub(j+1, A_nt-1, k, k),
                                            conjTranspose(Ajk),
                            scalar_t( 1.0), A.sub(j+1, A_nt-1, j, j),
                            layout, priority_zero, j-k+1, opts);
                    }
                }
            }

            if (tile_release_strategy == TileReleaseStrategy::All) {
                // update the status of the on-hold tiles held by the invocation of
                // the tileBcast routine, and then release them to free up memory
                // the origin must be updated with the latest modified copy.
                // for memory consistency
                // TODO: find better solution to handle tile release, and
                //       investigate the correctness of the task dependency
                if (lookahead > 0 && k >= lookahead) {
                    #pragma omp task depend(in:column[k]) \
                        depend(inout:column[k+1])
                    {
                        potrfReleasePanel(A, k - lookahead);
                    }
                }
            }


            if (tile_release_strategy == TileReleaseStrategy::Slate) {
                #pragma omp task depend(inout:column[k]) 
                {
                    potrfCleanTiles(A, k);
                }
            }
        }

        #pragma omp taskwait
        A.tileUpdateAllOrigin();
    }
    A.releaseWorkspace();
}

} // namespace specialization
} // namespace internal

//------------------------------------------------------------------------------
/// Version with target as template parameter.
/// @ingroup posv_specialization
///
template <Target target, typename scalar_t>
void potrf(HermitianMatrix<scalar_t>& A,
           Options const& opts)
{
    int64_t lookahead = get_option<int64_t>( opts, Option::Lookahead, 1 );

    internal::specialization::potrf(internal::TargetType<target>(),
                                    A, lookahead, opts);
}

//------------------------------------------------------------------------------
/// Distributed parallel Cholesky factorization.
///
/// Performs the Cholesky factorization of a Hermitian positive definite
/// matrix $A$.
///
/// The factorization has the form
/// \[
///     A = L L^H,
/// \]
/// if $A$ is stored lower, where $L$ is a lower triangular matrix, or
/// \[
///     A = U^H U,
/// \]
/// if $A$ is stored upper, where $U$ is an upper triangular matrix.
///
//------------------------------------------------------------------------------
/// @tparam scalar_t
///     One of float, double, std::complex<float>, std::complex<double>.
//------------------------------------------------------------------------------
/// @param[in,out] A
///     On entry, the n-by-n Hermitian positive definite matrix $A$.
///     On exit, if return value = 0, the factor $U$ or $L$ from the Cholesky
///     factorization $A = U^H U$ or $A = L L^H$.
///     If scalar_t is real, $A$ can be a SymmetricMatrix object.
///
/// @param[in] opts
///     Additional options, as map of name = value pairs. Possible options:
///     - Option::Lookahead:
///       Number of panels to overlap with matrix updates.
///       lookahead >= 0. Default 1.
///     - Option::Target:
///       Implementation to target. Possible values:
///       - HostTask:  OpenMP tasks on CPU host [default].
///       - HostNest:  nested OpenMP parallel for loop on CPU host.
///       - HostBatch: batched BLAS on CPU host.
///       - Devices:   batched BLAS on GPU device.
///
/// TODO: return value
/// @retval 0 successful exit
/// @retval >0 for return value = $i$, the leading minor of order $i$ of $A$ is not
///         positive definite, so the factorization could not
///         be completed, and the solution has not been computed.
///
/// @ingroup posv_computational
///
template <typename scalar_t>
void potrf(HermitianMatrix<scalar_t>& A,
           Options const& opts)
{
    Target target = get_option( opts, Option::Target, Target::HostTask );

    switch (target) {
        case Target::Host:
        case Target::HostTask:
            potrf<Target::HostTask>(A, opts);
            break;
        case Target::HostNest:
            potrf<Target::HostNest>(A, opts);
            break;
        case Target::HostBatch:
            potrf<Target::HostBatch>(A, opts);
            break;
        case Target::Devices:
            potrf<Target::Devices>(A, opts);
            break;
    }
    // todo: return value for errors?
}

//------------------------------------------------------------------------------
// Explicit instantiations.
template
void potrf<float>(
    HermitianMatrix<float>& A,
    Options const& opts);

template
void potrf<double>(
    HermitianMatrix<double>& A,
    Options const& opts);

template
void potrf< std::complex<float> >(
    HermitianMatrix< std::complex<float> >& A,
    Options const& opts);

template
void potrf< std::complex<double> >(
    HermitianMatrix< std::complex<double> >& A,
    Options const& opts);

} // namespace slate
